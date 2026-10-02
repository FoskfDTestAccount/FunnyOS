#!/usr/bin/env python3
"""
Read a screendump back and ask whether the guest's page is on it.

This is the half of the acceptance that no amount of internal checking can
reach. Everything else about "a program's text is on the screen" is
asserted from the inside: the interpreter compares the page the guest left
in its own memory against the manual, the kernel reports what it was handed
and where it put it, and a host suite compares the loader's 256 bytes
against a reference document. None of that can tell whether a single pixel
was ever written, because none of it looks at a screen.

So this opens the screendump and looks at it.

-------------------------------------------------------------------------
What it checks, and the one thing it deliberately does not

The expected picture is built from three sources:

  * where the kernel says it put the page and how big the console is, both
    read out of the serial log -- so no geometry is written down here and a
    machine with a different framebuffer needs no change;
  * the rows the interpreter reports, also from the log, which it read out
    of the guest's own memory;
  * the glyphs, from kernel/console/font8x16.c -- the same table the kernel
    draws with.

That last one is the honest limitation and it is worth stating plainly: a
wrong font would make the prediction and the rendering wrong together, and
this would not notice. What it does check is everything between the guest's
memory and the framebuffer -- that the block is at the origin the kernel
claims, that every cell holds the glyph of the character the program put
there, that the two colours are a cell attribute used consistently and are
not the console's own, that the cursor is drawn where the program's cursor
is, and that nothing outside the block was disturbed.

The colours are measured rather than assumed: the background is read out of
a cell the page is known to have left blank, and the foreground is the
other colour in the block. What that buys is not depending on the palette
being written down correctly here. What it costs is that **the palette
values themselves are not checked** -- a machine that rendered attribute 7
as orange on green would pass every line below, because the same two
colours would simply be orange and green. The one thing that is checked is
that they are not the console's two, which is what discarding the attribute
byte altogether would look like.

Whether the sixteen entries of the palette are the right sixteen is a claim
from general knowledge rather than from anything in this repository --
kernel/console/fb.c says so where the table is -- and nothing here tests it.
It is a thing to look at on a screen.

Usage: check-screen-pixels.py <screendump.ppm> <serial.log> <font8x16.c>

The log lines it reads, all written by user/vm/vm.c:

    Framebuffer    : 1024x768, 32 bpp, ...          -- the kernel, at boot
    Screen         : a program has the screen, 80 columns at cell 24,11 of
                     a 128x48 console
    screen : <name> 80x25                           -- the page's shape
    screen : <name> row 0 = "..."                   -- one per non-blank row
    screen : <name> has 22 row(s) with something on them
    screen : the cursor is at cell 22
"""

import re
import sys


FONT_WIDTH = 8
FONT_HEIGHT = 16
FONT_FIRST = 32
FONT_LAST = 126

CURSOR_HEIGHT = 2       # scan lines under the cursor cell


def fail(message):
    print(f'  [FAIL] {message}')
    return 1


def read_ppm(path):
    """Return (width, height, pixels) for a binary PPM, pixels as bytes."""
    with open(path, 'rb') as handle:
        data = handle.read()

    if data[:2] != b'P6':
        raise SystemExit(f'{path}: not a binary PPM (magic {data[:2]!r})')

    fields = []
    index = 2

    while len(fields) < 3:
        while index < len(data) and data[index:index + 1].isspace():
            index += 1
        if data[index:index + 1] == b'#':
            while index < len(data) and data[index:index + 1] != b'\n':
                index += 1
            continue
        end = index
        while end < len(data) and not data[end:end + 1].isspace():
            end += 1
        fields.append(int(data[index:end]))
        index = end

    width, height, maxval = fields
    if maxval != 255:
        raise SystemExit(f'{path}: {maxval} is not a maxval this reads')

    pixels = data[index + 1:index + 1 + width * height * 3]
    if len(pixels) != width * height * 3:
        raise SystemExit(f'{path}: truncated, {len(pixels)} of '
                         f'{width * height * 3} pixel bytes')

    return width, height, pixels


def read_font(path):
    """Return {codepoint: [row bytes]} from the generated font table."""
    text = open(path, encoding='utf-8', errors='replace').read()
    glyphs = {}

    for match in re.finditer(r"/\*\s*(\d+)\s+([\s\S]*?)\s*\*/\s*\{([^}]*)\}",
                             text):
        code = int(match.group(1))
        glyphs[code] = [int(v, 16)
                        for v in re.findall(r'0x([0-9a-fA-F]{2})',
                                            match.group(3))]

    return glyphs


