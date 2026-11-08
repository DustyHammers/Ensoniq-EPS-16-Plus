#ifndef EPS16_OUTPUT_STAGE_H
#define EPS16_OUTPUT_STAGE_H

/* Model of the EPS-16+'s DAC output stage: a Philips TDA1541A per stereo
   pair (non-oversampled, no digital filter ahead of it - data and clocks
   reach it through bare 74HCT34 buffers) followed by a zero-order hold and
   the board's own analog reconstruction filter. See ProbeMachine.c's call
   site for how this replaces the plain audio_to_pcm16()/32768.0f
   conversion when enabled, and PluginProcessor/ProbeMachineSink for how the
   16x-oversampled result reaches a second BandlimitedResampler
   configuration at host rate. 16x, not 8x: at 8x the highest converter
   rate the OS sweeps through during boot (active_voice 31, ~19531 Hz
   converter rate) oversamples to below twice the 109040.1090 Hz I/V pole,
   putting that pole beyond the oversampled Nyquist and making the
   prewarped-bilinear transform singular. 16x clears it at every
   active_voice from 0 to 31.

   Channels are indexed exactly as the plug-in's existing 8-wide output
   array: 0/1 Main L/R (main board U48), 2/3 Bus 2 L/R (OEX-6 U8), 4/5
   Bus 3 L/R (OEX-6 U11), 6/7 Aux L/R (OEX-6 U5). Channels 0/1 use the main
   board's loaded-filter poles (R168 = 17.8K); 2-7 use OEX-6's (R18 =
   17.0K). */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPS16_OUTPUT_STAGE_CHANNELS 8
#define EPS16_OUTPUT_STAGE_OVERSAMPLE 16

/* Per-channel state that evolves sample to sample: the reconstruction
   filter's integrator state (transposed direct form II; one value per
   first-order section, two for the second-order section), the previous
   TDA1541A segment index (for the next sample's charge-injection glitch;
   -1 means "no previous sample yet", so the very first sample after a
   reset or restore never charges a phantom glitch), and the deterministic
   noise generator's counter. This is exactly the state the task requires
   in the snapshot for save/restore continuity - the per-channel LSB
   tables are fixed, reproducible build-time constants and are not part of
   it. */
typedef struct {
    double hp_state;
    double lp1_state;
    double lp2_state;
    double bq_state1;
    double bq_state2;
    int32_t previous_segment;
    uint64_t noise_counter;
} Eps16OutputStageChannelState;

typedef struct {
    Eps16OutputStageChannelState channel[EPS16_OUTPUT_STAGE_CHANNELS];
    int enabled;
} Eps16OutputStageState;

/* Resets every channel's filter continuity state (to quiescent/zero),
   previous-segment tracking (to "none yet") and noise counter (to zero).
   Does not touch `enabled` - callers that need a true power-up default
   (enabled on) set that field directly. Call this on machine
   creation/reset and on any enabled/disabled transition, so neither a
   reboot nor a future toggle can leak state across it. */
void eps16_output_stage_reset(Eps16OutputStageState *state);

/* Sets `enabled`, resetting continuity state if this is an actual change
   so the switch cannot leak state across the transition. The seam for a
   later user-facing parameter: wiring one in is exactly a call here. */
void eps16_output_stage_set_enabled(Eps16OutputStageState *state, int enabled);
int eps16_output_stage_is_enabled(const Eps16OutputStageState *state);

/* The TDA1541A conversion alone (segment/low-word tables, charge-injection
   glitch and noise), with no hold and no analog filter: one PCM16 sample
   in, roughly in [-1, 1] out. eps16_output_stage_process() below is this
   plus the hold and filter; exposed separately because it is itself a
   meaningful characteristic (see the converter-alone THD+N test) and
   because the filter needs its own, later input unaffected by it. */
float eps16_output_stage_convert(Eps16OutputStageChannelState *channel_state,
                                 unsigned int channel, int16_t pcm_sample);

/* Converts one native-rate PCM16 sample for `channel` (0-7) through the
   TDA1541A model, then the zero-order hold (repetition) and the analog
   reconstruction filter cascade at EPS16_OUTPUT_STAGE_OVERSAMPLE times
   `native_rate_hz`, writing that many successive oversampled output
   samples (roughly in [-1, 1]) to `out`. Filter coefficients for this
   channel's variant (main board vs OEX-6) are recomputed internally
   whenever `native_rate_hz` changes from the last call. */
void eps16_output_stage_process(Eps16OutputStageState *state,
                                unsigned int channel, int16_t pcm_sample,
                                uint32_t native_rate_hz,
                                float out[EPS16_OUTPUT_STAGE_OVERSAMPLE]);

#ifdef __cplusplus
}
#endif

#endif
