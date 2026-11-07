#include "output_stage.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define EPS16_TEST_PI 3.14159265358979323846
#define FFT_SIZE (1u << 18) /* 2^18, per the task's converter-alone test */

static double g_real[FFT_SIZE];
static double g_imag[FFT_SIZE];

/* Iterative in-place radix-2 Cooley-Tukey FFT (forward if `sign` is -1,
   inverse if `sign` is +1; inverse also divides by N). Used only to
   band-limit the THD+N residual to 20 Hz-20 kHz by zeroing bins outside
   that range - never to locate or remove the fundamental, which is done
   exactly, in the time domain, so bin alignment/coherence is irrelevant. */
static void fft(double *real, double *imag, unsigned int n, int sign) {
    for (unsigned int i = 1, j = 0; i < n; ++i) {
        unsigned int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = real[i]; real[i] = real[j]; real[j] = t;
            t = imag[i]; imag[i] = imag[j]; imag[j] = t;
        }
    }
    for (unsigned int length = 2; length <= n; length <<= 1) {
        const double angle = sign * 2.0 * EPS16_TEST_PI / (double)length;
        const double wr = cos(angle);
        const double wi = sin(angle);
        for (unsigned int start = 0; start < n; start += length) {
            double cur_r = 1.0, cur_i = 0.0;
            for (unsigned int k = 0; k < length / 2; ++k) {
                const unsigned int a = start + k;
                const unsigned int b = start + k + length / 2;
                const double br = real[b] * cur_r - imag[b] * cur_i;
                const double bi = real[b] * cur_i + imag[b] * cur_r;
                real[b] = real[a] - br;
                imag[b] = imag[a] - bi;
                real[a] += br;
                imag[a] += bi;
                const double next_r = cur_r * wr - cur_i * wi;
                const double next_i = cur_r * wi + cur_i * wr;
                cur_r = next_r;
                cur_i = next_i;
            }
        }
    }
    if (sign == 1)
        for (unsigned int i = 0; i < n; ++i) { real[i] /= n; imag[i] /= n; }
}

static double measure_thd_n_db(double freq_hz, double fs_hz,
                               double amplitude_dbfs) {
    const double amplitude = pow(10.0, amplitude_dbfs / 20.0);
    Eps16OutputStageChannelState channel_state;
    channel_state.hp_state = channel_state.lp1_state = channel_state.lp2_state
        = channel_state.bq_state1 = channel_state.bq_state2 = 0.0;
    channel_state.previous_segment = -1;
    channel_state.noise_counter = 0;

    static double signal[FFT_SIZE];
    for (unsigned int n = 0; n < FFT_SIZE; ++n) {
        const double phase = 2.0 * EPS16_TEST_PI * freq_hz * (double)n / fs_hz;
        const int16_t pcm = (int16_t)lround(amplitude * 32767.0 * sin(phase));
        signal[n] = (double)eps16_output_stage_convert(&channel_state, 0, pcm);
    }

    /* Exact time-domain synchronous detection of the fundamental: a
       least-squares single-tone fit needs no bin alignment. */
    double dot_cos = 0.0, dot_sin = 0.0;
    for (unsigned int n = 0; n < FFT_SIZE; ++n) {
        const double phase = 2.0 * EPS16_TEST_PI * freq_hz * (double)n / fs_hz;
        dot_cos += signal[n] * cos(phase);
        dot_sin += signal[n] * sin(phase);
    }
    const double a_cos = 2.0 * dot_cos / FFT_SIZE;
    const double a_sin = 2.0 * dot_sin / FFT_SIZE;
    const double fundamental_amplitude = sqrt(a_cos * a_cos + a_sin * a_sin);

    for (unsigned int n = 0; n < FFT_SIZE; ++n) {
        const double phase = 2.0 * EPS16_TEST_PI * freq_hz * (double)n / fs_hz;
        const double fitted = a_cos * cos(phase) + a_sin * sin(phase);
        g_real[n] = signal[n] - fitted;
        g_imag[n] = 0.0;
    }

    fft(g_real, g_imag, FFT_SIZE, -1);
    const double bin_hz = fs_hz / FFT_SIZE;
    const unsigned int low_bin = (unsigned int)ceil(20.0 / bin_hz);
    const unsigned int high_bin = (unsigned int)floor(20000.0 / bin_hz);
    for (unsigned int k = 0; k < FFT_SIZE; ++k) {
        const unsigned int mirror = (FFT_SIZE - k) % FFT_SIZE;
        const int in_band = k <= high_bin && k >= low_bin;
        const int mirror_in_band = mirror <= high_bin && mirror >= low_bin;
        if (!in_band && !mirror_in_band) { g_real[k] = 0.0; g_imag[k] = 0.0; }
    }
    fft(g_real, g_imag, FFT_SIZE, 1);

    double sum_squares = 0.0;
    for (unsigned int n = 0; n < FFT_SIZE; ++n) sum_squares += g_real[n] * g_real[n];
    const double residual_rms = sqrt(sum_squares / FFT_SIZE);
    const double fundamental_rms = fundamental_amplitude / sqrt(2.0);
    return 20.0 * log10(residual_rms / fundamental_rms);
}

