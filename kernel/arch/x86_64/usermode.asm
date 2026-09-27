; Entering and leaving Ring 3.
;
; Two things live here that C cannot express: the iretq that drops the
; privilege level, and the context save used to come back out afterwards.

bits 64

section .text

; GDT_USER_CODE (0x18) and GDT_USER_DATA (0x20) with the requested
; privilege level in the low two bits.
%define SEL_USER_CODE 0x1B
%define SEL_USER_DATA 0x23

; ---------------------------------------------------------------------
; void usermode_enter(uint64_t entry, uint64_t stack_top, uint64_t arg)
;
; Does not return to the caller. Control leaves through the iretq below
; and reappears in Ring 3 at `entry`, with `arg` in rdi -- which is where
; the C calling convention expects a first parameter, so the program's
; entry point receives it as an ordinary argument.
; ---------------------------------------------------------------------
global usermode_enter
usermode_enter:
    cli

    ; Load the data segment registers. iretq sets cs and ss itself, but
    ; ds/es/fs/gs have to be loaded by hand, and leaving them on the
    ; kernel's selectors leaves Ring 3 code holding a descriptor it has no
    ; business using.
    mov ax, SEL_USER_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Arguments arrive as rdi = entry, rsi = stack_top, rdx = arg.
    ;
    ; The program's argument has to end up in rdi, so the entry address is
    ; moved out of the way first. iretq restores no general purpose
    ; registers: whatever is in them when it executes is what the program
    ; starts with, which is what makes this work at all.
    mov rcx, rdi            ; entry, parked out of the way
    mov rdi, rdx            ; arg becomes the program's first parameter

    ; iretq pops rip, cs, rflags, rsp, ss -- in that order -- so they go on
    ; the stack in the opposite order. Supplying ss and rsp at all is what
    ; tells the CPU this is a privilege change rather than a same-level
    ; return.
    push qword SEL_USER_DATA    ; ss
    push rsi                    ; rsp
    push qword 0x202            ; rflags: IF set, plus bit 1, which is
                                ; always 1 and reads back as 1
    push qword SEL_USER_CODE    ; cs
    push rcx                    ; rip = entry

    iretq

; ---------------------------------------------------------------------
; struct kernel_context {
;     uint64_t rbx, rbp, r12, r13, r14, r15;   /* 0x00 .. 0x28 */
;     uint64_t rsp;                            /* 0x30 */
;     uint64_t rip;                            /* 0x38 */
; };
;
; A hand-rolled setjmp/longjmp pair, used for exactly one thing: coming
; back to the kernel after a process exits.
;
; The syscall that ends a process runs on the kernel stack the CPU
; switched to on the way in, which is not the stack the kernel was using
; when it entered Ring 3. Returning normally would mean iretq-ing back
; into a dead address space. Longjmp is how the kernel abandons that
; context and resumes where it left off.
;
; Only the callee-saved registers are preserved, which is all the C
; calling convention requires across a longjmp.
; ---------------------------------------------------------------------

; int kernel_setjmp(struct kernel_context *ctx)
global kernel_setjmp
kernel_setjmp:
    mov [rdi + 0x00], rbx
    mov [rdi + 0x08], rbp
    mov [rdi + 0x10], r12
    mov [rdi + 0x18], r13
    mov [rdi + 0x20], r14
    mov [rdi + 0x28], r15

    ; The stack pointer as it will be once this call has returned.
    lea rax, [rsp + 8]
    mov [rdi + 0x30], rax

    ; And the address it will return to.
    mov rax, [rsp]
    mov [rdi + 0x38], rax

    xor eax, eax
    ret

; void kernel_longjmp(struct kernel_context *ctx, int value)
global kernel_longjmp
kernel_longjmp:
    mov rbx, [rdi + 0x00]
    mov rbp, [rdi + 0x08]
    mov r12, [rdi + 0x10]
    mov r13, [rdi + 0x18]
    mov r14, [rdi + 0x20]
    mov r15, [rdi + 0x28]

    mov rdx, [rdi + 0x38]       ; return address
    mov rsp, [rdi + 0x30]       ; stack as it was after setjmp returned

    ; Plant the return address just below that point and let ret pop it,
    ; which leaves rsp exactly where setjmp recorded it.
    sub rsp, 8
    mov [rsp], rdx

    mov eax, esi                ; setjmp returns this the second time
    ret
