/*
 * Linear framebuffer text console.
 *
 * Limine hands us a linear framebuffer (GOP on UEFI, VBE on BIOS) mapped
 * into the HHDM. This driver turns it into a plain character grid: 8x16
 * cells, cursor tracking, line wrap and scrollback.
 *
 * ---------------------------------------------------------------------
 * Why the framebuffer is never read back
 *
 * The boot protocol maps framebuffer regions as write-combining (WC),
 * unlike every other HHDM region, which is write-back. That choice is
 * correct -- WC is what a framebuffer wants -- but it has a sharp edge:
 * reads from WC memory bypass the cache entirely and cost a full
 * uncached transaction each.
 *
 * The obvious way to scroll is to memmove the framebuffer up by one text
 * row. That reads and writes 3 MiB. Written that way, this console took
 * roughly 60 seconds to print a boot log, because the read half was
 * ~3 million uncached transactions per scrolled line.
 *
 * So the framebuffer here is write-only. The authoritative screen content
 * lives in `g_cells`, an ordinary cached array. Scrolling moves that
 * array -- 6 KiB, effectively free -- and then repaints, which is pure
 * writes and therefore the fast path for WC memory.
 *
 * Anything added to this file must respect that: never read from g_addr.
 *
 * ---------------------------------------------------------------------
 * Two kinds of drawing
 *
 * The console's own log is one character grid with one colour, and it is
 * what almost everything in the kernel prints into. A program that needs
 * a screen of its own -- the DOS emulator, which puts a guest's text page
 * where a person can see it -- gets the other path: fb_draw_page, which
 * paints a rectangle of the framebuffer from cells the caller owns, with
 * the caller's colours.
 *
 * Nothing coordinates the two here. A program holding the screen turns the
 * log's *drawing* off with fb_set_output, and the grid keeps being
 * maintained while it is off, so the log is intact when it comes back.
 * Which one is on is decided in screen.c.
 */
#include <funnyos/fb.h>
#include <funnyos/bootinfo.h>

#include <funnyos/font8x16.h>

#include <libk/string.h>

/* Upper bounds for the character grid. A 1024x768 framebuffer needs the
 * grid comment below; larger displays still fit comfortably. */
#define CONSOLE_MAX_COLS 256
#define CONSOLE_MAX_ROWS 128

/* How thick the underline under a guest's cursor cell is, in scan lines.
 * Two of the sixteen is enough to read as a cursor and thin enough not to
 * eat the descenders of the character above it. */
#define FB_CURSOR_HEIGHT 2

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static uint8_t *g_addr;        /* framebuffer base (virtual, write-only) */
static uint64_t g_pitch;       /* bytes per scanline */
static uint64_t g_width;       /* pixels */
static uint64_t g_height;      /* pixels */
static uint8_t  g_bytes_pp;    /* bytes per pixel */
static uint8_t  g_red_shift,   g_red_size;
static uint8_t  g_green_shift, g_green_size;
static uint8_t  g_blue_shift,  g_blue_size;

static uint64_t g_cols;        /* character columns */
static uint64_t g_rows;        /* character rows */
static uint64_t g_cursor_x;
static uint64_t g_cursor_y;

static bool     g_ready;

/* Whether the console's own output is reaching the screen. See fb_set_output
 * in the header: the grid is maintained either way. */
static bool     g_output = true;

static uint32_t g_fg = 0xE8E8E8;   /* near-white */
static uint32_t g_bg = 0x101014;   /* near-black, faintly blue */

/* The authoritative copy of what is on screen. */
static char g_cells[CONSOLE_MAX_ROWS * CONSOLE_MAX_COLS];

/* ------------------------------------------------------------------ */
/* Pixel access (write-only)                                           */
/* ------------------------------------------------------------------ */

static uint32_t pack_color(uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >> 8) & 0xFF;
    uint32_t b = rgb & 0xFF;

    return ((r >> (8 - g_red_size))   << g_red_shift)   |
           ((g >> (8 - g_green_size)) << g_green_shift) |
           ((b >> (8 - g_blue_size))  << g_blue_shift);
}

static void put_pixel(uint64_t x, uint64_t y, uint32_t color)
{
    uint8_t *p = g_addr + y * g_pitch + x * g_bytes_pp;

    switch (g_bytes_pp) {
    case 4:
        *(volatile uint32_t *)p = color;
        break;
    case 3:
        p[0] = (uint8_t)(color);
        p[1] = (uint8_t)(color >> 8);
        p[2] = (uint8_t)(color >> 16);
        break;
    case 2:
        *(volatile uint16_t *)p = (uint16_t)color;
        break;
    default:
        break;
    }
}

static void fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t color)
{
    for (uint64_t dy = 0; dy < h; dy++)
        for (uint64_t dx = 0; dx < w; dx++)
            put_pixel(x + dx, y + dy, color);
}

