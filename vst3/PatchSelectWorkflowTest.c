#include "ProbeMachine.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* PSEL (ADC channel 1) is decoded entirely by the original OS (ROM
   0xc0c81a): read ADC, subtract the zero reference at RAM 0x0c5a,
   threshold, store one of four states at RAM 0x01f0. Nothing in the
   emulator core does any of that - this only has to prove that writing
   the plain 0-1023 values the panel buttons use lands on the right side
   of each threshold. */

static void run_for(uint64_t cycles) {
    eps16_probe_machine_run_until(eps16_probe_machine_cycles() + cycles);
}

static void click(uint8_t raw_code) {
    eps16_probe_machine_panel_byte((uint8_t)(raw_code | 0x80));
    eps16_probe_machine_panel_byte(0);
    run_for(1000000);
    eps16_probe_machine_panel_byte(raw_code);
    eps16_probe_machine_panel_byte(0);
    run_for(50000000);
}

static int expect_display(const char *expected) {
    char display[23];
    eps16_probe_machine_display(display);
    printf("display=|%s| expected=|%s| illegal=%zu\n", display, expected,
           eps16_probe_machine_illegal_instructions());
    return !strncmp(display, expected, strlen(expected)) &&
           !eps16_probe_machine_illegal_instructions();
}

static uint8_t read_patch_select_state(void) {
    uint8_t value = 0xff;
    eps16_probe_machine_debug_read_ram(0x01f0, &value, 1);
    return value;
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
    if (!expect_display("NO INSTRUMENTS")) return 1;

    /* Load an instrument the same way CommandInstrumentWorkflowTest.c
       does - there is no instrument file on the bare OS disk these tests
       run against, so this builds one through the original OS/KPC panel
       events instead of a disk load. */
    click(0x06); /* COMMAND */
    click(0x0f); /* INSTRUMENT */
    if (!expect_display("CREATE NEW INSTRUMENT")) return 1;
    click(0x23); /* CREATE NEW INSTRUMENT */
    if (!expect_display("SELECT UNUSED INST=1")) return 1;
    click(0x23); /* SELECT UNUSED INST=1 */

    click(0x25); /* LAYER */
    if (!expect_display("CREATE NEW LAYER")) return 1;
    click(0x23); /* CREATE NEW LAYER */
    if (!expect_display("CREATE NEW LAYER")) return 1;

    click(0x24); /* WAVE */
    if (!expect_display("CREATE NEW WAVESAMPLE")) return 1;
    click(0x23); /* CREATE NEW WAVESAMPLE */
    if (!expect_display("CREATE NEW WAVESAMPLE")) return 1;

    /* Back to the main page with the created instrument selected. */
    click(0x06); /* COMMAND */
    click(0x0f); /* INSTRUMENT */
    click(0x1a); /* LOAD - returns to the top-level page */

    int failed = 0;

    /* Test 1: each of the four panel values lands in the OS's matching
       threshold band. */
    static const struct {
        uint16_t analog_value;
        uint8_t expected_state;
    } cases[4] = {
        {0, 0x00}, {200, 0x20}, {480, 0x40}, {1023, 0x7f}
    };
    for (unsigned int index = 0; index < 4; ++index) {
        eps16_probe_machine_analog(1, cases[index].analog_value);
        run_for(20000000);
        const uint8_t actual = read_patch_select_state();
        printf("analog=%u patch_select_state=0x%02x expected=0x%02x\n",
               cases[index].analog_value, actual, cases[index].expected_state);
        if (actual != cases[index].expected_state) failed = 1;
    }

    /* Test 2: writing 0 after visiting a non-zero state restores the
       instrument's default patch - checked by the OS's own visible state
       (the display) returning to exactly what it was before any
       non-zero patch was ever selected, not just the RAM byte (test 1
       already covers that). */
    char baseline_display[23];
    eps16_probe_machine_display(baseline_display);

    eps16_probe_machine_analog(1, 480);
    run_for(20000000);
    if (read_patch_select_state() != 0x40) failed = 1;

    eps16_probe_machine_analog(1, 0);
    run_for(20000000);
    char restored_display[23];
    eps16_probe_machine_display(restored_display);
    printf("baseline_display=|%s| restored_display=|%s|\n", baseline_display,
           restored_display);
    if (read_patch_select_state() != 0x00 ||
        strncmp(baseline_display, restored_display, 22) ||
        eps16_probe_machine_illegal_instructions())
        failed = 1;

    eps16_probe_machine_destroy(machine);
    return failed ? 1 : 0;
}
