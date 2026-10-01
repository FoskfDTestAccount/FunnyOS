; scroll -- the window rectangle, the fill attribute, and the teletype
; scrolling itself off the bottom of the screen.
;
; Scrolling is the one text-mode operation whose result depends on what was
; already there, so a scroll that is really a clear looks the same as a
; scroll whenever the region happened to be blank. This program therefore
; fills the page with something first, and the sequence below is arranged
; so that every claim lands somewhere in the final screen that the other
; claims did not overwrite:
;
;   1. The page is filled directly, one letter per row: row 0 is 'A', row 1
;      is 'B', and so on to row 24 = 'Y', all with attribute 0x07. Written
;      straight to 0xB8000, because this is the text and not the firmware
;      operation being tested.
;
;   2. The cursor goes to the bottom-left corner and eighty 'x' characters
;      go out through the teletype. The eightieth lands on the last cell of
;      the last row, and the advance off its right edge scrolls the page --
;      the teletype's own auto-scroll, and the only way to see it: fewer
;      than eighty characters leaves the page alone.
;
;   3. AH=06h with AL=0 fills a window instead of scrolling no lines. The
;      window is rows 10..14 across all eighty columns, filled with the
;      attribute in BH -- 0x20, which is not the 0x07 the letters around it
;      carry, so the band is still identifiable after the scroll that
;      follows. (AL=0 is the case docs/dos-refs.md section 1 singles out:
;      a scroll of zero lines is "do nothing", and the two differ by a
;      whole screen.)
;
;   4. AH=06h with AL=1 scrolls the whole window up one line and fills the
;      row that comes in at the bottom with (space, BH) -- 0x30 here, again
;      a value nothing else in the program uses.
;
; Every letter moves twice, so what the final screen shows is the original
; fill shifted up by two rows, with the cleared band -- still carrying the
; attribute 0x20 that proves it was cleared rather than scrolled -- two
; rows above where the letters would otherwise have put it.
;
; This is the one sample that does NOT set the video mode first, and the
; reason is the same one that makes the others set it: 0Eh writes no
; attribute byte, so it needs the page to already carry the attribute the
; text should have. Every other sample gets that from the mode set's
; clear; this one writes the whole page itself, with its own attribute, in
; step 1 -- so the page it prints onto is one it made, and a mode set here
; would only be overwritten. That is also the shape of the claim: the
; screen is memory, and a program that fills it has nothing to ask the
; firmware for.
;
; ---------------------------------------------------------------------
; The 'x' characters are the check on the erratum of 2026-10-01
;
; BL is set to 0x0F before each teletype call and the expected attribute on
; the 'x' cells is 0x07, not 0x0F. That is deliberate and it is the point
; of setting BL at all: docs/dos-refs.md section 1 says AH=0Eh does NOT
; write an attribute byte in text mode, it keeps the one the cell already
; has, and BL is a graphics-mode foreground colour. So the eighty cells
; that were ('Y', 0x07) before step 2 must be ('x', 0x07) after it. An
; implementation that writes BL would leave 0x0F here and pass every other
; assertion in this program.
;
; The loops count in CX and BX, never in SI or DI. INT 10h AH=0Eh returns
; BX, CX, DX and the segment registers unchanged but MAY CHANGE SI and DI
; (same section), so a loop that kept its counter or its pointer there
; would be relying on something the interface does not promise. The fill
; loop below has no interrupt in it and could use anything; it uses CX and
; BX to keep the two loops reading the same way.
;
; Entry convention: the corpus convention, from replay.h -- a flat binary
; at 0x100 of its own segment, CS=DS=ES=SS=0x1000, SP=0xFFFE, FLAGS=0xF002.
; IF is clear; no
; interrupt is involved.
;
; Expected at HLT, page 0, cell index = row * 80 + col:
;   rows 0..8    'C' 'D' 'E' 'F' 'G' 'H' 'I' 'J' 'K', attribute 0x07
;   rows 9..13   the cleared band: (0x20, 0x20)
;   rows 14..21  'Q' .. 'X'                          attribute 0x07
;   row  22      80 x ('x', 0x07)   -- 0x07, not the BL of 0x0F; see above
;   row  23      blank: character 0x20 (the teletype's own fill, whose
;                attribute is the implementation's to choose and is
;                therefore not asserted -- see M4-E-report.md)
;   row  24      (0x20, 0x30) x 80  -- the 06h fill attribute, still visible
;
; The letters run 'B'..'X' after the teletype's scroll and 'C'..'X' after
; the 06h one, so the second band starts at 'Q' and not at 'P': two rows
; were taken off the top, and the AL=0 window was cleared in between.

        bits 16
        org 0x100

        ; 1. fill the page directly, one letter per row
        mov     ax, 0xB800
        mov     es, ax
        xor     di, di
        mov     al, 'A'
        mov     bx, 25
.row:
        mov     cx, 80
        mov     ah, 0x07
.cell:
        mov     [es:di], ax
        add     di, 2
        loop    .cell
        inc     al
        dec     bx
        jnz     .row

        ; 2. the cursor to the bottom-left, then eighty teletype writes
        mov     ah, 0x02
        mov     bh, 0x00
        mov     dh, 24
        mov     dl, 0
        int     0x10

        mov     cx, 80
.tele:
        mov     al, 'x'
        mov     ah, 0x0E
        mov     bh, 0x00
        mov     bl, 0x0F                ; ignored in text mode
        int     0x10
        loop    .tele

        ; 3. AH=06h AL=0: fill rows 10..14, all eighty columns
        mov     ax, 0x0600
        mov     bh, 0x20
        mov     cx, 0x0A00              ; CH=10 top row, CL=0 left column
        mov     dx, 0x0E4F              ; DH=14 bottom row, DL=79 right
        int     0x10

        ; 4. AH=06h AL=1: scroll the whole window up one line
        mov     ax, 0x0601
        mov     bh, 0x30
        mov     cx, 0x0000              ; CH=0, CL=0: top-left
        mov     dx, 0x184F              ; DH=24, DL=79: bottom-right
        int     0x10

        cli
        hlt
