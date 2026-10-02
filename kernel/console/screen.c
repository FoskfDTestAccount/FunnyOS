/* Kernel-owned text pages. The presenter pointer is NEVER retained. */
#include <funnyos/screen.h>
#include <funnyos/terminal.h>
#include <funnyos/fb.h>
#include <funnyos/mouse.h>
#include <funnyos/kprintf.h>
#include <funnyos/syscall.h>
#include <libk/string.h>
#include <funnyos/arch/x86_64/io.h>
#define PAGE_COUNT 9u
#define TAB_COLUMNS 20u
struct screen_page {
    const struct process *owner;
    bool used;
    char name[PROCESS_NAME_MAX];
    unsigned columns;
    uint64_t col,row,presents;
    uint16_t cursor;
    uint8_t cells[SCREEN_MAX_COLUMNS*SCREEN_ROWS*2u];
    struct mouse_event mouse[16];
    unsigned head,tail;
};
static struct screen_page g_pages[PAGE_COUNT];
static unsigned g_visible;
static volatile unsigned g_requested;
static bool g_initialized,g_pointer,g_pumping;
static unsigned g_x,g_y;
static uint8_t g_buttons;
static int64_t g_dx,g_dy;
static unsigned page_count(void)
{ unsigned n=0;for(unsigned i=0;i<PAGE_COUNT;i++) if(g_pages[i].used) n++;return n; }
static unsigned top_rows(void) { return page_count()>1 ? 1u : 0u; }
static void overlay(void) { if(g_pointer) fb_pointer(g_x,g_y); }
static void ensure_initialized(void)
{
    if(g_initialized || !fb_is_ready()) return;
    g_initialized=true;g_pages[0].used=true;g_pages[0].cursor=FB_NO_CURSOR;
    uint64_t w,h;unsigned cw,ch;
    fb_geometry(&w,&h,&cw,&ch);g_x=(unsigned)(w/2);g_y=(unsigned)(h/2);
    fb_set_overlay(overlay);
}
static struct screen_page *owned(const struct process *owner)
{
    if(!owner) return 0;
    for(unsigned i=1;i<PAGE_COUNT;i++) if(g_pages[i].used && g_pages[i].owner==owner) return &g_pages[i];
    return 0;
}
static void layout(void)
{
    uint64_t cols,rows;if(!fb_grid_size(&cols,&rows)) return;
    unsigned top=top_rows();fb_set_top(top);
    uint64_t available=rows>top ? rows-top : 0;
    for(unsigned i=1;i<PAGE_COUNT;i++) if(g_pages[i].used) {
        struct screen_page *p=&g_pages[i];
        p->col=cols>p->columns ? (cols-p->columns)/2 : 0;
        p->row=top+(available>SCREEN_ROWS ? (available-SCREEN_ROWS)/2 : 0);
    }
}
static void tabs(void)
{
    if(!top_rows()) return;
    uint64_t cols,rows;fb_grid_size(&cols,&rows);(void)rows;
    uint8_t cells[SCREEN_MAX_COLUMNS*2u];
    for(unsigned c=0;c<cols;c++) { cells[c*2]=' ';cells[c*2+1]=0x17; }
    unsigned ordinal=0;
    for(unsigned i=0;i<PAGE_COUNT;i++) if(g_pages[i].used) {
        const char *name=i ? g_pages[i].name : "Log";
        unsigned start=ordinal*TAB_COLUMNS;
        if(start<cols) {
            const char prefix[4]={'[',(char)('1'+ordinal),' ',0};
            unsigned c=start;
            for(unsigned j=0;j<3 && c<cols;j++,c++) cells[c*2]=(uint8_t)prefix[j];
            for(unsigned j=0;name[j] && c<cols;j++,c++) cells[c*2]=(uint8_t)name[j];
            if(c<cols) cells[c*2]=']';
            for(c=start;c<start+TAB_COLUMNS && c<cols;c++) cells[c*2+1]=i==g_visible ? 0x1f : 0x17;
        }
        ordinal++;
    }
    fb_draw_page(0,0,cols,1,cells,FB_NO_CURSOR);
}
static void repaint(void)
{
    fb_set_output(false);fb_fill_screen();
    if(!g_visible) { fb_set_output(true);fb_repaint(); }
    else {
        struct screen_page *p=&g_pages[g_visible];
        if(p->columns) fb_draw_page(p->col,p->row,p->columns,SCREEN_ROWS,p->cells,p->cursor);
    }
    tabs();overlay();
}
static void select(unsigned index)
{
    if(index>=PAGE_COUNT || !g_pages[index].used || g_visible==index) return;
    g_visible=index;
    kprintf("  Tabs           : selected slot %u (%s), pages %u\n",index,index?"Program":"Log",page_count());
    repaint();
}
bool screen_ready(void) { ensure_initialized();return fb_is_ready(); }
bool screen_acquire(const struct process *who)
{
    if(terminal_enabled()) return terminal_guest_acquire(who);
    if(!who || !screen_ready()) return false;
    if(owned(who)) return true;
    for(unsigned i=1;i<PAGE_COUNT;i++) if(!g_pages[i].used) {
        g_pages[i]=(struct screen_page){.used=true,.owner=who,.cursor=FB_NO_CURSOR};
        memcpy(g_pages[i].name,"Program",8);
        fb_set_output(false);g_visible=i;layout();
        kprintf("  Tabs           : pages %u, top rows %u\n",page_count(),top_rows());
        repaint();return true;
    }
    return false;
}
bool screen_present(const uint8_t *cells,unsigned columns,uint16_t cursor)
{
    if(terminal_enabled()) return terminal_guest_present(process_current(),cells,columns,cursor);
    struct screen_page *p=owned(process_current());
    if(!p || !cells || !columns || columns>SCREEN_MAX_COLUMNS) return false;
    bool resized=columns!=p->columns;p->columns=columns;p->cursor=cursor;
    memcpy(p->cells,cells,(size_t)columns*SCREEN_ROWS*2u);p->presents++;
    if(resized) {
        layout();uint64_t cols,rows;fb_grid_size(&cols,&rows);
        kprintf("  Screen         : a program has the screen, %u columns at cell %llu,%llu of a %llux%llu console\n",
                columns,(unsigned long long)p->col,(unsigned long long)p->row,
                (unsigned long long)cols,(unsigned long long)rows);
    }
    if(p==&g_pages[g_visible]) {
        if(resized) repaint();
        else { fb_draw_page(p->col,p->row,p->columns,SCREEN_ROWS,p->cells,p->cursor);tabs(); }
    }
    return true;
}
void screen_release_if_held_by(const struct process *who)
{
    if(terminal_enabled()) { terminal_guest_release(who);return; }
    struct screen_page *p=owned(who);if(!p) return;
    if(p->presents) kprintf("  Screen         : %llu page(s) presented by a program, the last %u columns at cell %llu,%llu\n",
                            (unsigned long long)p->presents,p->columns,
                            (unsigned long long)p->col,(unsigned long long)p->row);
    unsigned index=(unsigned)(p-g_pages);*p=(struct screen_page){0};
    if(g_visible==index) g_visible=0;
    g_requested=0;
    fb_set_output(false);layout();repaint();
}
void screen_release(void) { screen_release_if_held_by(process_current()); }
void screen_take_back(void)
{
    if(terminal_enabled()) terminal_panic();
    ensure_initialized();
    for(unsigned i=1;i<PAGE_COUNT;i++) g_pages[i]=(struct screen_page){0};
    g_visible=0;g_requested=0;g_pointer=false;
    fb_set_output(false);layout();repaint();
}
bool screen_keyboard_focus(const struct process *who)
{ if(terminal_enabled()) return terminal_focus(who);return !owned(who) || g_pages[g_visible].owner==who; }
/* IRQ-side requests only. No framebuffer painting or logging in an ISR. */
void screen_request_tab(unsigned ordinal) { if(ordinal>=1 && ordinal<=9) g_requested=ordinal; }
static void requested(void)
{
    bool enabled=interrupts_enabled();interrupts_disable();
    unsigned ordinal=g_requested;g_requested=0;
    if(enabled) interrupts_enable();
    if(!ordinal) return;
    unsigned n=0;
    for(unsigned i=0;i<PAGE_COUNT;i++) if(g_pages[i].used && ++n==ordinal) { select(i);return; }
}
static void mouse_event(const struct mouse_event *e)
{
    uint64_t width,height;unsigned cw,ch;
    if(!fb_geometry(&width,&height,&cw,&ch)) return;
    unsigned old_x=g_x,old_y=g_y;bool had_pointer=g_pointer;
    int64_t x=(int64_t)g_x+e->dx,y=(int64_t)g_y+e->dy;
    g_x=x<0 ? 0 : (x>=(int64_t)width ? (unsigned)width-1 : (unsigned)x);
    g_y=y<0 ? 0 : (y>=(int64_t)height ? (unsigned)height-1 : (unsigned)y);
    g_pointer=true;g_dx+=e->dx;g_dy+=e->dy;
    bool click=(e->buttons&1u) && !(g_buttons&1u);g_buttons=e->buttons;
    unsigned previous=g_visible;
    if(click && top_rows() && g_y<ch) {
        screen_request_tab(g_x/cw/TAB_COLUMNS+1u);requested();
    } else if(g_visible) {
        struct screen_page *p=&g_pages[g_visible];unsigned next=(p->head+1u)%16;
        if(next!=p->tail) { p->mouse[p->head]=*e;p->head=next; }
    }
    if(g_visible==previous && had_pointer) {
        fb_background_rect(old_x,old_y,12,16);
        if(!g_visible) fb_console_rect(old_x,old_y,12,16);
        else {
            struct screen_page *p=&g_pages[g_visible];
            fb_page_rect(p->col,p->row,p->columns,p->cells,p->cursor,old_x,old_y,12,16);
        }
        if(top_rows() && old_y<ch) tabs();
    }
    overlay();
    kprintf("  Mouse event    : dx=%d dy=%d total=%lld,%lld buttons=%u at=%u,%u\n",
            (int)e->dx,(int)e->dy,(long long)g_dx,(long long)g_dy,(unsigned)e->buttons,g_x,g_y);
}
void screen_poll_input(void)
{
    if(terminal_enabled()) { terminal_poll();return; }
    if(g_pumping || !screen_ready()) return;
    g_pumping=true;requested();
    struct mouse_event e;
    for(unsigned n=0;n<64 && mouse_poll(&e);n++) mouse_event(&e);
    g_pumping=false;
}
bool screen_mouse_poll(const struct process *who,struct mouse_event *out)
{
    if(terminal_enabled()) return terminal_mouse_poll(who,out);
    struct screen_page *p=owned(who);
    if(!p || p!=&g_pages[g_visible] || p->head==p->tail) return false;
    *out=p->mouse[p->tail];p->tail=(p->tail+1u)%16;return true;
}
