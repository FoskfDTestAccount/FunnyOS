/* Native text terminals. Display identity is independent of a process's
 * running context; all buffers are authoritative RAM, never sampled pixels. */
#include <funnyos/terminal.h>
#include <funnyos/fb.h>
#include <funnyos/serial.h>
#include <funnyos/kprintf.h>
#include <funnyos/arch/x86_64/io.h>
#include <libk/string.h>
#define MAX_COLS 256u
#define MAX_ROWS 128u
#define QUEUE 128u
#define RAW 512u
struct terminal {
    bool used, guest, raw, cancel;
    const struct process *guest_owner, *raw_owner;
    unsigned cols, rows, x, y;
    uint8_t cells[MAX_COLS*MAX_ROWS*2u];
    uint8_t guest_cells[MAX_COLS*25u*2u];
    unsigned guest_cols;
    uint16_t guest_cursor;
    int keys[QUEUE]; volatile unsigned kh,kt;
    uint8_t bytes[RAW]; volatile unsigned rh,rt; volatile bool overflow;
    struct mouse_event mouse[16]; unsigned mh,mt;
    bool prefix;
    bool down[256];
    uint16_t modifiers[8]; unsigned modifier_count;
};
static struct terminal g_terms[TERMINAL_MAX];
static bool g_enabled,g_pointer,g_dirty;
static unsigned g_visible,g_pointer_x,g_pointer_y;
static uint8_t g_buttons;
static volatile unsigned g_request,g_close;
static volatile bool g_new;
static bool g_host_release[128];
static unsigned g_pause_bytes,g_pause_terminal;
static char g_status[256]="Ctrl+Shift+T: new | Ctrl+Shift+W: close | Alt+1..8: switch | F12: log | Ctrl+Shift+K: stop DOS";
static void release_keys(struct terminal *t,bool only_modifiers);
static unsigned g_cw,g_ch; static uint64_t g_width,g_height;
static struct terminal *by_id(unsigned id)
{ return id && id<=TERMINAL_MAX && g_terms[id-1].used ? &g_terms[id-1] : NULL; }
static struct terminal *for_process(const struct process *p)
{ return p ? by_id(p->terminal) : NULL; }
bool terminal_enabled(void) { return g_enabled; }
bool terminal_exists(unsigned id) { return by_id(id)!=NULL; }
unsigned terminal_visible(void) { return g_visible; }
static unsigned count(void)
{ unsigned n=0;for(unsigned i=0;i<TERMINAL_MAX;i++) n+=g_terms[i].used;return n; }
static unsigned tab_width(void)
{
    unsigned n=count(); unsigned cols=(unsigned)(g_width/g_cw);
    unsigned width=n ? (cols>8 ? cols-8 : 0)/n : 0;
    return width>20 ? 20 : width;
}
static void pointer(void) { if(g_pointer) fb_pointer(g_pointer_x,g_pointer_y); }
static void draw_tabs(void)
{
    uint8_t cells[MAX_COLS*2];unsigned cols=(unsigned)(g_width/g_cw);
    for(unsigned c=0;c<cols;c++) { cells[c*2]=' ';cells[c*2+1]=0x17; }
    unsigned ordinal=0,width=tab_width();
    for(unsigned i=0;i<TERMINAL_MAX;i++) if(g_terms[i].used) {
        unsigned start=ordinal*width,end=start+width;
        const char label[]={'[',(char)('1'+ordinal),' ','T','e','r','m','i','n','a','l',' ',(char)('1'+i),0};
        for(unsigned j=0;label[j] && start+j+3<end;j++) cells[(start+j)*2]=(uint8_t)label[j];
        if(width>=3) { cells[(end-3)*2]='x';cells[(end-2)*2]=']'; }
        for(unsigned c=start;c<end;c++) cells[c*2+1]=i+1==g_visible ? 0x1f : 0x17;
        ordinal++;
    }
    unsigned plus=ordinal*width;
    for(unsigned j=0;j<3 && plus+j<cols;j++) cells[(plus+j)*2]="[+]"[j];
    for(unsigned j=0;j<5 && plus+3+j<cols;j++) cells[(plus+3+j)*2]="[Log]"[j];
    fb_draw_page(0,0,cols,1,cells,FB_NO_CURSOR);
    for(unsigned c=0;c<cols;c++) { cells[c*2]=g_status[c] ? (uint8_t)g_status[c] : ' ';cells[c*2+1]=8; }
    fb_draw_page(0,g_height/g_ch-1,cols,1,cells,FB_NO_CURSOR);
}
static void guest_origin(const struct terminal *t,uint64_t *col,uint64_t *row)
{
    unsigned cols=(unsigned)(g_width/g_cw),rows=(unsigned)(g_height/g_ch);
    *col=cols>t->guest_cols ? (cols-t->guest_cols)/2 : 0;
    *row=1+(rows>27 ? (rows-27)/2 : 0);
}
static void draw_content(struct terminal *t)
{
    if(t->guest) {
        uint64_t col,row;guest_origin(t,&col,&row);
        fb_draw_page(col,row,t->guest_cols,25,t->guest_cells,t->guest_cursor);
    } else fb_draw_page(0,1,t->cols,t->rows,t->cells,(uint16_t)(t->y*t->cols+t->x));
}
static void repaint(void)
{
    fb_fill_screen();struct terminal *t=by_id(g_visible);
    if(t) draw_content(t);
    if(!t) fb_repaint();
    draw_tabs();pointer();g_dirty=false;
}
void terminal_enable(void)
{
    g_enabled=true;fb_set_output(false);
    fb_geometry(&g_width,&g_height,&g_cw,&g_ch);
    fb_set_top(1);g_pointer_x=(unsigned)g_width/2;g_pointer_y=(unsigned)g_height/2;
    fb_set_overlay(pointer);
}
unsigned terminal_create(void)
{
    if(!g_enabled || g_width/g_cw<8 || g_height/g_ch<3) return 0;
    for(unsigned i=0;i<TERMINAL_MAX;i++) if(!g_terms[i].used) {
        struct terminal *t=&g_terms[i];*t=(struct terminal){.used=true};
        t->cols=(unsigned)(g_width/g_cw);t->rows=(unsigned)(g_height/g_ch)-2;
        if(t->cols>MAX_COLS) t->cols=MAX_COLS;
        if(t->rows>MAX_ROWS) t->rows=MAX_ROWS;
        for(unsigned j=0;j<t->cols*t->rows;j++) { t->cells[j*2]=' ';t->cells[j*2+1]=7; }
        struct terminal *old=by_id(g_visible);if(old && old->raw) release_keys(old,false);
        g_visible=i+1;repaint();return i+1;
    }
    return 0;
}
void terminal_destroy(unsigned id)
{
    struct terminal *t=by_id(id);if(!t) return;
    *t=(struct terminal){0};
    if(g_visible==id) {
        g_visible=0;
        for(unsigned i=0;i<TERMINAL_MAX;i++) if(g_terms[i].used) { g_visible=i+1;break; }
    }
    repaint();
}
static void putc(struct terminal *t,char c)
{
    if(c=='\r') { t->x=0;return; }
    if(c=='\n') { t->x=0;t->y++; }
    else if(c=='\b') { if(t->x) t->x--;else if(t->y) { t->y--;t->x=t->cols-1; }t->cells[(t->y*t->cols+t->x)*2]=' '; }
    else if(c=='\t') { do { putc(t,' '); } while(t->x&7u); }
    else if((unsigned char)c>=32) { t->cells[(t->y*t->cols+t->x)*2]=(uint8_t)c;t->x++; }
    if(t->x>=t->cols) { t->x=0;t->y++; }
    if(t->y>=t->rows) {
        memmove(t->cells,t->cells+t->cols*2,(t->rows-1)*t->cols*2);
        for(unsigned j=(t->rows-1)*t->cols;j<t->rows*t->cols;j++) { t->cells[j*2]=' ';t->cells[j*2+1]=7; }
        t->y=t->rows-1;
    }
}
void terminal_write(const struct process *p,const char *text,size_t size)
{
    struct terminal *t=for_process(p);if(!t) return;
    for(size_t i=0;i<size;i++) { serial_putc(text[i]);putc(t,text[i]); }
    if(p->terminal==g_visible && !t->guest) { draw_content(t);pointer(); }
}
void terminal_clear(const struct process *p)
{
    struct terminal *t=for_process(p);if(!t) return;
    for(unsigned j=0;j<t->cols*t->rows;j++) { t->cells[j*2]=' ';t->cells[j*2+1]=7; }
    t->x=t->y=0;if(p->terminal==g_visible) repaint();
}
void terminal_feedback(const char *text)
{
    kprintf("Terminal       : %s\n",text);
    memset(g_status,0,sizeof(g_status));
    for(unsigned i=0;text[i] && i+1<sizeof(g_status);i++) g_status[i]=text[i];
    repaint();
}
bool terminal_focus(const struct process *p) { return p && p->terminal==g_visible; }
bool terminal_pending(unsigned id)
{
    struct terminal *t=by_id(id);
    return t && (t->kh!=t->kt || t->rh!=t->rt || t->overflow);
}
bool terminal_work_pending(void) { return g_new || g_close || g_request; }
bool terminal_take_new(void)
{ bool enabled=interrupts_enabled();interrupts_disable();bool value=g_new;g_new=false;
  if(enabled) interrupts_enable();
  return value; }
