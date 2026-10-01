#!/usr/bin/env python3
"""Mutation campaign for the 8086 subsystem.

Applies one deliberate defect at a time to a *copy* of the tree, rebuilds
and runs every suite, and records which cases go red. Whatever no mutation
can redden is reported, because a case that cannot fail is worse than no
case at all: it occupies the place where a real one would go and gives
whoever reads the suite a reason to believe the behaviour is checked.

    python3 tools/verify-mutations.py                  every layer
    python3 tools/verify-mutations.py --layer intr     one layer
    python3 tools/verify-mutations.py --only rep       by substring
    python3 tools/verify-mutations.py --json out.json  machine-readable

Pointing it at somebody else's tree, or at somebody else's mutations,
needs no second copy of this file:

    python3 tools/verify-mutations.py --tree /path/to/checkout
    python3 tools/verify-mutations.py --mutations mine.json

`--tree` is copied before anything is written to it, and `--mutations` is
a JSON list of {"name", "file", "old", "new"} -- the shape of the table
below, written out. A mutation whose anchor does not appear exactly once
is reported, not skipped, so a table that has gone stale says so.

------------------------------------------------------------------------
What counts as evidence

Three things can happen to a mutation, and they mean different things. The
report keeps them apart because collapsing them is how a campaign flatters
itself:

  caught    the mutant compiled, changed the machine, and at least one
            case went red. This is the only kind that is evidence that a
            case is worth having.
  survived  the mutant compiled and changed the machine, but every case
            stayed green. That is the finding: an assertion nobody wrote.
  vacuous   the mutant applied but the *compiled code* came out identical
            to the pristine build, so nothing was tested at all. That is a
            defect in the mutant, not a gap in the suite, and a campaign
            that ignored it would report its own mistakes as coverage.

The probe is the third one: every rebuilt suite binary is hashed and
compared against the pristine build. Identical output means the compiler
emitted the same instructions from the mutated source, and no execution of
those binaries could ever disagree. It is deliberately one-directional --
"the binary differs" does not prove the behaviour differs, only that the
emitted code does, so a mutant that survives the probe is reported as a
*survival* rather than credited as coverage. The conservative direction is
the one that does not invent evidence.

------------------------------------------------------------------------
The two ways a campaign lies

Both are handled by construction rather than by care:

A mutation that fails to compile is not a green suite. It would be,
   if the runner treated "the build failed" as "no cases failed" -- every
   case would survive and the report would say a great many cases are
   dead. Compile failures are counted separately and never as survivals.

A mutation that is not picked up by the build is not a green suite
   either, and this one is nastier because it looks exactly like a
   survival. make compares timestamps; a source written in the same
   granularity as the binary built from it can look older, and the suite
   that runs is the pristine one. So the mutated file is given an mtime
   strictly newer than every build artefact, and a mutant whose build
   produced no compile at all is reported as a runner error rather than
   as a result.

------------------------------------------------------------------------
Why the tree is copied

Builds happen in /var/tmp, not in the repository: writing object files
onto a 9p mount is slow, and a run interrupted halfway would otherwise
leave the working tree holding a mutant. Copying also means the campaign
cannot corrupt the checkout it was pointed at, however badly it is
interrupted, and it needs dos/, libk/ and tools/ -- the last two for the
corpus suite, which assembles nasm samples with tools/bin2c.py.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

WORK_ROOT = os.environ.get('MUTATION_WORK', '/var/tmp/funyos-mutations/m4')
REPO      = os.path.join(WORK_ROOT, 'repo')
DOS       = os.path.join(REPO, 'dos')
BUILD     = os.path.join(WORK_ROOT, 'build')
PRISTINE  = os.path.join(WORK_ROOT, 'pristine')

# ---------------------------------------------------------------------
# The campaign that was written for test_verify.c, kept as it was.
#
# Its anchors were verified against the tree of the time and most
# still apply; the ones that no longer do are reported by name
# rather than silently skipped. Keeping them is worth it: they are
# the set that was shown to make M3's cases fail, so re-running
# them here says for free whether M4 changed any of that.
# ---------------------------------------------------------------------

M3_MUTATIONS = [
    # ---- mem.c -----------------------------------------------------
    ('a20 gate ignored', 'mem/mem.c',
     '    if (!mem->a20)\n        linear &= VM86_A20_MASK;\n', ''),
    ('unmapped reads return zero', 'mem/mem.c',
     '        mem->unmapped_reads++;\n        return 0xFF;',
     '        mem->unmapped_reads++;\n        return 0x00;'),
    ('read16 byte order swapped', 'mem/mem.c',
     '    return (uint16_t)(low | (high << 8));',
     '    return (uint16_t)(high | (low << 8));'),
    ('write16 byte order swapped', 'mem/mem.c',
     '    vm86_mem_write8(mem, linear, (uint8_t)(value & 0xFF));\n'
     '    vm86_mem_write8(mem, linear + 1, (uint8_t)(value >> 8));',
     '    vm86_mem_write8(mem, linear, (uint8_t)(value >> 8));\n'
     '    vm86_mem_write8(mem, linear + 1, (uint8_t)(value & 0xFF));'),

    # ---- decode.c --------------------------------------------------
    ('a fetch at offset 0 reads the next segment', 'cpu/decode.c',
     '    uint32_t addr = cpu->seg_base[VM86_CS] + cpu->ip;',
     '    uint32_t addr = cpu->seg_base[VM86_CS] + cpu->ip\n'
     '                    + (cpu->ip == 0 ? 0x10000u : 0u);'),
    ('disp8 not sign extended', 'cpu/decode.c',
     'offset = (uint16_t)(offset + vm86_sign_extend8(vm86_fetch8(cpu)));',
     'offset = (uint16_t)(offset + vm86_fetch8(cpu));'),
    ('the segment prefix is ignored', 'cpu/decode.c',
     '    if (cpu->prefix.segment != VM86_NO_SEGMENT)\n'
     '        return (enum vm86_seg)cpu->prefix.segment;\n\n'
     '    return natural;',
     '    (void)cpu;\n    return natural;'),
    ('bp uses ds instead of ss', 'cpu/decode.c',
     'enum vm86_seg seg = vm86_effective_seg(cpu, uses_bp ? VM86_SS : VM86_DS);',
     '(void)uses_bp;\n'
     '    enum vm86_seg seg = vm86_effective_seg(cpu, VM86_DS);'),

    # ---- step.c ----------------------------------------------------
    ('prefixes never cleared', 'cpu/step.c',
     '    cpu->prefix.segment = VM86_NO_SEGMENT;\n'
     '    cpu->prefix.repeat  = 0;\n'
     '    cpu->prefix.lock    = false;',
     '    cpu->prefix.lock    = false;'),
    ('the lock prefix is refused', 'cpu/step.c',
     '    case 0xF0: cpu->prefix.lock = true; return true;',
     '    case 0xF0: return false;'),
    ('unclaimed opcodes continue', 'cpu/step.c',
     '        cpu->fault = VM86_VECTOR_INVALID_OPCODE;\n'
     '        return VM86_FAULT;',
     '        return VM86_CONTINUE;'),

    # ---- flags.c ---------------------------------------------------
    ('parity inverted', 'cpu/flags.c',
     'return (value & 1) == 0;', 'return (value & 1) != 0;'),
    ('the sign flag is taken from the wrong bit', 'cpu/flags.c',
     'vm86_flag_set(cpu, VM86_SF, (result & (uint16_t)((mask >> 1) + 1)) != 0);',
     'vm86_flag_set(cpu, VM86_SF, (result & (uint16_t)((mask >> 2) + 1)) != 0);'),
    ('a logic operation does not clear the carry', 'cpu/flags.c',
     '    vm86_flag_set(cpu, VM86_CF, false);\n'
     '    vm86_flag_set(cpu, VM86_OF, false);\n\n'
     '    vm86_flags_result(cpu, width, result);',
     '    vm86_flag_set(cpu, VM86_OF, false);\n\n'
     '    vm86_flags_result(cpu, width, result);'),

    # ---- ops_alu.c: the shared arithmetic --------------------------
    ('add never carries out', 'cpu/ops_alu.c',
     '        wide = lhs + rhs;\n        cf   = wide > mask;',
     '        wide = lhs + rhs;\n        cf   = false;'),
    ('add overflow uses the wrong bit', 'cpu/ops_alu.c',
     'of = ((result ^ dst) & (result ^ src) & sign) != 0;',
     'of = ((result ^ dst) & (result ^ src) & (uint16_t)(sign >> 1)) != 0;'),
    ('auxiliary carry from bit 5', 'cpu/ops_alu.c',
     'bool af = ((dst ^ src ^ result) & 0x0010u) != 0;',
     'bool af = ((dst ^ src ^ result) & 0x0020u) != 0;'),
    ('sub carries on equality', 'cpu/ops_alu.c',
     '        cf   = lhs < rhs;\n        wide = lhs - rhs;',
     '        cf   = lhs <= rhs;\n        wide = lhs - rhs;'),
    ('adc ignores the incoming carry', 'cpu/ops_alu.c',
     '        wide = lhs + rhs + carry;',
     '        wide = lhs + rhs;'),
    ('sbb ignores the incoming borrow', 'cpu/ops_alu.c',
     '        cf   = lhs < rhs + carry;', '        cf   = lhs < rhs;'),
    ('inc and dec clobber the carry', 'cpu/ops_alu.c',
     '    bool af = ((value ^ result) & 0x0010u) != 0;\n\n'
     '    vm86_flag_set(cpu, VM86_OF, of);\n'
     '    vm86_flag_set(cpu, VM86_AF, af);\n\n'
     '    vm86_flags_result(cpu, width, result);\n\n'
     '    return result;\n}',
     '    bool af = ((value ^ result) & 0x0010u) != 0;\n\n'
     '    vm86_flag_set(cpu, VM86_CF, false);\n'
     '    vm86_flag_set(cpu, VM86_OF, of);\n'
     '    vm86_flag_set(cpu, VM86_AF, af);\n\n'
     '    vm86_flags_result(cpu, width, result);\n\n'
     '    return result;\n}'),
    ('dec overflows on the wrong value', 'cpu/ops_alu.c',
     'bool of = increment ? (result == sign) : (value == sign);',
     'bool of = increment ? (result == sign) : (result == sign);'),
    ('cmp writes its result', 'cpu/ops_alu.c',
     '    if (operation != VM86_ALU_CMP)\n'
     '        vm86_operand_write(cpu, &dst_operand, result);',
     '    vm86_operand_write(cpu, &dst_operand, result);'),

    # ---- ops_alu.c: the shifts -------------------------------------
    ('a shift by zero still runs', 'cpu/ops_alu.c',
     '    if (count == 0)\n        return VM86_CONTINUE;', ''),
    ('the shift count is masked to sixteen', 'cpu/ops_alu.c',
     '    for (uint8_t i = 0; i < count; i++) {',
     '    for (uint8_t i = 0; i < (uint8_t)(count & 15); i++) {'),
    ('sar reports overflow', 'cpu/ops_alu.c',
     '        default:   /* SAR never overflows: it is a divide by two. */\n'
     '            of = false;',
     '        default:   /* SAR never overflows: it is a divide by two. */\n'
     '            of = (value & msb) != 0;'),
    ('ror takes of from one bit', 'cpu/ops_alu.c',
     '            of = top != second;', '            of = top == second;'),
    ('shr does not keep the original sign', 'cpu/ops_alu.c',
     '            of = (original & msb) != 0;',
     '            of = (value & msb) != 0;\n            (void)original;'),
    ('rcl does not take the carry in', 'cpu/ops_alu.c',
     '            bool out = (value & msb) != 0;\n\n'
     '            value = (uint16_t)(((value << 1) | (carry ? 1u : 0u)) & mask);\n'
     '            carry = out;',
     '            bool out = (value & msb) != 0;\n\n'
     '            value = (uint16_t)((value << 1) & mask);\n'
     '            carry = out;'),

    # ---- ops_alu.c: groups 1 and 3, and the decimal adjustments -----
    ('82 is not claimed', 'cpu/ops_alu.c',
     '[0x82] = op_group1', '[0xF1] = op_group1'),
    ('the group 1 byte immediate is not sign extended', 'cpu/ops_alu.c',
     '        src = vm86_sign_extend8(vm86_fetch8(cpu));',
     '        src = vm86_fetch8(cpu);'),
    ('test writes its result', 'cpu/ops_alu.c',
     '    (void)vm86_alu(cpu, VM86_ALU_AND, width, operand, reg);',
     '    vm86_operand_write(cpu, &mr.operand,\n'
     '                       vm86_alu(cpu, VM86_ALU_AND, width, operand, reg));'),
    ('not clears the carry', 'cpu/ops_alu.c',
     '        vm86_operand_write(cpu, &mr.operand, (uint16_t)(~value & mask));',
     '        vm86_flag_set(cpu, VM86_CF, false);\n'
     '        vm86_operand_write(cpu, &mr.operand, (uint16_t)(~value & mask));'),
    ('neg subtracts from one', 'cpu/ops_alu.c',
     '    vm86_alu(cpu, VM86_ALU_SUB, width, 0, value));',
     '    vm86_alu(cpu, VM86_ALU_SUB, width, 1, value));'),
    ('mul never reports a wide product', 'cpu/ops_alu.c',
     '            vm86_flag_set(cpu, VM86_CF, cpu->ah != 0);',
     '            vm86_flag_set(cpu, VM86_CF, false);'),
    ('imul never reports a sign it lost', 'cpu/ops_alu.c',
     '            bool    fits    = product == (int16_t)(int8_t)product;',
     '            bool    fits    = true;'),
    ('division misses a wide byte quotient', 'cpu/ops_alu.c',
     '            if (value == 0 || ax / value > 0xFFu) {',
     '            if (value == 0) {'),
    ('division misses a wide word quotient', 'cpu/ops_alu.c',
     '            if (value == 0 || dividend / value > 0xFFFFu) {',
     '            if (value == 0) {'),
    ('the word divide puts quotient and remainder the wrong way round',
     'cpu/ops_alu.c',
     '            cpu->ax = (uint16_t)(dividend / value);\n'
     '            cpu->dx = (uint16_t)(dividend % value);',
     '            cpu->dx = (uint16_t)(dividend / value);\n'
     '            cpu->ax = (uint16_t)(dividend % value);'),
    ('idiv never overflows', 'cpu/ops_alu.c',
     '            if (quotient < lowest || quotient > highest) {',
     '            (void)lowest; (void)highest;\n'
     '            if (false) {'),
    ('the idiv remainder is off by one', 'cpu/ops_alu.c',
     '            int64_t remainder = dividend % divisor;',
     '            int64_t remainder = dividend % divisor + 1;'),
    ('cbw fills with zeroes', 'cpu/ops_alu.c',
     '    cpu->ah = (cpu->al & 0x80u) ? 0xFFu : 0x00u;',
     '    cpu->ah = 0x00u;'),
    ('aam puts the quotient in al', 'cpu/ops_alu.c',
     '    cpu->ah = (uint8_t)(al / base);\n    cpu->al = (uint8_t)(al % base);',
     '    cpu->ah = (uint8_t)(al % base);\n    cpu->al = (uint8_t)(al / base);'),
    ('aad adds where it should multiply', 'cpu/ops_alu.c',
     '    uint8_t al   = (uint8_t)(cpu->al + (uint8_t)(cpu->ah * base));',
     '    uint8_t al   = (uint8_t)(cpu->al + (uint8_t)(cpu->ah + base));'),
    ('the decimal high-digit test never fires', 'cpu/ops_alu.c',
     '    if (al > 0x9Fu || cf) {', '    if (cf) {'),
    ('aaa does not truncate al', 'cpu/ops_alu.c',
     '    cpu->al = (uint8_t)(al & 0x0Fu);', '    cpu->al = al;'),
    ('fe refuses the byte register form', 'cpu/ops_alu.c',
     '    if (mr.reg > 1) {', '    if (mr.mod == 3 || mr.reg > 1) {'),
    ('pop cs is executed', 'cpu/ops_alu.c',
     '        if (form == 7 && segment == VM86_CS) {', '        if (false) {'),
    # Retargeted: the segment push and pop branches gained braces in M4.
    ('push es becomes push ss', 'cpu/ops_alu.c',
     '        if (form == 6) {\n'
     '            vm86_push16(cpu, vm86_get_seg(cpu, (enum vm86_seg)segment));',
     '        if (form == 6) {\n'
     '            vm86_push16(cpu, vm86_get_seg(cpu, VM86_SS));'),

    # ---- ops_mov.c -------------------------------------------------
    ('push sp reads before the decrement', 'cpu/ops_mov.c',
     'vm86_push16(cpu, (uint16_t)(cpu->sp - 2));',
     'vm86_push16(cpu, vm86_reg_get16(cpu, index));'),
    ('pop of any register ends two too high', 'cpu/ops_mov.c',
     '        vm86_reg_set16(cpu, index, vm86_pop16(cpu));',
     '        vm86_reg_set16(cpu, index, (uint16_t)(vm86_pop16(cpu) + 2));'),
    ('push of a register pushes its number', 'cpu/ops_mov.c',
     '        vm86_push16(cpu, vm86_reg_get16(cpu, index));\n'
     '    }\n\n    return VM86_CONTINUE;',
     '        vm86_push16(cpu, index);\n'
     '    }\n\n    return VM86_CONTINUE;'),
    ('mov to a segment swaps direction', 'cpu/ops_mov.c',
     '    if (opcode == 0x8C) {', '    if (opcode == 0x8E) {'),
    ('a segment register write misses the cache', 'cpu/ops_mov.c',
     '    vm86_set_seg(cpu, (enum vm86_seg)mr.reg, value);',
     '    cpu->ds = value;'),
    ('lea folds the base in', 'cpu/ops_mov.c',
     '    vm86_reg_set16(cpu, mr.reg, (uint16_t)(mr.operand.addr - base));',
     '    vm86_reg_set16(cpu, mr.reg,\n'
     '                   (uint16_t)(mr.operand.addr - base + base));'),
    ('lea takes the subtraction base from ds', 'cpu/ops_mov.c',
     '    uint32_t base = vm86_linear(cpu, mr.operand.seg, 0);',
     '    uint32_t base = vm86_linear(cpu, VM86_DS, 0);'),
    ('lea reads memory', 'cpu/ops_mov.c',
     '    uint32_t base = vm86_linear(cpu, mr.operand.seg, 0);',
     '    uint32_t base = vm86_linear(cpu, mr.operand.seg, 0);\n'
     '    (void)vm86_mem_read16(cpu->mem, mr.operand.addr);'),
    ('lea accepts the register form', 'cpu/ops_mov.c',
     '    if (mr.operand.kind != VM86_OPERAND_MEMORY)\n'
     '        return invalid_instruction(cpu);\n\n'
     '    uint32_t base = vm86_linear(cpu, mr.operand.seg, 0);',
     '    uint32_t base = vm86_linear(cpu, (enum vm86_seg)VM86_DS, 0);'),
    ('the direct address is taken as linear', 'cpu/ops_mov.c',
     '    mem.addr  = vm86_linear(cpu, seg, offset);',
     '    mem.addr  = offset;'),
    ('the direct address ignores the override', 'cpu/ops_mov.c',
     '    enum vm86_seg seg   = vm86_effective_seg(cpu, VM86_DS);',
     '    enum vm86_seg seg   = VM86_DS;'),
    ('xlat uses bp', 'cpu/ops_mov.c',
     '    uint16_t offset = (uint16_t)(cpu->bx + cpu->al);',
     '    uint16_t offset = (uint16_t)(cpu->bp + cpu->al);'),
    ('xlat reads es', 'cpu/ops_mov.c',
     '    enum vm86_seg seg = vm86_effective_seg(cpu, VM86_DS);\n\n'
     '    uint16_t offset = (uint16_t)(cpu->bx + cpu->al);',
     '    enum vm86_seg seg = VM86_ES;\n\n'
     '    uint16_t offset = (uint16_t)(cpu->bx + cpu->al);'),
    ('xlat touches the flags', 'cpu/ops_mov.c',
     '    cpu->al = vm86_mem_read8(cpu->mem, vm86_linear(cpu, seg, offset));',
     '    vm86_flag_set(cpu, VM86_CF, false);\n'
     '    cpu->al = vm86_mem_read8(cpu->mem, vm86_linear(cpu, seg, offset));'),
    ('pop refuses the register form', 'cpu/ops_mov.c',
     '    /* POP r/m16 is the whole of group 8F, and it is /0. */\n'
     '    if (mr.reg != 0)',
     '    /* POP r/m16 is the whole of group 8F, and it is /0. */\n'
     '    if (mr.reg != 0 || mr.mod == 3)'),
    ('8f /1 is executed as /0', 'cpu/ops_mov.c',
     '    /* POP r/m16 is the whole of group 8F, and it is /0. */\n'
     '    if (mr.reg != 0)\n'
     '        return invalid_instruction(cpu);',
     '    if (false)\n'
     '        return invalid_instruction(cpu);'),
    ('the immediate is read before the modrm', 'cpu/ops_mov.c',
     '    struct vm86_modrm mr;\n'
     '    vm86_fetch_modrm(cpu, &mr, width);\n\n'
     '    if (mr.reg != 0)\n'
     '        return invalid_instruction(cpu);\n\n'
     '    uint16_t value = (width == 8) ? vm86_fetch8(cpu) : vm86_fetch16(cpu);',
     '    uint16_t value = (width == 8) ? vm86_fetch8(cpu) : vm86_fetch16(cpu);\n\n'
     '    struct vm86_modrm mr;\n'
     '    vm86_fetch_modrm(cpu, &mr, width);\n\n'
     '    if (mr.reg != 0)\n'
     '        return invalid_instruction(cpu);'),
    ('c6 and c7 accept any reg', 'cpu/ops_mov.c',
     '    if (mr.reg != 0)\n        return invalid_instruction(cpu);\n\n'
     '    uint16_t value = (width == 8) ? vm86_fetch8(cpu) : vm86_fetch16(cpu);',
     '    if (false)\n        return invalid_instruction(cpu);\n\n'
     '    uint16_t value = (width == 8) ? vm86_fetch8(cpu) : vm86_fetch16(cpu);'),
    ('mov to cs is allowed', 'cpu/ops_mov.c',
     '    if (mr.reg > VM86_DS || (opcode == 0x8E && mr.reg == VM86_CS))',
     '    if (mr.reg > VM86_DS)'),
    ('segment registers 4-7 are accepted', 'cpu/ops_mov.c',
     '    if (mr.reg > VM86_DS || (opcode == 0x8E && mr.reg == VM86_CS))',
     '    (void)opcode;\n    if (false)'),

    # ---- ops_str.c -------------------------------------------------
    ('the direction flag is ignored', 'cpu/ops_str.c',
     'return (int16_t)(vm86_flag_test(cpu, VM86_DF) ? -step : step);',
     'return (int16_t)step;\n    (void)cpu;'),
    ('the string step is always one', 'cpu/ops_str.c',
     '    int step = bits / 8;', '    int step = 1;\n    (void)bits;'),
    ('a string with no prefix still repeats', 'cpu/ops_str.c',
     '    if (cpu->prefix.repeat == 0) {\n'
     '        body(cpu, bits, delta, source);\n'
     '        return VM86_CONTINUE;\n'
     '    }',
     '    if (false) {\n'
     '        body(cpu, bits, delta, source);\n'
     '        return VM86_CONTINUE;\n'
     '    }'),
    ('f2 stops the way f3 does', 'cpu/ops_str.c',
     'bool stop_when_zf_set = (cpu->prefix.repeat == 0xF2);',
     'bool stop_when_zf_set = false;'),
    ('the stop condition is inverted', 'cpu/ops_str.c',
     'if (conditional && vm86_flag_test(cpu, VM86_ZF) == stop_when_zf_set)',
     'if (conditional && vm86_flag_test(cpu, VM86_ZF) != stop_when_zf_set)'),
    ('cmps swaps its operands', 'cpu/ops_str.c',
     '    uint16_t lhs = str_load(cpu, source, cpu->si, bits);\n'
     '    uint16_t rhs = str_load(cpu, VM86_ES, cpu->di, bits);',
     '    uint16_t lhs = str_load(cpu, VM86_ES, cpu->di, bits);\n'
     '    uint16_t rhs = str_load(cpu, source, cpu->si, bits);'),
    ('scas swaps its operands', 'cpu/ops_str.c',
     'vm86_alu(cpu, VM86_ALU_CMP, bits, accumulator, memory);',
     'vm86_alu(cpu, VM86_ALU_CMP, bits, memory, accumulator);'),
    ('movs writes through the read side', 'cpu/ops_str.c',
     '    uint16_t value = str_load(cpu, source, cpu->si, bits);\n\n'
     '    str_store(cpu, VM86_ES, cpu->di, bits, value);',
     '    uint16_t value = str_load(cpu, source, cpu->si, bits);\n\n'
     '    str_store(cpu, source, cpu->di, bits, value);'),
    ('lods writes the whole of ax', 'cpu/ops_str.c',
     '        cpu->al = (uint8_t)value;', '        cpu->ax = value;'),
    ('the count is tested against the wrong zero', 'cpu/ops_str.c',
     'while (cpu->cx != 0) {', 'while (cpu->cx != 0xFFFFu) {'),

    # ---- ops_ctl.c -------------------------------------------------
    ('jcc fetches only when the branch is taken', 'cpu/ops_ctl.c',
     '    int8_t displacement = (int8_t)vm86_fetch8(cpu);\n\n'
     '    if (vm86_conditions[opcode & 0x0F](cpu))\n'
     '        cpu->ip = (uint16_t)(cpu->ip + displacement);',
     '    if (vm86_conditions[opcode & 0x0F](cpu)) {\n'
     '        int8_t displacement = (int8_t)vm86_fetch8(cpu);\n'
     '        cpu->ip = (uint16_t)(cpu->ip + displacement);\n'
     '    }'),
    ('jcxz decrements the counter', 'cpu/ops_ctl.c',
     '    if (opcode == 0xE3) {', '    if (opcode == 0xFF) {'),
    ('the loop ignores the count running out', 'cpu/ops_ctl.c',
     '    if (cpu->cx == 0)\n        return VM86_CONTINUE;\n\n'
     '    bool take = true;',
     '    bool take = true;'),
    ('iret does not normalise the flags', 'cpu/ops_ctl.c',
     '    cpu->flags = vm86_pop16(cpu);\n\n'
     '    vm86_set_seg(cpu, VM86_CS, segment);\n'
     '    cpu->ip = offset;\n\n'
     '    /* FLAGS was written wholesale, so the hardwired bits have to be\n'
     '     * forced back before anything reads them again. See cpu.h. */\n'
     '    vm86_flags_normalise(cpu);',
     '    cpu->flags = vm86_pop16(cpu);\n\n'
     '    vm86_set_seg(cpu, VM86_CS, segment);\n'
     '    cpu->ip = offset;'),
    ('iret pops in the order int pushed', 'cpu/ops_ctl.c',
     '    uint16_t offset  = vm86_pop16(cpu);\n'
     '    uint16_t segment = vm86_pop16(cpu);\n\n'
     '    cpu->flags = vm86_pop16(cpu);',
     '    uint16_t segment = vm86_pop16(cpu);\n'
     '    uint16_t offset  = vm86_pop16(cpu);\n\n'
     '    cpu->flags = vm86_pop16(cpu);'),
    # The frame-order entry now lives in the M4 list below, where the code
    # it edits lives -- the frame moved to intr/deliver.c with the INT path,
    # and writing it twice would mean two mutants with one name.
    ('popf does not normalise the flags', 'cpu/ops_ctl.c',
     '    cpu->flags = vm86_pop16(cpu);\n\n'
     '    /* A wholesale write, so normalise. Without it a program could clear\n'
     '     * bits that cannot be cleared on an 8086, and a program that checks\n'
     '     * for them would conclude it is running on something else. */\n'
     '    vm86_flags_normalise(cpu);',
     '    cpu->flags = vm86_pop16(cpu);'),
    ('sahf writes the whole register', 'cpu/ops_ctl.c',
     '    cpu->flags = (uint16_t)((cpu->flags & 0xFF00u) | cpu->ah);',
     '    cpu->flags = cpu->ah;'),
    ('lahf takes the high byte', 'cpu/ops_ctl.c',
     '    cpu->ah = (uint8_t)(cpu->flags & 0x00FFu);',
     '    cpu->ah = (uint8_t)(cpu->flags >> 8);'),
    ('into fires without overflow', 'cpu/ops_ctl.c',
     '    if (!vm86_flag_test(cpu, VM86_OF))',
     '    if (vm86_flag_test(cpu, VM86_OF))'),
    # Retargeted: the vector table lookup moved to intr/deliver.c.
    ('the vector table is based on cs', 'intr/deliver.c',
     '    uint32_t entry = (uint32_t)vector * 4u;',
     '    uint32_t entry = (uint32_t)vector * 4u\n'
     '                     + vm86_linear(cpu, VM86_CS, 0);'),
    ('in writes the whole of ax', 'cpu/ops_ctl.c',
     '        cpu->al = 0xFF;', '        cpu->ax = 0xFFFF;'),
    ('the port is always taken from dx', 'cpu/ops_ctl.c',
     '    if (opcode & 0x08)\n        return cpu->dx;\n\n'
     '    return vm86_fetch8(cpu);',
     '    (void)opcode;\n    return cpu->dx;'),
    ('ret imm subtracts its adjustment', 'cpu/ops_ctl.c',
     '    cpu->sp = (uint16_t)(cpu->sp + adjustment);\n\n'
     '    return VM86_CONTINUE;\n}\n\n'
     'static enum vm86_result op_ret_far(',
     '    cpu->sp = (uint16_t)(cpu->sp - adjustment);\n\n'
     '    return VM86_CONTINUE;\n}\n\n'
     'static enum vm86_result op_ret_far('),
    ('the far call pushes its frame the other way', 'cpu/ops_ctl.c',
     '    vm86_push16(cpu, cpu->cs);\n'
     '    vm86_push16(cpu, cpu->ip);\n\n'
     '    vm86_set_seg(cpu, VM86_CS, segment);',
     '    vm86_push16(cpu, cpu->ip);\n'
     '    vm86_push16(cpu, cpu->cs);\n\n'
     '    vm86_set_seg(cpu, VM86_CS, segment);'),
    ('ff /3 and /5 accept a register', 'cpu/ops_ctl.c',
     '        if (mr.operand.kind != VM86_OPERAND_MEMORY) {\n'
     '            cpu->fault = VM86_VECTOR_INVALID_OPCODE;\n'
     '            return VM86_FAULT;\n'
     '        }',
     '        if (false) {\n'
     '            cpu->fault = VM86_VECTOR_INVALID_OPCODE;\n'
     '            return VM86_FAULT;\n'
     '        }'),
    ('sti does not set the interrupt flag', 'cpu/ops_ctl.c',
     '        vm86_flag_set(cpu, VM86_IF, true);',
     '        vm86_flag_set(cpu, VM86_IF, false);'),

    # ---- ops_186.c -------------------------------------------------
    ('pusha reads the live stack pointer', 'cpu/ops_186.c',
     '    vm86_push16(cpu, original_sp);',
     '    vm86_push16(cpu, (uint16_t)(original_sp - 8));'),
    ('popa writes the saved stack pointer', 'cpu/ops_186.c',
     '    (void)vm86_pop16(cpu);      /* the saved SP: meaningless, discarded */',
     '    cpu->sp = vm86_pop16(cpu);'),
    ('enter reads one frame twice', 'cpu/ops_186.c',
     '        cpu->bp = (uint16_t)(cpu->bp - 2);',
     '        cpu->bp = (uint16_t)(cpu->bp - 0);'),
    ('enter never links the frame', 'cpu/ops_186.c',
     '    if (level > 0)\n        vm86_push16(cpu, frame);', ''),
    ('bound compares unsigned', 'cpu/ops_186.c',
     '    int16_t value = (int16_t)vm86_reg_get16(cpu, mr.reg);\n'
     '    int16_t lower = (int16_t)vm86_mem_read16(cpu->mem, mr.operand.addr);\n'
     '    int16_t upper = (int16_t)vm86_mem_read16(cpu->mem, mr.operand.addr + 2);',
     '    uint16_t value = vm86_reg_get16(cpu, mr.reg);\n'
     '    uint16_t lower = vm86_mem_read16(cpu->mem, mr.operand.addr);\n'
     '    uint16_t upper = vm86_mem_read16(cpu->mem, mr.operand.addr + 2);'),
    ('bound excludes its lower end', 'cpu/ops_186.c',
     '    if (value < lower || value > upper) {',
     '    if (value <= lower || value > upper) {'),
    ('push imm8 is not sign extended', 'cpu/ops_186.c',
     '        vm86_push16(cpu, vm86_sign_extend8(vm86_fetch8(cpu)));',
     '        vm86_push16(cpu, vm86_fetch8(cpu));'),
    ('imul with an immediate reports the wrong thing', 'cpu/ops_186.c',
     '    bool does_not_fit = ((int32_t)(int16_t)result != product);',
     '    bool does_not_fit = (product != 0);'),
    ('outs writes its source', 'cpu/ops_186.c',
     '    cpu->si = (uint16_t)(cpu->si + delta);\n}',
     '    vm86_mem_write8(cpu->mem, src, 0x00);\n'
     '    cpu->si = (uint16_t)(cpu->si + delta);\n}'),

    # ---- the test file itself --------------------------------------
    #
    # A few properties are structural rather than a line of code: the
    # instruction pointer is a uint16_t, so an addition to it cannot
    # overflow the field and no edit to the emulator can make it. For
    # those, the expectation in the case is moved by one and the case is
    # shown to go red, which is the other half of "prove it can fail".
    ('expectation: the wrapping jump lands at FFF8', 'tests/test_verify.c',
     'vm86_expect_u16("IP", cpu->ip, 0xFFF9);',
     'vm86_expect_u16("IP", cpu->ip, 0xFFF8);'),
    ('expectation: the folded read is BEEF', 'tests/test_verify.c',
     'vm86_expect_u16("AX read the folded byte", cpu->ax, 0xBEEF);',
     'vm86_expect_u16("AX read the folded byte", cpu->ax, 0xBEEE);'),
]

# ---------------------------------------------------------------------
# What this milestone added
# ---------------------------------------------------------------------

M4_MUTATIONS = [
    # ---- intr/deliver.c --------------------------------------------
    ('a raise overwrites the pending set', 'intr/deliver.c',
     '    cpu->intr_pending[vector >> 3] |= (uint8_t)(1u << (vector & 7u));',
     '    cpu->intr_pending[vector >> 3] = (uint8_t)(1u << (vector & 7u));'),
    ('clearing a pending vector does nothing', 'intr/deliver.c',
     '    cpu->intr_pending[vector >> 3] &= (uint8_t)~(1u << (vector & 7u));',
     '    (void)vector;\n    (void)cpu;'),
    ('the highest pending vector is chosen', 'intr/deliver.c',
     '        unsigned bit = 0;\n'
     '        while (!(bits & (1u << bit)))\n'
     '            bit++;',
     '        unsigned bit = 7;\n'
     '        while (!(bits & (1u << bit)))\n'
     '            bit--;'),
    ('recognition ignores the shadow', 'intr/deliver.c',
     '    if (cpu->intr_shadow)\n        return false;\n\n', ''),
    ('recognition ignores the interrupt flag', 'intr/deliver.c',
     '    if (!vm86_flag_test(cpu, VM86_IF))\n        return false;\n\n', ''),
    ('the interrupt frame goes on in the wrong order', 'intr/deliver.c',
     '    vm86_push16(cpu, cpu->flags);\n'
     '    vm86_push16(cpu, cpu->cs);\n'
     '    vm86_push16(cpu, cpu->ip);',
     '    vm86_push16(cpu, cpu->flags);\n'
     '    vm86_push16(cpu, cpu->ip);\n'
     '    vm86_push16(cpu, cpu->cs);'),
    ('entering a handler leaves interrupts on', 'intr/deliver.c',
     '    vm86_flag_set(cpu, VM86_IF, false);\n', ''),

    # ---- intr/trap.c -----------------------------------------------
    ('the trap does not read the service number', 'intr/trap.c',
     '    uint8_t service = vm86_fetch8(cpu);', '    uint8_t service = 0;'),
    ('the trap forgets where it began', 'intr/trap.c',
     '    cpu->insn_ip = (uint16_t)(cpu->ip - 2u);\n\n', ''),
    ('the trap rewinds to the wrong byte', 'intr/trap.c',
     'cpu->insn_ip = (uint16_t)(cpu->ip - 2u);',
     'cpu->insn_ip = (uint16_t)(cpu->ip - 1u);'),
    ('a retry does not re-run the instruction', 'intr/trap.c',
     '    cpu->ip = cpu->insn_ip;', '    (void)0;'),
    ('a retry leaves interrupts off', 'intr/trap.c',
     '    vm86_flag_set(cpu, VM86_IF, true);', '    (void)0;'),

    # ---- intr/ivt.c ------------------------------------------------
    ('the stub does not end in iret', 'intr/ivt.c',
     '    vm86_mem_write8(cpu->mem, at + 3u, VM86_TRAP_IRET);',
     '    vm86_mem_write8(cpu->mem, at + 3u, 0x90u);'),
    ('the table points somewhere else', 'intr/ivt.c',
     '        vm86_mem_write16(cpu->mem, vector_entry(vector) + 2u,\n'
     '                         VM86_TRAP_SEGMENT);',
     '        vm86_mem_write16(cpu->mem, vector_entry(vector) + 2u,\n'
     '                         0x0000u);'),
    ('the stubs are spaced wrong', 'intr/ivt.c',
     '    return (uint16_t)((uint32_t)vector * VM86_TRAP_STRIDE);',
     '    return (uint16_t)((uint32_t)vector * 8u);'),
    ('the stub is not written into the table', 'intr/ivt.c',
     '        vm86_mem_write16(cpu->mem, vector_entry(vector), offset);\n', ''),
    ('every vector looks hooked', 'intr/ivt.c',
     '    return offset == stub_offset(vector) && segment == VM86_TRAP_SEGMENT;',
     '    (void)offset; (void)segment;\n    return true;'),
    ('nothing looks hooked', 'intr/ivt.c',
     '    return offset == stub_offset(vector) && segment == VM86_TRAP_SEGMENT;',
     '    (void)offset; (void)segment;\n    return false;'),

    # ---- intr/firmware.c -------------------------------------------
    ('int 12h answers the equipment word', 'intr/firmware.c',
     '    cpu->ax = vm86_mem_read16(cpu->mem, bda(VM86_BDA_MEMORY_KB));',
     '    cpu->ax = vm86_mem_read16(cpu->mem, bda(VM86_BDA_EQUIPMENT));'),
    ('int 11h answers the memory word', 'intr/firmware.c',
     '    cpu->ax = vm86_mem_read16(cpu->mem, bda(VM86_BDA_EQUIPMENT));',
     '    cpu->ax = vm86_mem_read16(cpu->mem, bda(VM86_BDA_MEMORY_KB));'),
    ('the conventional memory size is wrong', 'intr/firmware.c',
     '#define VM86_CONVENTIONAL_KB 640u', '#define VM86_CONVENTIONAL_KB 512u'),
    ('the equipment word loses its video bits', 'intr/firmware.c',
     '#define VM86_EQUIPMENT_WORD 0x002Du',
     '#define VM86_EQUIPMENT_WORD 0x0001u'),
    ('the memory service is never registered', 'intr/firmware.c',
     '    vm86_register_service(VM86_INT_MEMORY_SIZE, service_memory_size, NULL);\n',
     '    (void)service_memory_size;\n'),
    ('the memory size is never written', 'intr/firmware.c',
     '    vm86_mem_write16(cpu->mem, bda(VM86_BDA_MEMORY_KB), '
     'VM86_CONVENTIONAL_KB);\n', ''),
    ('the vector table is not installed', 'intr/firmware.c',
     '    vm86_install_ivt(cpu);\n', ''),

    # ---- intr/run.c ------------------------------------------------
    ('a halted machine stops before delivery', 'intr/run.c',
     '        if (cpu->intr_shadow == 0) {\n            int vector = vm86_interruptible',
     '        if (cpu->halted)\n            return VM86_STOP_HALT;\n\n'
     '        if (cpu->intr_shadow == 0) {\n            int vector = vm86_interruptible'),
    ('the budget is checked before delivery', 'intr/run.c',
     '        if (cpu->intr_shadow == 0) {\n            int vector = vm86_interruptible',
     '        if (retired >= steps)\n            return VM86_STOP_STEPS;\n\n'
     '        if (cpu->intr_shadow == 0) {\n            int vector = vm86_interruptible'),
    ('hlt returns instead of waiting', 'intr/run.c',
     '            */\n            break;\n\n        case VM86_FAULT: {',
     '            */\n            return VM86_STOP_HALT;\n\n        case VM86_FAULT: {'),
    ('the shadow is never spent', 'intr/run.c',
     '        if (cpu->intr_shadow != 0)\n            cpu->intr_shadow--;\n\n', ''),
    ('the shadow is spent before the budget', 'intr/run.c',
     '    for (;;) {\n        /*\n         * 1. The shadow, then delivery.',
     '    for (;;) {\n        if (cpu->intr_shadow) cpu->intr_shadow--;\n'
     '        /*\n         * 1. The shadow, then delivery.'),
    ('a delivery leaves the vector pending', 'intr/run.c',
     '                vm86_clear_pending(cpu, (uint8_t)vector);\n', ''),
    ('a delivery does not wake the machine', 'intr/run.c',
     '                cpu->halted = false;\n', ''),
    ('every fault stops the machine', 'intr/run.c',
     '            if (vm86_vector_is_stub(cpu, vector)) {', '            if (true) {'),
    ('no fault ever stops the machine', 'intr/run.c',
     '            if (vm86_vector_is_stub(cpu, vector)) {', '            if (false) {'),

    # ---- cpu: the three debts, and what the trap needs --------------
    ('sti does not open the grace period', 'cpu/ops_ctl.c',
     '        vm86_flag_set(cpu, VM86_IF, true);\n        cpu->intr_shadow = 1;',
     '        vm86_flag_set(cpu, VM86_IF, true);'),
    ('mov ss does not open the grace period', 'cpu/ops_mov.c',
     '    if (mr.reg == VM86_SS)\n        cpu->intr_shadow = 1;', '    (void)0;'),
    ('pop ss does not open the grace period', 'cpu/ops_alu.c',
     '            if (segment == VM86_SS)\n                cpu->intr_shadow = 1;',
     '            (void)segment;'),
    ('fe /7 is refused instead of trapped', 'cpu/ops_alu.c',
     '    if (mr.reg == 7)\n        return vm86_host_trap(cpu, opcode);\n\n', ''),
    ('rep never yields between iterations', 'cpu/ops_str.c',
     '        if (vm86_interruptible(cpu)) {\n'
     '            cpu->ip = cpu->insn_ip;\n'
     '            return VM86_CONTINUE;\n'
     '        }\n', ''),
    ('rep yields before the first element', 'cpu/ops_str.c',
     '    while (cpu->cx != 0) {\n        body(cpu, bits, delta, source);',
     '    while (cpu->cx != 0) {\n'
     '        if (vm86_interruptible(cpu)) {\n'
     '            cpu->ip = cpu->insn_ip;\n'
     '            return VM86_CONTINUE;\n'
     '        }\n'
     '        body(cpu, bits, delta, source);'),
    ('rep rewinds past its prefixes', 'cpu/ops_str.c',
     '            cpu->ip = cpu->insn_ip;',
     '            cpu->ip = (uint16_t)(cpu->insn_ip + 2u);'),
    ('where the instruction began is not recorded', 'cpu/step.c',
     '    cpu->insn_ip = cpu->ip;\n\n    uint8_t opcode;', '    uint8_t opcode;'),
    ('the fault is not cleared per instruction', 'cpu/step.c',
     '    cpu->fault = VM86_NO_FAULT;', '    (void)0;'),

    # ---- mem -------------------------------------------------------
    ('the a20 wrap runs the wrong way', 'mem/mem.c',
     '        linear &= VM86_A20_MASK;', '        linear |= VM86_A20_MASK;'),

    # ---- bios/bios13.c ---------------------------------------------
    ('the cylinder loses its top two bits', 'bios/bios13.c',
     '    return (uint16_t)((((uint16_t)cl >> BIOS13_CL_CYLINDER_SHIFT) << 8) | ch);',
     '    (void)cl;\n    return (uint16_t)ch;'),
    ('the cylinder and the sector share bits', 'bios/bios13.c',
     '    return (uint8_t)(cl & BIOS13_CL_SECTOR_MASK);',
     '    return (uint8_t)(cl & 0x3Fu);'),
    ('the lba forgets the head', 'bios/bios13.c',
     '    return ((uint32_t)cylinder * disk->heads + head) * disk->sectors\n'
     '           + (uint32_t)(sector - 1);',
     '    (void)head;\n'
     '    return ((uint32_t)cylinder * disk->heads) * disk->sectors\n'
     '           + (uint32_t)(sector - 1);'),
    ('sector zero is accepted', 'bios/bios13.c',
     '    if (sector == 0 ||', '    if (false ||'),
    ('a request for no sectors is accepted', 'bios/bios13.c',
     '    if (count == 0)\n        return BIOS13_STATUS_BAD_COMMAND;',
     '    if (false)\n        return BIOS13_STATUS_BAD_COMMAND;'),
    ('a successful call reports a failure', 'bios/bios13.c',
     '    disk->last_status = BIOS13_STATUS_OK;\n'
     '    cpu->ah           = BIOS13_STATUS_OK;\n'
     '    vm86_flag_set(cpu, VM86_CF, false);',
     '    disk->last_status = BIOS13_STATUS_OK;\n'
     '    cpu->ah           = BIOS13_STATUS_OK;\n'
     '    vm86_flag_set(cpu, VM86_CF, true);'),
    ('a failed call leaves the sector count standing', 'bios/bios13.c',
     '    cpu->al = 0;\n\n    vm86_flag_set(cpu, VM86_CF, true);',
     '    vm86_flag_set(cpu, VM86_CF, true);'),
    ('a read writes to the disk instead', 'bios/bios13.c',
     '        if (write)', '        if (!write)'),
    ('the dma boundary test is off by one', 'bios/bios13.c',
     '    if (((request->address & 0xFFFFu) + request->bytes) > 0x10000u)',
     '    if (((request->address & 0xFFFFu) + request->bytes) >= 0x10000u)'),
    ('a drive other than a: is accepted', 'bios/bios13.c',
     '    if (drive != BIOS13_DRIVE_A)', '    (void)drive;\n    if (false)'),

    # ---- bios/bios10.c ---------------------------------------------
    ('a text cell is one byte wide', 'bios/bios10.c',
     '    return page_base(st, page) + ((uint32_t)row * st->columns + col) * 2u;',
     '    return page_base(st, page) + ((uint32_t)row * st->columns + col);'),
    ('a page ignores its own base', 'bios/bios10.c',
     '    return VM86_TEXT_BASE + (uint32_t)page * st->page_bytes;',
     '    (void)page; (void)st;\n    return VM86_TEXT_BASE;'),
    ('a teletype write clobbers the attribute', 'bios/bios10.c',
     '    vm86_mem_write8(cpu->mem, cell_linear(st, page, row, col), ch);\n}',
     '    uint32_t linear = cell_linear(st, page, row, col);\n'
     '    vm86_mem_write8(cpu->mem, linear, ch);\n'
     '    vm86_mem_write8(cpu->mem, linear + 1u, ch);\n}'),
    ('a backspace climbs to the line above', 'bios/bios10.c',
     '        if (col > 0)\n            col--;', '        col--;'),
    ('the fill attribute is read from the wrong row', 'bios/bios10.c',
     '    return cell_attr(cpu, st, page, BIOS10_ROWS - 1u, col);',
     '    return cell_attr(cpu, st, page, BIOS10_ROWS - 2u, col);'),

    # ---- what main added while this tool was being written -----------
    ('the service flags are not merged into the frame', 'intr/trap.c',
     '    uint16_t merged = (uint16_t)((cpu->flags & (uint16_t)~(VM86_IF '
     '| VM86_TF))\n'
     '                                 | (frame & (VM86_IF | VM86_TF)));\n\n'
     '    vm86_mem_write16(cpu->mem, at, merged);',
     '    uint16_t merged = frame;\n\n'
     '    vm86_mem_write16(cpu->mem, at, merged);'),
    ('the frame is written back to the wrong word', 'intr/trap.c',
     '           + (uint16_t)(cpu->sp + 4u);',
     '           + (uint16_t)(cpu->sp + 2u);'),
    ('the video mode byte is never written', 'intr/firmware.c',
     '    vm86_mem_write8 (cpu->mem, bda(VM86_BDA_VIDEO_MODE), 3u);\n', ''),
    ('the active page byte is never written', 'intr/firmware.c',
     '    vm86_mem_write8 (cpu->mem, bda(VM86_BDA_ACTIVE_PAGE), 0u);\n', ''),
    ('the frame is written back without checking whose stack it is',
     'intr/trap.c',
     '    if (cpu->sp != cpu->intr_frame_sp)\n        return false;',
     '    if (false)\n        return false;'),
    ('the frame pointer is recorded in the wrong place', 'intr/deliver.c',
     '    cpu->intr_frame_sp = cpu->sp;',
     '    cpu->intr_frame_sp = (uint16_t)(cpu->sp + 6u);'),

    # ---- bios/bios16.c ---------------------------------------------
    ('the keyboard ring does not wrap', 'bios/bios16.c',
     '    return (offset >= VM86_BDA_KB_BUFFER_END) ? (uint16_t)VM86_BDA_KB_BUFFER',
     '    return (false) ? (uint16_t)VM86_BDA_KB_BUFFER'),
    ('a full keyboard buffer overwrites the oldest key', 'bios/bios16.c',
     '    if (next == kb_head(mem)) {',
     '    if (false && next == kb_head(mem)) {'),
    ('taking a key leaves the head where it was', 'bios/bios16.c',
     '    vm86_mem_write16(mem, bda_linear(VM86_BDA_KB_HEAD), kb_advance(head));',
     '    vm86_mem_write16(mem, bda_linear(VM86_BDA_KB_HEAD), head);'),
    ('a look at the queue consumes the key', 'bios/bios16.c',
     '    *word = vm86_mem_read16(mem, bda_linear(kb_head(mem)));\n\n'
     '    return true;',
     '    *word = vm86_mem_read16(mem, bda_linear(kb_head(mem)));\n'
     '    vm86_mem_write16(mem, bda_linear(VM86_BDA_KB_HEAD),\n'
     '                     kb_advance(kb_head(mem)));\n\n'
     '    return true;'),
    ('an empty queue reports a key waiting', 'bios/bios16.c',
     '            vm86_flag_set(cpu, VM86_ZF, true);',
     '            vm86_flag_set(cpu, VM86_ZF, false);'),
    ('a blocking read does not wait', 'bios/bios16.c',
     '            vm86_service_retry(cpu);',
     '            (void)cpu;'),

    # ---- bios/bios1a.c ---------------------------------------------
    ('the tick count is read big-endian', 'bios/bios1a.c',
     '    return (uint32_t)vm86_mem_read16(mem, at)\n'
     '         | ((uint32_t)vm86_mem_read16(mem, at + 2) << 16);',
     '    return (uint32_t)vm86_mem_read16(mem, at + 2)\n'
     '         | ((uint32_t)vm86_mem_read16(mem, at) << 16);'),
    ('the tick count is written big-endian', 'bios/bios1a.c',
     '    vm86_mem_write16(mem, at, (uint16_t)(value & 0xFFFFu));\n'
     '    vm86_mem_write16(mem, at + 2, (uint16_t)(value >> 16));',
     '    vm86_mem_write16(mem, at, (uint16_t)(value >> 16));\n'
     '    vm86_mem_write16(mem, at + 2, (uint16_t)(value & 0xFFFFu));'),
    ('the leftover fraction of a tick is thrown away', 'bios/bios1a.c',
     '    st->fraction = (uint32_t)(total % BIOS1A_TICKS_PER_MS_DEN);',
     '    st->fraction = 0;'),
    ('the count goes into the wrong registers', 'bios/bios1a.c',
     '        cpu->cx = (uint16_t)(ticks >> 16);\n'
     '        cpu->dx = (uint16_t)(ticks & 0xFFFFu);',
     '        cpu->dx = (uint16_t)(ticks >> 16);\n'
     '        cpu->cx = (uint16_t)(ticks & 0xFFFFu);'),
    ('al does not get the midnight flag', 'bios/bios1a.c',
     '        cpu->al = flags;', '        cpu->al = 0;\n        (void)flags;'),
    ('setting the count leaves the midnight flag up', 'bios/bios1a.c',
     '        vm86_mem_write8(mem, bda_linear(VM86_BDA_TICK_ROLLOVER), 0);\n'
     '        set_tick_count(mem, ((uint32_t)cpu->cx << 16) | (uint32_t)cpu->dx);',
     '        set_tick_count(mem, ((uint32_t)cpu->cx << 16) | (uint32_t)cpu->dx);'),

    # ---- cpu/ops_ctl.c: the jumps nothing was mutating --------------
    ('a short jump does not jump', 'cpu/ops_ctl.c',
     '    branch_short(cpu);', '    (void)cpu;\n    (void)branch_short;'),
    ('a near jump does not jump', 'cpu/ops_ctl.c',
     '    branch_near(cpu);', '    (void)cpu;\n    (void)branch_near;'),
    ('cmc clears the carry instead of inverting it', 'cpu/ops_ctl.c',
     '    vm86_flag_set(cpu, VM86_CF, !vm86_flag_test(cpu, VM86_CF));',
     '    vm86_flag_set(cpu, VM86_CF, false);'),
]


# ---------------------------------------------------------------------
# M5: the DOS layer.
#
# W4's, and the first set whose anchors are in dos/dos -- a directory that
# did not exist when the two tables above were written.
#
# The shape of these is different from M3's and M4's, and the difference
# is worth naming. An instruction mutant usually changes an answer, and
# some case notices. A *convention* mutant changes something no case
# observes directly: whether the carry flag is cleared on success, whether
# an unimplemented function puts a code in AX, which of the two halves of
# the version is which. Those are the ones a suite written from its own
# implementation would miss, so several of the entries below are exactly
# them.
# ---------------------------------------------------------------------

M5_MUTATIONS = [
    # ---- dos/dos/int21.c: the version -------------------------------
    ('the version halves swapped', 'dos/int21.c',
     '    cpu->ax = VM86_DOS_VERSION;',
     '    cpu->ax = (uint16_t)((VM86_DOS_VERSION >> 8) |\n'
     '                          (VM86_DOS_VERSION << 8));'),

    # ---- dos/dos/int21.c: the console functions ---------------------
    ('09h prints the terminator too', 'dos/int21.c',
     '        if (ch == (uint8_t)VM86_INT21_STRING_END)\n'
     '            break;',
     '        if (ch == (uint8_t)VM86_INT21_STRING_END) {\n'
     '            bios10_tty(cpu, st->video, ch);   /* mutated */\n'
     '            break;\n'
     '        }'),
    ('the string walk never stops on unmapped memory', 'dos/int21.c',
     '        if (vm86_mem_offset(mem, at) == VM86_MEM_UNMAPPED)\n'
     '            break;',
     '        /* mutated: the memory bound is the only one left */'),
    ('a successful call leaves the carry alone', 'dos/int21.c',
     '    vm86_flag_set(cpu, VM86_CF, false);\n}',
     '    (void)cpu;   /* mutated: success touches no flag */\n}'),

    # ---- dos/dos/int21.c: the vectors -------------------------------
    ('25h writes the address the wrong way round', 'dos/int21.c',
     '    vm86_mem_write16(cpu->mem, at,      cpu->dx);\n'
     '    vm86_mem_write16(cpu->mem, at + 2u, cpu->ds);',
     '    vm86_mem_write16(cpu->mem, at,      cpu->ds);\n'
     '    vm86_mem_write16(cpu->mem, at + 2u, cpu->dx);'),
    ('35h hands back the offset in ES', 'dos/int21.c',
     '    cpu->bx = vm86_mem_read16(cpu->mem, at);\n\n'
     '    /* Through the accessor, because ES has a cached base and writing '
     'the\n'
     '     * register directly would leave the cache describing the old\n'
     '     * segment. See the note on seg_base in cpu.h. */\n'
     '    vm86_set_seg(cpu, VM86_ES, vm86_mem_read16(cpu->mem, at + 2u));',
     '    cpu->bx = vm86_mem_read16(cpu->mem, at + 2u);\n\n'
     '    vm86_set_seg(cpu, VM86_ES, vm86_mem_read16(cpu->mem, at));'),

    # ---- dos/dos/int21.c: the DTA -----------------------------------
    ('1Ah keeps a linear address', 'dos/int21.c',
     '    st->dta_segment = cpu->ds;\n'
     '    st->dta_offset  = cpu->dx;',
     '    uint32_t linear = ((uint32_t)cpu->ds << 4) + cpu->dx;\n'
     '    st->dta_segment = (uint16_t)(linear >> 4);\n'
     '    st->dta_offset  = (uint16_t)(linear & 0x000Fu);'),
    ('the default DTA ignores the PSP segment', 'dos/int21.c',
     '    st->dta_offset  = psp_segment ? '
     '(uint16_t)VM86_INT21_DEFAULT_DTA : 0u;',
     '    st->dta_offset  = (uint16_t)VM86_INT21_DEFAULT_DTA;'),

    # ---- dos/dos/int21.c: the drive and the errors ------------------
    ('0Eh ignores its argument', 'dos/int21.c',
     '    if (cpu->dl != (uint8_t)VM86_INT21_DRIVE_F) {',
     '    if (false && cpu->dl != (uint8_t)VM86_INT21_DRIVE_F) {'),
    ('19h always answers with drive A:', 'dos/int21.c',
     '    cpu->al = (uint8_t)VM86_INT21_DRIVE_F;',
     '    cpu->al = 0u;'),
    ('an unknown function returns an error code in AX', 'dos/int21.c',
     '        cpu->al = 0u;\n'
     '        vm86_flag_set(cpu, VM86_CF, true);',
     '        cpu->ax = 1u;\n'
     '        vm86_flag_set(cpu, VM86_CF, true);'),
    ('an error sets the carry but no code', 'dos/int21.c',
     '    vm86_flag_set(cpu, VM86_CF, true);\n'
     '    cpu->ax = code;',
     '    vm86_flag_set(cpu, VM86_CF, true);\n'
     '    (void)code;'),

    # ---- dos/dos/int21.c: terminating -------------------------------
    ('4Ch exits with zero instead of AL', 'dos/int21.c',
     '        vm86_service_exit(cpu, cpu->al);',
     '        vm86_service_exit(cpu, 0u);'),
    ('INT 20h exits with a code of its own', 'dos/int21.c',
     '    (void)ctx;   /* INT 20h carries nothing; see the note in '
     'int21.h */\n\n'
     '    vm86_service_exit(cpu, 0u);',
     '    (void)ctx;\n\n'
     '    vm86_service_exit(cpu, 1u);'),

    # ---- dos/intr/trap.c: the exit reaching the run loop ------------
    ('the trap does not report the exit', 'intr/trap.c',
     '    if (cpu->exited)\n'
     '        return VM86_EXIT;',
     '    if (false && cpu->exited)\n'
     '        return VM86_EXIT;'),
    ('the trap never writes a service\'s flags back', 'intr/trap.c',
     '    if (have_frame) {',
     '    if (false && have_frame) {'),

    # ---- dos/dos/psp.c: the loader and the dispatcher's PSP ---------
    ('the loader points every segment but CS at zero', 'dos/psp.c',
     '    vm86_set_seg(cpu, VM86_DS, start->segment);',
     '    vm86_set_seg(cpu, VM86_DS, 0u);'),
]


def layer_of(path):
    """Which layer a mutant belongs to, from the file it edits."""
    head = path.split('/')[0]
    return head if head in ('cpu', 'mem', 'intr', 'bios', 'dos') else 'tests'


# W5/W6 targeted resource defects. EXPECTED diagnoses live in
# inject-resources.py; the ordinary campaign can also run these.
M56_MUTATIONS = [('W5 mount ignores boot signature',
  'dos/fat.c',
  'image[510]!=0x55 || image[511]!=0xAA',
  'false'),
 ('W5 read never shortens at EOF',
  'dos/fat.c',
  'if(count>size-f->position) count=size-f->position;',
  'if(count>size-f->position) count=0;'),
 ('W5 close leaves descriptor live',
  'dos/fat.c',
  'f->used=false; return 0;',
  'f->used=true; return 0;'),
 ('W5 seek loses signed displacement',
  'dos/fat.c',
  'int64_t n=base+offset;',
  'int64_t n=base+(uint32_t)offset;'),
 ('W5 writes only the first FAT', 'dos/fat.c', 'i<v->copies;i++', 'i<1;i++'),
 ('W5 truncate retains old size',
  'dos/fat.c',
  'if(!count || f->position>old) put32(p+28,f->position);',
  'if(count && f->position>old) put32(p+28,f->position);'),
 ('W5 allows writing a readonly file',
  'dos/fat.c',
  '((a&1) && (create || mode))',
  '(false && (create || mode))'),
 ('W5 search restarts on FindNext',
  'dos/int21.c',
  'index|=(uint32_t)vm86_mem_read8(cpu->mem,guest_address(seg,off,15+i))<<(8*i);',
  'index=0;'),
 ('W5 DTA size is one byte late',
  'dos/int21.c',
  'result[26+i]=p[28+i];',
  'result[27+i]=p[28+i];'),
 ('W5 read ignores invalid guest buffers',
  'dos/int21.c',
  'if(!guest_range(cpu,cpu->ds,cpu->dx,count)) { dos_fail(cpu,13); return; }',
  'if(false && !guest_range(cpu,cpu->ds,cpu->dx,count)) { dos_fail(cpu,13); return; }'),
 ('W5 file read answer always zero',
  'dos/int21.c',
  'cpu->ax=(uint16_t)total; dos_ok(cpu);',
  'cpu->ax=0; dos_ok(cpu);'),
 ('W5 seek swaps AX and DX',
  'dos/int21.c',
  'cpu->ax=(uint16_t)pos; cpu->dx=(uint16_t)(pos>>16);',
  'cpu->dx=(uint16_t)pos; cpu->ax=(uint16_t)(pos>>16);'),
 ('W6 availability says zero for a waiting key',
  'dos/int21.c',
  '? 0xFF : 0; dos_ok(cpu);',
  '? 0 : 0; dos_ok(cpu);'),
 ('W6 direct empty input clears ZF',
  'dos/int21.c',
  'cpu->al=0; vm86_flag_set(cpu,VM86_ZF,true); dos_ok(cpu);',
  'cpu->al=0; vm86_flag_set(cpu,VM86_ZF,false); dos_ok(cpu);'),
 ('W6 echo call never echoes',
  'dos/int21.c',
  'if(fn==0x01) bios10_tty(cpu,st->video,ch);',
  'if(false && fn==0x01) bios10_tty(cpu,st->video,ch);'),
 ('W6 no echo call echoes',
  'dos/int21.c',
  'if(fn==0x01) bios10_tty(cpu,st->video,ch);',
  'if(fn==0x01 || fn==0x07) bios10_tty(cpu,st->video,ch);'),
 ('W6 extended key loses its scan byte',
  'dos/int21.c',
  '*ch=st->extended_key; st->extended_pending=false;',
  '*ch=0; st->extended_pending=false;'),
 ('W6 line count includes carriage return',
  'dos/int21.c',
  'guest_address(seg,off,1),st->line_length);',
  'guest_address(seg,off,1),(uint8_t)(st->line_length+1));'),
 ('W6 line buffer does not reserve carriage return',
  'dos/int21.c',
  'st->line_length<max-1u',
  'st->line_length<max'),
 ('W6 backspace never removes a character',
  'dos/int21.c',
  'st->line_length--; bios10_tty(cpu,st->video,8);',
  'st->line_length+=0; bios10_tty(cpu,st->video,8);'),
 ('W6 IRET forgets the outer interrupt frame',
  'cpu/ops_ctl.c',
  'cpu->intr_frame_sp=cpu->intr_frames[cpu->intr_depth-1].sp;',
  'cpu->intr_frame_sp=0;')]

MUTATIONS = list(M3_MUTATIONS) + list(M4_MUTATIONS) + list(M5_MUTATIONS) + list(M56_MUTATIONS)


# ---------------------------------------------------------------------
# The tree under test
# ---------------------------------------------------------------------

def refresh_tree(source):
    """A private copy, so the campaign cannot touch the checkout."""
    if os.path.exists(WORK_ROOT):
        shutil.rmtree(WORK_ROOT)
    os.makedirs(REPO)
    for part in ('dos', 'libk', 'tools'):
        shutil.copytree(os.path.join(source, part), os.path.join(REPO, part))


# Not every suite uses the standard harness, and the exceptions do not
# announce themselves: the corpus prints its own summary and names a
# failing case with an outcome word instead of a count, and the bios
# programs suite calls itself "bios programs" -- with a space, which is
# not a suite name anywhere else -- and appends nothing at all. A parser
# that only understood the common shape would drop that suite silently.
#
# Silently is the part that matters. A suite missing from the report looks
# exactly like a suite with nothing to say, and the report still reads as
# complete; the check below refuses to produce one unless every binary
# that ran was seen.
SUITE_HEADER = re.compile(r'^=== (.+?): (\d+) cases.*===$')
CASE_OK      = re.compile(r'^  ok  (.+)$')
CASE_RED     = re.compile(r'^  --  (.+)$')
RED_SUFFIX   = re.compile(r' \([^()]*\)$')


def case_name(tail):
    """The name of a red case, with whatever the suite appended removed.

    What follows the name is not the same in any two harnesses -- "(3
    failed)", a corpus outcome word, or nothing -- so the suffix is
    stripped and the result is then *checked* against the names the
    pristine run reported. A name that comes out of the strip and was
    never a case is a defect in this parser, and the campaign says so
    rather than inventing a case no suite contains.
    """
    stripped = RED_SUFFIX.sub('', tail)
    return stripped or tail


def parse(out, universe=None):
    """Per suite, which cases ran and which of them failed.

    Suite names are normalised to the spelling the binaries use, so that
    "bios programs" and test_bios_programs are the same suite here.
    """
    results = {}
    suite = None
    for line in out.splitlines():
        head = SUITE_HEADER.match(line)
        if head:
            suite = head.group(1).replace(' ', '_')
            results.setdefault(suite, {})
            continue
        ok = CASE_OK.match(line)
        if ok and suite:
            results[suite][ok.group(1)] = True
            continue
        red = CASE_RED.match(line)
        if red and suite:
            name = case_name(red.group(1))
            if universe is not None and name not in universe.get(suite, ()):
                name = red.group(1)
            results[suite][name] = False
    return results


def build_and_run(jobs, timeout):
    """make test, forced, with a wall clock on the whole thing.

    The timeout has to kill the process *group*: make's children are the
    suite binaries, and a mutant that makes an instruction spin forever
    hangs inside one of those rather than inside make.
    """
    cmd = ['make', '-B', '-j%d' % jobs, 'BUILD=%s' % BUILD, 'test']
    proc = subprocess.Popen(cmd, cwd=DOS, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            start_new_session=True)
    try:
        out, _ = proc.communicate(timeout=timeout)
        return proc.returncode, out, False
    except subprocess.TimeoutExpired:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        out, _ = proc.communicate()
        return None, out, True


def digest_binaries():
    """A hash per suite binary: the probe's whole input."""
    out = {}
    if not os.path.isdir(BUILD):
        return out
    for name in os.listdir(BUILD):
        if not name.startswith('test_'):
            continue
        with open(os.path.join(BUILD, name), 'rb') as fh:
            out[name] = hashlib.sha256(fh.read()).hexdigest()
    return out


