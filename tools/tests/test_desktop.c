/* Host oracle uses actual framebuffer/screen/keyboard code with only hardware
 * and process identity stubbed. Expected pixels come from the font+palette,
 * not from the framebuffer under test. ASan guards all clipped rectangles. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <funnyos/bootinfo.h>
#include <funnyos/fb.h>
#include <funnyos/font8x16.h>
#include <funnyos/mouse.h>
#include <funnyos/ps2.h>
#include <funnyos/screen.h>
#include <funnyos/terminal.h>
#include <funnyos/process.h>
#include <funnyos/arch/x86_64/irq.h>
#define W 1024u
#define H 768u
bool test_interrupts=true;
static uint32_t pixels[W*H];
static struct limine_framebuffer descriptor={.address=pixels,.width=W,.height=H,
 .pitch=W*4,.bpp=32,.red_mask_shift=16,.red_mask_size=8,
 .green_mask_shift=8,.green_mask_size=8,.blue_mask_shift=0,.blue_mask_size=8};
static struct process processes[9];
static struct process *current;
static struct mouse_event queued[128];static unsigned qhead,qtail;
static char diagnostics[16384];static size_t diag_length;
static unsigned checks;
#define CHECK(expr) do { if(!(expr)) { fprintf(stderr,"line %d: %s\n",__LINE__,#expr);abort(); } checks++; } while(0)
struct limine_framebuffer *bootinfo_framebuffer(void) { return &descriptor; }
struct process *process_current(void) { return current; }
bool mouse_poll(struct mouse_event *e)
{ if(qhead==qtail) return false;*e=queued[qtail++];return true; }
void kprintf(const char *format,...)
{
    char text[1024];va_list ap;va_start(ap,format);vsnprintf(text,sizeof(text),format,ap);va_end(ap);
    size_t n=strlen(text);if(diag_length+n<sizeof(diagnostics)) { memcpy(diagnostics+diag_length,text,n+1);diag_length+=n; }
    for(size_t i=0;i<n;i++) fb_putc(text[i]);
}
void serial_putc(char c) { (void)c; }
void process_session_yield(bool waiting) { (void)waiting; }
bool irq_register(uint8_t v,irq_handler_fn fn,void *ctx) { (void)v;(void)fn;(void)ctx;return true; }
bool lapic_ready(void) { return true; }
bool ioapic_ready(void) { return true; }
void ioapic_route_isa(unsigned n,uint8_t v) { (void)n;(void)v; }
/* Direct byte injection covers stream state without pretending port stubs
 * have executed a real device initialization. QEMU tests cover that instead. */