static void draw_glyph(uint64_t cell_x, uint64_t cell_y, char c,
                       uint32_t fg, uint32_t bg)
{
    /* Anything outside printable ASCII renders as a blank rather than
     * whatever happens to sit at index 0, which makes encoding mistakes
     * show up as missing characters instead of wrong ones. */
    unsigned idx = 0;
    if ((unsigned char)c >= FONT8X16_FIRST && (unsigned char)c <= FONT8X16_LAST)
        idx = (unsigned char)c - FONT8X16_FIRST;

    const uint8_t *rows = g_font8x16[idx];
    uint64_t x0 = cell_x * FONT8X16_WIDTH;
    uint64_t y0 = cell_y * FONT8X16_HEIGHT;

    for (uint64_t ry = 0; ry < FONT8X16_HEIGHT; ry++) {
        uint8_t bits = rows[ry];
        for (uint64_t rx = 0; rx < FONT8X16_WIDTH; rx++)
            put_pixel(x0 + rx, y0 + ry, (bits & (0x80 >> rx)) ? fg : bg);
    }
}

/*
 * The console's own glyphs, drawn only while the console has the screen.
 *
 * The grid is updated whether or not this draws -- that is the whole point
 * of g_output, and the reason this is a separate function rather than a
 * check inside draw_glyph: the calls that update the grid and the calls
 * that paint it are the same calls, and only the painting is conditional.
 */
static void paint_cell(uint64_t cell_x, uint64_t cell_y, char c,
                       uint32_t fg, uint32_t bg)
{
    if (g_output)
        draw_glyph(cell_x, cell_y, c, fg, bg);
}

/*
 * The sixteen colours of an IBM PC text attribute, as the values a VGA
 * digital-to-analogue converter is loaded with.
 *
 * These are the standard table -- the first eight are the CGA palette
 * doubled to 0xAA, the second eight the same with 0x55 added -- and the
 * repository has no document that states them: docs/dos-refs.md is about
 * services and interrupts. So they are from general knowledge rather than
 * checked against a source in the tree, and the one corroboration
 * available locally is boot/limine.conf's own defaults, which name 0x00aa00
 * and 0x00aaaa as green and cyan.
 *
 * A wrong value here is visible rather than subtle -- the text comes out
 * the wrong colour -- which is the redeeming feature of a table nobody can
 * check inside this repository.
 */
static const uint32_t g_text_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

/* ------------------------------------------------------------------ */
/* Grid operations                                                     */
/* ------------------------------------------------------------------ */

static inline char cell_get(uint64_t col, uint64_t row)
{
    return g_cells[row * g_cols + col];
}

static inline void cell_set(uint64_t col, uint64_t row, char c)
{
    g_cells[row * g_cols + col] = c;
}

/*
 * Repaint the whole screen from the character grid.
 *
 * Costly in pixel count but cheap in practice: it is writes only, and WC
 * memory is built for writes. The alternative -- moving the framebuffer
 * itself -- costs uncached reads, which is what made this console slow in
 * the first place.
 */
static void repaint(void)
{
    uint32_t fg = pack_color(g_fg);
    uint32_t bg = pack_color(g_bg);

    for (uint64_t r = 0; r < g_rows; r++)
        for (uint64_t c = 0; c < g_cols; c++)
            draw_glyph(c, r, cell_get(c, r), fg, bg);
}

static void scroll_up(void)
{
    /* Move the text up one row. This is the actual scroll; the framebuffer
     * simply follows in repaint(). */
    memmove(g_cells,
            g_cells + g_cols,
            (size_t)((g_rows - 1) * g_cols));
    memset(g_cells + (g_rows - 1) * g_cols, ' ', (size_t)g_cols);

    if (g_output)
        repaint();
}

/* ------------------------------------------------------------------ */
/* Public interface                                                    */
/* ------------------------------------------------------------------ */

bool fb_init(void)
{
    struct limine_framebuffer *fb = bootinfo_framebuffer();
    if (!fb || !fb->address)
        return false;

    g_addr   = (uint8_t *)fb->address;
    g_pitch  = fb->pitch;
    g_width  = fb->width;
    g_height = fb->height;

    g_red_shift   = fb->red_mask_shift;
    g_red_size    = fb->red_mask_size;
    g_green_shift = fb->green_mask_shift;
    g_green_size  = fb->green_mask_size;
    g_blue_shift  = fb->blue_mask_shift;
    g_blue_size   = fb->blue_mask_size;

    switch (fb->bpp) {
    case 32: g_bytes_pp = 4; break;
    case 24: g_bytes_pp = 3; break;
    case 16: g_bytes_pp = 2; break;
    default: return false;      /* unsupported depth; stay on serial */
    }

    g_cols = g_width  / FONT8X16_WIDTH;
    g_rows = g_height / FONT8X16_HEIGHT;

    if (g_cols == 0 || g_rows == 0)
        return false;
    if (g_cols > CONSOLE_MAX_COLS || g_rows > CONSOLE_MAX_ROWS) {
        /* Refuse rather than scribble past the grid. Falling back to
         * serial-only is better than corrupting memory. */
        return false;
    }

    g_ready = true;
    fb_clear();

    return true;
}

