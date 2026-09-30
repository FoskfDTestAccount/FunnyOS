; rep_movs -- a block copy, and the flags it does not touch.
;
; `rep movsb` is one dispatch that performs five moves, and the only way
; to see the loop is to run one: SI and DI have to end five past where
; they started, CX has to end at zero, and the five bytes have to have
; arrived in the right order at the right place.
;
; The source is a data blob at the very top of the image with a jump over
; it, which is how a .COM program of the era is laid out and which also
; pins its address: the two-byte jump puts it at 0x0102, so SI ends at
; 0x0107 no matter how the code below is edited. A label at the bottom of
; the file would move every time an instruction above it changed length,
; and a sample whose expectations move with it is a sample nobody can
; trust.
;
; The direction is forward -- DF starts clear and the sample never sets
; it -- so both pointers advance. The destination is somewhere else
; entirely, so a REP that performed the moves but did not advance the
; pointers, or one that copied backwards, lands somewhere else.
;
; The flags are the other half. MOVS is not a comparison and sets
; nothing, so every flag this sample ends with is the one the machine
; started with: an assertion in its own right, since an implementation
; that ran the bytes through vm86_alu() to get them somewhere would show
; up here and nowhere in the register values.
;
; Expected at HLT: AX 0x0000  BX 0x0000  CX 0x0000  DX 0x0000
;                  SI 0x0107  DI 0x0305  BP 0x0000  SP 0xFFFE
; FLAGS: nothing in the sample writes flags, so they are the power-on
; value and every writable flag is clear. Nothing is undefined here, so
; nothing is ignored.
;   FLAGS = 0x0000
; Memory: 0:0x0300 = 11 22 33 44 55

        bits 16
        org 0x100

        jmp     start

source:
        db 0x11, 0x22, 0x33, 0x44, 0x55

start:
        mov     si, source
        mov     di, 0x0300
        mov     cx, 0x0005

        rep     movsb

        hlt
