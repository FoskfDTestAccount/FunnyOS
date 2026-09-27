; FunnyOS x86-64 kernel entry point
;
; State established by Limine before jumping here (see limine-protocol.md,
; "Machine State at Entry"):
;   - CPU is already in 64-bit long mode with paging enabled
;     (PG/PAE/LME/LMA are all set)
;   - CS = 0x28 (64-bit code segment), DS/ES/SS/FS/GS = 0x30 (64-bit data)
;   - IF is cleared, DF is cleared
;   - Kernel is mapped in the higher half, at or above 0xffffffff80000000
;   - A20 is open, legacy PICs are fully masked
;
; What Limine does NOT do, and the kernel must therefore handle itself:
;   - It does not set up a stack (rsp is undefined)   <-- first job here
;   - It does not load an IDT (undefined below base revision 5;
;     base revision 5+ guarantees base 0 / limit 0, i.e. still unusable)
;   - It does not install a TSS beyond the GDT it provides

bits 64

section .text
global _start
extern kmain

_start:
    ; Establish our own stack immediately. Until this is done we cannot
    ; safely call any function or take any interrupt.
    mov rsp, stack_top

    ; Align the stack frame and clear rbp so stack unwinding later on
    ; starts from a well-defined terminator.
    and rsp, ~0xF
    xor rbp, rbp

    ; Enter C.
    call kmain

    ; kmain is not expected to return. If it does, park here.
.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
stack_bottom:
    resb 65536                  ; 64 KiB kernel stack.
                                ; Enough for M0; will be revisited once
                                ; interrupts are enabled in M1.
stack_top:
