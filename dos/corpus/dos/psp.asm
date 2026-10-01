; psp -- the W3 acceptance program: a .COM that reads its own PSP and
; prints what it finds.
;
; This is the first program in the project that is loaded the way DOS loads
; one rather than the way M3's corpus loads one. Nothing here is special:
; it prints through INT 10h, exactly like hello.asm, and every byte it
; reads it reads with a plain `mov ax, [offset]` against DS -- which the
; loader has pointed at the PSP. There is no service call between the
; program and any of the values below, which is the point: a PSP is an
; interface programs read *directly*, so the only thing that can be tested
; here is whether the bytes are right.
;
; ---------------------------------------------------------------------
; What it prints, and why each line is there
;
;   CS/DS/ES/SS   All four have to be the same segment, and it has to be
;                 the PSP's. A loader that set only CS leaves a program
;                 whose data is somewhere else, and the symptom shows up
;                 as `mov si, message` reading the wrong bytes -- a long
;                 way from the loader.
;
;   RET word      The zero word the loader pushed. A bare RET at the end
;                 of a program pops it, IP becomes zero, and execution
;                 arrives at PSP:0000. This is the whole of CP/M
;                 compatibility and it is one `pop` away from being
;                 visible.
;
;   PSP:0000      CD 20 -- the INT 20h that zero lands on.
;   PSP:0002      The segment of the first byte past the program's memory.
;                 A .COM gets everything up to the top of conventional
;                 memory, so this is about the machine, not the program.
;   PSP:000A/000C The INT 22h that was in the vector table before the
;                 program started. Dword, so it prints as two lines.
;   PSP:0016      The parent PSP segment.
;   PSP:0018      The first byte of the job file table, which for a
;                 program with no files open is FF.
;   PSP:002C      The environment's segment.
;   PSP:0032/0034 The JFT's size and its far pointer.
;   PSP:0040      The DOS version to report.
;   PSP:0050      The three bytes of the old INT 21h entry.
;   PSP:005C/005D The first default FCB: its drive byte, then its eleven
;                 name-and-extension bytes. With no argument on the
;                 command line those eleven are blanks, not zeroes --
;                 which is the part of this that a loader would get wrong
;                 by doing the obvious thing.
;   PSP:006C      The second FCB's drive byte. It sits twelve bytes into
;                 the first FCB's twenty.
;   PSP:0080      The command tail's length.
;   TAIL          The tail itself, printed as characters between quotes.
;
; ---------------------------------------------------------------------
; Entry convention
;
; The DOS one, from docs/dos-refs-dos.md section 2: CS = DS = ES = SS = the
; PSP segment, IP = 0x100, SP = 0xFFFE with a zero word at SS:FFFE, and IF
; SET. That last one is the difference from corpus/bios, whose samples are
; entered with interrupts off -- see that directory's replay.h for why the
; two conventions exist rather than one.
;
; Expected at HLT: the lines below, whose values are the loader's, not this
; program's. It asserts nothing itself -- the suite does, and the values it
; asserts against come from docs/dos-refs-dos.md rather than from the
; loader, which is the only thing that keeps a loader and a test from
; agreeing on the same mistake.

        bits 16
        org 0x100

start:
        ; Set the mode first, the way every DOS program of the era does and
        ; for the reason hello.asm gives: the mode set is what clears the
        ; page and gives every cell the default attribute. Without it the
        ; characters go into cells whose attribute byte is zero -- black on
        ; black -- and the program prints a screenful of nothing that every
        ; assertion about memory would still call correct.
        mov     ax, 0x0003
        int     0x10

        mov     si, m_cs
        mov     ax, cs
        call    report

        mov     si, m_ds
        mov     ax, ds
        call    report

        mov     si, m_es
        mov     ax, es
        call    report

        mov     si, m_ss
        mov     ax, ss
        call    report

        ; The word a bare RET would return through. Popping it here is the
        ; only way to look at it, and it is what makes the CP/M exit a
        ; thing this program can demonstrate rather than describe.
        pop     ax
        mov     si, m_ret
        call    report

        mov     si, m_exit
        mov     ax, [0x0000]
        call    report

        mov     si, m_top
        mov     ax, [0x0002]
        call    report

        mov     si, m_int22
        mov     ax, [0x000A]
        call    report

        mov     si, m_int22s
        mov     ax, [0x000C]
        call    report

        mov     si, m_parent
        mov     ax, [0x0016]
        call    report

        mov     si, m_jft
        mov     al, [0x0018]
        xor     ah, ah
        call    report

        mov     si, m_env
        mov     ax, [0x002C]
        call    report

        mov     si, m_jftsize
        mov     ax, [0x0032]
        call    report

        mov     si, m_jftptr
        mov     ax, [0x0034]
        call    report

        mov     si, m_jftptr_seg
        mov     ax, [0x0036]
        call    report

        mov     si, m_version
        mov     ax, [0x0040]
        call    report

        ; Three bytes rather than a word: CD 21 CB, and the CB is the
        ; whole point of the field.
        mov     si, m_gate
        call    puts
        mov     si, 0x0050
        mov     cx, 3
        call    put_bytes
        call    newline

        mov     si, m_fcb1
        mov     al, [0x005C]
        xor     ah, ah
        call    report

        mov     si, m_fcb1name
        call    puts
        mov     si, 0x005D
        mov     cx, 11
        call    put_bytes
        call    newline

        mov     si, m_fcb2
        mov     al, [0x006C]
        xor     ah, ah
        call    report

        mov     si, m_taillen
        mov     al, [0x0080]
        xor     ah, ah
        call    report

        mov     si, m_tail
        call    puts
        mov     cl, [0x0080]
        xor     ch, ch
        mov     si, 0x0081
        call    put_chars
        mov     si, m_close
        call    puts
        call    newline

        cli
        hlt

