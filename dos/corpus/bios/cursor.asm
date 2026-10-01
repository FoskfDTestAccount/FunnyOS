; cursor -- where the cursor is, which page it is on, and the two rules
; about when writing moves it.
;
; Three claims, all of them about state a program cannot see on the screen
; and therefore has to ask the firmware for. Every service number and every
; register below is from docs/dos-refs.md section 1.
;
;   1. AH=02h puts the cursor somewhere on a named page (DH row, DL column,
;      BH page) and AH=03h reads it back (DH/DL position, CX shape). The
;      program stores what it read at 1000:0800, so the suite checks the
;      value the firmware RETURNED and not the one the program believes it
;      set. A 03h that answered with the wrong register would be invisible
;      to a program that only ever sets and never asks.
;
;   2. AH=09h writes a character with the attribute in BL and does NOT move
;      the cursor. So the 03h after it must still report the position the
;      02h put there. That is the difference between 09h and 0Eh -- the one
;      item section 1 lists as easy to get wrong by merging the two rules
;      into one -- and both are exercised here, 0Eh by hello.asm.
;
;   3. The page is real. Writing to page 1 while page 0 is showing leaves
;      page 0 alone, and the page a write lands on is the one named in BH
;      rather than the one that happens to be active.
;
;      That second half is the one that needed a step of its own. Every
;      write in the first two claims names the page that was already
;      active, so a service that ignored BH entirely would agree with the
;      firmware on every one of them. Step 5 writes to page 0 while page 1
;      is the active page, and page 0's cell 564 is where it must land.
;
; The rows and the columns are chosen so that ignoring the page shows up
; immediately. Page 0 gets 'A' at cell 410 and page 1 gets 'B' at cell 163,
; and cell 163 of page 0 is asserted untouched -- so a write aimed at page 1
; that landed on page 0 would put 'B' on the blank. Step 5 is the same test
; from the other side. What the two share is an index used on two pages, not
; one cell: 410 and 163 are different cells of their own pages.
;
; The suite finds page 1 by reading the page stride out of 0040:004C
; instead of assuming it. That word is 0x1000 on this machine -- a page
; stride rounded up to a 4 KiB boundary, not the 4000 bytes of content
; (docs/dos-refs.md section 7) -- and a suite that hard-coded its own
; number would be testing its own arithmetic rather than the machine's.
; Section 7 also warns that the field is not dependable across hardware and
; that some sources advise ignoring it in favour of columns times rows;
; reading it here means this program follows whatever the firmware says.
;
; Entry convention: the corpus convention, from replay.h -- a flat binary
; at 0x100 of its own segment, CS=DS=ES=SS=0x1000, SP=0xFFFE, FLAGS=0xF002.
; IF is clear; no
; interrupt is involved.
;
; Expected at HLT:
;   page 0, cell 410 (row 5, col 10) = 'A', attribute 0x1E
;   page 0, cell 163 (row 2, col 3)  = (space, 0x07): what the mode set
;     left there, NOT what the page 1 write would have put there
;   page 1, cell 163                 = 'B', attribute 0x2F
;   0040:0050 = 0x050A    page 0 cursor, row 5 col 10
;   0040:0052 = 0x0203    page 1 cursor, row 2 col 3
;   0040:0062 = 0x01      the active page
;   0040:004C = 0x1000    the page stride the mode set established
;   1000:0800 = 0x05      the row AH=03h returned
;   1000:0801 = 0x0A      the column AH=03h returned
;   page 0, cell 564 (row 7, col 4) = 'C', attribute 0x4B -- written with
;     BH=0 while page 1 was the active page. A service that used the active
;     page instead would put it on page 1 and leave this cell as the mode
;     set left it, which is (space, 0x07).
;   0040:0050 = 0x0704    page 0 cursor after step 5: row 7 col 4

        bits 16
        org 0x100

        ; Mode 3 first, for two reasons that are both about what follows.
        ; It clears the screen, so the two cells that must stay untouched
        ; are (space, 0x07) rather than whatever a cleared machine holds;
        ; and it establishes the page stride at 0040:004C that the suite
        ; uses to find page 1, which is 4096 and not the 4000 bytes of
        ; content (dos-refs section 7).
        mov     ax, 0x0003              ; 80x25 colour text
        int     0x10

        ; 1. the cursor to page 0, row 5, column 10
        mov     ah, 0x02
        mov     bh, 0x00
        mov     dh, 0x05
        mov     dl, 0x0A
        int     0x10

        ; 2. write 'A' there with attribute 0x1E, and do not move
        mov     ah, 0x09
        mov     al, 'A'
        mov     bh, 0x00
        mov     bl, 0x1E
        mov     cx, 0x0001
        int     0x10

        ; 3. read the cursor back and keep what the firmware said
        mov     ah, 0x03
        mov     bh, 0x00
        int     0x10
        mov     [0x0800], dh
        mov     [0x0801], dl

        ; 4. page 1 is where the next write goes
        mov     ax, 0x0501              ; AH=05h select page, AL=1
        int     0x10

        mov     ah, 0x02
        mov     bh, 0x01
        mov     dh, 0x02
        mov     dl, 0x03
        int     0x10

        mov     ah, 0x09
        mov     al, 'B'
        mov     bh, 0x01
        mov     bl, 0x2F
        mov     cx, 0x0001
        int     0x10

        ; 5. the page a write lands on is the one named in BH, not the one
        ;    that happens to be active. Active is 1 at this point, and this
        ;    writes to page 0 -- which is the only way the rule in claim 3
        ;    gets exercised at all. Every call above names the page that was
        ;    already active, so "uses BH" and "uses the active page" give the
        ;    same answer for all of them, and a service that ignored BH would
        ;    pass every assertion before this one.
        mov     ah, 0x02                ; cursor on page 0, row 7, column 4
        mov     bh, 0x00
        mov     dh, 0x07
        mov     dl, 0x04
        int     0x10

        mov     ah, 0x09                ; write 'C' there, attribute 0x4B
        mov     al, 'C'
        mov     bh, 0x00
        mov     bl, 0x4B
        mov     cx, 0x0001
        int     0x10

        cli
        hlt
