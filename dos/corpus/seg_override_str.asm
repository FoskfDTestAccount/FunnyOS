; seg_override_str -- which side of a string instruction a prefix moves.
;
; MOVS reads through DS:SI and writes through ES:DI, and the two sides are
; not symmetric: the read takes a segment override and the write does not.
; There is no prefix on any 8086 that sends a string write anywhere but
; ES:DI, and the way to show that is to arrange for the wrong answer to
; be available. Both segments here hold a different byte at offset 0x0020
; -- DS:0x0020 = 0xDD, a decoy sitting at exactly the destination offset
; in the segment the write must not use -- and the sample checks that the
; decoy is still there afterwards.
;
; Two phases, because the two prefixes say different things:
;
;   phase 1, `ds: rep movsb`   the prefix names the segment the read was
;                              already using. The copy is the plain one:
;                              ES:0x0020 <- DS:0x0010 = 0xCC.
;
;   phase 2, `es: rep movsb`   the prefix moves the READ to ES, so the
;                              source byte is ES:0x0010 = 0xAA. The
;                              destination does not move: ES:0x0020 gets
;                              the 0xAA, not the 0xEE still sitting in
;                              DS:0x0010.
;
; Phase 2 is the one that would catch a symmetric implementation -- one
; that read the prefix and applied it to "the segment" of the string
; instruction rather than to the DS side. Such an implementation writes
; the 0xEE into ES:0x0020, or worse, writes into DS:0x0020 and overwrites
; the decoy.
;
; The invariant the four bytes below check is the one the segment table
; for the string instructions gives, and it is worth stating carefully
; because it is easy to state loosely: the prefix moves the *read*, and
; the *write* never moves. With DS and ES holding different bytes that
; means the byte which gets copied changes when a prefix is added -- and
; the destination does not. There is no prefix on this processor that
; sends a string write anywhere but ES:DI.
;
; Expected at HLT: AX 0x3000  BX 0x0000  CX 0x0000  DX 0x0000
;                  SI 0x0011  DI 0x0021  BP 0x0000  SP 0xFFFE
;                  DS 0x4000  ES 0x3000
; FLAGS: nothing here writes flags, so they are the power-on value.
;   FLAGS = 0x0000
; Memory: 0x4000:0x0010 = ee   0x4000:0x0020 = dd   (the decoy, untouched)
;         0x3000:0x0010 = aa   0x3000:0x0020 = aa   (both phases wrote here)

        bits 16
        org 0x100

        mov     ax, 0x4000
        mov     ds, ax
        mov     ax, 0x3000
        mov     es, ax

; ---- phase 1: the prefix names the side that was already in use -------
        mov     byte [0x0010], 0xcc
        mov     byte [0x0020], 0xdd
        mov     byte es:[0x0020], 0x00

        mov     si, 0x0010
        mov     di, 0x0020
        mov     cx, 0x0001
        ds rep movsb

; ---- phase 2: the prefix moves the read, and only the read ------------
        mov     byte [0x0010], 0xee
        mov     byte es:[0x0010], 0xaa
        mov     byte es:[0x0020], 0x00

        mov     si, 0x0010
        mov     di, 0x0020
        mov     cx, 0x0001
        es rep movsb

        hlt
