; chip_shift_count -- how many times a shift with a CL count shifts.
;
; The second chip-dependent question. The 8086 uses the whole of CL; the
; 80186 and later mask it, so the same instruction shifts a different
; number of times on the two parts.
;
; Two counts, because one is not enough to tell the two masking rules
; apart. 20 rules out a four-bit mask and not a five-bit one, since
; 20 mod 32 is still 20; 33 rules out the five-bit mask, since 33 mod 32
; is 1. Between them, neither truncation can produce the right answer by
; accident:
;
;   mov cl, 20 ; 0xFFFF shl cl      the 8086 shifts 20 times -> 0x0000
;                                   a 4-bit mask shifts 4      -> 0xFFF0
;   mov cl, 33 ; 0x0001 shl cl      the 8086 shifts 33 times -> 0x0000
;                                   a 5-bit mask shifts 1      -> 0x0002
;
; The second one is there because 20 is not a discriminating number
; against a five-bit mask -- 20 mod 32 is still 20 -- so a single count
; chosen to match the documentation would only rule out one of the two
; truncations. 33 rules out the other.
;
; Both results are in the flags as well as the registers: a full 16-bit
; shift of a non-zero value empties it, so CF is clear and ZF is set, and
; a truncated shift leaves something behind and clears ZF.
;
; Expected at HLT: AX 0x0000  BX 0x0000  CX 0x0021  DX 0x0000
;                  SI 0x0000  DI 0x0000  BP 0x0000  SP 0xFFFE
; FLAGS: the last shift produced 0x0000, so ZF and PF are set and SF is
; clear; CF is the last bit shifted out, and by then the value was
; already zero, so CF is clear. AF is undefined after any shift, and OF
; is undefined for a count other than one, which both of these are.
;   FLAGS = ZF | PF = 0x0044, ignoring AF | OF = 0x0810

        bits 16
        org 0x100

        mov     ax, 0xFFFF
        mov     cl, 20
        shl     ax, cl
        mov     bx, ax                  ; 0x0000 on an 8086, 0xFFF0 on a 186

        mov     dx, 0x0001
        mov     cl, 33
        shl     dx, cl                  ; 0x0000 on an 8086, 0x0002 on a 186

        hlt
