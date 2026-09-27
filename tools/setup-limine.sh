#!/bin/bash
#
# FunnyOS —— Limine 引导器准备脚本
#
# 幂等：可重复执行。所有下载物放在项目内的 .cache/ 下，
# 因为 WSL 会在空闲时关机，/tmp 是 tmpfs 会被清空。
#
set -e

LIMINE_VER="12.9.1"
PROJ="/mnt/c/FunnyOS"
CACHE="$PROJ/.cache"
BIN_DST="$PROJ/boot/limine"
DOC_DST="$PROJ/docs"

mkdir -p "$CACHE"

# ---------------------------------------------------------------- 二进制
if [ ! -d "$BIN_DST/limine-binary" ]; then
    echo "=== 下载 Limine 二进制包 ==="
    cd "$CACHE"
    [ -f limine-binary.tar.gz ] || curl -fsSL -o limine-binary.tar.gz \
        "https://github.com/Limine-Bootloader/Limine/releases/download/v${LIMINE_VER}/limine-binary.tar.gz"

    rm -rf "$CACHE/bin-extract"
    mkdir -p "$CACHE/bin-extract"
    tar -xzf limine-binary.tar.gz -C "$CACHE/bin-extract"

    mkdir -p "$BIN_DST"
    cp -r "$CACHE/bin-extract/limine-binary" "$BIN_DST/"
fi

# 宿主工具是 C 源码（limine.c），需要现场编译
if [ ! -x "$BIN_DST/limine-binary/limine" ]; then
    echo "=== 编译 limine 宿主工具 ==="
    ( cd "$BIN_DST/limine-binary" && make )
fi

# ---------------------------------------------------------------- 源码
if [ ! -f "$CACHE/limine-${LIMINE_VER}/limine-protocol/include/limine.h" ]; then
    echo "=== 下载 Limine 源码包 ==="
    cd "$CACHE"
    [ -f "limine-${LIMINE_VER}.tar.xz" ] || curl -fsSL -o "limine-${LIMINE_VER}.tar.xz" \
        "https://github.com/Limine-Bootloader/Limine/releases/download/v${LIMINE_VER}/limine-${LIMINE_VER}.tar.xz"
    tar -xf "limine-${LIMINE_VER}.tar.xz" -C "$CACHE"
fi

SRC="$CACHE/limine-${LIMINE_VER}"

echo "=== 复制协议头文件与文档 ==="
cp "$SRC/limine-protocol/include/limine.h" "$BIN_DST/limine.h"
cp "$SRC/limine-protocol/PROTOCOL.md"      "$DOC_DST/limine-protocol.md"
cp "$SRC/USAGE.md"                         "$DOC_DST/limine-usage.md"

# CONFIG.md 在源码包中的位置随版本变动，直接全树查找
CONFIG_MD=$(find "$SRC" -name 'CONFIG.md' | head -1)
if [ -n "$CONFIG_MD" ]; then
    cp "$CONFIG_MD" "$DOC_DST/limine-config.md"
    echo "  CONFIG.md <- $CONFIG_MD"
else
    echo "  !! 未找到 CONFIG.md"
fi

echo
echo "=== 结果 ==="
ls -la "$BIN_DST/limine-binary/limine" "$BIN_DST/limine.h"
"$BIN_DST/limine-binary/limine" --version