def read_log(path):
    with open(path, encoding='utf-8', errors='replace') as handle:
        return handle.read()


def field(pattern, log, what):
    match = re.search(pattern, log)
    if not match:
        raise SystemExit(f'the serial log has no {what}, so nothing about '
                         f'the screen can be located')
    return match.groups()


def main():
    ppm_path, log_path, font_path = sys.argv[1], sys.argv[2], sys.argv[3]

    log = read_log(log_path)

    fb_w, fb_h = (int(v) for v in field(
        r'Framebuffer\s+:\s+(\d+)x(\d+),', log, 'framebuffer line'))

    columns, col0, row0, grid_cols, grid_rows = (int(v) for v in field(
        r'Screen\s+:\s+a program has the screen, (\d+) columns at cell '
        r'(\d+),(\d+) of a (\d+)x(\d+) console', log, 'placement line'))

    page_cols, rows = (int(v) for v in field(
        r'screen : \w+ (\d+)x(\d+)', log, 'page shape'))

    page = {}
    for match in re.finditer(r'screen : \w+ row (\d+) = "(.*)"', log):
        page[int(match.group(1))] = match.group(2)

    if not page:
        raise SystemExit('the serial log reports no rows of the page, so '
                         'there is nothing to look for on the screen')

    cursor = int(field(r'screen : the cursor is at cell (\d+)', log,
                       'cursor line')[0])

    if page_cols != columns:
        return fail(f'the kernel placed a {columns}-column page and the '
                    f'interpreter reports a {page_cols}-column one')

    out_of_range = [r for r in page if r >= rows]
    if out_of_range:
        return fail(f'the interpreter reports rows {out_of_range} and the '
                    f'page has {rows}')

    glyphs = read_font(font_path)
    missing = sorted({ord(ch) for line in page.values() for ch in line
                      if not FONT_FIRST <= ord(ch) <= FONT_LAST})
    if missing:
        return fail(f'the page holds characters the font has no glyph for: '
                    f'{missing}')

    width, height, pixels = read_ppm(ppm_path)

    if (width, height) != (fb_w, fb_h):
        return fail(f'the screendump is {width}x{height} and the kernel was '
                    f'given a {fb_w}x{fb_h} framebuffer')

    cell_w = fb_w // grid_cols
    cell_h = fb_h // grid_rows

    if (cell_w, cell_h) != (FONT_WIDTH, FONT_HEIGHT):
        return fail(f'a console cell is {cell_w}x{cell_h} pixels and the '
                    f'font is {FONT_WIDTH}x{FONT_HEIGHT}; the two are not the '
                    f'same picture')

    # ------------------------------------------------------------------
    # Nothing outside the block was touched.

    stride = fb_w * 3
    x0 = col0 * cell_w * 3
    x1 = (col0 + columns) * cell_w * 3
    y0 = row0 * cell_h
    y1 = (row0 + rows) * cell_h

    tab_rows = 1 if re.search(r'Tabs\s+: pages [2-9], top rows 1', log) else 0
    # The reserved strip is verified separately by the desktop suite. Every
    # other pixel outside the guest page retains the old strict assertion.
    console_bg = pixels[tab_rows * cell_h * stride:tab_rows * cell_h * stride + 3]
    blank_row = console_bg * fb_w

    outside = 0
    for y in range(tab_rows * cell_h, fb_h):
        row = pixels[y * stride:(y + 1) * stride]

        if y < y0 or y >= y1:
            if row != blank_row:
                outside += 1
        elif row[:x0] != console_bg * (x0 // 3) or \
                row[x1:] != console_bg * ((len(row) - x1) // 3):
            outside += 1

    if outside:
        return fail(f'{outside} scan line(s) outside the guest\'s page are '
                    f'not the console\'s background colour -- the page was '
                    f'placed somewhere other than where the kernel said, or '
                    f'it was drawn outside its own rectangle')

    # ------------------------------------------------------------------
    # The two colours.
    #
    # Taken from the pixels of the first row the interpreter reports as
    # having text on it -- not from a blank cell somewhere in the block.
    #
    # That choice was made twice. The first version read the background out
    # of the block's last row, which is a cell the page leaves blank and is
    # therefore all one colour in any correct rendering; it is also a cell
    # a machine that never drew the page leaves all one colour, and the
    # colour in that case is the console's. So a page with nothing drawn on
    # it and a page drawn in the console's own colours produced the same
    # measurement and the same message -- and the message named the second
    # cause while the fault was the first. That was found by injection: a
    # change that drew only row 0 was reported as "the attribute byte was
    # not used", which is a different bug in a different file.
    #
    # Reading the colours out of a row that is *supposed* to have text on
    # it separates the two: a row nobody drew has one colour, and a row
    # drawn in the wrong colours has two that are not the page's.
    first = min(page)

    colours = {}
    for dy in range(cell_h):
        stripe = pixels[(y0 + first * cell_h + dy) * stride + x0:
                        (y0 + first * cell_h + dy) * stride + x1]
        for index in range(0, len(stripe), 3):
            pixel = stripe[index:index + 3]
            colours[pixel] = colours.get(pixel, 0) + 1

    if len(colours) < 2:
        return fail(f'row {first} is the first row the interpreter reports '
                    f'as having text on it, and it is one colour -- nothing '
                    f'was drawn there, so there is nothing to read off the '
                    f'screen')

    ranked = sorted(colours.items(), key=lambda item: -item[1])
    cell_bg, ink = ranked[0][0], ranked[1][0]

    print(f'  Page           : {page_cols}x{rows} at cell {col0},{row0} of a '
          f'{grid_cols}x{grid_rows} console')
    print(f'  Colours        : background {cell_bg.hex()}, '
          f'foreground {ink.hex()} (read off the screen, not from a table)')

    # ------------------------------------------------------------------
    # Every cell holds the font's glyph for the character the program left
    # there, and the cursor is where the program's cursor is.

    cursor_cell = None if cursor == 0xFFFF else cursor

    mismatches = []
    checked = 0

    for cell_row in range(rows):
        line = page.get(cell_row, '')

        for cell_col in range(columns):
            cell = cell_row * columns + cell_col

            if cell_col < len(line):
                bits = glyphs[ord(line[cell_col])]
            else:
                bits = glyphs[ord(' ')]

            for dy in range(cell_h):
                py = y0 + cell_row * cell_h + dy
                base = py * stride + (x0 + cell_col * cell_w * 3)

                if cell == cursor_cell and dy >= cell_h - CURSOR_HEIGHT:
                    wanted = ink * cell_w
                else:
                    wanted = b''.join(
                        ink if bits[dy] & (0x80 >> dx) else cell_bg
                        for dx in range(cell_w))

                checked += 1

                if pixels[base:base + cell_w * 3] != wanted and \
                        len(mismatches) < 4:
                    mismatches.append((cell_row, cell_col, dy,
                                       pixels[base:base + cell_w * 3]))

    if mismatches:
        for cell_row, cell_col, dy, got in mismatches:
            line = page.get(cell_row, '')
            shown = line[cell_col] if cell_col < len(line) else ' '

            print(f'  [FAIL] cell {cell_row},{cell_col} scan line {dy} is not '
                  f'the font\'s glyph for {shown!r} '
                  f'(the screen has {got.hex()})')

        print('         a scan line in the page does not match the font, so '
              'the screen is not')
        print('         showing the characters the program put in its '
              'memory. Row 0 of the')
        print(f'         page is "{page.get(0, "")}"')
        return 1

    # ------------------------------------------------------------------
    # And last, because it is the least specific of the claims: the console
    # has two colours of its own, and a page drawn in them is a page whose
    # attribute byte was discarded.
    #
    # This comes after the glyph comparison deliberately. Both faults look
    # the same from a blank cell, and the glyph comparison tells them apart
    # -- so the vague message is only reached by a screen whose characters
    # are all correct, which is what "the colours are wrong" means.

    if cell_bg == console_bg:
        return fail('every character on the page is the font\'s, and the '
                    'colours are the console\'s own -- the attribute byte '
                    'was not used, so what is on the screen is the kernel\'s '
                    'idea of the text and not the program\'s')

    print(f'  Glyphs         : {checked} scan lines match the font for the '
          f'page\'s {len(page)} non-blank row(s)')
    print(f'  Cursor         : '
          + ('none' if cursor_cell is None
             else f'drawn at cell {cursor_cell} '
                  f'({cursor_cell // columns},{cursor_cell % columns})'))

    for row in sorted(page)[:4]:
        print(f'  Row {row:<11}: "{page[row]}"')
    if len(page) > 4:
        print(f'  ...            : and {len(page) - 4} more non-blank row(s)')

    print('  [ok]   the program\'s page is on the screen, character for '
          'character')

    return 0


if __name__ == '__main__':
    sys.exit(main())
