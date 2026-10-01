; direct -- the same screen, written without the BIOS at all.
;
; M4-8 says the text buffer at 0xB8000 is ordinary guest memory: the host
; renders from it and does not redirect the writes. This program is the one
; that can tell. It never executes an INT; it stores characters and
; attributes straight into 0xB8000, and the screen it leaves behind has to
; be byte for byte what hello.asm leaves behind with the same text.
;
; If the two ever disagree then somebody has made the display two pieces of
; state -- a shadow copy, a redirection at the memory layer, a service that
; keeps a screen of its own -- and the only other symptom would be
; "programs that write video memory directly do not work", which is a whole
; class of DOS programs and not a thing anybody would guess from a failing
; BIOS call.
;
; The attribute byte written here is 0x07, which is VM86_ATTR_DEFAULT in
; firmware.h. It has to be that value and not any other, because hello.asm
; reaches the same cells through AH=0Eh and 0Eh does not write an
; attribute -- it keeps the one already there (docs/dos-refs.md section 1),
; and the one already there is 0x07 because hello.asm sets the video mode
; first and a mode set clears the page to (space, VM86_ATTR_DEFAULT).
;
; This program does not set the mode, because it uses no interrupt at all,
; and so the page it writes into is the cleared one: zero everywhere. That
; is the one visible difference between the two screens and it is the
; right one -- hello.asm leaves a mode-3 screen, this leaves the same
; message on a screen of zeroes. What the suite compares is the cells the
; message occupies, which must be identical down to the attribute, and
; then each page's own shape around them.
;
; The cursor is deliberately NOT part of the comparison. Storing to memory
; does not move the firmware's cursor, on this machine or on a real one:
; the position at 0040:0050 is a BIOS variable and only the service that
; owns it may write it. So this program leaves the cursor where it found it
; and hello.asm leaves it at cell 22, and the two screens still match.
;
; Entry convention: the corpus convention, from replay.h -- a flat binary
; at 0x100, CS=DS=ES=SS=0, SP=0xFFFE, FLAGS=0xF002. IF is clear and this
; program never needs it: no interrupt is involved anywhere in it.
;
; Expected at HLT:
;   page 0, cells 0..21 = "M4 hello from the BIOS", attribute 0x07 each
;   the cursor untouched (cell 0 on a freshly powered-on machine)
;   the 4000-byte page identical to the one hello.asm leaves

        bits 16
        org 0x100

        mov     ax, 0xB800
        mov     es, ax
        xor     di, di
        mov     si, message

.next:
        cmp     si, message_end
        je      done

        mov     al, [si]
        mov     ah, 0x07                ; character low, attribute high
        mov     [es:di], ax

        add     di, 2
        inc     si
        jmp     .next

done:
        cli
        hlt

message:
        db "M4 hello from the BIOS"
message_end:
