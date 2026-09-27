; Interrupt and exception entry stubs.
;
; Every one of the 256 vectors gets its own tiny stub. That is deliberate:
; a shared catch-all cannot report which vector fired, and "an interrupt
; happened" is a useless diagnostic. Each stub normalises the stack into
; the same shape and jumps to a single handler.
;
; Stack layout on entry to isr_common (addresses increasing downwards):
;
;     [r15..rax]            pushed here, forming struct interrupt_frame
;     [vector]              pushed by the stub
;     [error code]          pushed by the CPU, or a dummy 0 from the stub
;     [rip]                 pushed by the CPU
;     [cs]
;     [rflags]
;     [rsp]                 only when the fault came from Ring 3
;     [ss]                  likewise
;
; The stub always pushes a vector and always leaves exactly one error code
; on the stack, so the handler sees one uniform frame whether or not the
; CPU supplied an error code of its own.

bits 64

section .text

extern isr_dispatch
global isr_common

; ---------------------------------------------------------------------
; Vector stubs
;
; The CPU pushes an error code for a fixed set of vectors and for no
; others. For the rest the stub pushes a zero, so the frame layout is
; identical in both cases.
; ---------------------------------------------------------------------

%assign vec 0
%rep 256

global isr%[vec]
isr%[vec]:
%if (vec == 8) || (vec >= 10 && vec <= 14) || (vec == 17) || (vec == 21) || (vec == 29) || (vec == 30)
    ; The CPU has already pushed an error code for this vector.
    push qword vec
%else
    push qword 0
    push qword vec
%endif
    jmp isr_common

%assign vec vec + 1
%endrep

; ---------------------------------------------------------------------
; Common trampoline
; ---------------------------------------------------------------------

isr_common:
    ; Save the general purpose registers in a fixed order so the C side
    ; can describe them with a plain struct.
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; The SysV ABI requires DF clear on entry to a C function, and an
    ; interrupt can arrive with it set.
    cld

    mov rdi, rsp            ; first argument: pointer to the frame
    call isr_dispatch

    ; Restore. Only reached when the handler returns; the M1 handlers all
    ; panic, but interrupt handlers in M2 will take this path.
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16             ; discard vector and error code
    iretq

; ---------------------------------------------------------------------
; idt_load(const struct idtr *)
; ---------------------------------------------------------------------

global idt_load
idt_load:
    lidt [rdi]
    ret

; ---------------------------------------------------------------------
; Vector address table, so the C side can install handlers in a loop
; instead of naming 256 symbols.
; ---------------------------------------------------------------------

section .rodata
global isr_stub_table
isr_stub_table:
%assign tvec 0
%rep 256
    dq isr%[tvec]
%assign tvec tvec + 1
%endrep
