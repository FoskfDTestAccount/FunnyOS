#!/bin/bash
#
# FunnyOS boot test.
#
# Boots QEMU headless, redirects the serial port to a file, then asserts
# against that output. This is the automated regression harness for the
# whole project -- every later milestone adds its assertions here.
#
# Usage: bash tools/run-qemu-test.sh [ISO path] [bios|uefi]
#
# Both boot paths are worth testing because they go through completely
# different firmware stacks:
#   BIOS path: Limine's BIOS stage (stage1/stage2) loaded via El Torito
#   UEFI path: Limine's EFI executable loaded via El Torito
# Modern machines are essentially UEFI-only, so the UEFI path is the one
# that ultimately matters for shipping.
#
set -u

BUILD_DIR="${FUNYOS_BUILD_DIR:-/var/tmp/funyos-build}"
ISO="${1:-$BUILD_DIR/funyos.iso}"
MODE="${2:-bios}"
TIMEOUT_SECS="${QEMU_TIMEOUT:-20}"

if [ ! -f "$ISO" ]; then
    echo "ERROR: ISO not found: $ISO" >&2
    echo "       Run make first." >&2
    exit 1
fi

if [ "$MODE" != "bios" ] && [ "$MODE" != "uefi" ]; then
    echo "ERROR: boot mode must be bios or uefi, got: $MODE" >&2
    exit 1
fi

LOG="$BUILD_DIR/serial-$MODE.log"
mkdir -p "$BUILD_DIR"
rm -f "$LOG"

# ------------------------------------------------------------- accel
if [ -e /dev/kvm ] && [ -w /dev/kvm ]; then
    ACCEL="-enable-kvm -cpu host"
    echo "  Acceleration : KVM (hardware)"
else
    ACCEL="-cpu max"
    echo "  Acceleration : TCG (software emulation, slow)"
fi

# ------------------------------------------------------------- firmware
if [ "$MODE" = "uefi" ]; then
    OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
    OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
    OVMF_VARS="$BUILD_DIR/OVMF_VARS_4M.fd"

    if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS_SRC" ]; then
        echo "ERROR: OVMF firmware not found ($OVMF_CODE)" >&2
        echo "       Install with: apt-get install ovmf" >&2
        exit 1
    fi

    # The UEFI variable store must be writable, and the copy under
    # /usr/share is not, so clone the template on every run.
    cp "$OVMF_VARS_SRC" "$OVMF_VARS"

    FIRMWARE_ARGS="-M q35 \
        -drive if=pflash,format=raw,unit=0,readonly=on,file=$OVMF_CODE \
        -drive if=pflash,format=raw,unit=1,file=$OVMF_VARS"
    BOOT_ARGS=""
    echo "  Firmware     : OVMF (UEFI)"
else
    FIRMWARE_ARGS=""
    # Under legacy BIOS the boot device must be named explicitly.
    BOOT_ARGS="-boot d"
    echo "  Firmware     : SeaBIOS (legacy BIOS)"
fi

echo "  Boot mode    : $MODE"
echo "  ISO          : $ISO"
echo "  Serial log   : $LOG"
echo "  Timeout      : ${TIMEOUT_SECS}s"
echo

# shellcheck disable=SC2086
timeout "$TIMEOUT_SECS" qemu-system-x86_64 \
    -m 512 \
    -cdrom "$ISO" \
    $BOOT_ARGS \
    -serial "file:$LOG" \
    -display none \
    -no-reboot \
    $FIRMWARE_ARGS \
    $ACCEL \
    >/dev/null 2>&1

QEMU_STATUS=$?
# 124 means timeout killed QEMU. The kernel halts and never exits on its
# own, so a timeout is the expected outcome, not an error.
if [ "$QEMU_STATUS" -eq 124 ]; then
    echo "  QEMU killed by timeout (expected: kernel halts and does not exit)"
else
    echo "  QEMU exited on its own, status $QEMU_STATUS (possible triple fault)"
fi

echo
echo "=================== serial output ==================="
if [ -s "$LOG" ]; then
    # On the UEFI path OVMF writes its own console output (full of ANSI
    # cursor positioning sequences) to the serial port as well. Strip it
    # for readability only; assertions still run against the raw log.
    sed -e 's/\x1b\[[0-9;]*[a-zA-Z]//g' "$LOG" | cat -s
else
    echo "(no serial output at all)"
fi
echo "====================================================="
echo

# ------------------------------------------------------------- assertions
FAILED=0

expect_present() {
    if grep -q "$1" "$LOG" 2>/dev/null; then
        printf '  [ok]   %s\n' "$2"
    else
        printf '  [FAIL] %s\n' "$2"
        FAILED=1
    fi
}

expect_absent() {
    if grep -q "$1" "$LOG" 2>/dev/null; then
        printf '  [FAIL] %s\n' "$2"
        FAILED=1
    else
        printf '  [ok]   %s\n' "$2"
    fi
}

if [ "$MODE" = "uefi" ]; then
    FW_EXPECT="Firmware       : UEFI 64-bit"
else
    FW_EXPECT="Firmware       : x86 BIOS (legacy)"
fi

echo "Assertions ($MODE path):"
expect_present "x86-64 kernel, M0 boot verification"  "kernel entry ran, serial output works"
expect_present "Bootloader     : Limine"               "bootloader info request parsed"
expect_present "$FW_EXPECT"                           "firmware type confirmed as $MODE"
expect_present "Base revision  : 3 (confirmed"        "boot protocol base revision accepted"
expect_present "HHDM offset"                          "HHDM request parsed"
expect_present "Usable memory"                        "memory map request parsed and summarised"
expect_present "Framebuffer    :"                     "framebuffer request parsed"
expect_present "M0 boot verification PASSED"          "completion marker present"
expect_absent  "PANIC"                                "no kernel panic"

# These two guard the VMware boot failure: when Limine cannot match the
# device it booted from with a readable volume, it warns and then fails to
# resolve a boot()-relative kernel path. The config now locates the kernel
# by filesystem label instead, so neither message should ever appear.
expect_absent  "Failed to open executable"            "bootloader resolved the kernel path"
expect_absent  "Could not meaningfully match"         "bootloader matched boot device to a volume"

echo
if [ "$FAILED" -eq 0 ]; then
    echo "====> M0 test PASSED ($MODE)"
    exit 0
else
    echo "====> M0 test FAILED ($MODE)"
    exit 1
fi
