#include "ProbeMachine.h"

#include <math.h>
#include <stdio.h>

/* Test 1 from the output-stage task: with the flag off, the signal path
   must be bit-identical to the pre-output-stage build -
   (float)audio_to_pcm16(x)/32768.0f straight into the 1x
   BandlimitedResampler, no converter model, no hold, no filter. There is
   no public accessor for the raw pre-conversion int32 sample to compare
   against directly, so this checks the two structural properties that
   distinguish the bypass path from the enabled one: every frame's
   clock_divider is the full native-rate divider (the enabled path would
   instead queue EPS16_OUTPUT_STAGE_OVERSAMPLE frames per native sample,
   each at 1/8th the divider), and every output sample is an exact
   multiple of 1/32768 (an integer PCM16 code divided by 32768 - the
   enabled path's converter/hold/filter math practically never lands on
   an exact multiple). Test 2 (OutputStageMagnitudeResponseTest.c's
   sibling, native/output_stage_test.c) separately verifies the enabled
   path actually differs. */

static int is_exact_pcm16_over_32768(float value) {
    const double scaled = (double)value * 32768.0;
    return fabs(scaled - (double)(int32_t)lround(scaled)) < 1e-4;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s COMBINED_ROM KPC_ROM OS_DISK\n", argv[0]);
        return 2;
    }
    Eps16ProbeMachine *machine = eps16_probe_machine_create();
    char error[256] = {0};
    if (!machine || !eps16_probe_machine_begin(machine) ||
        !eps16_probe_machine_initialize(argv[1], argv[2], argv[3], error,
                                        sizeof(error))) {
        fprintf(stderr, "machine initialization failed: %s\n", error);
        return 1;
    }

    eps16_probe_machine_set_output_stage_enabled(0);
    if (eps16_probe_machine_output_stage_enabled()) {
        fprintf(stderr, "stage did not report disabled\n");
        return 1;
    }

    eps16_probe_machine_run_until(100000000);

    int failed = 0;
    size_t total_frames = 0;
    Eps16ProbeAudioFrame frames[64];
    for (int pass = 0; pass < 2000; ++pass) {
        eps16_probe_machine_run_until(eps16_probe_machine_cycles() + 50000);
        size_t count;
        while ((count = eps16_probe_machine_drain_audio(frames, 64)) > 0) {
            for (size_t index = 0; index < count; ++index) {
                const Eps16ProbeAudioFrame *frame = &frames[index];
                if (frame->clock_divider < 16 ||
                    (frame->clock_divider % 16) != 0) {
                    fprintf(stderr,
                            "bypass frame has a non-1x clock_divider=%u\n",
                            frame->clock_divider);
                    failed = 1;
                }
                for (unsigned int channel = 0; channel < 8; ++channel) {
                    if (!is_exact_pcm16_over_32768(frame->outputs[channel])) {
                        fprintf(stderr,
                                "bypass output[%u]=%.9f is not an exact "
                                "PCM16/32768 value\n",
                                channel, (double)frame->outputs[channel]);
                        failed = 1;
                    }
                }
            }
            total_frames += count;
            if (count < 64) break;
        }
    }
    printf("bypass_frames_checked=%zu\n", total_frames);
    if (!total_frames) {
        fprintf(stderr, "no audio frames were produced to check\n");
        failed = 1;
    }

    /* Confirms the two checks above are actually discriminating: turning
       the stage back on changes both properties they test. */
    eps16_probe_machine_set_output_stage_enabled(1);
    if (!eps16_probe_machine_output_stage_enabled()) {
        fprintf(stderr, "stage did not report re-enabled\n");
        failed = 1;
    }
    int saw_subdivided_frame = 0;
    int saw_non_exact_output = 0;
    for (int pass = 0; pass < 2000 && !(saw_subdivided_frame &&
                                        saw_non_exact_output);
         ++pass) {
        eps16_probe_machine_run_until(eps16_probe_machine_cycles() + 50000);
        size_t count;
        while ((count = eps16_probe_machine_drain_audio(frames, 64)) > 0) {
            for (size_t index = 0; index < count; ++index) {
                const Eps16ProbeAudioFrame *frame = &frames[index];
                if ((frame->clock_divider % 16) != 0) saw_subdivided_frame = 1;
                for (unsigned int channel = 0; channel < 8; ++channel)
                    if (!is_exact_pcm16_over_32768(frame->outputs[channel]))
                        saw_non_exact_output = 1;
            }
            if (count < 64) break;
        }
    }
    printf("enabled_saw_subdivided_frame=%d enabled_saw_non_exact_output=%d\n",
           saw_subdivided_frame, saw_non_exact_output);
    if (!saw_subdivided_frame || !saw_non_exact_output) {
        fprintf(stderr,
                "enabling the stage did not change either property; the "
                "bypass checks above would not have caught a regression\n");
        failed = 1;
    }

    eps16_probe_machine_destroy(machine);
    return failed ? 1 : 0;
}
