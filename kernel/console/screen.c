/*
 * Who is on the screen. See the header for what this is for and why it is
 * a layer rather than a pair of functions in fb.c.
 */
#include <funnyos/screen.h>

#include <funnyos/fb.h>
#include <funnyos/kprintf.h>
#include <funnyos/syscall.h>

/*
 * The two spellings of "no cursor" have to be one value.
 *
 * fb.h cannot use the system call header's name -- the framebuffer is
 * below the interface and does not know it exists -- and the system call
 * header cannot use fb.h's, because user programs include it and have no
 * framebuffer. So the value is written twice and the compiler is asked
 * whether they still agree, which is the difference between a duplicated
 * fact and a duplicated fact somebody is watching.
 */
_Static_assert(FB_NO_CURSOR == SCREEN_NO_CURSOR,
               "the framebuffer and the system call interface disagree "
               "about what 'no cursor' is");

/*
 * Who holds the screen, and what the holder last put on it.
 *
 * `owner` is the process, not a flag, because releasing on teardown has to
 * know whether this process is the one to release. A boolean would make a
 * child's death drop a parent's hold.
 */
static const struct process *g_owner;

static unsigned g_columns;      /* columns of the page last presented   */
static uint64_t g_origin_col;   /* where that page sits on the screen   */
static uint64_t g_origin_row;
static uint16_t g_cursor;
static uint64_t g_presents;

/*
 * Everything the holder's tenure consists of, cleared together.
 *
 * It is always cleared as a set -- on release, on the kernel taking the
 * screen back, and when a new holder arrives -- because these variables
 * only mean anything as a set. Five variables cleared by three copies of
 * the same five assignments is three chances for the next one added to be
 * left out of two of them.
 *
 * The origin is cleared along with the rest even though nothing reads it
 * while g_columns is zero. "Nothing reads it" is exactly the kind of
 * statement that stops being true, and a stale screen coordinate is a
 * quiet thing to draw from.
 */
static void forget_the_holder(void)
{
    g_owner      = NULL;
    g_columns    = 0;
    g_origin_col = 0;
    g_origin_row = 0;
    g_cursor     = FB_NO_CURSOR;
    g_presents   = 0;
}

bool screen_ready(void)
{
    return fb_is_ready();
}

/*
 * Where a page `columns` wide, of SCREEN_ROWS rows, goes on this screen.
 *
 * Centred, both ways. A real DOS machine's text page filled the display
 * because the display was the same size as the page; this console is a
 * hundred and twenty-eight cells across and forty-eight down, so an eighty
 * by twenty-five page in the corner of it looks like a window onto
 * something else rather than like a screen. Centred is what a person
 * expects and it costs one division.
 *
 * A page wider or taller than the screen is not centred, because it cannot
 * be: it starts at the origin and fb_draw_page clips it. That is a
 * forty-column page on a small framebuffer, and refusing to draw it at all
 * would be worse than showing the part that fits.
 */
static void place_page(unsigned columns)
{
    uint64_t cols = 0;
    uint64_t rows = 0;

    if (!fb_grid_size(&cols, &rows)) {
        g_origin_col = 0;
        g_origin_row = 0;
        return;
    }

    g_origin_col = cols > columns     ? (cols - columns) / 2u     : 0u;
    g_origin_row = rows > SCREEN_ROWS ? (rows - SCREEN_ROWS) / 2u : 0u;

    /*
     * Said when the placement is decided, not when it is given up.
     *
     * The mode that never gives the screen back is why: a program that
     * leaves its page up and waits for a key is a program whose placement
     * has to be knowable while it waits, and a listener reading the serial
     * log has nothing else to go on. See tools/run-screen-test.sh, which
     * is exactly that listener -- and which needs the console's size as
     * well as the page's, to turn cells into pixels without being told
     * what this machine's character cells measure.
     */
    kprintf("  Screen         : a program has the screen, %u columns at cell "
            "%llu,%llu of a %llux%llu console\n", columns,
            (unsigned long long)g_origin_col,
            (unsigned long long)g_origin_row,
            (unsigned long long)cols, (unsigned long long)rows);
}

bool screen_acquire(const struct process *who)
{
    if (!who)
        return false;

    if (g_owner)
        return g_owner == who;

    if (!fb_is_ready())
        return false;

    forget_the_holder();
    g_owner = who;

    /*
     * The log stops drawing before the screen is cleared, in that order.
     * The other way round leaves a window in which the console could print
     * into a screen it no longer owns -- and on this machine that window is
     * a timer tick wide.
     */
    fb_set_output(false);
    fb_fill_screen();

    return true;
}

bool screen_present(const uint8_t *cells, unsigned columns, uint16_t cursor)
{
    if (!g_owner || !cells)
        return false;

    if (columns == 0 || columns > SCREEN_MAX_COLUMNS)
        return false;

    /*
     * A page of a different width is a different page on a different
     * screen, so the geometry is worked out again and what was there is
     * cleared -- otherwise a guest that switched to forty columns would
     * leave the right-hand half of its old page visible beside the new
     * one.
     */
    if (columns != g_columns) {
        g_columns = columns;
        place_page(columns);
        fb_fill_screen();
    }

    fb_draw_page(g_origin_col, g_origin_row, g_columns, SCREEN_ROWS,
                 cells, cursor);

    g_cursor = cursor;
    g_presents++;

    return true;
}

/*
 * Put the log back, and say what the program put on the screen.
 *
 * The summary is printed into the log rather than to the screen directly,
 * so it lands at the log's own cursor in the log's own colour and is
 * followed by whatever the kernel prints next. It is printed BEFORE
 * fb_set_output(true) for the same reason screen_acquire turns the output
 * off first: between the two lines the log would be drawing over a screen
 * it does not yet own, one cell at a time, at its old cursor.
 *
 * What it reports is what the kernel was handed and where it put it, which
 * is the half of "it reached the screen" that the kernel knows. That the
 * pixels are there is a different question and is asked differently -- see
 * tools/run-screen-test.sh, which reads them back out of a screendump.
 */
static void hand_back_to_the_log(void)
{
    if (g_presents) {
        kprintf("  Screen         : %llu page(s) presented by a program, "
                "the last %u columns at cell %llu,%llu\n",
                (unsigned long long)g_presents, g_columns,
                (unsigned long long)g_origin_col,
                (unsigned long long)g_origin_row);
    }

    forget_the_holder();

    fb_set_output(true);
    fb_repaint();
}

void screen_release(void)
{
    if (!g_owner)
        return;

    hand_back_to_the_log();
}

void screen_release_if_held_by(const struct process *who)
{
    if (g_owner && g_owner == who)
        screen_release();
}

void screen_take_back(void)
{
    if (!g_owner)
        return;

    /*
     * No summary. Whatever is being printed now is the thing worth reading,
     * and a count of pages from a program that has just taken the machine
     * down is not.
     */
    forget_the_holder();

    fb_set_output(true);
    fb_repaint();
}
