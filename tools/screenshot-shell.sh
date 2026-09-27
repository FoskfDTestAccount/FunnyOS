#!/bin/bash
#
# Capture the shell mid-session.
#
# Boots QEMU headless, types a few commands through the monitor, then
# takes a screendump. The serial-only tests prove the shell works; this
# proves what it looks like, which is a different question and the one a
# README has to answer.
#
# Usage: bash tools/screenshot-shell.sh [ISO] [bios|uefi] [out.png]
#
set -u

BUILD_DIR="${FUNYOS_BUILD_DIR:-/var/tmp/funyos-build}"
ISO="${1:-$BUILD_DIR/funyos.iso}"
MODE="${2:-uefi}"
OUT="${3:-$BUILD_DIR/screenshot-shell.png}"

PPM="$BUILD_DIR/screenshot-shell.ppm"
BOOT_WAIT="${SHELL_SHOT_WAIT:-8}"
KEY_DELAY="${SHELL_SHOT_KEY_DELAY:-0.12}"

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

# `sendkey` takes key names, not characters, and the two agree only for
# letters and digits. A period is `dot`; sending `.` makes QEMU reject the
# command and the character silently never arrives, which looks exactly
# like a shell that cannot find a file.
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

echo "  Booting $MODE, typing a session, then capturing..."

# shellcheck disable=SC2086
{
    sleep "$BOOT_WAIT"

    type_text "cls"; echo "sendkey ret"; sleep 0.6
    type_text "dir"; echo "sendkey ret"; sleep 1.0
    type_text "type notes.txt"; echo "sendkey ret"; sleep 1.0

    echo "screendump $PPM"
    sleep 2
    echo "quit"
} | timeout $((BOOT_WAIT + 40)) qemu-system-x86_64 \
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
