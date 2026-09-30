; shift_loop -- a shift loop, and a shift whose count is zero.
;
; Two things live here because they are the same instruction seen from two
; sides.
;
; 1. A real loop. SI is shifted left one place per pass until it is empty,
;    and the exit test is the JNZ immediately after the SHL, so the loop
;    ends on the pass that shifts the last set bit out. 0x0021 has its two
;    set bits at positions 0 and 5, so the value is empty after 16 shifts
;    and BP -- counted before the shift, where it cannot disturb the flags
;    the JNZ reads -- ends at 0x0010.
;
; 2. The count of zero. ZF is set deliberately, and then AX, which is NOT
;    zero, is shifted by a count of zero. The Intel manual's rule for the
;    shift group is that a count of zero affects no flag at all -- the
;    operation did not happen, so nothing about it happened either. The
;    JNZ straight afterwards therefore reads the ZF from the XOR before
;    it, and must not be taken.
;
;    An implementation that recomputes the flags from the unshifted
;    operand gets ZF = 0 here (AX is 0x1234), takes the jump, and reports
;    0x0002 in DX. This is the case the manual calls out and the one that
;    is invisible to a test that runs a shift once, by itself, with the
;    flags it chose.
;
; Expected at HLT: AX 0x1234  BX 0x0000  CX 0x0000  DX 0x0001
;                  SI 0x0000  DI 0x0000  BP 0x0010  SP 0xFFFE
; FLAGS: the last instruction to touch them is `xor bx, bx`, and the
; shift of zero and the JNZ after it must leave them alone. XOR clears CF
; and OF, sets ZF and PF from the result (0x0000 -> both set), and leaves
; AF undefined -- the manual says so for the logical operations, and the
; XOR is the last one, so AF is what has to be ignored.
;   FLAGS = ZF | PF = 0x0044, ignoring AF = 0x0010

        bits 16
        org 0x100

; ---- 1. a real SHL/JNZ loop -----------------------------------------
        mov     si, 0x0021
        mov     bp, 0x0000

.spin:
        inc     bp              ; count the pass first: the SHL below is
                                ; what the JNZ after it must read
        shl     si, 1
        jnz     .spin

; ---- 2. a count of zero changes nothing -----------------------------
        mov     ax, 0x1234
        xor     bx, bx          ; BX = 0 and ZF = 1
        mov     cl, 0x00
        shl     ax, cl          ; count 0: AX unchanged and so is every flag
        jnz     .recomputed     ; must not be taken
        mov     dx, 0x0001
        jmp     .done

.recomputed:
        mov     dx, 0x0002

.done:
        hlt
