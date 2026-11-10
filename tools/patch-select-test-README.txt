Patch Select test tool
======================

What this proves
----------------
The two Patch Select caps on the panel are not panel-matrix buttons. They
are one analog line (PSEL, ADC channel 1), and the original OS does all the
decoding: it reads the ADC, subtracts a zero reference, thresholds the
result, and stores one of four states at RAM 0x01f0.

The emulator's only job is to put the right voltage on that line. This tool
checks that it does, by booting the real OS, creating an instrument through
the original OS/KPC panel events, driving the line with each of the four
values the panel buttons use, and reading back what the OS decoded:

    value written      state the OS stores at 0x01f0
    0                  0x00   (neither cap held)
    200                0x20   (one cap)
    480                0x40   (the other cap)
    1023               0x7f   (both)

It then writes 0 again and checks the instrument returns to its default
patch, comparing the OS's own display against what it showed before any
patch was selected.

That means you do not need a patch-select instrument, or the hardware, to
know the feature works end to end.

How to run it
-------------
Double-click run-patch-select-test.command.

The first time, macOS may refuse to open it because it was downloaded. If
so, right-click it and choose Open, then confirm.

It finds your files automatically, looking in the same places the plugin
does:

    ~/Library/Audio/Plug-Ins/VST3/EPS_files
    /Library/Audio/Plug-Ins/VST3/EPS_files
    an EPS_files folder beside this script
    this folder

and in each one looks for:

    combined ROM   eps16plus-rom.bin, or any .bin/.rom of exactly 131072 bytes
    KPC ROM        eps16plus-kpc.bin or "Ensoniq EPS KPC2 v2.33 27c256.BIN",
                   or any .bin/.rom of exactly 32768 bytes
    OS disk        EPS130OS.img or EPS130OS.hfe, or any .img of 819200 bytes

You can also pass all three paths yourself, in a Terminal:

    zsh run-patch-select-test.command <combined-rom> <kpc-rom> <os-disk>

Your files are only read, never written.

If it cannot find a ROM
-----------------------
This tool takes a COMBINED 128 KB ROM image only. If yours is a split
upper/lower pair, point it at a combined image instead, or pass one on the
command line.

Reading the output
------------------
Each line starting "analog=" shows the value written, the state the OS
actually stored, and the state expected. A mismatch on any of the four, or
a display that does not return to its baseline, exits non-zero and prints
FAIL.

What it does not tell you
-------------------------
Which physical cap is 0x20 and which is 0x40. That is not recorded in the
schematics and the ROM does not label the two buttons, so the left/right
assignment is a convention. If a patch-select instrument ever comes out
with its two single-cap variations swapped against a reference, that is
this, and it is a two-constant change.
