; disk -- a sector read that must work and a sector read that must fail.
;
; INT 13h reports failure in the CARRY FLAG (docs/dos-refs.md section 4),
; and the second half of this program exists to make a machine that reports
; failure anywhere else -- in AH, or in nothing at all -- fail this sample
; rather than pass it. A BIOS that sets AH=04h and leaves CF clear tells
; the caller the read worked, and the caller goes on to use a buffer that
; was never filled. So: a read of a sector that is there (CF must clear), a
; read of a sector that is not (CF must set), and the program only says OK
; if both happened.
;
; The first read is checked against the bytes as well as against the flags,
; because CF=0 is also what a machine that did nothing at all would return.
; Sector 1 -- linear sector 0, the very first sector of the disk, which is
; where an off-by-one in the cylinder/head/sector arithmetic shows up first
; -- must contain 4D 34 0D 0A: "M4", carriage return, line feed.
;
; The second read asks for cylinder 80 of an eighty-cylinder disk. Every
; byte of it is out of range, so it must come back with CF set, AH=04h, and
; must leave the buffer at 1000:0900 alone. Two words are planted there
; first and the suite asserts them afterwards: a failed read that
; half-filled the buffer is worse than one that failed loudly, because the
; caller cannot tell stale data from real data.
;
; A note on the carry flag, kept because it explains why this program is
; written the way it is: for a while a host service could not return CF at
; all. The stub ends in a real IRET, which pops FLAGS from the frame the
; guest's own INT pushed, so a service that set cpu->flags had the setting
; thrown away by the IRET that followed. That is fixed in the trap now --
; see M4-E-report.md for how it was measured, and docs/dos-refs.md section
; 4 for the interface -- and this program did not change: it reads the
; carry because that is how INT 13h reports, and it gets a real answer.
; (The same gap covered INT 16h AH=01h's ZF; nothing in this corpus uses
; 01h.)
;
; The output loop keeps its pointer in memory rather than in SI: INT 10h
; AH=0Eh returns BX, CX, DX and the segment registers unchanged but MAY
; CHANGE SI and DI (docs/dos-refs.md section 1). A pointer in SI would work
; or not depending on the implementation, which is not a thing to build a
; test on.
;
; Entry convention: the corpus convention, from replay.h -- a flat binary
; at 0x100 of its own segment, CS=DS=ES=SS=0x1000, SP=0xFFFE, FLAGS=0xF002.
; IF is clear; INT 13h
; does not need interrupts and this program never enables them.
;
; Expected at HLT:
;   page 0, cells 0..1 = "OK", attribute 0x07, cursor at cell 2
;   (and "FAIL" at cells 0..3 if either read behaved wrongly)
;   the attribute is the one the mode set's clear left: 0Eh writes no
;   attribute byte, so the 0x0F in BL is ignored -- and set to a value the
;   cells do not have, so a machine that writes BL fails here rather than
;   passing by luck
;   the buffer at 1000:0600 = 4D 34 0D 0A, then the rest of the sector
;   the buffer at 1000:0900 = CD AB 34 12, exactly as it was planted

        bits 16
        org 0x100

        ; Mode 3 first: 0Eh writes no attribute byte, so without a mode
        ; set the verdict lands on a page of attribute 0x00 and nobody can
        ; see it. See hello.asm.
        mov     ax, 0x0003
        int     0x10

        ; 1. read the first sector of the disk into 1000:0600
        mov     ax, 0x0201              ; AH=02h read, AL=1 sector
        mov     cx, 0x0001              ; CH=0 cylinder, CL=1 -> sector 1
        mov     dx, 0x0000              ; DH=0 head, DL=0 drive A
        mov     bx, 0x0600              ; ES:BX, and ES is 0
        int     0x13

        jc      read_bad
        cmp     word [0x0600], 0x344D   ; "M4" little-endian
        jne     read_bad
        cmp     word [0x0602], 0x0A0D   ; CR, LF
        jne     read_bad

        ; 2. the same read one cylinder past the end, which must fail
        mov     word [0x0900], 0xABCD
        mov     word [0x0902], 0x1234

        mov     ax, 0x0201
        mov     cx, 0x5001              ; cylinder 80, on an 80-cylinder disk
        mov     dx, 0x0000
        mov     bx, 0x0900
        int     0x13

        jnc     read_bad                ; a read past the end must not succeed

        mov     si, ok_text
        jmp     say

read_bad:
        mov     si, bad_text

say:
        mov     [say_text], si
say_loop:
        ; The pointer comes out of memory every time round, because INT 10h
        ; may change SI and DI and this loop must not depend on it. BX is
        ; the pointer AND the register BL lives in, so the page and the
        ; colour go in after it is loaded and before the call.
        mov     bx, [say_text]
        mov     al, [bx]
        or      al, al
        jz      finished
        mov     ah, 0x0E
        mov     bh, 0x00                ; the page; all 0Eh takes
        mov     bl, 0x0F                ; ignored in text mode, so the cells
        int     0x10                    ; keep 0x07 -- and set to a value
                                        ; they do NOT have, so a machine that
                                        ; wrongly writes BL is caught here
        mov     bx, [say_text]
        add     bx, 1
        mov     [say_text], bx
        jmp     say_loop

finished:
        cli
        hlt

say_text:
        dw 0
ok_text:
        db "OK", 0
bad_text:
        db "FAIL", 0