; ---------------------------------------------------------------------
; Output
;
; Every call goes through INT 10h AH=0Eh, the one video function every DOS
; program of the era used. It takes AL as the character, BH as the page and
; BL as a colour it ignores in text mode, and it leaves BX, CX and DX alone
; -- which is why the values below are kept in DX while a character is
; being written.
; ---------------------------------------------------------------------

; SI = a NUL-terminated string, AX = a value: print both, then a newline.
report:
        call    puts
        call    puthex
        call    newline
        ret

puts:                                   ; SI = string
        push    ax
        push    bx
        push    si

.loop:
        lodsb                           ; AL = [DS:SI], SI++
        test    al, al
        jz      .done
        call    putchar
        jmp     .loop

.done:
        pop     si
        pop     bx
        pop     ax
        ret

put_chars:                              ; SI = pointer, CX = count
        push    ax
        push    bx
        push    cx
        push    si

.loop:
        jcxz    .done
        lodsb
        call    putchar
        loop    .loop

.done:
        pop     si
        pop     cx
        pop     bx
        pop     ax
        ret

put_bytes:                              ; SI = pointer, CX = count, as hex
        push    ax
        push    cx
        push    si

.loop:
        jcxz    .done
        lodsb
        call    puthex_byte
        loop    .loop

.done:
        pop     si
        pop     cx
        pop     ax
        ret

puthex:                                 ; AX = value, four digits
        push    ax
        mov     al, ah
        call    puthex_byte
        pop     ax
        call    puthex_byte
        ret

puthex_byte:                            ; AL = value, two digits
        push    ax
        push    bx
        push    cx
        push    dx

        mov     dl, al
        mov     cx, 2

.next:
        ; Four one-bit rotations move the top nibble to the bottom, and
        ; four more fetch the next one. Sixteen rotations is the identity,
        ; so the loop ends exactly when it has shown every digit.
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

putchar:                                ; AL = character
        push    ax
        push    bx
        mov     ah, 0x0E
        mov     bh, 0x00
        mov     bl, 0x07
        int     0x10
        pop     bx
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

; ---------------------------------------------------------------------
; The labels. Fixed width so the values line up in a column, because this
; is meant to be read off a screen as well as parsed by a test.
; ---------------------------------------------------------------------

m_cs        db "CS       = ", 0
m_ds        db "DS       = ", 0
m_es        db "ES       = ", 0
m_ss        db "SS       = ", 0
m_ret       db "RET word = ", 0
m_exit      db "PSP:0000 = ", 0
m_top       db "PSP:0002 = ", 0
m_int22     db "PSP:000A = ", 0
m_int22s    db "PSP:000C = ", 0
m_parent    db "PSP:0016 = ", 0
m_jft       db "PSP:0018 = ", 0
m_env       db "PSP:002C = ", 0
m_jftsize   db "PSP:0032 = ", 0
m_jftptr    db "PSP:0034 = ", 0
m_jftptr_seg db "PSP:0036 = ", 0
m_version   db "PSP:0040 = ", 0
m_gate      db "PSP:0050 = ", 0
m_fcb1      db "PSP:005C = ", 0
m_fcb1name  db "PSP:005D = ", 0
m_fcb2      db "PSP:006C = ", 0
m_taillen   db "PSP:0080 = ", 0
m_tail      db "TAIL     = ", 0x22, 0
m_close     db 0x22, 0
