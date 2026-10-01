#include "harness.h"
#include <vm86/int21.h>
#include <vm86/host.h>
#include <vm86/firmware.h>
#include "../bios/bios10.h"
#include "../bios/bios16.h"
#include "../corpus/bios/replay.h"
static struct bios10_state video;
static struct bios16_state keyboard;
static struct int21_state dos;
extern const uint8_t corpus_keys[];
extern const unsigned long corpus_keys_size;
static void init(struct vm86_cpu *cpu)
{
    vm86_mem_clear(cpu->mem); vm86_reset(cpu,cpu->mem);
    vm86_clear_services(); vm86_install_firmware(cpu);
    bios10_reset(&video); cpu->ax=3; bios10_service(cpu,&video); bios16_init(cpu,&keyboard); int21_reset(&dos,&video,0x1000);
    vm86_set_seg(cpu,VM86_DS,0x1000);
    vm86_register_service(0x21,int21_service,&dos);
    vm86_register_service(0x23,int21_break_service,NULL);
    vm86_register_service(0x09,bios16_irq,&keyboard);
}
static void press(struct vm86_cpu *cpu,uint8_t scan) { bios16_key_arrived(&keyboard,scan); bios16_irq(cpu,&keyboard); }
static void call(struct vm86_cpu *cpu,uint8_t fn) { cpu->ah=fn; int21_service(cpu,&dos); }
static void polling(struct vm86_cpu *cpu)
{
    init(cpu); call(cpu,0x0B); vm86_expect_u16("empty status",cpu->al,0);
    cpu->dl=0xFF; call(cpu,6); vm86_expect_flag("empty direct input ZF",cpu,VM86_ZF,true); vm86_expect_u16("empty direct input AL",cpu->al,0);
    press(cpu,0x1E); call(cpu,0x0B); vm86_expect_u16("available status FFh",cpu->al,0xFF);
    call(cpu,0x0B); vm86_expect_u16("status does not consume",cpu->al,0xFF);
    call(cpu,6); vm86_expect_u16("direct input a",cpu->al,'a'); vm86_expect_flag("direct input clears ZF",cpu,VM86_ZF,false);
    call(cpu,0x0B); vm86_expect_u16("consumed",cpu->al,0);
    cpu->dl='!'; call(cpu,6); vm86_expect_mem8("direct output",cpu,0xB8000,'!');
}
static void character_modes(struct vm86_cpu *cpu)
{
    init(cpu); press(cpu,0x30); call(cpu,7); vm86_expect_u16("07h character",cpu->al,'b'); vm86_expect_mem8("07h does not echo",cpu,0xB8000,' ');
    press(cpu,0x2E); call(cpu,8); vm86_expect_u16("08h character",cpu->al,'c'); vm86_expect_mem8("08h does not echo",cpu,0xB8000,' ');
    press(cpu,0x20); call(cpu,1); vm86_expect_u16("01h character",cpu->al,'d'); vm86_expect_mem8("01h echoes",cpu,0xB8000,'d');
    press(cpu,0xE0); press(cpu,0x48); call(cpu,7); vm86_expect_u16("extended first byte zero",cpu->al,0);
    call(cpu,0x0B); vm86_expect_u16("extended second byte available",cpu->al,0xFF);
    call(cpu,7); vm86_expect_u16("extended second byte scan",cpu->al,0x48);
    press(cpu,0x1D); press(cpu,0x2E); call(cpu,7); vm86_expect_u16("07h returns Ctrl-C",cpu->al,3); press(cpu,0x9D);
    press(cpu,0x2A); press(cpu,0x30); press(cpu,0xAA); call(cpu,8); vm86_expect_u16("shift make/break uppercase",cpu->al,'B');
    press(cpu,0x30); call(cpu,8); vm86_expect_u16("shift break restores lowercase",cpu->al,'b');
}
static void line(struct vm86_cpu *cpu)
{
    init(cpu); cpu->dx=0x800; vm86_mem_write8(cpu->mem,0x10800,4);
    press(cpu,0x1E); press(cpu,0x30); press(cpu,0x0E); press(cpu,0x2E); press(cpu,0x1C);
    call(cpu,0x0A);
    vm86_expect_mem8("line count excludes CR",cpu,0x10801,2);
    vm86_expect_mem8("line first char",cpu,0x10802,'a'); vm86_expect_mem8("line backspace editing",cpu,0x10803,'c'); vm86_expect_mem8("line includes CR",cpu,0x10804,13);
    vm86_expect_bool("line finished",dos.line_active,false);
    init(cpu); cpu->dx=0x800; vm86_mem_write8(cpu->mem,0x10800,2);
    press(cpu,0x1E); press(cpu,0x30); press(cpu,0x1C); call(cpu,0x0A);
    vm86_expect_mem8("capacity reserves CR",cpu,0x10801,1); vm86_expect_mem8("full buffer CR",cpu,0x10803,13);
    init(cpu); cpu->dx=0x800; vm86_mem_write8(cpu->mem,0x10800,0); call(cpu,0x0A); vm86_expect_mem8("zero capacity",cpu,0x10801,0);
}
static void blocking_retry(struct vm86_cpu *cpu)
{
    init(cpu);
    static const uint8_t code[]={0xB4,0x07,0xCD,0x21,0x89,0xC3,0xFA,0xF4};
    vm86_test_load(cpu,code,sizeof(code)); vm86_flag_set(cpu,VM86_IF,true);
    uint16_t sp=cpu->sp;
    (void)vm86_run(cpu,100);
    vm86_expect_u16("blocking read keeps one live frame",cpu->sp,(uint16_t)(sp-6));
    vm86_expect_bool("blocking read has not retired result",cpu->bx==0,true);
    press(cpu,0x1E); vm86_expect_u16("read resumes",vm86_run(cpu,100),VM86_STOP_HALT);
    vm86_expect_u16("retried result",cpu->bl,'a'); vm86_expect_u16("no leaked interrupt frame",cpu->sp,sp);
}
static void program(struct vm86_cpu *cpu)
{
    (void)cpu;
    struct vm86_bios_machine *m=vm86_bios_machine_new();
    struct vm86_dos_start start={.segment=0x1000,.environment=0xF00,.parent=0x1000,.path="F:\\KEYS.COM"};
    struct vm86_dos_psp psp;
    vm86_expect_u16("load KEYS.COM",vm86_bios_load_dos(m,&start,corpus_keys,(uint32_t)corpus_keys_size,&psp),VM86_DOS_LOADED);
    struct vm86_bios_plan plan={.slices=10,.steps_per_slice=100};
    vm86_expect_u16("guest waits for input",vm86_bios_run(m,&plan),VM86_BIOS_UNFINISHED);
    static const uint8_t scans[]={0x1E,0x9E,0x2A,0x30,0xB0,0xAA,0xE0,0x48,0xE0,0xC8,0x3B,0xBB,0x2E,0xAE,0x20,0xA0,0x0E,0x8E,0x12,0x92,0x1C};
    enum vm86_bios_stop stop=VM86_BIOS_UNFINISHED;
    for(unsigned i=0;i<sizeof(scans);i++) { vm86_bios_feed_key(m,scans[i]); stop=vm86_bios_run(m,&plan); }
    vm86_expect_u16("keyboard .COM exited",stop,VM86_BIOS_EXITED); vm86_expect_u16("all scan/ASCII pairs and DOS line agreed",vm86_bios_exit_code(m),0);
    vm86_expect_u16("no dropped flags",(uint16_t)vm86_flags_declined(),0);
    vm86_bios_machine_free(m);
}
static void ctrl_c(struct vm86_cpu *cpu)
{
    static const uint8_t code[]={0xB4,0x08,0xCD,0x21,0xFA,0xF4};
    init(cpu); vm86_test_load(cpu,code,sizeof(code)); vm86_flag_set(cpu,VM86_IF,true);
    press(cpu,0x1D); press(cpu,0x2E);
    vm86_expect_u16("08h Ctrl-C reaches INT23",vm86_run(cpu,100),VM86_STOP_EXIT);
    vm86_expect_u16("default break code",cpu->exit_code,3);
    init(cpu); vm86_test_load(cpu,code,sizeof(code)); vm86_flag_set(cpu,VM86_IF,true);
    /* Replace INT23 with a real guest handler, which sets CX and IRETs. */
    vm86_mem_write16(cpu->mem,0x23*4,0x500); vm86_mem_write16(cpu->mem,0x23*4+2,0);
    static const uint8_t handler[]={0xB9,0x34,0x12,0xCF};
    for(unsigned j=0;j<sizeof(handler);j++) vm86_mem_write8(cpu->mem,0x500+j,handler[j]);
    press(cpu,0x1D); press(cpu,0x2E);
    vm86_expect_u16("guest break handler returns",vm86_run(cpu,100),VM86_STOP_HALT);
    vm86_expect_u16("guest INT23 actually ran",cpu->cx,0x1234);
    vm86_expect_u16("nested return has no leak",cpu->sp,0xFFFE);
    vm86_expect_u16("nested break flag frames",(uint16_t)vm86_flags_declined(),0);
}
static const struct vm86_test tests[]={
    {"nonblocking input and status",polling},{"echo/no echo and extended keys",character_modes},
    {"buffered line editing",line},{"blocking retry",blocking_retry},{"set-1 DOS .COM",program},{"Ctrl-C vector and nested IRET",ctrl_c}
};
VM86_TEST_MAIN("input",tests)
