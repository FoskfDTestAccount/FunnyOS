; repne_scasb -- a scan that stops on the first match.
;
; REPNE is where a single-instruction test tells you the least. The loop
; has two ways to end -- CX runs out, or ZF comes up -- and only the
; second one is interesting, because it is the one a program uses. It
; also leaves a number behind that the program reads: CX after an early
; exit still counts the elements the loop never looked at, because the
; decrement happens once per iteration and the last iteration counts.
;
; The kernel is 0x33, the third of five bytes. So the loop stops on the
; third comparison:
;
;   compare 0x33 with [DI]=0x11   not equal, ZF=0, CX 5 -> 4, continue
;   compare 0x33 with [DI]=0x22   not equal, ZF=0, CX 4 -> 3, continue
;   compare 0x33 with [DI]=0x33   equal,     ZF=1, CX 3 -> 2, stop
;
; DI ends one past the byte that matched, which is what a program does
; with it: subtract to get the index. CX ends at 2 -- three iterations
; were run and two elements were never examined -- and that 2 is the
; assertion. A loop that decremented only on the way round, or that
; tested CX before the last decrement, reports 3 or 1.
;
; The haystack sits at the top of the image behind a two-byte jump, which
; pins it at 0x0102 rather than letting it move whenever an instruction
; above it changes length.
;
; Expected at HLT: AX 0x0033  BX 0x0000  CX 0x0002  DX 0x0000
;                  SI 0x0000  DI 0x0105  BP 0x0000  SP 0xFFFE
; FLAGS: the last comparison is 0x33 - 0x33 = 0, which sets ZF and PF
; and clears CF, SF and OF; AF is the borrow out of the low nibble, and
; there is none. SCAS sets all six the way CMP does, so nothing is
; ignored.
;   FLAGS = ZF | PF = 0x0044

        bits 16
        org 0x100

        jmp     start

haystack:
        db 0x11, 0x22, 0x33, 0x44, 0x55

start:
        mov     di, haystack
        mov     al, 0x33
        mov     cx, 0x0005

        repne   scasb

        hlt
