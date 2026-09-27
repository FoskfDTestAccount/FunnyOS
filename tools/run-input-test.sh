#!/bin/bash
#
# Keyboard end-to-end test.
#
# Types on the emulated keyboard by driving the QEMU monitor's `sendkey`
# command, then asserts on what came back out of the serial port. This is
# the only test that exercises the input path as a whole: the 8042 raises
# IRQ 1, the IO APIC routes it to a vector, the handler drains and decodes
# the scancodes, and the line discipline edits them.
#
# The decoder's own unit test (kbd_selftest, run on every boot) proves the
# translation tables; it cannot prove that an interrupt ever arrives. This
# can, and that is the part most likely to be silently broken.
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
TIMEOUT_SECS="${QEMU_TIMEOUT:-30}"

if [ ! -f "$ISO" ]; then
    echo "ERROR: ISO not found: $ISO" >&2
    echo "       Run make first." >&2
    exit 1
fi

LOG="$BUILD_DIR/serial-input-$MODE.log"
rm -f "$LOG"
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
#
# Two lines are typed:
#
#   hello<enter>                  plain characters
#   H, i, backspace, i<enter>     shift, then an editing correction
#
# The second is the one that matters. It checks that shift reaches the
# decoder and that backspace removes the character it should, which a
# plain-typing test cannot distinguish from a driver that simply appends
# whatever it is given.
type_keys() {
    for key in "$@"; do
        echo "sendkey $key"
        sleep "$KEY_DELAY"
    done
}

# shellcheck disable=SC2086
{
    sleep "$BOOT_WAIT"

    type_keys h e l l o
    echo "sendkey ret"
    sleep 0.5

    type_keys shift-h i backspace i
    echo "sendkey ret"
    sleep 0.5

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

echo "=================== serial output (input section) ==================="
if [ -s "$LOG" ]; then
    sed -n '/\[console loop\]/,$p' "$LOG" | cat -v
else
    echo "(no serial output at all)"
fi
echo "====================================================================="
echo

# ---------------------------------------------------------------- asserts
FAILED=0

expect_present() {
    if grep -qF "$1" "$LOG" 2>/dev/null; then
        printf '  [ok]   %s\n' "$2"
    else
        printf '  [FAIL] %s\n' "$2"
        FAILED=1
    fi
}

expect_absent() {
    if grep -qF "$1" "$LOG" 2>/dev/null; then
        printf '  [FAIL] %s\n' "$2"
        FAILED=1
    else
        printf '  [ok]   %s\n' "$2"
    fi
}

echo "Assertions (keyboard path, $MODE):"

# The kernel reaches its input loop at all, which means the keyboard
# controller initialised and claimed its vector.
expect_present "Keyboard       : PS/2"          "keyboard controller initialised"
expect_present "Scancode decoder: all cases passed" "scancode translation tables verified"
expect_present "[console loop]"                 "kernel reached the input loop"

# Typed characters arrive, are decoded and are echoed.
expect_present 'echo (5 bytes): "hello"'        "plain characters typed and echoed correctly"

# Shift produces an uppercase letter, and backspace erases the character
# it should. The expected result is "Hi" and not "hi": the shift applies
# to the first letter only, and the backspace removes the first "i", so
# exactly one lowercase "i" survives. A driver that ignored shift would
# produce "hi", and one that ignored backspace would produce "Hii".
expect_present 'echo (2 bytes): "Hi"'           "shift and backspace both behaved"

# Nothing arrived unclaimed. A keyboard routed to the wrong vector shows
# up here rather than as a wrong character.
expect_absent "unhandled interrupt"             "keyboard interrupt reached its handler"
expect_absent "PANIC"                           "no kernel panic"

echo
if [ "$FAILED" -eq 0 ]; then
    echo "====> input test PASSED ($MODE)"
    exit 0
else
    echo "====> input test FAILED ($MODE)"
    exit 1
fi
