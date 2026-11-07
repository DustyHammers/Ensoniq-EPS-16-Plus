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

    /* Real hardware has two note inputs: its own keyboard, which always
       plays the selected instrument regardless of MIDI IN MODE, and the
       MIDI jack, which MIDI IN MODE governs. eps16_probe_machine_midi()
       models the keyboard as note on/off hard-coded to MIDI channel 1 (see
       the rationale there - deliberate divergence from upstream, never
       MIDI BASE CHANNEL, never a plug-in parameter): the 0x90 below is a
       channel-1 note on, so it now takes the local keyboard path
       (live_note(), the same panel/KPC byte queue as
       eps16_probe_machine_keyboard() above) instead of reaching the wire,
       which moves 3 bytes off the wire total and adds 2 bytes to the panel
       total. Channels 2-16 remain external MIDI - note on/off there, and
       every other channel-voice message on every channel including
       channel 1 (aftertouch, CC, program change, channel pressure, pitch
       bend), still reach the wire with their status byte and channel
       nibble intact, so the original OS still performs multitimbral track
       routing and controller interpretation. */
    const size_t midi_before = eps16_probe_machine_midi_rx_consumed();
    const size_t panel_before = eps16_probe_machine_panel_rx_consumed();
    eps16_probe_machine_midi(0x90, 60, 100); /* Note on, channel 1: keyboard path */
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
    /* Wire total: 11 messages, all 3 bytes except the two channel-voice
       messages below that are 2 bytes (program change 0xc1, channel
       pressure 0xd2) = 9*3 + 2*2 = 31 bytes, minus the channel-1 note on
       (0x90, 3 bytes) that now takes the keyboard path instead = 28.
       Panel total: +2 bytes for that same note on (note 60 is within the
       EPS's 36-96 key range, so live_note() does not drop it). */
    if (midi_after != midi_before + 28 || panel_after != panel_before + 2 ||
        !expect_analog(0, 511) || !expect_analog(2, 1023) ||
        eps16_probe_machine_illegal_instructions())
        failed = 1;

    eps16_probe_machine_destroy(machine);
    return failed ? 1 : 0;
}
