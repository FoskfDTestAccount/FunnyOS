; seg_override -- the same offset read through two different segments.
;
; A segment override is a property of the operand, not of the address, so
; the only way to see one is to make the same offset mean two different
; things. Here DS and ES point at different paragraphs and both are asked
; for offset 0x0010:
;
;   mov word [0x0010], 0x1111      writes DS:0x0010 = 0x20010
;   mov word es:[0x0010], 0x2222   writes ES:0x0010 = 0x30010
;   mov bx, [0x0010]               reads DS:0x0010 -> 0x1111
;   mov dx, es:[0x0010]            reads ES:0x0010 -> 0x2222
;
; An implementation that resolves the prefix into the address but forgets
; to stop using the default segment, or one that applies an override to
; the wrong side, gives BX = DX or writes both values to the same place.
; Both segments are non-zero, which is the whole point: with DS = ES = 0
; -- which is where every per-instruction test starts -- the override
; changes nothing and an implementation that ignores it passes.
;
; Expected at HLT: AX 0x3000  BX 0x1111  CX 0x0000  DX 0x2222
;                  SI 0x0000  DI 0x0000  BP 0x0000  SP 0xFFFE
;                  DS 0x2000  ES 0x3000
; FLAGS: nothing here writes flags, so they are the power-on value.
;   FLAGS = 0x0000
; Memory: 0x2000:0x0010 = 11 11 and 0x3000:0x0010 = 22 22

        bits 16
        org 0x100

        mov     ax, 0x2000
        mov     ds, ax
        mov     ax, 0x3000
        mov     es, ax

        mov     word [0x0010], 0x1111
        mov     word es:[0x0010], 0x2222

        mov     bx, [0x0010]
        mov     dx, es:[0x0010]

        hlt
