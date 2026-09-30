; chip_pushsp -- the value PUSH SP puts on the stack.
;
; This is the first of the questions whose answer depends on which chip is
; being emulated rather than on which answer reads better. The 8086
; pushes the SP it has *after* its own decrement; the 80186 and later push
; the value before it. Programs of the era run this instruction precisely
; to find out which part they are on, so getting it backwards is not a
; curiosity -- it is a machine that answers the question wrong.
;
; SP is moved somewhere small and unmistakable first, and the pushed word
; is read straight back. Without that it would be easy to write a check
; that passes for the wrong reason: at the entry SP of 0xFFFE the two
; candidate values are 0xFFFE and 0xFFFC, and 0xFFFE is also what the
; machine was booted with, so a sample that compared against a register
; holding the old SP would agree with either implementation.
;
;   mov sp, 0x8000
;   push sp        the 8086 stores 0x7FFE and leaves SP = 0x7FFE
;   pop bx         BX = 0x7FFE, SP = 0x8000
;
; An implementation that pushed the value in the register before
; decrementing it leaves 0x8000 on the stack, and BX comes back 0x8000.
;
; Expected at HLT: AX 0x0000  BX 0x7FFE  CX 0x0000  DX 0x0000
;                  SI 0x0000  DI 0x0000  BP 0x0000  SP 0x8000
; FLAGS: nothing here writes flags, so they are the power-on value.
;   FLAGS = 0x0000
; Memory: 0:0x7FFE = 7F FE

        bits 16
        org 0x100

        mov     sp, 0x8000
        push    sp
        pop     bx

        hlt
