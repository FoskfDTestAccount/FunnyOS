; chip_segb -- when a segment register write reaches the cached base.
;
; The 8086 takes the new value immediately: the instruction after
; `mov ds, ax` already addresses through the new segment. What the manual
; says is deferred for the instruction following `mov ss, ...` -- and,
; per the errata, following any `mov segreg` or `pop segreg` -- is
; *interrupt recognition*, which is a different thing, and which nothing
; here can observe because there is no interrupt delivery yet.
;
; The distinction matters to an implementation rather than to a reader.
; Segment bases are cached in seg_base[], and a cache is exactly where a
; stale value survives unnoticed: a version that marked the base dirty
; and refreshed it "soon" rather than on the next access would be right
; for every program that loaded a segment and then did something else
; first, which is nearly all of them. So the sample puts the access
; immediately after the write, with nothing in between, and gives the old
; and new segments different contents so the answer can tell them apart.
;
; Both the segment a plain operand defaults to (DS) and the one that
; cannot be avoided (SS, reached through BP) are checked, because they go
; through different paths in the addressing code:
;
;   DS = 0x2000   [0x0000] -> 0x20000        DS = 0x0000  [0x0000] -> 0
;   SS = 0x3000   [bp]     -> 0x30000        SS = 0x0000  [bp]     -> 0
;
; Each is primed with a byte, then the segment is set back and the byte
; is read. A stale base reads the other one's byte, so every one of the
; three memory words below is asserted.
;
; Expected at HLT: AX 0x3000  BX 0x4422  CX 0x0000  DX 0x0000
;                  SI 0x0000  DI 0x0000  BP 0x0000  SP 0xFFFE
;                  DS 0x2000  ES 0x0000  SS 0x3000
;   (BX is BL = 0x22 from the DS probe and BH = 0x44 from the SS one.)
; FLAGS: nothing here writes flags, so they are the power-on value.
;   FLAGS = 0x0000
; Memory: 0x2000:0x0000 = 22   0x3000:0x0000 = 44   0x0000:0x0000 = 33

        bits 16
        org 0x100

; ---- the data segment, reached by a plain operand --------------------
        mov     ax, 0x2000
        mov     ds, ax
        mov     byte [0x0000], 0x22
        mov     ax, 0x0000
        mov     ds, ax
        mov     byte [0x0000], 0x11

        mov     ax, 0x2000
        mov     ds, ax
        mov     bl, [0x0000]            ; the instruction after the write:
                                        ; must see DS = 0x2000 -> 0x22

; ---- the stack segment, reached through BP ---------------------------
        mov     bp, 0x0000
        mov     ax, 0x3000
        mov     ss, ax
        mov     byte [bp], 0x44
        mov     ax, 0x0000
        mov     ss, ax
        mov     byte [bp], 0x33

        mov     ax, 0x3000
        mov     ss, ax
        mov     bh, [bp]                ; must see SS = 0x3000 -> 0x44

        hlt
