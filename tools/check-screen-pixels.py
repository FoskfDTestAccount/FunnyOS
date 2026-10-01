#!/usr/bin/env python3
"""
Read a screendump back and ask whether the guest's page is on it.

This is the other half of W2's acceptance. Everything else about "a .COM's
text reaches the screen" is checked from the inside: the interpreter
asserts the page the guest left in its own memory, cell by cell, against
what the manual and the sample's own header say, and the kernel reports
what it was handed and where it put it. None of that can tell whether a
single pixel was ever written, because none of it looks at the screen.

So this opens the screendump and looks at it.

-------------------------------------------------------------------------
What it checks, and what it deliberately does not

The expected picture is built from three things:

  * where the kernel says it put the page and how big the console is,
    both read out of the serial log -- so no geometry is written down
    here and a machine with a different framebuffer needs no change;
  * the text the interpreter reports is in row 0 of the page, also from
    the log, which is the guest's own memory;
  * the glyphs, from kernel/console/font8x16.c -- the same table the
    kernel draws with.

That last one is the honest limitation and it is worth stating plainly:
a wrong font would make the prediction and the rendering wrong together,
and this would not notice. What it does check is everything between the
guest's memory and the framebuffer -- that the block is at the origin the
kernel claims, that each cell holds the glyph of the character the guest
put there, that the two colours are used consistently and are not the
console's own, that the cursor is drawn where the guest's cursor is, and
that nothing outside the block was disturbed.

The colours are measured rather than assumed: the background is read out
of a cell the page is known to have left blank, and the foreground is the
other colour in the block. What that buys is not depending on the palette
being written down correctly here. What it costs is that **the palette
values themselves are not checked** -- a machine that rendered attribute
7 as orange on green would pass every line below, because the same two
colours would simply be orange and green. The one thing that is checked
is that they are not the console's two, which is what discarding the
attribute byte altogether would look like.

Whether the sixteen entries of the palette are the right sixteen is a
claim from general knowledge rather than from anything in this repository
-- kernel/console/fb.c says so where the table is -- and nothing here
tests it. It is a thing to look at on a screen.

Usage: check-screen-pixels.py <screendump.ppm> <serial.log> <font8x16.c>
"""

import re
import sys


