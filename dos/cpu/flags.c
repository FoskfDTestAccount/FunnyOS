/*
 * Flag computation.
 *
 * The flags are where an 8086 emulator is either right or subtly,
 * persistently wrong, and where being wrong is hardest to notice: a
 * program with a bad carry flag does not crash, it takes the other branch
 * and produces a plausible wrong answer several thousand instructions
 * later.
 *
 * So the rules live together, in one file, stated once each.
 */
#include <vm86/ops.h>

bool vm86_parity(uint8_t value)
{
    /*
     * Fold the byte down to one bit: XOR each half over the other until
     * only the lowest bit is left, and it holds the parity of the whole.
     *
     * The result is 1 for an odd number of set bits, so the flag -- which
     * is set for an EVEN count -- is the inverse. Getting that backwards
     * is a classic, and it is invisible until something tests PF, which
     * of the common instructions only the string comparisons do.
     */
    value ^= (uint8_t)(value >> 4);
    value ^= (uint8_t)(value >> 2);
    value ^= (uint8_t)(value >> 1);

    return (value & 1) == 0;
}

void vm86_flags_result(struct vm86_cpu *cpu, uint8_t width, uint16_t result)
{
    uint16_t mask = (width == 8) ? 0x00FFu : 0xFFFFu;

    result &= mask;

    vm86_flag_set(cpu, VM86_ZF, result == 0);

    /* The sign flag is the top bit of the result at this width: bit 7 for
     * a byte, bit 15 for a word. */
    vm86_flag_set(cpu, VM86_SF, (result & (uint16_t)((mask >> 1) + 1)) != 0);

    /* Parity looks at the low byte only, whatever the width. A 16-bit
     * result still produces an 8-bit parity flag. */
    vm86_flag_set(cpu, VM86_PF, vm86_parity((uint8_t)result));
}

void vm86_flags_logic(struct vm86_cpu *cpu, uint8_t width, uint16_t result)
{
    /*
     * After AND, OR, XOR and TEST the carry and overflow flags are
     * cleared and the auxiliary carry flag is undefined.
     *
     * AF is left exactly as it was rather than cleared. "Undefined" means
     * the hardware does not promise anything, so any value is legal --
     * and leaving it alone is both what later parts actually do and the
     * choice that cannot break a program that ignores the manual and
     * reads AF anyway.
     */
    vm86_flag_set(cpu, VM86_CF, false);
    vm86_flag_set(cpu, VM86_OF, false);

    vm86_flags_result(cpu, width, result);
}
