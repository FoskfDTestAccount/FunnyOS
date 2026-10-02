/* Native acceptance fixture: intentionally mutate the presenter buffer after
 * presenting. Switching back must show the kernel snapshot, not these bytes. */
#include <libu/libu.h>
#include <funnyos/syscall.h>
static unsigned char page[80u*25u*2u];
static void present(const char *title)
{
    for(unsigned i=0;i<80u*25u;i++) { page[i*2]=' ';page[i*2+1]=0x17; }
    for(unsigned i=0;title[i];i++) page[i*2]=(unsigned char)title[i];
    u_screen_acquire();u_screen_present(page,80,80);
    for(unsigned i=0;i<80u*25u;i++) page[i*2]='X';
}
static void wait_raw(const char *marker)
{
    u_kbd_acquire();uputs(marker);uputs("\n");
    /* Interactive preview: arrows, modifier releases and an E0 prefix are
     * normal input, not evidence of a leaked host shortcut. Keep logging the
     * full stream so the automated oracle can assert exact isolation. */
    unsigned extended=0,alt=0;
    for (;;) {
        int byte=u_kbd_poll();
        if(byte==-1) continue;
        if(byte<0) { uputs("G raw queue error: FAIL\n");u_exit(1); }
        uprintf("G raw key: %02x\n",(unsigned)byte);
        if(byte==0xe0) { extended=1;continue; }
        unsigned code=(unsigned)byte&0x7fu;
        unsigned released=(unsigned)byte&0x80u;
        if(code==0x38) alt=!released;
        if(!extended && alt && code>=2 && code<=10) {
            uputs("G raw switch leaked: FAIL\n");u_exit(1);
        }
        extended=0;
        if(code==0x1c && !released) break;
    }
    unsigned events=0;struct syscall_mouse_event e;
    while(u_mouse_poll(&e)==1) events++;
    uprintf("G owner mouse events: %u\n",events);
    u_kbd_release();
}
int desktop_test(unsigned child)
{
    if(child) {
        present("G child kernel snapshot");
        wait_raw("G child ready");
        /* Deliberately don't release: process destruction must remove only
         * this child's tab, leaving the parent's kernel snapshot intact. */
        return 23;
    }
    present("G parent kernel snapshot");
    wait_raw("G parent ready");
    long code=u_spawn("funnycom",13);
    uprintf("G child returned %ld\n",code);
    if(code!=23) return 1;
    wait_raw("G parent restored");
    u_screen_release();uputs("G desktop acceptance: PASS\n");
    return 0;
}
