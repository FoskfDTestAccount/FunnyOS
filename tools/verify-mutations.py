#!/usr/bin/env python3
"""Mutation campaign for dos/tests/test_verify.c.

Copies the dos/ tree into WSL's own filesystem, applies one deliberate
defect at a time, rebuilds the verify suite and records which cases go
red.  The union of the cases that ever fail is the evidence that no case
is vacuous; whatever is left over is reported so it can be attacked by
hand.
"""
import os
import re
import shutil
import subprocess
import sys

# The repository root: the directory above this script's own.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'dos')

# The build happens outside the repository, and outside /mnt if the tree
# is there: writing object files onto a 9p mount is slow, and a build that
# is interrupted halfway leaves files the other side cannot stat or remove.
REPO = os.environ.get('MUTATION_WORK', '/var/tmp/funyos-mutations/repo')
WORK = os.path.join(REPO, 'dos')
BIN = os.path.join(os.path.dirname(REPO), 'test_verify')

SOURCES = [
    'tests/test_verify.c', 'tests/harness.c',
    'cpu/cpu.c', 'cpu/decode.c', 'cpu/step.c', 'cpu/flags.c',
    'cpu/ops_alu.c', 'cpu/ops_mov.c', 'cpu/ops_str.c', 'cpu/ops_ctl.c',
    'cpu/ops_186.c',
    'mem/mem.c', '../libk/string.c',
]

MUTATIONS = [
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
    ('push es becomes push ss', 'cpu/ops_alu.c',
     '        if (form == 6)\n'
     '            vm86_push16(cpu, vm86_get_seg(cpu, (enum vm86_seg)segment));',
     '        if (form == 6)\n'
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
     '    if (false)'),

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
    ('the interrupt frame goes on in the wrong order', 'cpu/ops_ctl.c',
     '    vm86_push16(cpu, cpu->flags);\n'
     '    vm86_push16(cpu, cpu->cs);\n'
     '    vm86_push16(cpu, cpu->ip);',
     '    vm86_push16(cpu, cpu->flags);\n'
     '    vm86_push16(cpu, cpu->ip);\n'
     '    vm86_push16(cpu, cpu->cs);'),
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
    ('the vector table is based on cs', 'cpu/ops_ctl.c',
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


def reset():
    if os.path.exists(REPO):
        shutil.rmtree(REPO)
    os.makedirs(REPO)
    shutil.copytree(SRC, WORK)
    shutil.copytree(ROOT + '/libk', REPO + '/libk')


def build():
    cmd = ['gcc', '-std=c17', '-g', '-O2', '-Wall', '-Wextra', '-Werror',
           '-fno-builtin', '-Iinclude', '-I../libk/include', '-o', BIN] + SOURCES
    return subprocess.run(cmd, cwd=WORK, capture_output=True, text=True)


CASE = re.compile(r'^  --  (.*) \(\d+ failed\)$', re.M)


def run():
    """Run the suite, with a bound on the wall clock.

    A mutation that removes a loop's exit condition makes the emulator
    spin inside a single instruction and the step limit in the harness
    cannot help, because the step never returns.  That is a caught
    mutation like any other -- the suite does not pass -- but it has to
    be caught by a timeout rather than by a red case.
    """
    try:
        proc = subprocess.run([BIN], capture_output=True, text=True,
                              timeout=20)
    except subprocess.TimeoutExpired:
        return None, 'TIMEOUT'

    return set(CASE.findall(proc.stdout)), proc.stdout


def main():
    reset()
    result = build()
    if result.returncode != 0:
        print('the pristine tree does not build:')
        print(result.stderr[-4000:])
        return 1

    failing, out = run()
    if failing is None:
        print('the pristine tree does not finish')
        return 1
    if failing:
        print('the pristine tree is not green: %s' % sorted(failing))
        print(out[-4000:])
        return 1

    all_cases = set(re.findall(r'\{ "([^"]+)",', open(
        os.path.join(WORK, 'tests/test_verify.c'), encoding='utf-8').read()))
    print('pristine: %d cases, all green\n' % len(all_cases))

    covered = {}
    problems = []
    for name, path, old, new in MUTATIONS:
        full = os.path.join(WORK, path)
        original = open(full, encoding='utf-8').read()
        if original.count(old) != 1:
            problems.append('%s: pattern appears %d times in %s'
                            % (name, original.count(old), path))
            continue
        open(full, 'w', encoding='utf-8', newline='\n').write(
            original.replace(old, new))
        result = build()
        if result.returncode != 0:
            problems.append('%s: did not compile' % name)
            print('%-46s did not compile' % name, flush=True)
        else:
            failing, _ = run()
            if failing is None:
                problems.append('%s: the suite hung' % name)
                print('%-46s the suite hung' % name, flush=True)
            else:
                for case in failing:
                    covered.setdefault(case, []).append(name)
                print('%-46s %2d case(s) red' % (name, len(failing)),
                      flush=True)
        open(full, 'w', encoding='utf-8', newline='\n').write(original)

    missed = sorted(all_cases - set(covered))
    print('\n%d of %d cases were made to fail by at least one mutation'
          % (len(covered), len(all_cases)))
    if missed:
        print('\nnever failed by any mutation:')
        for case in missed:
            print('  - %s' % case)
    if problems:
        print('\nmutations that did not apply or did not compile:')
        for line in problems:
            print('  - %s' % line)

    return 0


sys.exit(main())
