bits 64

section .text

; ---------------------------------------------------------------------
; long u_syscall(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2)
;
; The whole system call mechanism, in five instructions.
;
; The incoming registers are the ones the C calling convention already
; uses for the first four arguments, and the kernel's ABI is those same
; registers with the number moved into rax. So a call is a rename and an
; INT: no stack frame to build, nothing to unwind, and no marshalling
; table to keep in step with anything.
;
; One entry point rather than one per arity. A call that takes fewer
; arguments passes zeroes, which costs a register move and saves having
; to add a wrapper the next time a call grows a parameter.
;
; INT 0x80 is a trap into the kernel. The kernel switches to its own
; stack, handles the call, and returns with iretq.
; ---------------------------------------------------------------------
global u_syscall
u_syscall:
    mov rax, rdi            ; call number
    mov rdi, rsi            ; argument 0
    mov rsi, rdx            ; argument 1
    mov rdx, rcx            ; argument 2
    int 0x80
    ret                     ; result is already in rax
