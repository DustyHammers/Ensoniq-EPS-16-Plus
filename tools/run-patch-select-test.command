#!/bin/zsh
# Runs the Patch Select workflow test against your own EPS files.
#
# The test boots the real OS, creates an instrument through the original
# OS/KPC panel events, then drives PSEL (analog channel 1) with each of the
# four values the panel buttons use and reads back the state the OS decodes
# into RAM 0x01f0. It needs no build toolchain - the binary beside this
# script is prebuilt and universal.
#
# Nothing is written to your EPS files; they are only read.

emulate -L zsh
setopt null_glob

ROOT="${0:A:h}"
PROBE="$ROOT/eps16_patch_select_workflow_test"

finish() {
  print ""
  # Finder gives this script a terminal; a pipe or CI run does not.
  [[ -t 0 ]] && read -k 1 "?Press any key to close "
  exit $1
}

if [[ ! -f "$PROBE" ]]; then
  print "Cannot find eps16_patch_select_workflow_test next to this script."
  finish 1
fi

# Downloaded binaries are quarantined by Gatekeeper, and the artifact zip
# does not always preserve the executable bit.
xattr -d com.apple.quarantine "$PROBE" 2>/dev/null
chmod +x "$PROBE" 2>/dev/null

if (( $# == 3 )); then
  # Explicit paths win outright - no discovery.
  for FILE in "$1" "$2" "$3"; do
    if [[ ! -f "$FILE" ]]; then
      print "Not a file: $FILE"
      finish 1
    fi
  done
  print "Combined ROM : $1"
  print "KPC ROM      : $2"
  print "OS disk      : $3"
  print ""
  "$PROBE" "$1" "$2" "$3"
  STATUS=$?
  print ""
  (( STATUS == 0 )) && print "PASS" || print "FAIL (exit $STATUS)"
  finish $STATUS
fi

# Same search order the plugin itself uses (see
# Eps16PlusProcessor::resourceSearchDirectories()).
DIRS=(
  "$HOME/Library/Audio/Plug-Ins/VST3/EPS_files"
  "/Library/Audio/Plug-Ins/VST3/EPS_files"
  "$ROOT/EPS_files"
  "$ROOT"
)

# first_named <size> <name>...  - first existing file with one of these names
first_named() {
  local dir name
  for dir in $DIRS; do
    for name in "$@"; do
      [[ -f "$dir/$name" ]] && { print -r -- "$dir/$name"; return 0 }
    done
  done
  return 1
}

# first_sized <bytes> <glob>...  - first file matching a glob at exactly that size
first_sized() {
  local want=$1; shift
  local dir pattern file size
  for dir in $DIRS; do
    for pattern in "$@"; do
      for file in "$dir"/${~pattern}; do
        [[ -f "$file" ]] || continue
        # wc -c rather than stat, whose size flag differs between the BSD
        # stat on macOS (-f%z) and GNU stat (-c%s).
        size=$(wc -c < "$file" 2>/dev/null | tr -d ' ') || continue
        [[ "$size" == "$want" ]] && { print -r -- "$file"; return 0 }
      done
    done
  done
  return 1
}

# Combined 128K ROM. The test has no split-ROM path, so an upper/lower pair
# will not do.
ROM=$(first_named eps16plus-rom.bin) || ROM=$(first_sized 131072 '*.bin' '*.rom') || ROM=""
KPC=$(first_named eps16plus-kpc.bin 'Ensoniq EPS KPC2 v2.33 27c256.BIN') \
  || KPC=$(first_sized 32768 '*.bin' '*.rom') || KPC=""
OSDISK=$(first_named EPS130OS.img EPS130OS.hfe) || OSDISK=$(first_sized 819200 '*.img') || OSDISK=""

print "Searched:"
for dir in $DIRS; do
  [[ -d "$dir" ]] && print "  $dir" || print "  $dir  (not present)"
done
print ""
print "Combined ROM : ${ROM:-NOT FOUND}"
print "KPC ROM      : ${KPC:-NOT FOUND}"
print "OS disk      : ${OSDISK:-NOT FOUND}"
print ""

if [[ -z "$ROM" || -z "$KPC" || -z "$OSDISK" ]]; then
  print "Missing one or more files."
  print ""
  print "The test needs a COMBINED 128 KB ROM (eps16plus-rom.bin, or any"
  print ".bin/.rom of exactly 131072 bytes), a 32 KB KPC ROM, and an 800 KB"
  print "OS disk image. If your ROM is a split upper/lower pair, this test"
  print "cannot use it - it takes a combined image only."
  print ""
  print "You can also pass all three explicitly:"
  print "  zsh ${0:t} <combined-rom> <kpc-rom> <os-disk>"
  finish 1
fi

print "Running - this boots the OS and builds an instrument, so it takes"
print "a few seconds."
print ""

"$PROBE" "$ROM" "$KPC" "$OSDISK"
STATUS=$?

print ""
if (( STATUS == 0 )); then
  print "PASS - the OS decoded all four Patch Select states correctly"
  print "(0 -> 0x00, 200 -> 0x20, 480 -> 0x40, 1023 -> 0x7f) and returned"
  print "to the default patch when released."
else
  print "FAIL (exit $STATUS) - see the lines above. Each 'analog=' line"
  print "shows the value written, the state the OS stored at RAM 0x01f0,"
  print "and what was expected."
fi
finish $STATUS
