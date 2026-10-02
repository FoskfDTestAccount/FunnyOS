#!/usr/bin/env bash
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${FUNYOS_BUILD_DIR:-/var/tmp/funyos-build}
mkdir -p "$BUILD/desktop-host"
cc -std=c17 -g -O1 -Wall -Wextra -Werror -fno-builtin \
  -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie \
  -I"$ROOT/tools/tests/stubs" -I"$ROOT/kernel/include" -I"$ROOT/libk/include" -I"$ROOT/boot/limine" \
  "$ROOT/tools/tests/test_desktop.c" "$ROOT/kernel/console/fb.c" \
  "$ROOT/kernel/console/screen.c" "$ROOT/kernel/console/terminal.c" "$ROOT/kernel/console/mouse_decode.c" \
  "$ROOT/kernel/console/ps2.c" "$ROOT/kernel/console/font8x16.c" \
  -o "$BUILD/desktop-host/test-desktop"
"$BUILD/desktop-host/test-desktop"
# Unused hardware paths are discarded so this probe cannot execute rdmsr.
cc -std=c17 -g -O1 -Wall -Wextra -Werror -fno-builtin \
  -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -I"$ROOT/kernel/include" -I"$ROOT/libk/include" -I"$ROOT/boot/limine" \
  "$ROOT/tools/tests/test_ioapic.c" -o "$BUILD/desktop-host/test-ioapic"
"$BUILD/desktop-host/test-ioapic"
