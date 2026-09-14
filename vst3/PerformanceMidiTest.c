#include "ProbeMachine.h"

#include <stdint.h>
#include <stdio.h>

static void run_for(uint64_t cycles) {
    eps16_probe_machine_run_until(eps16_probe_machine_cycles() + cycles);
}

static int expect_analog(unsigned int channel, unsigned int expected) {
    const unsigned int actual = eps16_probe_machine_analog_value(channel);
    printf("analog[%u]=%u expected=%u\n", channel, actual, expected);
    return actual == expected;
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
    eps16_probe_machine_run_until(220000000);

    int failed = 0;
    const size_t keyboard_before = eps16_probe_machine_panel_rx_consumed();
    eps16_probe_machine_keyboard(36, 127, 1);
    run_for(20000000);
    const size_t keyboard_after_note =
        eps16_probe_machine_panel_rx_consumed();
    eps16_probe_machine_keyboard(36, 1, 0);
    run_for(20000000);
    const size_t keyboard_after_release =
        eps16_probe_machine_panel_rx_consumed();
    printf("gui_keyboard_bytes=%zu/%zu/%zu\n", keyboard_before,
           keyboard_after_note, keyboard_after_release);
    if (keyboard_after_note != keyboard_before + 2 ||
        keyboard_after_release != keyboard_after_note + 2)
        failed = 1;

    /* Host transport is serialized into the real MC68681 channel-A receive
       path. The original OS must consume the realtime bytes. */
    const size_t clock_before = eps16_probe_machine_midi_rx_consumed();
    eps16_probe_machine_midi(0xfa, 0, 0);
    eps16_probe_machine_midi(0xf8, 0, 0);
    eps16_probe_machine_midi(0xfc, 0, 0);
    run_for(2000000);
    const size_t clock_after = eps16_probe_machine_midi_rx_consumed();
    printf("midi_clock_bytes=%zu/%zu\n", clock_before, clock_after);
    if (clock_after != clock_before + 3) failed = 1;

    /* DAW MIDI is external MIDI, not the local EPS keyboard or its physical
       wheels. Preserve status bytes (including their channel nibble) and let
       the original OS perform multitimbral track routing and controller
       interpretation. */
    const size_t midi_before = eps16_probe_machine_midi_rx_consumed();
    const size_t panel_before = eps16_probe_machine_panel_rx_consumed();
    eps16_probe_machine_midi(0x90, 60, 100);
    eps16_probe_machine_midi(0xa0, 60, 50);
    eps16_probe_machine_midi(0xb0, 1, 127);  /* Mod wheel */
    eps16_probe_machine_midi(0xb0, 7, 96);   /* Volume */
    eps16_probe_machine_midi(0xb0, 64, 127); /* Sustain */
    eps16_probe_machine_midi(0xb0, 70, 1);   /* Patch variation */
    eps16_probe_machine_midi(0xc1, 17, 0);   /* Program, channel 2 */
    eps16_probe_machine_midi(0xd2, 41, 0);   /* Channel pressure, channel 3 */
    eps16_probe_machine_midi(0xe3, 1, 65);   /* Pitch bend, channel 4 */
    eps16_probe_machine_midi(0x9f, 67, 88);  /* Note on, channel 16 */
    eps16_probe_machine_midi(0x8f, 67, 0);   /* Note off, channel 16 */
    run_for(40000000);
    const size_t midi_after = eps16_probe_machine_midi_rx_consumed();
    const size_t panel_after = eps16_probe_machine_panel_rx_consumed();
    printf("channel_voice_midi=%zu/%zu panel=%zu/%zu illegal=%zu\n",
           midi_before, midi_after, panel_before, panel_after,
           eps16_probe_machine_illegal_instructions());
    if (midi_after != midi_before + 31 || panel_after != panel_before ||
        !expect_analog(0, 511) || !expect_analog(2, 1023) ||
        eps16_probe_machine_illegal_instructions())
        failed = 1;

    eps16_probe_machine_destroy(machine);
    return failed ? 1 : 0;
}