static double measure_chain_response_db(double freq_hz, uint32_t native_rate_hz) {
    const double amplitude = pow(10.0, -3.0 / 20.0);
    Eps16OutputStageState state;
    for (unsigned int c = 0; c < EPS16_OUTPUT_STAGE_CHANNELS; ++c) {
        state.channel[c].hp_state = state.channel[c].lp1_state =
            state.channel[c].lp2_state = state.channel[c].bq_state1 =
                state.channel[c].bq_state2 = 0.0;
        state.channel[c].previous_segment = -1;
        state.channel[c].noise_counter = 0;
    }
    state.enabled = 1;

    const double seconds = 2.0;
    const uint64_t native_samples = (uint64_t)(seconds * native_rate_hz);
    const uint32_t analog_rate_hz = native_rate_hz * EPS16_OUTPUT_STAGE_OVERSAMPLE;
    const uint64_t discard_analog_samples = analog_rate_hz; /* 1 s settle */

    double dot_cos = 0.0, dot_sin = 0.0;
    uint64_t measured = 0;
    uint64_t analog_index = 0;
    for (uint64_t n = 0; n < native_samples; ++n) {
        const double native_phase =
            2.0 * EPS16_TEST_PI * freq_hz * (double)n / (double)native_rate_hz;
        const int16_t pcm =
            (int16_t)lround(amplitude * 32767.0 * sin(native_phase));
        float out[EPS16_OUTPUT_STAGE_OVERSAMPLE];
        eps16_output_stage_process(&state, 0, pcm, native_rate_hz, out);
        for (unsigned int k = 0; k < EPS16_OUTPUT_STAGE_OVERSAMPLE; ++k) {
            if (analog_index >= discard_analog_samples) {
                const double analog_phase = 2.0 * EPS16_TEST_PI * freq_hz *
                    (double)analog_index / (double)analog_rate_hz;
                dot_cos += (double)out[k] * cos(analog_phase);
                dot_sin += (double)out[k] * sin(analog_phase);
                ++measured;
            }
            ++analog_index;
        }
    }
    const double a_cos = 2.0 * dot_cos / (double)measured;
    const double a_sin = 2.0 * dot_sin / (double)measured;
    const double output_amplitude = sqrt(a_cos * a_cos + a_sin * a_sin);
    return 20.0 * log10(output_amplitude / amplitude);
}

static int check_close(const char *label, double actual, double expected,
                       double tolerance) {
    const double diff = fabs(actual - expected);
    printf("%s actual=%.4f expected=%.4f diff=%.4f tolerance=%.4f %s\n",
           label, actual, expected, diff, tolerance,
           diff <= tolerance ? "OK" : "FAIL");
    return diff <= tolerance;
}

int main(void) {
    int ok = 1;

    /* "1000.6 Hz ... coherent" means an exact integer number of cycles
       over the 2^18-sample window; 1487 cycles is the nearest integer and
       lands at 1000.621 Hz, which is what "1000.6 Hz" rounds from. Using
       that exact frequency (rather than literally 1000.6) avoids a
       spectral-leakage bias in the fundamental fit that would otherwise
       swamp a measurement at these very low target levels. */
    const double coherent_freq_hz = 1487.0 * 176400.0 / (double)FFT_SIZE;
    ok &= check_close("thd_n_0dBFS",
                      measure_thd_n_db(coherent_freq_hz, 176400.0, 0.0),
                      -95.0, 3.0);
    ok &= check_close("thd_n_-60dBFS",
                      measure_thd_n_db(coherent_freq_hz, 176400.0, -60.0),
                      -42.0, 3.0);

    static const double frequencies[4] = {1000.0, 10000.0, 15000.0, 20000.0};
    static const double expected_44642[4] = {-0.01, 0.01, 0.16, -0.16};
    static const double expected_29761[4] = {-0.02, -0.94, -2.16, -4.90};
    for (int i = 0; i < 4; ++i) {
        char label[64];
        snprintf(label, sizeof(label), "chain_44642_%.0fHz", frequencies[i]);
        ok &= check_close(label,
                          measure_chain_response_db(frequencies[i], 44642U),
                          expected_44642[i], 0.3);
    }
    for (int i = 0; i < 4; ++i) {
        char label[64];
        snprintf(label, sizeof(label), "chain_29761_%.0fHz", frequencies[i]);
        ok &= check_close(label,
                          measure_chain_response_db(frequencies[i], 29761U),
                          expected_29761[i], 0.3);
    }

    return ok ? 0 : 1;
}
