; adc_sbb_chain -- a carry chain that only exists between instructions.
;
; The point of this sample is the one thing a per-opcode test cannot see:
; the carry an `adc` consumes is not set up by the test, it is produced by
; the instruction before it. A handler that computes ADC as "add these two
; and the carry" is right; one that shares a path with ADD and forgets the
; incoming CF is right for every ADD test in the suite and wrong here.
;
; A 48-bit addition, low word first, then the same subtraction back. The
; chains are chosen so the carry out is 1, 1, 0 in the addition and the
; borrow out is 1, 1, 0 in the subtraction, so a link that drops either
; one changes the half above it.
;
;   CX:BX:AX = 0x0000:FFFF:FFFF  (the value)
;   BP:DI:SI = 0x0000:0000:0001  (the addend)
;
;   add ax,si   0xFFFF + 0x0001        = 0x0000, CF = 1
;   adc bx,di   0xFFFF + 0x0000 + CF   = 0x0000, CF = 1
;   adc cx,bp   0x0000 + 0x0000 + CF   = 0x0001, CF = 0
;   sub ax,si   0x0000 - 0x0001        = 0xFFFF, CF = 1 (borrow)
;   sbb bx,di   0x0000 - 0x0000 - CF   = 0xFFFF, CF = 1
;   sbb cx,bp   0x0001 - 0x0000 - CF   = 0x0000, CF = 0
;
; Expected at HLT: AX 0xFFFF  BX 0xFFFF  CX 0x0000  DX 0x0000
;                  SI 0x0001  DI 0x0000  BP 0x0000  SP 0xFFFE
; FLAGS (Intel 8086 Family User's Manual, chapter 3, ADD/ADC/SUB/SBB):
; the last operation is 0x0001 - 0x0000 - 1 = 0, so ZF and PF are set;
; CF is clear because nothing was borrowed; SF and OF are clear; AF is
; clear because the low nibble borrowed nothing. All six are defined, so
; this sample ignores nothing.
;   FLAGS = ZF | PF = 0x0044

        bits 16
        org 0x100

        mov     cx, 0x0000
        mov     bx, 0xFFFF
        mov     ax, 0xFFFF
        mov     bp, 0x0000
        mov     di, 0x0000
        mov     si, 0x0001

        add     ax, si
        adc     bx, di
        adc     cx, bp

        sub     ax, si
        sbb     bx, di
        sbb     cx, bp

        hlt
