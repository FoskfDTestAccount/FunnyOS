; ext_pusha -- the 186 register set, pushed and popped.
;
; PUSHA is ordinary code by the late 1980s -- compilers targeted the 186
; as their floor -- so it is here twice over: once as a round trip, and
; once for the value it saves as SP, which is the part nobody notices.
;
; Round trip. Eight registers are loaded with recognisable values, PUSHA
; puts them away, every one of them is clobbered, and POPA brings them
; back. The clobbering is what makes this a test: without it a POPA that
; wrote nothing at all would pass.
;
; The SP PUSHA saved is the value it had *before the first push*, unlike
; PUSH SP, which pushes the value after its own decrement. They are
; different instructions from different years and the two rules really
; are different; a PUSHA implemented as eight `push`es of the register
; file gets this one wrong, because by the time it pushes SP the register
; has already moved four times.
;
; So the sample reads the saved SP back out of the stack instead of
; trusting it. The stack after the second PUSHA, from 0xFFEE up:
;
;   0xFFEE  DI 0x6666        0xFFF4  SP 0xFFFE   <- the value under test
;   0xFFF0  SI 0x5555        0xFFF6  BX 0x2222
;   0xFFF2  BP 0x7777        0xFFF8  DX 0x4444
;                            0xFFFA  CX 0x3333
;                            0xFFFC  AX 0x1111
;
; BP is left pointing at the bottom of the block, so the word for SP is
; at BP+6. An implementation that pushed the current SP would leave
; 0xFFF6 there, and 0xFFFE is asserted where 0xFFF6 would be.
;
; Expected at HLT: AX 0xFFFE  BX 0x2222  CX 0x3333  DX 0x4444
;                  SI 0x5555  DI 0x6666  BP 0xFFEE  SP 0xFFEE
; FLAGS: neither PUSHA nor POPA touches them, and neither does MOV, so
; the last instruction to write them is the `xor bp, bp` in the clobber
; block. XOR clears CF and OF and sets ZF and PF from a zero result, and
; leaves AF undefined -- the manual says so for the logical operations,
; and the XOR is what wrote last.
;   FLAGS = ZF | PF = 0x0044, ignoring AF = 0x0010
; Memory: 0:0xFFF4 = FF FE   (the SP PUSHA saved)
;         0:0xFFEE = 66 66   (DI, pushed last, so it is where SP points)

        bits 16
        org 0x100

; ---- a round trip ----------------------------------------------------
        mov     ax, 0x1111
        mov     bx, 0x2222
        mov     cx, 0x3333
        mov     dx, 0x4444
        mov     si, 0x5555
        mov     di, 0x6666
        mov     bp, 0x7777

        pusha

        xor     ax, ax
        xor     bx, bx
        xor     cx, cx
        xor     dx, dx
        xor     si, si
        xor     di, di
        xor     bp, bp

        popa

; ---- what PUSHA saved as SP -----------------------------------------
        mov     sp, 0xFFFE
        pusha

        mov     bp, sp
        mov     ax, [bp+6]      ; the word PUSHA stored for SP

        hlt
