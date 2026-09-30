; ext_shifts -- shifts whose count is an operand of the instruction.
;
; `shl dx, 2` is a 186 encoding: on an 8086 a shift by anything other than
; one has to go through CL. That makes it worth its own sample rather
; than a line in the shift-loop one -- it is a different opcode, decoded
; by a different group, and the count arrives as an immediate rather than
; in CL.
;
; The first shift is written so its carry can be seen. A shift's CF is
; the last bit shifted out, and for `0x4000 << 2` that is the bit that
; leaves the top on the second step, so CF ends set. `adc bx, 0` is the
; way to read it: it adds nothing but the carry, so BX comes out as the
; carry itself, and the sample reports it as a number instead of only
; asserting it in the flag mask.
;
; The second shift is a SAR, so it also covers the direction that has to
; fill from the sign rather than from zero: 0xFFF0 is -16, and four
; places of arithmetic shift leave -1, not 0x0FFF.
;
;   shl dx, 2   0x4000 << 2 = 0x0000, CF = 1   (also ZF, since it is zero)
;   adc bx, 0   BX = 0 + 0 + 1 = 1
;   sar si, 4   0xFFF0 >> 4 = 0xFFFF, CF = 0
;
; Expected at HLT: AX 0x0000  BX 0x0001  CX 0x0000  DX 0x0000
;                  SI 0xFFFF  DI 0x0000  BP 0x0000  SP 0xFFFE
; FLAGS: the flags left at the end are the SAR's, because it is the last
; instruction to write any. It produced 0xFFFF, so ZF is clear and SF is
; set, and its CF is clear -- the bit it shifted out last was a zero.
; (The ADC before it also left CF clear, and the sample is arranged so
; the two agree; the assertion is on CF being clear, not on which
; instruction put it there.) PF is the parity of the low byte of the
; result, 0xFF, whose eight bits make it even, so PF is set. AF and OF
; are undefined: AF after any shift, and OF because the manual defines it
; only for a count of one and this count is four.
;   FLAGS = SF | PF = 0x0084, ignoring AF | OF = 0x0810

        bits 16
        org 0x100

        xor     bx, bx          ; BX = 0 and CF = 0
        mov     dx, 0x4000
        shl     dx, 2           ; the 186 immediate form: C1 /4
        adc     bx, 0           ; BX = the carry the shift produced

        mov     si, 0xFFF0
        sar     si, 4

        hlt
