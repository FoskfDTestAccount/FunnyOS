#include <funnyos/fb.h>
#include <funnyos/bootinfo.h>

#include <funnyos/font8x16.h>

#include <libk/string.h>

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static uint8_t *g_addr;        /* framebuffer base (virtual) */
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

/* ------------------------------------------------------------------ */
/* Pixel access                                                        */
/* ------------------------------------------------------------------ */

/* Pack a 24-bit RGB value into the framebuffer's native pixel format
 * using the masks the bootloader reported. */
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
        /* Little-endian 24bpp: low byte first. */
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
    /* Anything outside printable ASCII renders as a filled block, which
     * makes encoding mistakes visible rather than silent. */
    unsigned idx;
    if ((unsigned char)c >= FONT8X16_FIRST && (unsigned char)c <= FONT8X16_LAST)
        idx = (unsigned char)c - FONT8X16_FIRST;
    else
        idx = 0;

    const uint8_t *rows = g_font8x16[idx];
    uint64_t x0 = cell_x * FONT8X16_WIDTH;
    uint64_t y0 = cell_y * FONT8X16_HEIGHT;

    for (uint64_t ry = 0; ry < FONT8X16_HEIGHT; ry++) {
        uint8_t bits = rows[ry];
        for (uint64_t rx = 0; rx < FONT8X16_WIDTH; rx++) {
            uint32_t color = (bits & (0x80 >> rx)) ? fg : bg;
            put_pixel(x0 + rx, y0 + ry, color);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Scrolling                                                           */
/* ------------------------------------------------------------------ */

/*
 * Move the whole text area up by one character row and blank the last
 * one. Copying the framebuffer wholesale is the simple approach; it is
 * only paid when the screen actually overflows.
 */
static void scroll_up(void)
{
    uint64_t line_bytes = FONT8X16_HEIGHT * g_pitch;

    memmove(g_addr,
            g_addr + line_bytes,
            (size_t)(g_pitch * g_height - line_bytes));

    fill_rect(0, g_height - FONT8X16_HEIGHT, g_width, FONT8X16_HEIGHT,
              pack_color(g_bg));
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
        draw_glyph(g_cursor_x, g_cursor_y, ' ', pack_color(g_fg), pack_color(g_bg));
        return;

    case '\t':
        /* Advance to the next 8-column stop. */
        g_cursor_x = (g_cursor_x + 8) & ~(uint64_t)7;
        break;

    default:
        draw_glyph(g_cursor_x, g_cursor_y, c, pack_color(g_fg), pack_color(g_bg));
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
