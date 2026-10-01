; hello -- the acceptance program: one line of text through INT 10h.
;
; This is the program M4's acceptance sentence is about. It takes the whole
; main path in one run: INT 10h, the vector table, the FE 38 stub, the host
; trap, bios10_service, the character bytes at 0xB8000, and the host
; reading that memory back to draw the screen.
;
; It uses AH=0Eh, teletype output, which is the one video call every DOS
; program of the era makes and the one the acceptance path is written
; around.
;
; IT SETS THE MODE FIRST, and that is not decoration. 0Eh writes the
; character into the cell and keeps whatever attribute byte was already
; there (docs/dos-refs.md section 1), and a machine whose memory has just
; been cleared has 0x00 there -- black on black. Setting a mode is what
; clears the screen, which is what a real POST does and what a real
; program does for itself; a program that skips it prints characters
; nobody can see while every assertion about memory still passes. So this
; sample sets mode 3 and then prints, and the two facts worth having are
; in the order they matter: the mode clear gives the page its attribute,
; and 0Eh leaves that attribute alone.
;
; Two things about the call itself, both from docs/dos-refs.md section 1:
;
;   * AH=0Eh takes AL and BH and nothing else. It does NOT take an
;     attribute: in text mode it writes the character into the cell and
;     leaves that cell's attribute byte alone, and BL is a foreground
;     colour for the graphics modes this machine does not have. (The
;     erratum that corrected this is why BL is set to 0x07 below rather
;     than left over: a machine that wrongly writes BL produces the same
;     screen here, and scroll.asm is where that is made to show.)
;
;   * BX, CX, DX and the segment registers come back unchanged, but SI
;     and DI MAY BE CHANGED. So the text pointer is not kept in SI -- it
;     lives in memory and is reloaded around each call. A program that
;     held it in SI would work on an implementation that happened to
;     leave SI alone and break on one that did not, and that is the worse
;     kind of portability, because it is invisible until it is not.
;
; Entry convention: the corpus convention, from replay.h -- a flat binary
; at 0x100 of its own segment, CS=DS=ES=SS=0x1000, SP=0xFFFE, FLAGS=0xF002.
; IF is CLEAR, unlike
; real DOS, so a sample that wants to be interrupted has to say `sti`
; itself. This one never enables interrupts and never needs them.
;
; Expected at HLT:
;   page 0, cells 0..21 = "M4 hello from the BIOS", attribute 0x07 on
;     each -- inherited, not written: the mode set filled the page with
;     (space, VM86_ATTR_DEFAULT) and 0Eh wrote only the characters
;   the cursor at cell 22
;   every other cell of the page still (space, 0x07) from the mode set

        bits 16
        org 0x100

        mov     ax, 0x0003              ; mode 3: 80x25 colour text
        int     0x10

        mov     word [text], message

.next:
        mov     bx, [text]
        cmp     bx, message_end
        je      done

        mov     al, [bx]
        mov     ah, 0x0E                ; teletype output
        mov     bh, 0x00                ; page 0; it also clears BX's top
        mov     bl, 0x07                ; half, which is why the pointer
        int     0x10                    ; is reloaded before it is used

        mov     bx, [text]
        add     bx, 1
        mov     [text], bx
        jmp     .next

done:
        cli
        hlt

text:
        dw 0
message:
        db "M4 hello from the BIOS"
message_end:
