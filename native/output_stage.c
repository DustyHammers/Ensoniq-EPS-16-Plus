#include "output_stage.h"

#include <assert.h>
#include <math.h>

#define EPS16_OUTPUT_STAGE_PI 3.14159265358979323846

/* ---- deterministic PRNG (splitmix64) and Gaussian helper --------------
   Used both to build the fixed, reproducible per-channel TDA1541A tables
   below (from a fixed per-channel seed, at first use) and, via the
   per-channel noise_counter persisted in the snapshot, to generate the
   converter's own sample-to-sample noise deterministically. */

static uint64_t eps16_splitmix64_next(uint64_t *state) {
    uint64_t z = (*state += (uint64_t)0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * (uint64_t)0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * (uint64_t)0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static double eps16_gaussian_from(uint64_t *state) {
    const uint64_t r1 = eps16_splitmix64_next(state);
    const uint64_t r2 = eps16_splitmix64_next(state);
    double u1 = (double)(r1 >> 11) * (1.0 / 9007199254740992.0);
    const double u2 = (double)(r2 >> 11) * (1.0 / 9007199254740992.0);
    if (u1 < 1e-300) u1 = 1e-300;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * EPS16_OUTPUT_STAGE_PI * u2);
}

static double eps16_output_stage_channel_noise(
    Eps16OutputStageChannelState *channel) {
    uint64_t state = channel->noise_counter;
    const double sample = eps16_gaussian_from(&state);
    channel->noise_counter = state;
    return sample;
}

/* ---- TDA1541A per-channel LSB tables -----------------------------------
   Each die/half-die gets 64 segment entries (top 6 bits, three cascaded
   2-bit active dividers under DEM) and 1024 low-word entries (bottom 10
   bits, a passive emitter-scaled current divider). Built once per process
   from a fixed per-channel seed, so results are reproducible; they are
   pure functions of the channel index and never change afterward, so they
   are not part of the snapshot. */

static float eps16_segment_table[EPS16_OUTPUT_STAGE_CHANNELS][64];
static float eps16_low_table[EPS16_OUTPUT_STAGE_CHANNELS][1024];
static int eps16_output_stage_tables_ready;

static void eps16_generate_channel_tables(unsigned int channel) {
    uint64_t seed = (uint64_t)0x9E3779B97F4A7C15ULL ^
        ((uint64_t)0xA24BAED4963EE407ULL * (uint64_t)(channel + 1u));

    /* Bit k's cumulative multiplicative error is prod(1+alpha[j]) for
       j = k..9; weight[k] = 2^k times that product. Walking k from 9 down
       to 0 while accumulating the running product builds exactly that. */
    double alpha[10];
    for (unsigned int k = 0; k < 10; ++k)
        alpha[k] = eps16_gaussian_from(&seed) * 3.5e-4;
    double weight[10];
    double running = 1.0;
    for (int k = 9; k >= 0; --k) {
        running *= (1.0 + alpha[(unsigned int)k]);
        weight[k] = running * (double)(1u << (unsigned int)k);
    }
    for (unsigned int n = 0; n < 1024; ++n) {
        double sum = 0.0;
        for (unsigned int k = 0; k < 10; ++k)
            if (n & (1u << k)) sum += weight[k];
        eps16_low_table[channel][n] = (float)sum;
    }
    /* Each segment's DEM residual is a small fraction of one segment's own
       step (1024 LSB), not of its cumulative output value - otherwise the
       high-order segments would carry tens-of-LSB errors, which is not
       what "1.0 LSB max integral linearity" describes. */
    for (unsigned int m = 0; m < 64; ++m) {
        const double error = eps16_gaussian_from(&seed) * 2.5e-4;
        eps16_segment_table[channel][m] =
            (float)((double)m * 1024.0 + 1024.0 * error);
    }
}

static void eps16_output_stage_ensure_tables(void) {
    if (eps16_output_stage_tables_ready) return;
    for (unsigned int channel = 0; channel < EPS16_OUTPUT_STAGE_CHANNELS;
         ++channel)
        eps16_generate_channel_tables(channel);
    eps16_output_stage_tables_ready = 1;
}

/* ---- analog reconstruction filter: discrete coefficients ---------------
   Four cascaded real poles (HP, LP, LP, then a 2-pole LP section),
   prewarped-bilinear-transformed at the current oversampled converter
   rate (EPS16_OUTPUT_STAGE_OVERSAMPLE times native_rate_hz), each
   realized in transposed direct form II (one state word per first-order
   section, two for the biquad - exactly the state kept above). */

typedef struct {
    double b0, b1, a1;
} Eps16OnePole;

typedef struct {
    double b0, b1, b2, a1, a2;
} Eps16Biquad;

typedef struct {
    double re, im;
} Eps16Complex;

static Eps16Complex eps16_cplx_add(Eps16Complex a, Eps16Complex b) {
    Eps16Complex result;
    result.re = a.re + b.re;
    result.im = a.im + b.im;
    return result;
}

static Eps16Complex eps16_cplx_mul(Eps16Complex a, Eps16Complex b) {
    Eps16Complex result;
    result.re = a.re * b.re - a.im * b.im;
    result.im = a.re * b.im + a.im * b.re;
    return result;
}

static Eps16Complex eps16_cplx_div(Eps16Complex a, Eps16Complex b) {
    const double denominator = b.re * b.re + b.im * b.im;
    Eps16Complex result;
    result.re = (a.re * b.re + a.im * b.im) / denominator;
    result.im = (a.im * b.re - a.re * b.im) / denominator;
    return result;
}

static double eps16_cplx_abs(Eps16Complex a) {
    return sqrt(a.re * a.re + a.im * a.im);
}

/* tan(pi*fc/fs) (the prewarped-bilinear substitution below) only has the
   right sign and is only finite for fc < fs/2; at fc == fs/2 it is a pole
   and beyond it it is negative, which is exactly what turned a stable
   lowpass/highpass into a divergent one at the high end of the converter
   rate range (active_voice sweeps the rate from well below the 109040 Hz
   I/V pole up past it during boot). No corner this filter is built from
   is physically allowed to reach Nyquist, so clamp it below there before
   prewarping rather than let the math go singular. */
static double eps16_clamp_corner_hz(double corner_hz, double fs_hz) {
    const double nyquist_margin = 0.49 * fs_hz;
    return corner_hz < nyquist_margin ? corner_hz : nyquist_margin;
}

static void eps16_one_pole_lowpass(Eps16OnePole *section, double fc_hz,
                                   double fs_hz) {
    const double c = tan(EPS16_OUTPUT_STAGE_PI *
                         eps16_clamp_corner_hz(fc_hz, fs_hz) / fs_hz);
    const double b0 = c / (1.0 + c);
    section->b0 = b0;
    section->b1 = b0;
    section->a1 = (c - 1.0) / (c + 1.0);
    assert(isfinite(section->a1) && fabs(section->a1) < 1.0);
}

static void eps16_one_pole_highpass(Eps16OnePole *section, double fc_hz,
                                    double fs_hz) {
    const double c = tan(EPS16_OUTPUT_STAGE_PI *
                         eps16_clamp_corner_hz(fc_hz, fs_hz) / fs_hz);
    const double b0 = 1.0 / (1.0 + c);
    section->b0 = b0;
    section->b1 = -b0;
    section->a1 = (c - 1.0) / (c + 1.0);
    assert(isfinite(section->a1) && fabs(section->a1) < 1.0);
}

static void eps16_biquad_lowpass(Eps16Biquad *section, double f0_hz,
                                 double q, double fs_hz) {
    const double w0 = 2.0 * EPS16_OUTPUT_STAGE_PI *
        eps16_clamp_corner_hz(f0_hz, fs_hz) / fs_hz;
    const double alpha = sin(w0) / (2.0 * q);
    const double cos_w0 = cos(w0);
    const double a0 = 1.0 + alpha;
    section->b0 = ((1.0 - cos_w0) / 2.0) / a0;
    section->b1 = (1.0 - cos_w0) / a0;
    section->b2 = ((1.0 - cos_w0) / 2.0) / a0;
    section->a1 = (-2.0 * cos_w0) / a0;
    section->a2 = (1.0 - alpha) / a0;
    /* Pole magnitude for a conjugate pair is sqrt(a2); squared here to
       avoid the sqrt. */
    assert(isfinite(section->a2) && fabs(section->a2) < 1.0);
}

static Eps16Complex eps16_one_pole_response(const Eps16OnePole *section,
                                            Eps16Complex z_inv) {
    const Eps16Complex b0 = {section->b0, 0.0};
    const Eps16Complex b1 = {section->b1, 0.0};
    const Eps16Complex one = {1.0, 0.0};
    const Eps16Complex a1 = {section->a1, 0.0};
    const Eps16Complex numerator =
        eps16_cplx_add(b0, eps16_cplx_mul(b1, z_inv));
    const Eps16Complex denominator =
        eps16_cplx_add(one, eps16_cplx_mul(a1, z_inv));
    return eps16_cplx_div(numerator, denominator);
}

static Eps16Complex eps16_biquad_response(const Eps16Biquad *section,
                                          Eps16Complex z_inv) {
    const Eps16Complex z_inv2 = eps16_cplx_mul(z_inv, z_inv);
    const Eps16Complex b0 = {section->b0, 0.0};
    const Eps16Complex b1 = {section->b1, 0.0};
    const Eps16Complex b2 = {section->b2, 0.0};
    const Eps16Complex one = {1.0, 0.0};
    const Eps16Complex a1 = {section->a1, 0.0};
    const Eps16Complex a2 = {section->a2, 0.0};
    const Eps16Complex numerator = eps16_cplx_add(
        eps16_cplx_add(b0, eps16_cplx_mul(b1, z_inv)),
        eps16_cplx_mul(b2, z_inv2));
    const Eps16Complex denominator = eps16_cplx_add(
        eps16_cplx_add(one, eps16_cplx_mul(a1, z_inv)),
        eps16_cplx_mul(a2, z_inv2));
    return eps16_cplx_div(numerator, denominator);
}

static double eps16_one_pole_process(const Eps16OnePole *section,
                                     double *state, double x) {
    const double y = section->b0 * x + *state;
    *state = section->b1 * x - section->a1 * y;
    return y;
}

static double eps16_biquad_process(const Eps16Biquad *section, double *s1,
                                   double *s2, double x) {
    const double y = section->b0 * x + *s1;
    *s1 = section->b1 * x - section->a1 * y + *s2;
    *s2 = section->b2 * x - section->a2 * y;
    return y;
}

/* fc/f0/Q are the exact poles of the full loaded network given in the
   task, not isolated-stage values: I/V stage R125||C125, coupling C126,
   then R155/C159 -> R169/C157 -> R167 with R168 (or OEX-6's R18) and C158
   around the op-amp. The high-pass and the 109040.1090 Hz low-pass are
   shared by every channel; only the first low-pass pole and the biquad
   differ between the main board and OEX-6. */
typedef struct {
    Eps16OnePole hp;
    Eps16OnePole lp1;
    Eps16OnePole lp2;
    Eps16Biquad bq;
    double cascade_gain;
    uint32_t rate_hz;
} Eps16OutputStageFilterSet;

static Eps16OutputStageFilterSet eps16_main_filter_set;
static Eps16OutputStageFilterSet eps16_oex_filter_set;

static void eps16_ensure_filter_set(Eps16OutputStageFilterSet *filter_set,
                                    double lp1_fc_hz, double bq_f0_hz,
                                    double bq_q, uint32_t rate_hz) {
    if (filter_set->rate_hz == rate_hz) return;
    const double fs_hz = (double)rate_hz;
    eps16_one_pole_highpass(&filter_set->hp, 2.697541, fs_hz);
    eps16_one_pole_lowpass(&filter_set->lp1, lp1_fc_hz, fs_hz);
    eps16_one_pole_lowpass(&filter_set->lp2, 109040.1090, fs_hz);
    eps16_biquad_lowpass(&filter_set->bq, bq_f0_hz, bq_q, fs_hz);

    /* Normalize the cascade to unity gain at 1 kHz so enabling this stage
       does not change output level. */
    const double theta = 2.0 * EPS16_OUTPUT_STAGE_PI * 1000.0 / fs_hz;
    const Eps16Complex z_inv = {cos(theta), -sin(theta)};
    Eps16Complex response = eps16_one_pole_response(&filter_set->hp, z_inv);
    response = eps16_cplx_mul(
        response, eps16_one_pole_response(&filter_set->lp1, z_inv));
    response = eps16_cplx_mul(
        response, eps16_one_pole_response(&filter_set->lp2, z_inv));
    response = eps16_cplx_mul(response,
                              eps16_biquad_response(&filter_set->bq, z_inv));
    const double magnitude = eps16_cplx_abs(response);
    filter_set->cascade_gain = magnitude > 1e-12 ? 1.0 / magnitude : 1.0;
    filter_set->rate_hz = rate_hz;
}

/* ---- public API ---------------------------------------------------- */

void eps16_output_stage_reset(Eps16OutputStageState *state) {
    for (unsigned int channel = 0; channel < EPS16_OUTPUT_STAGE_CHANNELS;
         ++channel) {
        Eps16OutputStageChannelState *channel_state = &state->channel[channel];
        channel_state->hp_state = 0.0;
        channel_state->lp1_state = 0.0;
        channel_state->lp2_state = 0.0;
        channel_state->bq_state1 = 0.0;
        channel_state->bq_state2 = 0.0;
        channel_state->previous_segment = -1;
        channel_state->noise_counter = 0;
    }
}

void eps16_output_stage_set_enabled(Eps16OutputStageState *state,
                                    int enabled) {
    enabled = enabled ? 1 : 0;
    if (state->enabled == enabled) return;
    state->enabled = enabled;
    eps16_output_stage_reset(state);
}

int eps16_output_stage_is_enabled(const Eps16OutputStageState *state) {
    return state->enabled;
}

float eps16_output_stage_convert(Eps16OutputStageChannelState *channel_state,
                                 unsigned int channel, int16_t pcm_sample) {
    if (channel >= EPS16_OUTPUT_STAGE_CHANNELS) return 0.0f;
    eps16_output_stage_ensure_tables();

    const int32_t code = (int32_t)pcm_sample + 32768;
    const unsigned int segment = ((unsigned int)code >> 10) & 63u;
    const unsigned int low_index = (unsigned int)code & 1023u;
    double value = (double)eps16_segment_table[channel][segment] +
                   (double)eps16_low_table[channel][low_index] - 32768.0;
    if (channel_state->previous_segment >= 0)
        value += 0.95 * fabs((double)segment -
                             (double)channel_state->previous_segment);
    channel_state->previous_segment = (int32_t)segment;
    value += 0.146 * eps16_output_stage_channel_noise(channel_state);
    return (float)(value / 32768.0);
}

void eps16_output_stage_process(Eps16OutputStageState *state,
                                unsigned int channel, int16_t pcm_sample,
                                uint32_t native_rate_hz,
                                float out[EPS16_OUTPUT_STAGE_OVERSAMPLE]) {
    if (channel >= EPS16_OUTPUT_STAGE_CHANNELS || native_rate_hz == 0) {
        for (unsigned int k = 0; k < EPS16_OUTPUT_STAGE_OVERSAMPLE; ++k)
            out[k] = 0.0f;
        return;
    }
    Eps16OutputStageFilterSet *filter_set =
        channel < 2 ? &eps16_main_filter_set : &eps16_oex_filter_set;
    eps16_ensure_filter_set(filter_set, channel < 2 ? 27033.4428 : 26964.0157,
                            channel < 2 ? 23685.8437 : 24267.9333,
                            channel < 2 ? 1.739579 : 1.759418,
                            native_rate_hz * EPS16_OUTPUT_STAGE_OVERSAMPLE);

    Eps16OutputStageChannelState *channel_state = &state->channel[channel];
    const double converter_output =
        (double)eps16_output_stage_convert(channel_state, channel, pcm_sample);

    for (unsigned int k = 0; k < EPS16_OUTPUT_STAGE_OVERSAMPLE; ++k) {
        /* The zero-order hold: every one of the oversampled sub-samples
           starts from the same, single converter output - the repetition
           itself is the hold. Only the filter's own memory makes the
           results differ. */
        double x = converter_output;
        x = eps16_one_pole_process(&filter_set->hp, &channel_state->hp_state,
                                   x);
        x = eps16_one_pole_process(&filter_set->lp1, &channel_state->lp1_state,
                                   x);
        x = eps16_one_pole_process(&filter_set->lp2, &channel_state->lp2_state,
                                   x);
        x = eps16_biquad_process(&filter_set->bq, &channel_state->bq_state1,
                                 &channel_state->bq_state2, x);
        out[k] = (float)(x * filter_set->cascade_gain);
    }
}