unsigned terminal_take_close(void)
{ bool enabled=interrupts_enabled();interrupts_disable();unsigned id=g_close;g_close=0;
  if(enabled) interrupts_enable();
  return id; }
int terminal_key_poll(const struct process *p)
{
    struct terminal *t=for_process(p);if(!t || !terminal_focus(p) || t->kh==t->kt) return KEY_NONE;
    int key=t->keys[t->kt];t->kt=(t->kt+1)%QUEUE;return key;
}
static void raw_push(struct terminal *t,uint8_t byte)
{
    unsigned next=(t->rh+1)%RAW;
    if(next==t->rt) t->overflow=true;
    else { t->bytes[t->rh]=byte;t->rh=next; }
}
static void raw_key(struct terminal *t,unsigned code,bool release)
{
    if(code>=128) raw_push(t,0xe0);
    raw_push(t,(uint8_t)((code&127u)|(release ? 0x80u : 0u)));
    t->down[code]=!release;
}
static bool modifier(unsigned code)
{ return (code&127u)==0x38 || (code&127u)==0x1d || code==0x2a || code==0x36; }
static void flush_modifiers(struct terminal *t)
{
    for(unsigned i=0;i<t->modifier_count;i++) raw_key(t,t->modifiers[i],false);
    t->modifier_count=0;
}
static void release_keys(struct terminal *t,bool only_modifiers)
{
    t->modifier_count=0;t->prefix=false;
    for(unsigned code=0;code<256;code++) if(t->down[code] && (!only_modifiers || modifier(code)))
        raw_key(t,code,true);
}
void terminal_keyboard(uint8_t byte,bool extended,int key,const struct kbd_state *state)
{
    unsigned code=byte&0x7fu;bool release=(byte&0x80u)!=0;
    struct terminal *t=by_id(g_visible);
    if(g_request && g_request<10) {
        unsigned ordinal=0;
        for(unsigned i=0;i<TERMINAL_MAX;i++) if(g_terms[i].used && ++ordinal==g_request) { t=&g_terms[i];break; }
    }
    /* Pause is a six-byte E1 sequence, including a second E1. Preserve it
     * verbatim for raw consumers; its embedded Ctrl-looking bytes are not
     * ordinary modifiers or tab shortcuts. */
    if(g_pause_bytes) {
        struct terminal *target=by_id(g_pause_terminal);
        if(target && target->raw) raw_push(target,byte);
        g_pause_bytes--;return;
    }
    if(byte==0xe1) {
        g_pause_bytes=5;g_pause_terminal=t ? (unsigned)(t-g_terms)+1 : 0;
        if(t && t->raw) raw_push(t,byte);
        return;
    }
    if(byte==0xe0) { if(t) t->prefix=true;return; }
    if(!extended && release && g_host_release[code]) {
        g_host_release[code]=false;if(t) t->prefix=false;return;
    }
    /* Host actions only enqueue work. Deferring modifier makes prevents
     * Ctrl/Shift/Alt prefixes leaking into a guest before the host chord is
     * recognizable. Ordinary chords flush them in their original order. */
    bool hotkey=false;
    if(!extended && !release && code>=2 && code<=10 && state->alt) {
        g_request=code-1u;hotkey=true;
    } else if(!extended && !release && state->ctrl && state->shift && (code==0x14 || code==0x11 || code==0x25)) {
        if(code==0x14) g_new=true;
        else if(code==0x11) g_close=g_visible;
        else if(t && t->guest) t->cancel=true;
        hotkey=true;
    } else if(!extended && !release && code==0x58) { g_request=10;hotkey=true; }
    if(hotkey) {
        g_host_release[code]=true;
        if(t && t->raw) release_keys(t,true);
        return;
    }
    if(!t || g_new) return;
    t->prefix=false;
    if(t->raw) {
        unsigned scan=code+(extended ? 128u : 0u);
        if(modifier(scan) && !release) {
            bool duplicate=t->down[scan];
            for(unsigned i=0;i<t->modifier_count;i++) duplicate|=t->modifiers[i]==scan;
            if(!duplicate && t->modifier_count<8) t->modifiers[t->modifier_count++]=(uint16_t)scan;
            return;
        }
        if(release) {
            bool pending=false;
            for(unsigned i=0;i<t->modifier_count;i++) pending|=t->modifiers[i]==scan;
            if(pending) flush_modifiers(t);
            if(t->down[scan]) raw_key(t,scan,true);
        } else { flush_modifiers(t);raw_key(t,scan,false); }
    } else if(key!=KEY_NONE) {
        unsigned next=(t->kh+1)%QUEUE;
        if(next!=t->kt) { t->keys[t->kh]=key;t->kh=next; }
    }
}
static void select_ordinal(unsigned ordinal)
{
    unsigned n=0;
    for(unsigned i=0;i<TERMINAL_MAX;i++) if(g_terms[i].used && ++n==ordinal) {
        struct terminal *old=by_id(g_visible);
        if(old && old->raw && g_visible!=i+1) release_keys(old,false);
        g_visible=i+1;g_dirty=true;kprintf("Terminal       : selected session %u\n",g_visible);return;
    }
}
void terminal_poll(void)
{
    if(!g_enabled) return;
    bool enabled=interrupts_enabled();interrupts_disable();
    unsigned requested=g_request;g_request=0;
    if(enabled) interrupts_enable();
    if(requested==10) {
        struct terminal *old=by_id(g_visible);if(old && old->raw) release_keys(old,false);
        g_visible=0;g_dirty=true;
    } else if(requested) select_ordinal(requested);
    struct mouse_event e;
    while(mouse_poll(&e)) {
        int64_t x=(int64_t)g_pointer_x+e.dx,y=(int64_t)g_pointer_y+e.dy;
        g_pointer_x=(unsigned)(x<0 ? 0 : x>=(int64_t)g_width ? g_width-1 : (uint64_t)x);
        g_pointer_y=(unsigned)(y<0 ? 0 : y>=(int64_t)g_height ? g_height-1 : (uint64_t)y);
        g_pointer=true;g_dirty=true;
        bool press=(e.buttons&1) && !(g_buttons&1);g_buttons=e.buttons;
        if(press && g_pointer_y<g_ch) {
            unsigned width=tab_width(),cell=g_pointer_x/g_cw,n=count();
            if(cell>=n*width+3 && cell<n*width+8) {
                struct terminal *old=by_id(g_visible);if(old && old->raw) release_keys(old,false);
                g_visible=0;g_dirty=true;
            } else if(cell>=n*width && cell<n*width+3) g_new=true;
            else if(width && cell/width<n) {
                select_ordinal(cell/width+1);
                if(cell%width==width-3) g_close=g_visible;
            }
        } else {
            struct terminal *t=by_id(g_visible);
            if(t && t->guest) { unsigned next=(t->mh+1)%16;if(next!=t->mt) { t->mouse[t->mh]=e;t->mh=next; } }
        }
    }
    if(g_dirty) repaint();
}
bool terminal_guest_acquire(const struct process *p)
{
    struct terminal *t=for_process(p);if(!t || (t->guest_owner && t->guest_owner!=p)) return false;
    if(t->guest_owner==p) return true;
    t->guest=true;t->guest_owner=p;t->guest_cols=0;t->guest_cursor=FB_NO_CURSOR;
    if(terminal_focus(p)) repaint();
    return true;
}
bool terminal_guest_present(const struct process *p,const uint8_t *cells,unsigned cols,uint16_t cursor)
{
    struct terminal *t=for_process(p);if(!t || t->guest_owner!=p || cols>MAX_COLS || !cols) return false;
    if(cols>g_width/g_cw || g_height/g_ch<27) return false;
    memcpy(t->guest_cells,cells,cols*25u*2u);t->guest_cols=cols;t->guest_cursor=cursor;
    if(terminal_focus(p)) repaint();
    return true;
}
void terminal_guest_release(const struct process *p)
{
    struct terminal *t=for_process(p);if(!t || t->guest_owner!=p) return;
    t->guest=false;t->cancel=false;t->guest_owner=NULL;t->mh=t->mt=0;if(terminal_focus(p)) repaint();
}
bool terminal_mouse_poll(const struct process *p,struct mouse_event *e)
{
    struct terminal *t=for_process(p);if(!t || !terminal_focus(p) || t->mt==t->mh) return false;
    *e=t->mouse[t->mt];t->mt=(t->mt+1)%16;return true;
}
bool terminal_raw_acquire(const struct process *p)
{
    bool enabled=interrupts_enabled();interrupts_disable();
    struct terminal *t=for_process(p);
    bool ok=t && (!t->raw_owner || t->raw_owner==p);
    if(ok && !t->raw) {
        t->raw=true;t->raw_owner=p;t->rh=t->rt=t->kh=t->kt=0;t->overflow=false;
        t->prefix=false;t->modifier_count=0;memset(t->down,0,sizeof(t->down));
    }
    if(enabled) interrupts_enable();
    return ok;
}
int terminal_raw_poll(const struct process *p)
{
    struct terminal *t=for_process(p);if(!t || t->raw_owner!=p || !terminal_focus(p)) return -1;
    if(t->overflow) return -10;
    if(t->rh==t->rt) return -1;
    int byte=t->bytes[t->rt];t->rt=(t->rt+1)%RAW;return byte;
}
bool terminal_raw_release(const struct process *p)
{
    bool enabled=interrupts_enabled();interrupts_disable();
    struct terminal *t=for_process(p);bool ok=t && t->raw_owner==p;
    if(ok) { t->raw=false;t->raw_owner=NULL;t->rh=t->rt=t->kh=t->kt=0;t->overflow=false; }
    if(enabled) interrupts_enable();
    return ok;
}
bool terminal_busy(unsigned id) { struct terminal *t=by_id(id);return t && (t->guest || t->raw); }
void terminal_panic(void) { g_enabled=false;fb_set_overlay(NULL); }

int terminal_control(const struct process *p,unsigned operation)
{
    if(!for_process(p)) return -9;
    if(operation==5) { struct terminal *t=for_process(p);bool cancelled=t->cancel;t->cancel=false;return cancelled; }
    if(operation==2) return (int)count();
    if(operation==3) return (int)p->terminal;
    if(operation==0) { if(count()==TERMINAL_MAX) return -8;g_new=true;return 0; }
    if(operation==1) { if(terminal_busy(p->terminal)) return -1;g_close=p->terminal;return 0; }
    return -6;
}
