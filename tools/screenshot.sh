#!/bin/bash
#
# Capture what the kernel actually draws on screen.
#
# Boots QEMU headless, waits for the kernel to reach its halt state, then
# asks the QEMU monitor for a screendump and converts it to PNG.
#
# This is the only way to verify the framebuffer console end to end: the
# serial log proves the kernel ran, but says nothing about whether the
# glyphs render or whether the pixel format was decoded correctly.
#
# Usage: bash tools/screenshot.sh [ISO] [bios|uefi] [out.png]
#
set -u

BUILD_DIR="${FUNYOS_BUILD_DIR:-/var/tmp/funyos-build}"
ISO="${1:-$BUILD_DIR/funyos.iso}"
MODE="${2:-uefi}"
OUT="${3:-$BUILD_DIR/screenshot.png}"

PPM="$BUILD_DIR/screenshot.ppm"
WAIT="${SHOT_WAIT:-10}"

if [ ! -f "$ISO" ]; then
    echo "ERROR: ISO not found: $ISO" >&2
    exit 1
fi

rm -f "$PPM" "$OUT"

if [ -e /dev/kvm ] && [ -w /dev/kvm ]; then
    ACCEL="-enable-kvm -cpu host"
else
    ACCEL="-cpu max"
fi

if [ "$MODE" = "uefi" ]; then
    cp /usr/share/OVMF/OVMF_VARS_4M.fd "$BUILD_DIR/OVMF_VARS_4M.fd"
    FIRMWARE="-M q35 \
        -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
        -drive if=pflash,format=raw,unit=1,file=$BUILD_DIR/OVMF_VARS_4M.fd"
    BOOT_ARGS=""
else
    FIRMWARE=""
    BOOT_ARGS="-boot d"
fi

echo "  Booting $MODE, waiting ${WAIT}s before capture..."

# Drive the QEMU monitor on stdin with a timed command sequence: let the
# guest boot, grab the screen, then quit.
# shellcheck disable=SC2086
{ sleep "$WAIT"; echo "screendump $PPM"; sleep 2; echo "quit"; } | \
timeout $((WAIT + 20)) qemu-system-x86_64 \
    -m 512 \
    -cdrom "$ISO" \
    $BOOT_ARGS \
    -serial null \
    -display none \
    -vga std \
    -monitor stdio \
    -no-reboot \
    $FIRMWARE \
    $ACCEL \
    >/dev/null 2>&1

if [ ! -s "$PPM" ]; then
    echo "ERROR: screendump produced nothing ($PPM)" >&2
    exit 1
fi

python3 - "$PPM" "$OUT" <<'PY'
import sys
from PIL import Image

src, dst = sys.argv[1], sys.argv[2]
img = Image.open(src)
img.save(dst)
print(f"  Captured {img.width}x{img.height} -> {dst}")
PY
