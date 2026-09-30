# The corpus

Guest programs, assembled from source, that the interpreter is graded on
after they have run.

Every other suite in `dos/tests` measures one instruction: give the
interpreter a few bytes and some registers and compare the result. That
misses everything that lives *between* instructions — a carry an `adc`
inherits from the instruction before it, a `rep` whose count is zero, a
segment prefix that belongs to the instruction after it, a loop whose
exit test reads a flag that a shift by zero did not write. Those only
appear when a program runs, and M3's acceptance sentence is about running
a *binary*, so a program is what is here.

## How it works

`dos/Makefile` assembles each `.asm` with `nasm -f bin` and turns it into
a C array with `tools/bin2c.py`, the same way the kernel embeds its init
program. `dos/tests/test_corpus.c` holds the expected final state of each
one, and `corpus/replay.c` — reachable from the test as
`corpus/replay.h` — loads an image into guest memory, runs it, and
compares. Adding a sample means adding the `.asm` and one case to the
table; the wildcard in the Makefile picks it up.

The replayer is a function rather than a `main`, and it writes through a
callback rather than to `printf`, because the same code has to run inside
a guest later, where there is no stdio and no filesystem. That is also
why the samples are embedded rather than read from disk.

## The convention a sample is entered with

Frozen, and described in full at the top of `replay.h`: a flat binary
loaded at linear `0x100`, entered at `0x0000:0x0100` with every segment
zero, `SP = 0xFFFE`, and `FLAGS = VM86_FLAG_ALWAYS_SET`. It is the `.COM`
layout minus the PSP, and it is chosen so that adding the PSP in M5 moves
nothing.

Every sample must end with the `HLT` it stops on, as the last byte. The
replayer takes that as the terminal state, and the instruction limit is
the backstop for a sample that never gets there — reported as its own
outcome, never as a state mismatch.

## What is here

| Sample | What only a program can show |
|---|---|
| `adc_sbb_chain` | a carry that exists only between instructions |
| `shift_loop` | a shift loop, and the count of zero that must change nothing |
| `rep_movs` | a REP loop, and the flags it does not touch |
| `repne_scasb` | stopping on a match, and the CX an early exit leaves behind |
| `rep_zero` | a REP with `CX = 0`: zero iterations, and no flags moved |
| `seg_override` | the same offset through two different segments |
| `seg_override_str` | which side of a string instruction a prefix moves |
| `ext_pusha` | the 186 register set, and the SP that PUSHA saves |
| `ext_enter` | a stack frame with a non-zero nesting level |
| `ext_imul` | the three-operand multiply, including a negative product |
| `ext_shifts` | shifts whose count is an immediate operand |
| `chip_pushsp` | `PUSH SP`: the 8086 pushes the value after the decrement |
| `chip_lea` | `LEA` with a segment override: the offset, not the offset plus base |
| `chip_segb` | a segment write reaching the cached base on the next instruction |
| `chip_shift_count` | a `CL` count the 8086 does not truncate |
| `chip_movcs` | `MOV CS`: the one sample that deviates from the 8086 rather than extending it |
| `illegal_ff7` | an encoding in the FF group that names no operation |

Each sample's header comment carries the arithmetic, the flags, and which
flag — if any — the instruction set leaves undefined and the expectation
therefore ignores. The mask is a consequence of the last instruction that
wrote flags, so it differs from sample to sample and cannot be copied
between them, and too wide a mask is the dangerous direction.
