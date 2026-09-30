; ext_enter -- a stack frame with a nesting level that is not zero.
;
; ENTER is easy to implement for level 0, which is the level almost every
; compiler emits, and the shape of the easy version -- push BP, BP := SP,
; SP := SP - n -- does not extend to level 1 or above. So a level-0-only
; ENTER gets written, and the program that needed a level, a program with
; a real nested procedure, gets a corrupt frame instead of an obvious
; error.
;
; Level 2 is the smallest level that has a loop in it, and the loop is
; what does the damage. ENTER's definition, from the 186 manual:
;
;   push BP
;   temp := SP
;   for i := 1 to level-1:
;       BP := BP - 2
;       push word [BP]          ; the caller's saved BP, from the chain
;   push temp                   ; the frame pointer for the new level
;   BP := temp
;   SP := SP - frameSize
;
; With SP = 0xFFFE, BP = 0x1000, frameSize = 4 and level = 2 that is:
;
;   push BP         SP 0xFFFC   [0xFFFC] := 0x1000
;   temp := 0xFFFC
;   i = 1           BP := 0x0FFE, push [0x0FFE]   SP 0xFFFA
;   push temp       SP 0xFFF8   [0xFFF8] := 0xFFFC
;   BP := 0xFFFC    SP := 0xFFF4
;
; and LEAVE is its inverse: SP := BP, then pop BP.
;
; Two things make this checkable rather than merely plausible. The word at
; 0x0FFE is given a distinctive value first, so the frame's one chained
; word has to appear on the stack as 0xABCD -- and it has to be read
; through SS, which is why DS is moved elsewhere before the ENTER runs.
; An implementation that walked the chain through DS would read the zero
; it finds in the other segment. A level-0-only implementation would
; leave [0xFFF8] as zero instead of the frame pointer.
;
; Expected at HLT: AX 0x2000  BX 0x0000  CX 0x0000  DX 0x0000
;                  SI 0x0000  DI 0x0000  BP 0x1000  SP 0xFFFE
;                  DS 0x2000  ES 0x0000  SS 0x0000
; FLAGS: nothing here writes flags, so they are the power-on value.
;   FLAGS = 0x0000
; Memory: 0:0xFFF8 = FF FC   (the frame pointer ENTER pushed)
;         0:0xFFFA = AB CD   (the chained frame, read through SS)
;         0:0xFFFC = 10 00   (the BP the ENTER saved)

        bits 16
        org 0x100

        mov     word [0x0FFE], 0xABCD   ; the enclosing frame's saved BP

        mov     sp, 0xFFFE
        mov     bp, 0x1000

        mov     ax, 0x2000
        mov     ds, ax                  ; ENTER must still walk the frame
                                        ; through SS, not through DS

        enter   0x0004, 0x0002
        leave

        hlt