def save_binaries(where):
    if os.path.exists(where):
        shutil.rmtree(where)
    shutil.copytree(BUILD, where)


def make_newer_than_the_build(path):
    """Defeat the timestamp granularity that make compares on.

    A source written in the same tick as the binary built from it looks
    older to make, the rebuild is skipped, and the suite that runs is the
    pristine one -- which reads as a mutant that survived. The check on
    the build output below catches that too; this is the belt.
    """
    newest = 0.0
    for dirpath, _, files in os.walk(BUILD):
        for name in files:
            newest = max(newest, os.path.getmtime(os.path.join(dirpath, name)))
    stamp = max(time.time(), newest + 1.0)
    os.utime(path, (stamp, stamp))


# ---------------------------------------------------------------------
# The campaign
# ---------------------------------------------------------------------

def first_error(out):
    """The compiler's own words for why a mutant did not build.

    A mutation that removes the only use of a variable fails -Werror, and
    "did not compile" on its own sends the next reader to work out which
    one -- when the answer is one line further down the build log. It is
    also the difference between a mutant that was wrong and one that the
    suite caught, which is the whole point of counting them apart.
    """
    for line in out.splitlines():
        if 'error:' in line:
            return line.strip()
    return 'did not compile'


def campaign(args, mutations, table_size):
    refresh_tree(args.tree or ROOT)

    sys.stdout.write('baseline: building and running every suite...\n')
    sys.stdout.flush()
    rc, out, hung = build_and_run(args.jobs, args.timeout)
    if hung:
        print('the pristine tree does not finish; refusing to report')
        return 1
    if rc != 0:
        print('the pristine tree is not green:')
        for line in out.splitlines():
            if line.startswith('  --  '):
                print('   ' + line.strip())
        return 1

    baseline = parse(out)
    all_cases = {}
    for suite, cases in baseline.items():
        for case, ok in cases.items():
            if not ok:
                print('the pristine tree has a red case: %s / %s'
                      % (suite, case))
                return 1
            all_cases.setdefault(suite, []).append(case)

    total = sum(len(v) for v in all_cases.values())
    print('baseline: %d suites, %d cases, all green\n'
          % (len(all_cases), total))

    # Every binary that ran has to be visible in that output. A suite the
    # tool cannot see is worse than a suite that fails: its cases are
    # missing from the coverage rather than wrong in it, and the report
    # still reads as complete. This is not hypothetical -- it caught
    # "bios programs", whose header has a space in it.
    unseen = sorted(name[len('test_'):] for name in digest_binaries()
                    if name[len('test_'):] not in all_cases)
    if unseen:
        print('these suites ran but nothing in their output was parsed: %s'
              % ', '.join(unseen))
        print('refusing to report: their cases would be missing from the '
              'coverage rather than red in it')
        return 1

    # The probe is only a probe if the build is reproducible. Built twice
    # from the same source, the binaries have to be identical -- otherwise
    # "the binary changed" says nothing about the mutation.
    first = digest_binaries()
    rc, _, hung = build_and_run(args.jobs, args.timeout)
    second = digest_binaries()
    if hung or rc != 0 or first != second:
        print('the build is not reproducible, so the probe proves nothing;'
              ' refusing to report')
        return 1

    save_binaries(PRISTINE)
    print('probe: the build is reproducible (%d suite binaries)\n'
          % len(first))

    covered = {}      # case -> [mutant, ...]
    caught    = []    # (mutant, [case, ...])
    survived  = []    # mutant that changed the code and reddened nothing
    vacuous   = []    # mutant the compiler threw away
    broken    = []    # (mutant, why)

    for name, path, old, new in mutations:
        full = os.path.join(DOS, path)
        if not os.path.exists(full):
            broken.append((name, 'no such file: %s' % path))
            continue

        with open(full, encoding='utf-8') as fh:
            original = fh.read()

        if original.count(old) != 1:
            broken.append((name, 'the anchor appears %d times in %s'
                           % (original.count(old), path)))
            continue

        with open(full, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(original.replace(old, new))
        make_newer_than_the_build(full)

        try:
            rc, out, hung = build_and_run(args.jobs, args.timeout)

            if hung:
                # It compiled and ran and something never came back. That
                # is a caught mutation -- the machine is broken -- but
                # which case hung cannot be said, so it is not credited.
                mark, red = 'HUNG', []
                broken.append((name, 'the suite hung'))
            elif rc is None or '  CCh ' not in out:
                mark, red = 'NOBUILD', []
                broken.append((name, 'the build did not recompile'))
            elif 'error:' in out or ': error' in out:
                mark, red = 'NOCOMPILE', []
                broken.append((name, first_error(out)))
            else:
                red, unknown = [], []
                for suite, cases in parse(out, all_cases).items():
                    for case, ok in cases.items():
                        if ok:
                            continue
                        if case in all_cases.get(suite, ()):
                            red.append(case)
                        else:
                            unknown.append('%s / %s' % (suite, case))
                red.sort()

                if unknown:
                    mark = 'PARSER'
                    red = []
                    broken.append((name, 'case name(s) this parser invented: '
                                   '%s' % ', '.join(unknown)))
                elif digest_binaries() == second:
                    mark = 'VACUOUS'
                    vacuous.append(name)
                elif red:
                    mark = 'caught'
                    caught.append((name, red))
                    for case in red:
                        covered.setdefault(case, []).append(name)
                else:
                    mark = 'SURVIVED'
                    survived.append(name)
        finally:
            with open(full, 'w', encoding='utf-8', newline='\n') as fh:
                fh.write(original)

        print('%-9s %-46s %d case(s) red' % (mark, name, len(red)),
              flush=True)

    return report(args, all_cases, caught, survived, vacuous, broken,
                  covered, len(mutations), table_size)


def report(args, all_cases, caught, survived, vacuous, broken, covered,
           ran, table_size):
    partial = ran != table_size

    missed_by_suite = {}
    for suite, cases in sorted(all_cases.items()):
        missed_by_suite[suite] = [c for c in cases if c not in covered]

    never = [c for suite in missed_by_suite.values() for c in suite]
    total = sum(len(v) for v in all_cases.values())

    if partial:
        print('\n(these %d mutations are a subset of the %d in the table, so '
              '"never reddened" below means "not reddened by this subset")'
              % (ran, len(MUTATIONS)))

    print('\n%d mutations: %d caught, %d survived, %d vacuous, %d broken'
          % (len(caught) + len(survived) + len(vacuous) + len(broken),
             len(caught), len(survived), len(vacuous), len(broken)))
    print('%d of %d cases were reddened by at least one mutation'
          % (total - len(never), total))

    print('\n%-12s %6s %6s %s' % ('suite', 'cases', 'red', 'never red'))
    for suite, cases in sorted(all_cases.items()):
        red = len(cases) - len(missed_by_suite[suite])
        print('%-12s %6d %6d %6d' % (suite, len(cases), red,
                                     len(missed_by_suite[suite])))

    if never:
        print('\nnever reddened by any mutation -- no case here can fail:')
        for suite, cases in sorted(missed_by_suite.items()):
            for case in cases:
                print('  %-10s %s' % (suite, case))

    if survived:
        print('\nmutations that changed the machine and nothing noticed:')
        for name in survived:
            print('  - %s' % name)

    if vacuous:
        print('\nmutations the compiler threw away -- a defect in the '
              'mutant, not a gap in the suite:')
        for name in vacuous:
            print('  - %s' % name)

    if broken:
        print('\nmutations that did not run:')
        for name, why in broken:
            print('  - %-46s %s' % (name, why))

    if args.json:
        with open(args.json, 'w', encoding='utf-8', newline='\n') as fh:
            json.dump({
                'cases': all_cases,
                'caught': [{'mutant': n, 'cases': c} for n, c in caught],
                'survived': survived,
                'vacuous': vacuous,
                'broken': [{'mutant': n, 'why': w} for n, w in broken],
                'never_reddened': never,
            }, fh, indent=2, sort_keys=True)
        print('\nwrote %s' % args.json)

    # A campaign whose mutants mostly failed to apply is not a result.
    return 1 if len(broken) * 2 > len(caught) + len(survived) else 0


def load_mutations(path):
    """Extra mutations from a file, so nobody has to edit this one.

    A campaign run against somebody else's suite should not mean a second
    copy of this tool: the table is data, and a file of it is enough. The
    format is either a list of {"name", "file", "old", "new"} objects or a
    list of four-element lists, which is what this file's own table looks
    like written out.
    """
    with open(path, encoding='utf-8') as fh:
        loaded = json.load(fh)

    out = []
    for entry in loaded:
        if isinstance(entry, dict):
            out.append((entry['name'], entry['file'], entry['old'],
                        entry['new']))
        else:
            name, path_, old, new = entry
            out.append((name, path_, old, new))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--tree', default=None,
                        help='the checkout to test; default is the one this '
                             'script lives in, and it is copied, never '
                             'edited in place')
    parser.add_argument('--mutations', default=None,
                        help='a JSON file of extra mutations, in the shape of '
                             'the table in this file')
    parser.add_argument('--layer', action='append', default=None,
                        help='cpu, mem, intr, bios or tests; repeatable')
    parser.add_argument('--only', default=None,
                        help='run mutations whose name contains this')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 1)
    parser.add_argument('--timeout', type=float, default=600.0,
                        help='seconds for one build-and-run')
    parser.add_argument('--json', default=None,
                        help='write the full result here')
    parser.add_argument('--list', action='store_true',
                        help='print the mutations and stop')
    args = parser.parse_args()

    table = list(MUTATIONS)
    if args.mutations:
        table += load_mutations(args.mutations)

    mutations = table
    if args.layer:
        wanted = set(args.layer)
        mutations = [m for m in mutations if layer_of(m[1]) in wanted]
    if args.only:
        mutations = [m for m in mutations if args.only in m[0]]

    if args.list:
        for name, path, _, _ in mutations:
            print('%-12s %-46s %s' % (layer_of(path), name, path))
        print('\n%d mutations' % len(mutations))
        return 0

    if not mutations:
        print('no mutation matched; nothing to do')
        return 1

    return campaign(args, mutations, len(table))


if __name__ == '__main__':
    sys.exit(main())
