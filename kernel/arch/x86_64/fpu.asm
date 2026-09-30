; Floating point state save and restore.
;
; Three instructions that have no C spelling. Everything else about the
; FPU is in fpu.c.

bits 64

default rel

section .text

; ---------------------------------------------------------------------
; void fpu_save(void *area)
;
; FXSAVE64 rather than FXSAVE: the difference is that the 64-bit form
; writes full-width x87 instruction and data pointers instead of the
; truncated 32-bit ones. In long mode those pointers are 64-bit, so the
; truncated form throws away the top half of an address that the CPU
; actually has.
;
; The instruction checks alignment itself -- a misaligned area raises
; #GP, which is a much better outcome than silently corrupting whatever
; is next to it.
; ---------------------------------------------------------------------
global fpu_save
fpu_save:
    fxsave64 [rdi]
    ret

; ---------------------------------------------------------------------
; void fpu_restore(const void *area)
; ---------------------------------------------------------------------
global fpu_restore
fpu_restore:
    fxrstor64 [rdi]
    ret

; ---------------------------------------------------------------------
; void fpu_init_state(void *area)
;
; Produce the state a program should see before it has run any floating
; point code, by creating it for real rather than by writing the fields in
; C.
;
; The FXSAVE image has a layout with reserved areas, an abridged tag word
; and a control word -- writing all of that by hand means encoding facts
; about the FPU that the FPU already knows. Resetting it and saving the
; result is both shorter and impossible to get subtly wrong.
;
;   FNINIT     x87 reset: control word 0x037F, all exceptions masked,
;              round to nearest.
;   LDMXCSR    SSE control and status: 0x1F80, all exceptions masked,
;              round to nearest, flush-to-zero off.
; ---------------------------------------------------------------------
global fpu_init_state
fpu_init_state:
    fninit
    ldmxcsr [default_mxcsr]
    fxsave64 [rdi]
    ret

section .rodata
align 4
default_mxcsr:
    dd 0x1F80