bool fb_is_ready(void)
{
    return g_ready;
}

void fb_clear(void)
{
    if (!g_ready)
        return;

    memset(g_cells, ' ', (size_t)(g_rows * g_cols));

    if (g_output)
        fill_rect(0, 0, g_width, g_height, pack_color(g_bg));

    g_cursor_x = 0;
    g_cursor_y = 0;
}

void fb_set_fg(uint32_t rgb) { g_fg = rgb; }
void fb_set_bg(uint32_t rgb) { g_bg = rgb; }
void fb_reset_color(void)    { g_fg = 0xE8E8E8; g_bg = 0x101014; }

void fb_set_output(bool enabled)
{
    g_output = enabled;
}

void fb_repaint(void)
{
    if (g_ready)
        repaint();
}

void fb_fill_screen(void)
{
    if (!g_ready)
        return;

    fill_rect(0, 0, g_width, g_height, pack_color(g_bg));
}

bool fb_grid_size(uint64_t *cols, uint64_t *rows)
{
    if (!g_ready)
        return false;

    if (cols)
        *cols = g_cols;
    if (rows)
        *rows = g_rows;

    return true;
}

void fb_draw_page(uint64_t col, uint64_t row, uint64_t columns,
                  uint64_t rows, const uint8_t *cells, uint16_t cursor)
{
    if (!g_ready || !cells || columns == 0)
        return;

    for (uint64_t r = 0; r < rows; r++) {
        if (row + r >= g_rows)
            break;

        for (uint64_t c = 0; c < columns; c++) {
            if (col + c >= g_cols)
                break;

            uint64_t at  = (r * columns + c) * 2u;
            uint8_t  ch  = cells[at];
            uint8_t  att = cells[at + 1u];

            draw_glyph(col + c, row + r, (char)ch,
                       pack_color(g_text_palette[att & 0x0Fu]),
                       pack_color(g_text_palette[(att >> 4) & 0x07u]));
        }
    }

    if (cursor == FB_NO_CURSOR)
        return;

    uint64_t cell_col = cursor % columns;
    uint64_t cell_row = cursor / columns;

    if (cell_row >= rows || col + cell_col >= g_cols || row + cell_row >= g_rows)
        return;

    /*
     * An underline, in the foreground colour the cell itself is using.
     *
     * A cursor on a cell whose character is a space is the case that makes
     * this worth doing properly: a block cursor would be visible there too,
     * but it would hide the character under it, and the guest's cursor is
     * usually sitting one past the last character it wrote.
     */
    uint8_t  att = cells[(cell_row * columns + cell_col) * 2u + 1u];
    uint32_t fg  = pack_color(g_text_palette[att & 0x0Fu]);

    uint64_t x0 = (col + cell_col) * FONT8X16_WIDTH;
    uint64_t y0 = (row + cell_row) * FONT8X16_HEIGHT +
                  FONT8X16_HEIGHT - FB_CURSOR_HEIGHT;

    for (uint64_t dy = 0; dy < FB_CURSOR_HEIGHT; dy++)
        for (uint64_t dx = 0; dx < FONT8X16_WIDTH; dx++)
            put_pixel(x0 + dx, y0 + dy, fg);
}

void fb_putc(char c)
{
    if (!g_ready)
        return;

    uint32_t fg = pack_color(g_fg);
    uint32_t bg = pack_color(g_bg);

    switch (c) {
    case '\n':
        g_cursor_x = 0;
        g_cursor_y++;
        break;

    case '\r':
        g_cursor_x = 0;
        return;                 /* no vertical movement */

    case '\b':
        if (g_cursor_x > 0)
            g_cursor_x--;
        cell_set(g_cursor_x, g_cursor_y, ' ');
        paint_cell(g_cursor_x, g_cursor_y, ' ', fg, bg);
        return;

    case '\t':
        /* Advance to the next 8-column stop, blanking what is crossed. */
        do {
            cell_set(g_cursor_x, g_cursor_y, ' ');
            paint_cell(g_cursor_x, g_cursor_y, ' ', fg, bg);
            g_cursor_x++;
        } while ((g_cursor_x & 7) && g_cursor_x < g_cols);
        break;

    default:
        cell_set(g_cursor_x, g_cursor_y, c);
        paint_cell(g_cursor_x, g_cursor_y, c, fg, bg);
        g_cursor_x++;
        break;
    }

    if (g_cursor_x >= g_cols) {
        g_cursor_x = 0;
        g_cursor_y++;
    }

    if (g_cursor_y >= g_rows) {
        scroll_up();
        g_cursor_y = g_rows - 1;
    }
}
