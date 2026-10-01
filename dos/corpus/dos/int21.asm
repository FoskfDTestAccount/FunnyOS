; int21 -- the W4 acceptance program: a .COM that asks DOS for things and
; prints what it was told.
;
; A .COM loaded the way DOS loads one, entered with IF set and the four
; segment registers pointed at its PSP (docs/dos-refs-dos.md section 2),
; which then calls INT 21h for every function W4 covers and prints the
; answers.
;
; ---------------------------------------------------------------------
; What it prints, and what each line is here to catch
;
;   DOS version = 1E03   AH=30h. The whole word, printed as four hex
;                        digits, because the mistake worth catching is AL
;                        and AH the wrong way round -- 031E is the same
;                        two bytes and would pass any check that only
;                        asked whether the call answered at all. 3.30 is
;                        the value docs/dos-refs-dos.md section 6 chose.
;
;   W4: INT 21h ...      AH=09h, a '$'-terminated string, followed by a
;                        character and a CRLF written with AH=02h. The
;                        three output functions are the ones this whole
;                        milestone exists to make work, and they go
;                        through the same teletype the BIOS uses -- so
;                        this line appearing at all is the claim.
;
;   vector 21h  = ...    AH=35h, reading the vector table back. It must
;                        come out as the firmware's own stub for 21h,
;                        which is at a position the *interpreter's* map
;                        gives rather than anything the DOS layer
;                        decides: F000:0084, four bytes per vector from a
;                        base of zero.
;
;   vector 60h  = ...    AH=25h then AH=35h: written, then read back. A
;                        pair, because either one alone is satisfied by a
;                        function that ignores its arguments.
;
;   DTA         = ...    AH=1Ah then AH=2Fh, and the same argument: the
;                        only way to know 1Ah stored what it was given is
;                        to ask 2Fh. The value must be the pair this
;                        program handed in -- segment and offset, not a
;                        linear address.
;
;   drive       = 05     AH=19h. The machine's one drive is F:, which is
;                        index 5, and the shell on this machine calls its
;                        storage F:\.
;
;   select F:   = 06     AH=0Eh. AL is the count, which is 6 rather than
;                        1 because DOS counts the numbering's range and
;                        not the media that are present.
;
;   select A:   = 000F CF=1
;                        AH=0Eh for a drive that is not there. This is
;                        the line that proves the function reads DL at
;                        all -- without it, a version that ignored its
;                        argument and always said yes would pass
;                        everything above.
;
;   function 55 = 5500 CF=1
;                        A function number this machine has not got. AL
;                        goes to zero and the carry flag goes up, and AH
;                        is left holding the 55 that selected the call --
;                        so the four digits are both halves of the claim.
;                        docs/dos-refs-dos.md section 9 has the source
;                        for that answer and for why it is not an error
;                        code in AX.
;
; ---------------------------------------------------------------------
; How it ends
;
; With `INT 21h AH=4Ch` and a return code of 7, so the exit is a thing
; that happened rather than a thing the absence of a crash implies. Seven
; because zero is what a machine that never reached the call would report,
; and the two have to differ for this to assert anything.
;
; Nothing here is asserted by the program. The host suite
; (dos/tests/test_int21.c) and the screen test read the page and compare
; it against docs/dos-refs-dos.md, which is the only arrangement in which
; a service and its test cannot agree on the same mistake.

        bits 16
        org 0x100

start:
        ; Mode 3 first, the way every DOS program of the era does: the
        ; mode set is what clears the page and gives each cell the default
        ; attribute. Without it the characters land in cells whose
        ; attribute byte is zero -- black on black -- and the program
        ; prints a screenful of nothing that every memory assertion would
        ; still call correct.
        mov     ax, 0x0003
        int     0x10

        ; --- AH=30h: the version -------------------------------------
        mov     ah, 0x30
        int     0x21
        mov     si, m_version
        call    puts
        call    puthex
        call    newline

        ; --- AH=09h: a '$'-terminated string --------------------------
        mov     dx, m_hello
        mov     ah, 0x09
        int     0x21

        ; --- AH=02h: one character, then CR and LF --------------------
        mov     dl, '!'
        mov     ah, 0x02
        int     0x21
        mov     dl, 0x0D
        mov     ah, 0x02
        int     0x21
        mov     dl, 0x0A
        mov     ah, 0x02
        int     0x21

        ; --- AH=35h: where the firmware's own 21h stub is -------------
        mov     si, m_vec21
        call    puts
        mov     al, 0x21
        mov     ah, 0x35
        int     0x21                    ; ES:BX
        call    put_far
        call    newline

        ; --- AH=25h then AH=35h: write a vector, read it back ---------
        mov     si, m_vec60
        call    puts
        mov     al, 0x60
        mov     dx, 0x1234
        mov     ah, 0x25
        int     0x21                    ; DS:DX into vector 60h
        mov     al, 0x60
        mov     ah, 0x35
        int     0x21                    ; and read it back
        call    put_far
        call    newline

        ; --- AH=1Ah then AH=2Fh: the transfer address -----------------
        mov     si, m_dta
        call    puts
        mov     dx, 0x0200
        mov     ah, 0x1A
        int     0x21                    ; DS:DX
        mov     ah, 0x2F
        int     0x21                    ; ES:BX
        call    put_far
        call    newline

        ; --- AH=19h: which drive --------------------------------------
        mov     si, m_drive
        call    puts
        mov     ah, 0x19
        int     0x21                    ; AL
        call    puthex_byte
        call    newline

        ; --- AH=0Eh: select the drive that is there -------------------
        mov     si, m_sel_f
        call    puts
        mov     dl, 5                   ; F:
        mov     ah, 0x0E
        int     0x21                    ; AL = the count
        call    puthex_byte
        call    newline

        ; --- AH=0Eh: and a drive that is not --------------------------
        mov     si, m_sel_a
        call    puts
        mov     dl, 0                   ; A:, which this machine has not
        mov     ah, 0x0E
        int     0x21
        call    put_ax_cf
        call    newline

        ; --- a function number this machine has not got ---------------
        mov     si, m_unknown
        call    puts
        mov     ah, 0x55
        int     0x21
        call    put_ax_cf
        call    newline

        ; --- and out, with a code -------------------------------------
        mov     ax, 0x4C07
        int     0x21

; ---------------------------------------------------------------------
; Output
;
; Every character goes through INT 21h AH=02h -- not INT 10h the way
; psp.asm does it. That is the point of this program: the BIOS path was
; W3's and this one is the DOS layer's, so a program that printed through
; the firmware would be showing the screen working rather than INT 21h.
; ---------------------------------------------------------------------

puts:                                   ; SI = NUL-terminated string
        push    ax
        push    si

.loop:
        lodsb
        test    al, al
        jz      .done
        call    putchar
        jmp     .loop

.done:
        pop     si
        pop     ax
        ret

putchar:                                ; AL = character
        push    ax
        push    dx
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        pop     dx
        pop     ax
        ret

newline:
        push    ax
        mov     al, 0x0D
        call    putchar
        mov     al, 0x0A
        call    putchar
        pop     ax
        ret

put_far:                                ; ES:BX, as SEG:OFF
        mov     ax, es
        call    puthex
        mov     al, ':'
        call    putchar
        mov     ax, bx
        call    puthex
        ret

; AX, then the carry flag: "<AX> CF=<c>".
;
; The caller must reach this with nothing between it and the INT 21h whose
; flag is being reported. The flag is read here and nowhere else, because
; everything below goes through INT 21h and a service may leave the flags
; however it likes -- a flag read after any other call is not the flag the
; call produced.
put_ax_cf:
        call    mark_cf
        call    puthex
        mov     si, m_cf
        call    puts
        call    put_cf
        ret

; Keep the carry flag while it is still the one the call produced.
;
; A subroutine and not a macro, which is a decision about this file rather
; than about the 8086. NASM wants a macro defined before the line that
; invokes it, and the two call sites are both above the output routines --
; so a macro here would have to be defined at the top of the program,
; away from the thing it belongs to. A `call` costs two bytes and changes
; no flag, which is what makes it usable: the MOV below does not touch
; the flags either, and the JNC reads the one the INT left.
mark_cf:
        mov     byte [g_cf], 0
        jnc     .done
        mov     byte [g_cf], 1

.done:
        ret

put_cf:                                 ; the saved flag, as '0' or '1'
        push    ax
        mov     al, [g_cf]
        add     al, '0'
        call    putchar
        pop     ax
        ret

puthex:                                 ; AX, four digits
        push    ax
        mov     al, ah
        call    puthex_byte
        pop     ax
        call    puthex_byte
        ret

puthex_byte:                            ; AL, two digits
        push    ax
        push    bx
        push    cx
        push    dx

        mov     dl, al
        mov     cx, 2

.next:
        ; Four one-bit rotations bring the top nibble down, and four more
        ; fetch the next one. Sixteen rotations is the identity, so the
        ; loop ends exactly when every digit has been shown.
        rol     dl, 1
        rol     dl, 1
        rol     dl, 1
        rol     dl, 1

        mov     al, dl
        and     al, 0x0F
        cmp     al, 10
        jb      .digit
        add     al, 'A' - 10
        jmp     .out

.digit:
        add     al, '0'

.out:
        call    putchar
        loop    .next

        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------
; The strings. Fixed width so the values line up in a column, because
; this is meant to be read off a screen as well as parsed by a test.
;
; m_hello is the one handed to AH=09h, so it ends in '$' rather than NUL
; -- and nothing else in this file may contain a '$', because that is
; what would end it early.
; ---------------------------------------------------------------------

m_version   db "DOS version = ", 0
m_vec21     db "vector 21h  = ", 0
m_vec60     db "vector 60h  = ", 0
m_dta       db "DTA         = ", 0
m_drive     db "drive       = ", 0
m_sel_f     db "select F:   = ", 0
m_sel_a     db "select A:   = ", 0
m_unknown   db "function 55 = ", 0
m_cf        db " CF=", 0

m_hello     db "W4: INT 21h says hello$"

; The carry flag, parked while the digits in front of it are printed.
g_cf        db 0
