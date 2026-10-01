#!/bin/bash
#
# Keyboard and shell test.
#
# Types on the emulated keyboard by driving the QEMU monitor's `sendkey`
# command, then asserts on what came back out of the serial port.
#
# This is the only test that covers the input path as a whole: the 8042
# raises IRQ 1, the IO APIC routes it to a vector, the handler drains and
# decodes the scancodes, the line discipline echoes and edits them, a
# Ring 3 program receives the finished line through a system call, and its
# answer comes back out through another one. Everything else in the suite
# tests one of those pieces in isolation.
#
# The assertions match whole lines rather than substrings. Typing `echo
# zebra` puts the word on the screen twice -- once as the echo of what was
# typed, once as the command's output -- and only an exact-line match
# tells the two apart. A substring match would pass on a shell that echoed
# the input and never ran anything.
#
# Usage: bash tools/run-input-test.sh [ISO path] [bios|uefi]
#
set -u

BUILD_DIR="${FUNYOS_BUILD_DIR:-/var/tmp/funyos-build}"
ISO="${1:-$BUILD_DIR/funyos.iso}"
MODE="${2:-bios}"

# Long enough for firmware, the bootloader and the kernel's own startup,
# which includes a 200 ms timer self-check.
BOOT_WAIT="${INPUT_BOOT_WAIT:-8}"
KEY_DELAY="${INPUT_KEY_DELAY:-0.15}"
TIMEOUT_SECS="${QEMU_TIMEOUT:-45}"

if [ ! -f "$ISO" ]; then
    echo "ERROR: ISO not found: $ISO" >&2
    echo "       Run make first." >&2
    exit 1
fi

LOG="$BUILD_DIR/serial-input-$MODE.log"
CLEAN="$BUILD_DIR/serial-input-$MODE.clean"
rm -f "$LOG" "$CLEAN"
mkdir -p "$BUILD_DIR"

if [ -e /dev/kvm ] && [ -w /dev/kvm ]; then
    ACCEL="-enable-kvm -cpu host"
    echo "  Acceleration : KVM (hardware)"
else
    ACCEL="-cpu max"
    echo "  Acceleration : TCG (software emulation, slow)"
fi

if [ "$MODE" = "uefi" ]; then
    cp /usr/share/OVMF/OVMF_VARS_4M.fd "$BUILD_DIR/OVMF_VARS_4M.fd"
    FIRMWARE_ARGS="-M q35 \
        -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
        -drive if=pflash,format=raw,unit=1,file=$BUILD_DIR/OVMF_VARS_4M.fd"
    BOOT_ARGS=""
else
    FIRMWARE_ARGS=""
    BOOT_ARGS="-boot d"
fi

echo "  Boot mode    : $MODE"
echo "  Serial log   : $LOG"
echo

# ---------------------------------------------------------------- input

# One character at a time. `sendkey` takes key names, not text, and the
# two agree only for letters and digits -- a period is `dot`, a space is
# `spc`. Sending `.` makes QEMU reject the command, the character never
# arrives, and it looks exactly like a shell that cannot find a file.
keyname() {
    case "$1" in
        ' ')  echo spc ;;
        '.')  echo dot ;;
        ',')  echo comma ;;
        '/')  echo slash ;;
        ';')  echo semicolon ;;
        "'")  echo apostrophe ;;
        '-')  echo minus ;;
        '=')  echo equal ;;
        '\')  echo backslash ;;
        *)    echo "$1" ;;
    esac
}

type_text() {
    local text="$1" i c
    for (( i = 0; i < ${#text}; i++ )); do
        c="${text:$i:1}"
        echo "sendkey $(keyname "$c")"
        sleep "$KEY_DELAY"
    done
}

type_line() {
    type_text "$1"
    echo "sendkey ret"
    sleep 0.6
}

# shellcheck disable=SC2086
{
    sleep "$BOOT_WAIT"

    type_line "dir"
    type_line "echo zebra"
    type_line "help"
    type_line "type readme.txt"
    type_line "xyzzy"

    echo "quit"
} | timeout "$TIMEOUT_SECS" qemu-system-x86_64 \
    -m 512 \
    -cdrom "$ISO" \
    $BOOT_ARGS \
    -serial "file:$LOG" \
    -display none \
    -monitor stdio \
    -no-reboot \
    $FIRMWARE_ARGS \
    $ACCEL \
    >/dev/null 2>&1

# Strip the CR the kernel's console emits on both channels, so lines can
# be matched exactly. The raw log is left alone for anything that needs
# what actually went out.
tr -d '\r' < "$LOG" > "$CLEAN" 2>/dev/null || : > "$CLEAN"

echo "=================== serial output (shell session) ==================="
sed -n '/FunnyCOM 0.1/,$p' "$CLEAN" 2>/dev/null | cat -s
echo "====================================================================="
echo

# ---------------------------------------------------------------- asserts
FAILED=0
CHECKS=0

# Match a whole line. See the note at the top for why this matters.
expect_line() {
    CHECKS=$((CHECKS + 1))
    if grep -qxF "$1" "$CLEAN" 2>/dev/null; then
        printf '  [ok]   %s\n' "$2"
    else
        printf '  [FAIL] %s\n' "$2"
        FAILED=1
    fi
}

expect_present() {
    CHECKS=$((CHECKS + 1))
    if grep -qF "$1" "$CLEAN" 2>/dev/null; then
        printf '  [ok]   %s\n' "$2"
    else
        printf '  [FAIL] %s\n' "$2"
        FAILED=1
    fi
}

expect_absent() {
    CHECKS=$((CHECKS + 1))
    if grep -qF "$1" "$CLEAN" 2>/dev/null; then
        printf '  [FAIL] %s\n' "$2"
        FAILED=1
    else
        printf '  [ok]   %s\n' "$2"
    fi
}

echo "Assertions (keyboard and shell, $MODE):"

# The keyboard controller came up and its translation tables are right.
# This is printed by the kernel before the shell starts.
expect_present "Keyboard       : PS/2"              "keyboard controller initialised"
expect_present "Scancode decoder: all cases passed" "scancode translation tables verified"

# A Ring 3 program started, reached its prompt, and is waiting for input.
expect_present "FunnyCOM 0.1"                       "shell started in Ring 3"
expect_present "F:\\>"                              "shell printed its prompt"

# `dir` listed the files, so the keyboard produced a line, the shell
# parsed it, and the file system answered.
expect_present "README.TXT"                         "DIR listed README.TXT"
expect_present "NOTES.TXT"                          "DIR listed NOTES.TXT"
expect_present "HELLO.C"                            "DIR listed HELLO.C"
expect_present "file(s)"                            "DIR printed its summary line"

# The whole line this time. "zebra" on its own can only come from the
# command's own output, not from the echo of `echo zebra`.
expect_line    "zebra"                              "ECHO ran and printed its argument"

# Help came from the command table rather than from anything hardcoded.
expect_present "Available commands:"                "HELP worked"
expect_present "TYPE <file>"                        "HELP listed the TYPE command"

# A file's contents, read through open/read/close. The phrase is from
# README.TXT and appears nowhere else we could be printing by accident.
expect_present "native x86-64 operating system"     "TYPE printed README.TXT"

# An unrecognised command is reported rather than ignored or fatal.
expect_present "Bad command or file name: xyzzy"    "unknown command reported"

# Nothing arrived on a vector with no handler, and nothing escalated.
expect_absent  "unhandled interrupt"                "every interrupt reached its handler"
expect_absent  "PANIC"                              "no kernel panic"
expect_absent  "program fault"                      "the shell did not fault"
expect_absent  "CPU EXCEPTION"                      "no CPU exception"

echo
if [ "$FAILED" -eq 0 ]; then
    echo "  $CHECKS assertions run"
    echo "====> input test PASSED ($MODE)"
    exit 0
else
    echo "  $CHECKS assertions run"
    echo "====> input test FAILED ($MODE)"
    exit 1
fi
