; rep_zero -- a REP whose count is zero.
;
; This is the most valuable case in the set, and it is the one a
; per-instruction test cannot reach: a test of MOVSB decides for itself
; whether to enter the loop, and the question here is whether the
; instruction does.
;
; The 8086 tests CX before the first iteration, so a REP with CX = 0
; performs no iteration at all -- not one, and not a wrapped-around
; 65535. The classic wrong implementation is a `do { ... } while (--cx)`,
; which runs the body once with CX = 0, decrements to 0xFFFF and then
; moves 65535 more bytes, and the count works out to exactly 65536 -- a
; whole segment. Which is why the destination is in a different segment
; from the code: a runaway copy here destroys the 64 KiB of ES and cannot
; touch the program, so what the sample reports is the copy that should
; not have happened rather than a machine that stopped making sense.
; Pointed at its own segment instead -- which is what DS = ES = 0 would
; mean -- the same bug overwrites the instructions after the REP and the
; run ends in a corrupted spin. That is a true report and a useless one:
; it says "this did not stop", and sends the reader looking for a loop,
; where the actual mistake is a count that was never tested.
;
; The source is primed with two distinctive bytes for the same reason.
; Guest memory is zero, so a copy of 65536 zeroes into a zero destination
; is invisible: "nothing was copied" and "zeroes were copied" leave the
; same bytes behind. With something to carry, the destination tells them
; apart, and the first bytes of the copy land exactly where the assertion
; is looking.
;
; The flags are the second half, and they are checked by sandwiching the
; REP between two instructions that write them:
;
;   stc          CF = 1 on purpose
;   rep movsb    CX = 0: no move, and no flag may move either
;   sbb ax, ax   AX = 0 - 0 - CF, so it reads the CF the REP had to leave
;
; AX ends at 0xFFFF if the carry survived and 0x0000 if something on the
; way cleared it. That is the answer the sample reports, rather than
; something asserted only in the expected-state table, so a mistake shows
; up as a wrong value rather than as two numbers that differ.
;
; Expected at HLT: AX 0xFFFF  BX 0x0000  CX 0x0000  DX 0x0000
;                  SI 0x0200  DI 0x0300  BP 0x0000  SP 0xFFFE
;                  DS 0x2000  ES 0x3000
; FLAGS: the last instruction is `sbb ax, ax` = 0 - 0 - 1 = 0xFFFF. CF
; is set because the subtraction borrowed; SF because the result is
; negative read as signed; AF because the low nibble borrowed from bit 3
; (0 - 1 does); PF because 0xFF has eight bits set. ZF and OF are clear.
; All six are defined for SBB, so nothing is ignored.
;   FLAGS = CF | AF | SF | PF = 0x0095
; Memory: 0x3000:0x0300 = 00 00 00 00   the destination, untouched
;         0x2000:0x0200 = AB CD ...     the source, also untouched

        bits 16
        org 0x100

        mov     ax, 0x2000
        mov     ds, ax
        mov     ax, 0x3000
        mov     es, ax

        mov     byte [0x0200], 0xab     ; something for a copy to carry
        mov     byte [0x0201], 0xcd

        mov     cx, 0x0000
        mov     si, 0x0200
        mov     di, 0x0300

        stc                             ; CF = 1
        rep     movsb                   ; zero iterations: the whole sample

        sbb     ax, ax                  ; AX = 0 - 0 - CF, reading what the
                                        ; REP left behind
        hlt
