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
 */
#include <funnyos/fb.h>
#include <funnyos/bootinfo.h>

#include <funnyos/font8x16.h>

#include <libk/string.h>

/* Upper bounds for the character grid. A 1024x768 framebuffer needs the
 * grid comment below; larger displays still fit comfortably. */
#define CONSOLE_MAX_COLS 256
#define CONSOLE_MAX_ROWS 128

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

    fill_rect(0, 0, g_width, g_height, pack_color(g_bg));
    g_cursor_x = 0;
    g_cursor_y = 0;
}

void fb_set_fg(uint32_t rgb) { g_fg = rgb; }
void fb_set_bg(uint32_t rgb) { g_bg = rgb; }
void fb_reset_color(void)    { g_fg = 0xE8E8E8; g_bg = 0x101014; }

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
        draw_glyph(g_cursor_x, g_cursor_y, ' ', fg, bg);
        return;

    case '\t':
        /* Advance to the next 8-column stop, blanking what is crossed. */
        do {
            cell_set(g_cursor_x, g_cursor_y, ' ');
            draw_glyph(g_cursor_x, g_cursor_y, ' ', fg, bg);
            g_cursor_x++;
        } while ((g_cursor_x & 7) && g_cursor_x < g_cols);
        break;

    default:
        cell_set(g_cursor_x, g_cursor_y, c);
        draw_glyph(g_cursor_x, g_cursor_y, c, fg, bg);
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
