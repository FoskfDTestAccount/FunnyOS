#!/bin/bash
#
# FunnyOS -- Limine bootloader provisioning.
#
# Idempotent: safe to re-run. All downloads land in the project's .cache/
# directory because WSL shuts itself down when idle and /tmp is a tmpfs
# that gets wiped.
#
set -e

LIMINE_VER="12.9.1"
PROJ="/mnt/c/FunnyOS"
CACHE="$PROJ/.cache"
BIN_DST="$PROJ/boot/limine"
DOC_DST="$PROJ/docs"

mkdir -p "$CACHE"

# ------------------------------------------------------------- binaries
if [ ! -d "$BIN_DST/limine-binary" ]; then
    echo "=== Downloading Limine binary release ==="
    cd "$CACHE"
    [ -f limine-binary.tar.gz ] || curl -fsSL -o limine-binary.tar.gz \
        "https://github.com/Limine-Bootloader/Limine/releases/download/v${LIMINE_VER}/limine-binary.tar.gz"

    rm -rf "$CACHE/bin-extract"
    mkdir -p "$CACHE/bin-extract"
    tar -xzf limine-binary.tar.gz -C "$CACHE/bin-extract"

    mkdir -p "$BIN_DST"
    cp -r "$CACHE/bin-extract/limine-binary" "$BIN_DST/"
fi

# The host tool ships as C source (limine.c) and has to be compiled.
if [ ! -x "$BIN_DST/limine-binary/limine" ]; then
    echo "=== Building the limine host tool ==="
    ( cd "$BIN_DST/limine-binary" && make )
fi

# ------------------------------------------------------------- sources
if [ ! -f "$CACHE/limine-${LIMINE_VER}/limine-protocol/include/limine.h" ]; then
    echo "=== Downloading Limine source release ==="
    cd "$CACHE"
    [ -f "limine-${LIMINE_VER}.tar.xz" ] || curl -fsSL -o "limine-${LIMINE_VER}.tar.xz" \
        "https://github.com/Limine-Bootloader/Limine/releases/download/v${LIMINE_VER}/limine-${LIMINE_VER}.tar.xz"
    tar -xf "limine-${LIMINE_VER}.tar.xz" -C "$CACHE"
fi

SRC="$CACHE/limine-${LIMINE_VER}"

echo "=== Copying protocol header and documentation ==="
cp "$SRC/limine-protocol/include/limine.h" "$BIN_DST/limine.h"
cp "$SRC/limine-protocol/PROTOCOL.md"      "$DOC_DST/limine-protocol.md"
cp "$SRC/USAGE.md"                         "$DOC_DST/limine-usage.md"

# The location of CONFIG.md varies between releases; just search for it.
CONFIG_MD=$(find "$SRC" -name 'CONFIG.md' | head -1)
if [ -n "$CONFIG_MD" ]; then
    cp "$CONFIG_MD" "$DOC_DST/limine-config.md"
    echo "  CONFIG.md <- $CONFIG_MD"
else
    echo "  !! CONFIG.md not found"
fi

echo
echo "=== Result ==="
ls -la "$BIN_DST/limine-binary/limine" "$BIN_DST/limine.h"
"$BIN_DST/limine-binary/limine" --version
