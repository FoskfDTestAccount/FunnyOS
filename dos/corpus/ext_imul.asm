; ext_imul -- the three-operand multiply, and what CF and OF mean.
;
; The 186 form `imul r16, r/m16, imm16` computes the same product as the
; 8086 one-operand form but keeps only the low half, and the manual is
; explicit that CF and OF are not about that: they say whether the
; product would have fitted in a signed 16-bit result, which is the same
; question the one-operand form answers about DX:AX. The trap is the
; phrase "the high half is not zero", which is how it reads and is wrong
; for every negative product -- -2 x 3 is 0xFFFF:FFFA, whose high half is
; 0xFFFF and which nevertheless fits.
;
; The instruction has two encodings, `69` with a sixteen-bit immediate
; and `6B` with a sign-extended byte, and the sample uses both: the first
; multiply's immediate does not fit in a signed byte, so the assembler
; has to reach for 69, and the other two fit and are emitted as 6B.
;
; Three answers are checked, and each is collected into a register rather
; than left to the flags, because only the last of them is still in the
; flags when the machine stops:
;
;   0x0100 x 0x0100 = 0x10000   does not fit  -> CF = OF = 1  (checked by JO)
;   0x0002 x 3      = 0x0006    fits          -> CF = OF = 0  (still in FLAGS)
;   3 x -2          = 0xFFFA    fits, negative-> CF = OF = 0  (still in FLAGS)
;
; The third one is the reason the sample exists. Its product is negative,
; so an implementation that decided by looking at the sign of the result
; -- or at its high bits -- gets CF = OF = 1 here, and the two positive
; cases above would not have caught it.
;
; The last multiply is also what leaves the flags the sample ends with,
; and it is the one place where a defined-flag check is unusually
; valuable: CF and OF are asserted clear even though the product's low
; half has its top bit set.
;
; Expected at HLT: AX 0x0002  BX 0x0006  CX 0x0100  DX 0x0000
;                  SI 0x0001  DI 0x0000  BP 0xFFFA  SP 0xFFFE
; FLAGS: IMUL defines CF and OF and leaves everything else undefined --
; the manual says so, and it is why the mask below is so wide on a sample
; that otherwise has nothing undefined in it. Both defined flags are
; clear, so the expected value is zero and the ignored bits are ZF, SF,
; AF and PF.
;   FLAGS = 0x0000, ignoring ZF | SF | AF | PF = 0x00D4

        bits 16
        org 0x100

; ---- a product that does not fit ------------------------------------
        mov     cx, 0x0100
        imul    dx, cx, 0x0100          ; 0x10000 -> low half 0x0000
        jo      .overflowed
        mov     si, 0x0002              ; OF was clear: wrong
        jmp     .fits

.overflowed:
        mov     si, 0x0001

; ---- a product that fits --------------------------------------------
.fits:
        mov     ax, 0x0002
        imul    bx, ax, 0x0003          ; 6

; ---- a product that fits and is negative ----------------------------
        mov     bp, 0x0003
        imul    bp, bp, byte -2         ; -6 = 0xFFFA

        hlt
