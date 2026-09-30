; chip_lea -- what a segment override does to LEA.
;
; LEA is the odd one out among the instructions that take a memory
; operand: it computes the address and does not go there. So the segment
; the operand names has nothing to contribute -- the base is not part of
; an offset, and the offset is what LEA writes. `lea bx, es:[bp]` and
; `lea bx, [bp]` put the same number in BX.
;
; The mistake this guards against is folding the base in, which produces
; answers that look like addresses and are wrong by a multiple of sixteen.
; It is invisible in any test whose segments are all zero -- which is
; where every per-instruction test starts -- so the segment here is
; deliberately a big, odd number, and the offset deliberately small:
;
;   ES = 0x1234, so ES's base is 0x12340
;   lea bx, es:[bp]         BP = 0x0010 -> BX = 0x0010, not 0x12350
;   lea dx, [si+3]          SI = 0x0020 -> DX = 0x0023, and [si+3] is a
;                           displacement, so the offset has to be summed
;                           rather than copied
;   lea di, [bx+si]         -> 0x0010 + 0x0020 = 0x0030, so the base and
;                           index registers add
;
; LEA reads nothing, so there is no memory to check; the evidence is
; entirely in the three registers, and all three ways of getting LEA
; wrong land somewhere else: folding the base in gives 0x12350, reading
; the operand instead of computing it finds the zero that lives at
; 0x12350 and gives 0x0000, and dropping the displacement gives 0x0020.
;
; Expected at HLT: AX 0x1234  BX 0x0010  CX 0x0000  DX 0x0023
;                  SI 0x0020  DI 0x0030  BP 0x0010  SP 0xFFFE
;                  ES 0x1234
; FLAGS: nothing here writes flags, so they are the power-on value.
;   FLAGS = 0x0000

        bits 16
        org 0x100

        mov     ax, 0x1234
        mov     es, ax

        mov     bp, 0x0010
        mov     si, 0x0020

        lea     bx, es:[bp]
        lea     dx, [si+3]
        lea     di, [bx+si]

        hlt
