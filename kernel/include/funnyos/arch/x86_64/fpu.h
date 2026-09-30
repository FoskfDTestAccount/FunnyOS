/*
 * Floating point and SSE.
 *
 * ---------------------------------------------------------------------
 * The invariant this file depends on
 *
 * The kernel never touches a vector register. Not XMM, not YMM, not the
 * x87 stack. That is not a style preference, it is the reason interrupt
 * handling is as cheap as it is:
 *
 *   An interrupt does not save floating point state. The CPU pushes
 *   general purpose registers and the return frame, and nothing else.
 *   Save and restore of XMM and the x87 stack is entirely the operating
 *   system's job, precisely so that an OS which never uses them can skip
 *   it.
 *
 * So because the kernel is compiled with -mgeneral-regs-only, an interrupt
 * taken while a program is in the middle of a floating point calculation
 * leaves that calculation's registers exactly where they were. Handler
 * runs, returns, program continues, nothing was lost. That is what makes
 * the whole arrangement work without an FXSAVE on every timer tick.
 *
 * The moment kernel code uses a vector register, that stops being true and
 * every interrupt starts silently corrupting whatever the interrupted
 * program had loaded. The compiler flag is the only thing standing between
 * this design and that failure, which is why it is in CFLAGS with a
 * comment and not left to be rediscovered.
 *
 * ---------------------------------------------------------------------
 * What this file does instead
 *
 * Enables the hardware (SSE and x87 are otherwise simply unavailable, or
 * worse, available in a state the OS has said it cannot preserve), gives
 * each process a place to keep its state, and saves and restores it when
 * control changes hands between a process and the kernel.
 *
 * With one process at a time -- which is what M2 has -- the save and
 * restore are in effect a no-op: the same state goes back where it came
 * from. They are here now because the alternative is a scheduler that
 * silently interleaves two programs' registers, and because that bug is
 * invisible until it is not.
 */
#ifndef FUNNYOS_ARCH_X86_64_FPU_H
#define FUNNYOS_ARCH_X86_64_FPU_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Size and alignment of the FXSAVE image.
 *
 * 512 bytes is the architectural size, and covers the x87 stack and
 * control words, XMM0-15 and MXCSR. It does NOT cover YMM0-15, which is
 * why this kernel deliberately does not enable the AVX extensions -- see
 * fpu_init.
 */
#define FPU_STATE_SIZE  512
#define FPU_STATE_ALIGN 16

/*
 * Enable the x87 FPU and SSE on this processor.
 *
 * Returns false if the resulting control register state is not what was
 * asked for, which is the sort of thing that only happens on a CPU or a
 * hypervisor that is not playing straight, and is better reported than
 * assumed away.
 */
bool fpu_init(void);

bool fpu_ready(void);

/* Whether the processor advertises AVX, for the boot report only. The
 * kernel does not enable it. */
bool fpu_avx_present(void);

/* Fill an area with the architectural default state: x87 reset, all
 * exceptions masked, round to nearest. This is what a program sees before
 * it has executed its first floating point instruction. */
void fpu_init_state(void *area);

/* Save the live state into an area, and load an area into the live state.
 *
 * Both require the area to be 16-byte aligned, and both are exactly one
 * instruction. They are in assembly only because there is no way to spell
 * FXSAVE that is worth writing in C.
 */
void fpu_save(void *area);
void fpu_restore(const void *area);

#endif /* FUNNYOS_ARCH_X86_64_FPU_H */