#include "../../kernel/console/kbd.c"
static void events(int dx,int dy,unsigned buttons)
{ queued[qhead++]=(struct mouse_event){dx,dy,buttons};screen_poll_input(); }
static void tab(unsigned n) { screen_request_tab(n);screen_poll_input(); }
static void fill(uint8_t *p,char letter,uint8_t attr)
{ for(unsigned i=0;i<80u*25u;i++) { p[i*2]=(uint8_t)letter;p[i*2+1]=attr; } }
static void expected_cell(unsigned x0,unsigned y0,char c,uint32_t fg,uint32_t bg)
{
    const uint8_t *font=g_font8x16[(unsigned char)c-FONT8X16_FIRST];
    for(unsigned y=0;y<16;y++) for(unsigned x=0;x<8;x++)
        CHECK(pixels[(y0+y)*W+x0+x]==((font[y]&(0x80u>>x))?fg:bg));
}
static void decoder(void)
{
    struct mouse_decoder d={0};struct mouse_event e;
    CHECK(!mouse_decode(&d,0x00,&e));CHECK(d.at==0);
    CHECK(!mouse_decode(&d,0x08,&e));CHECK(!mouse_decode(&d,7,&e));CHECK(mouse_decode(&d,3,&e));
    CHECK(e.dx==7 && e.dy==-3 && e.buttons==0);
    CHECK(!mouse_decode(&d,0x39,&e));CHECK(!mouse_decode(&d,249,&e));CHECK(mouse_decode(&d,253,&e));
    CHECK(e.dx==-7 && e.dy==3 && e.buttons==1);
    /* 9-bit values beyond int8_t range must not be sign-extended early. */
    mouse_decode(&d,0x08,&e);mouse_decode(&d,200,&e);CHECK(mouse_decode(&d,0,&e));CHECK(e.dx==200);
    mouse_decode(&d,0x38,&e);mouse_decode(&d,1,&e);CHECK(mouse_decode(&d,2,&e));CHECK(e.dx==-255 && e.dy==254);
    mouse_decode(&d,0xcb,&e);mouse_decode(&d,255,&e);CHECK(mouse_decode(&d,255,&e));CHECK(e.dx==0 && e.dy==0 && e.buttons==3);
}
static unsigned key_bytes,aux_bytes;
static void key_sink(uint8_t b) { key_bytes=key_bytes*256+b; }
static void aux_sink(uint8_t b) { aux_bytes=aux_bytes*256+b; }
static void demux(void)
{
    ps2_set_sink(false,key_sink);ps2_set_sink(true,aux_sink);
    ps2_dispatch(1,0x1e);ps2_dispatch(0x21,0x08);ps2_dispatch(1,0x9e);ps2_dispatch(0x21,7);
    CHECK(key_bytes==0x1e9e && aux_bytes==0x0807);
    ps2_dispatch(0xc1,0xff);ps2_dispatch(0x61,0xff);ps2_dispatch(0,0xff);
    CHECK(key_bytes==0x1e9e && aux_bytes==0x0807);
}
int main(void)
{
    decoder();demux();CHECK(fb_init());CHECK(screen_ready());
    for(unsigned i=0;i<9;i++) snprintf(processes[i].name,sizeof(processes[i].name),"page%u",i);
    /* One-page rendering remains the legacy output byte for byte. */
    fb_clear();fb_putc('A');expected_cell(0,0,'A',0xe8e8e8,0x101014);
    for(unsigned y=16;y<H;y++) for(unsigned x=0;x<W;x++) CHECK(pixels[y*W+x]==0x101014);
    for(unsigned x=8;x<W;x++) CHECK(pixels[x]==0x101014);
    fb_repaint();expected_cell(0,0,'A',0xe8e8e8,0x101014);
    fb_putc('\b');expected_cell(0,0,' ',0xe8e8e8,0x101014);
    uint8_t page[80u*25u*2u];fill(page,'A',0x17);current=&processes[0];CHECK(screen_acquire(current));
    CHECK(screen_present(page,80,FB_NO_CURSOR));expected_cell(24*8,12*16,'A',0xaaaaaa,0x0000aa);
    memset(page,'X',sizeof(page));tab(1);tab(2);
    expected_cell(24*8,12*16,'A',0xaaaaaa,0x0000aa);
    CHECK(!screen_acquire(0));current=&processes[1];CHECK(!screen_present(page,80,0));
    CHECK(screen_acquire(current));fill(page,'B',0x24);CHECK(screen_present(page,80,FB_NO_CURSOR));
    expected_cell(24*8,12*16,'B',0xaa0000,0x00aa00);
    tab(2);expected_cell(24*8,12*16,'A',0xaaaaaa,0x0000aa);
    screen_release_if_held_by(&processes[1]);tab(2);
    expected_cell(24*8,12*16,'A',0xaaaaaa,0x0000aa);
    /* Normal Alt chord still forwarded; host Alt+digit make/break consumed. */
    current=&processes[0];CHECK(kbd_raw_acquire(current));
    kbd_receive(0x38);kbd_receive(0x1e);kbd_receive(0x9e);kbd_receive(0xb8);
    CHECK(kbd_raw_poll(current)==0x38);CHECK(kbd_raw_poll(current)==0x1e);
    CHECK(kbd_raw_poll(current)==0x9e);CHECK(kbd_raw_poll(current)==0xb8);
    kbd_receive(0x38);kbd_receive(2);kbd_receive(0x82);kbd_receive(0xb8);
    CHECK(kbd_raw_poll(current)==-1);CHECK(!screen_keyboard_focus(current));
    kbd_receive(0x1e);kbd_receive(0x9e);CHECK(kbd_raw_poll(current)==-1);
    kbd_receive(0x38);kbd_receive(3);kbd_receive(0x83);kbd_receive(0xb8);
    CHECK(kbd_raw_poll(current)==-1);CHECK(screen_keyboard_focus(current));
    /* Right Alt carries E0: neither its prefix nor a digit may leak. */
    kbd_receive(0xe0);kbd_receive(0x38);kbd_receive(2);kbd_receive(0x82);
    kbd_receive(0xe0);kbd_receive(0xb8);
    CHECK(kbd_raw_poll(current)==-1);CHECK(!screen_keyboard_focus(current));
    kbd_receive(0xe0);kbd_receive(0x38);kbd_receive(3);kbd_receive(0x83);
    kbd_receive(0xe0);kbd_receive(0xb8);
    CHECK(kbd_raw_poll(current)==-1);CHECK(screen_keyboard_focus(current));
    kbd_receive(0x1c);CHECK(kbd_raw_poll(current)==0x1c);
    kbd_receive(0xe0);kbd_receive(0x48);CHECK(kbd_raw_poll(current)==0xe0);CHECK(kbd_raw_poll(current)==0x48);
    CHECK(kbd_raw_release(current));
    /* Move over a guest glyph, then leave it: full glyph must be restored. */
    events(192-512,192-384,0);CHECK(pixels[196*W+193]==0xffffff);
    events(20,3,0);expected_cell(192,192,'A',0xaaaaaa,0x0000aa);
    events(32767,32767,0);CHECK(strstr(diagnostics,"at=1023,767")!=0);
    events(-32768,-32768,0);CHECK(strstr(diagnostics,"at=0,0")!=0);
    events(40,8,1);CHECK(!screen_keyboard_focus(current));events(0,0,0);
    events(160,0,1);CHECK(screen_keyboard_focus(current));events(0,0,0);
    struct mouse_event owner_event;CHECK(screen_mouse_poll(current,&owner_event));
    screen_release();CHECK(screen_acquire(current));
    for(unsigned i=1;i<8;i++) { current=&processes[i];CHECK(screen_acquire(current)); }
    current=&processes[8];CHECK(!screen_acquire(current));
    screen_take_back();CHECK(!screen_present(page,80,0));
    CHECK(screen_acquire(current));screen_release();CHECK(screen_keyboard_focus(current));
    terminal_enable();
    CHECK(terminal_create()==1);CHECK(terminal_create()==2);
    processes[0].terminal=1;processes[1].terminal=2;
    terminal_write(&processes[0],"Alpha",5);terminal_write(&processes[1],"Beta",4);
    expected_cell(0,16,'B',0xaaaaaa,0x000000);
    kbd_receive(0x38);kbd_receive(2);kbd_receive(0x82);kbd_receive(0xb8);terminal_poll();
    CHECK(terminal_visible()==1);expected_cell(0,16,'A',0xaaaaaa,0x000000);
    terminal_clear(&processes[1]);expected_cell(0,16,'A',0xaaaaaa,0x000000);
    CHECK(terminal_raw_acquire(&processes[0]));
    /* Both host shortcuts consume modifier makes and their breaks. */
    kbd_receive(0x1d);kbd_receive(0x2a);kbd_receive(0x14);kbd_receive(0x94);
    kbd_receive(0xaa);kbd_receive(0x9d);
    CHECK(terminal_take_new());CHECK(terminal_raw_poll(&processes[0])==-1);
    kbd_receive(0xe0);kbd_receive(0x38);kbd_receive(3);kbd_receive(0x83);
    kbd_receive(0xe0);kbd_receive(0xb8);terminal_poll();
    CHECK(terminal_visible()==2);CHECK(terminal_raw_poll(&processes[0])==-1);
    kbd_receive(0x1e);CHECK(terminal_key_poll(&processes[0])==KEY_NONE);
    CHECK(terminal_key_poll(&processes[1])=='a');
    kbd_receive(0x38);kbd_receive(2);kbd_receive(0x82);kbd_receive(0xb8);terminal_poll();
    kbd_receive(0xe0);kbd_receive(0x48);
    CHECK(terminal_raw_poll(&processes[0])==0xe0);CHECK(terminal_raw_poll(&processes[0])==0x48);
    kbd_receive(0xe0);kbd_receive(0xc8);
    CHECK(terminal_raw_poll(&processes[0])==0xe0);CHECK(terminal_raw_poll(&processes[0])==0xc8);
    const uint8_t pause[]={0xe1,0x1d,0x45,0xe1,0x9d,0xc5};
    for(unsigned i=0;i<sizeof(pause);i++) kbd_receive(pause[i]);
    for(unsigned i=0;i<sizeof(pause);i++) CHECK(terminal_raw_poll(&processes[0])==pause[i]);
    CHECK(terminal_raw_release(&processes[0]));
    CHECK(terminal_guest_acquire(&processes[0]));fill(page,'G',0x17);
    CHECK(terminal_guest_present(&processes[0],page,80,FB_NO_CURSOR));
    expected_cell(192,176,'G',0xaaaaaa,0x0000aa);
    memset(page,'Z',sizeof(page));terminal_guest_release(&processes[0]);
    expected_cell(0,16,'A',0xaaaaaa,0x000000);
    for(unsigned i=3;i<=TERMINAL_MAX;i++) CHECK(terminal_create()==i);
    CHECK(terminal_create()==0);terminal_destroy(4);CHECK(terminal_create()==4);
    for(unsigned i=1;i<=TERMINAL_MAX;i++) terminal_destroy(i);
    CHECK(terminal_visible()==0);CHECK(terminal_create()==1);
    /* Backspace at a soft-wrap boundary must erase the previous row's
     * final cell, not the first cell of the newly advanced row. */
    char wrapped[W/8];memset(wrapped,'A',sizeof(wrapped));
    terminal_write(&processes[0],wrapped,sizeof(wrapped));
    terminal_write(&processes[0],"\bZ",2);
    expected_cell(W-8,16,'Z',0xaaaaaa,0x000000);
    printf("Desktop host suite: %u checks passed\n",checks);
}
