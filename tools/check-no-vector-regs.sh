#!/bin/bash
#
# Verify that the kernel touches no vector registers.
#
# The kernel is compiled with -mgeneral-regs-only, and that flag is the
# only thing standing between this design and silent corruption:
#
#   An interrupt does not save floating point state. So if kernel code
#   reaches for an XMM register, then every interrupt taken while a
#   program is doing floating point arithmetic begins destroying that
#   program's data.
#
# The failure is invisible. The program does not fault; it gets a wrong
# answer, somewhere, later, and only if a timer tick happened to land
# between two particular instructions. That is not a bug anyone finds by
# looking, so it is checked mechanically instead.
#
# This runs on the linked kernel rather than on the sources, which is the
# point: the C can be perfectly innocent and the compiler still be
# entitled to vectorise a loop. Only the output settles it.
#
# Usage: bash tools/check-no-vector-regs.sh [kernel.elf]
#
set -u

BUILD_DIR="${FUNYOS_BUILD_DIR:-/var/tmp/funyos-build}"
KERNEL="${1:-$BUILD_DIR/funyos.elf}"

if [ ! -f "$KERNEL" ]; then
    echo "  ERROR: kernel not found: $KERNEL" >&2
    exit 1
fi

DISASM=$(objdump -d "$KERNEL")
LINES=$(printf '%s\n' "$DISASM" | wc -l)
HITS=$(printf '%s\n' "$DISASM" | grep -E '%xmm|%ymm|%zmm|%mm[0-7]')

if [ -n "$HITS" ]; then
    echo "  ERROR: the kernel uses vector registers."
    echo
    printf '%s\n' "$HITS" | head -20 | sed 's/^/         /'
    echo
    echo "         Kernel code must not touch XMM, YMM or the MMX registers: an"
    echo "         interrupt does not save them, so any use here corrupts"
    echo "         whatever the interrupted program had loaded."
    echo
    echo "         The usual cause is -mgeneral-regs-only being dropped from"
    echo "         CFLAGS. See kernel/include/funnyos/arch/x86_64/fpu.h."
    exit 1
fi

echo "  Verified: kernel touches no vector registers ($LINES lines disassembled)"
exit 0