FONT_WIDTH = 8
FONT_HEIGHT = 16
FONT_FIRST = 32
FONT_LAST = 126


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

    for match in re.finditer(r"/\*\s*(\d+)\s+'(.*?)'\s*\*/\s*\{([^}]*)\}",
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

    page_cols, rows, text = field(
        r'screen : \w+ (\d+)x(\d+), row 0 = "(.*)"', log, 'page report')

    page_cols = int(page_cols)
    rows      = int(rows)
    text      = text.rstrip()

    cursor = int(field(r'screen : the cursor is at cell (\d+)', log,
                       'cursor line')[0])

    if page_cols != columns:
        return fail(f'the kernel placed a {columns}-column page and the '
                    f'interpreter reports a {page_cols}-column one')

    if len(text) > columns:
        return fail(f'row 0 is {len(text)} characters wide and the page is '
                    f'{columns}')

    glyphs = read_font(font_path)
    missing = sorted({ord(ch) for ch in text
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

    console_bg = pixels[0:3]
    blank_row = console_bg * fb_w

    outside = 0
    for y in range(fb_h):
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
    # The block is two colours, and they are the cell attribute.

    cell_bg = pixels[((row0 + rows - 1) * cell_h) * stride +
                     (col0 * cell_w) * 3:
                     ((row0 + rows - 1) * cell_h) * stride +
                     (col0 * cell_w) * 3 + 3]

    # A cell the page leaves blank has to be one colour throughout, or the
    # page is not the one hello.asm's header describes. It is the source of
    # the background colour for everything below, which is why it is read
    # first: a colour taken from a table here would make this check agree
    # with the kernel's palette by being the same idea twice.
    for dy in range(cell_h):
        row = pixels[(y0 + (rows - 1) * cell_h + dy) * stride + x0:
                     (y0 + (rows - 1) * cell_h + dy) * stride + x0 + cell_w * 3]
        if row != cell_bg * cell_w:
            return fail(f'the last row of the page is not blank -- cell '
                        f'{rows - 1},0 is not one colour, and every cell of '
                        f'hello.asm\'s page except row 0 is a space')

    ink = None
    for dy in range(cell_h):
        row = pixels[(y0 + dy) * stride + x0:(y0 + dy) * stride + x1]
        for index in range(0, len(row), 3):
            if row[index:index + 3] != cell_bg:
                ink = row[index:index + 3]
                break
        if ink:
            break

    if ink is None:
        return fail('the whole page is one colour -- nothing was drawn on '
                    'it, which is what an unrendered or unwritten '
                    'framebuffer looks like')

    if ink == cell_bg:
        return fail('the page has one colour, so nothing on it can be read')

    # The console's own two colours, in a page that has its own, are what
    # throwing the attribute byte away looks like: fb_draw_page has an fg
    # and a bg in hand either way, and using the wrong pair is one line.
    if cell_bg == console_bg:
        return fail('the page is drawn in the console\'s own background '
                    'colour -- the attribute byte was not used, so what is '
                    'on the screen is the kernel\'s idea of the text and '
                    'not the program\'s')

    print(f'  Page           : {page_cols}x{rows} at cell {col0},{row0} of a '
          f'{grid_cols}x{grid_rows} console')
    print(f'  Colours        : background {cell_bg.hex()}, '
          f'foreground {ink.hex()} (read off the screen, not from a table)')

    # ------------------------------------------------------------------
    # Every cell holds the font's glyph for the character the guest left
    # there, and the cursor is where the guest's cursor is.

    if cursor == 0xFFFF:
        cursor_cell = None
    else:
        cursor_cell = cursor

    mismatches = []
    checked = 0

    for cell_row in range(rows):
        for cell_col in range(columns):
            cell = cell_row * columns + cell_col

            if cell_row == 0 and cell_col < len(text):
                bits = glyphs[ord(text[cell_col])]
            else:
                bits = glyphs[ord(' ')]

            for dy in range(cell_h):
                py = y0 + cell_row * cell_h + dy
                base = py * stride + (x0 + cell_col * cell_w * 3)

                if cell == cursor_cell and dy >= cell_h - 2:
                    wanted = ink * cell_w
                else:
                    wanted = b''.join(
                        ink if bits[dy] & (0x80 >> dx) else cell_bg
                        for dx in range(cell_w))

                checked += 1

                if pixels[base:base + cell_w * 3] != wanted and \
                        len(mismatches) < 4:
                    mismatches.append((cell_row, cell_col, dy,
                                       pixels[base:base + cell_w * 3],
                                       wanted))

    if mismatches:
        for cell_row, cell_col, dy, got, wanted in mismatches:
            shown = text[cell_col] if cell_row == 0 and \
                cell_col < len(text) else ' '

            print(f'  [FAIL] cell {cell_row},{cell_col} scan line {dy} is not '
                  f'the font\'s glyph for {shown!r}')

        print('         a scan line in the page does not match the font, so '
              'the screen is not')
        print('         showing the characters the guest put in its memory '
              '(row 0 of the page:')
        print(f'         "{text}")')
        return 1

    print(f'  Glyphs         : {checked} scan lines match the font for the '
          f'page\'s characters')
    print(f'  Cursor         : '
          + ('none' if cursor_cell is None
             else f'drawn at cell {cursor_cell} '
                  f'({cursor_cell // columns},{cursor_cell % columns})'))
    print(f'  Text           : "{text}"')
    print('  [ok]   the guest\'s page is on the screen, character for '
          'character')

    return 0


if __name__ == '__main__':
    sys.exit(main())
