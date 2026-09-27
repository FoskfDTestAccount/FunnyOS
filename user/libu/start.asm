bits 64

section .text.entry

extern u_main
extern u_exit

; ---------------------------------------------------------------------
; Program entry point.
;
; The kernel reaches this through an iretq, which is not how a function
; is normally called: there is no return address on the stack and no
; caller to return to, so the stack is not in the state the ABI assumes
; on entry to a function.
;
; That is the entire reason this stub is written in assembly rather than
; being a C function. Re-aligning the stack and then making a real call is
; what turns an iretq into something the compiler's assumptions hold for.
;
; The initial argument arrives in rdi, which is where the calling
; convention expects a first parameter to be -- so it passes straight
; through to u_main without being touched.
; ---------------------------------------------------------------------

global _start
_start:
    ; Mark the outermost stack frame, so anything that walks the frame
    ; chain stops here instead of following garbage off the top.
    xor ebp, ebp

    ; iretq leaves rsp wherever the kernel put it. The ABI wants it
    ; 16-byte aligned at the point of a call, which is what this makes it.
    and rsp, -16

    call u_main

    mov edi, eax            ; the program's return value is its exit code
    call u_exit

    ; u_exit does not return. If it somehow did, stopping beats running
    ; off the end of the image.
    hlt
    jmp $
