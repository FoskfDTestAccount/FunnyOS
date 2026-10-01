#include "harness.h"
#include "fat_fixture.h"
#include <vm86/fat.h>
#include <vm86/int21.h>
#include <vm86/host.h>
#include "../bios/bios10.h"
#include "../corpus/bios/replay.h"
static uint8_t image[51200];
static struct fat_volume volume;
static struct int21_state dos;
static struct bios10_state video;
extern const uint8_t corpus_files[];
extern const unsigned long corpus_files_size;
static void init(struct vm86_cpu *cpu)
{
    fixture(image); fat_mount(&volume,image,sizeof(image));
    vm86_mem_clear(cpu->mem); vm86_reset(cpu,cpu->mem);
    bios10_reset(&video); int21_reset(&dos,&video,0x1000); dos.files=&volume;
    vm86_set_seg(cpu,VM86_DS,0x1000);
}
static void path(struct vm86_cpu *cpu,const char *s)
{
    cpu->dx=0x600;
    for(unsigned i=0;;i++) { vm86_mem_write8(cpu->mem,0x10600+i,(uint8_t)s[i]); if(!s[i]) break; }
}
static void call(struct vm86_cpu *cpu,uint16_t ax) { cpu->ax=ax; vm86_flag_set(cpu,VM86_CF,true); int21_service(cpu,&dos); }
static void registers(struct vm86_cpu *cpu)
{
    init(cpu); path(cpu,"F:\\HELLO.TXT"); call(cpu,0x3D00);
    vm86_expect_flag("open clears CF",cpu,VM86_CF,false); vm86_expect_u16("open handle AX",cpu->ax,5);
    cpu->bx=cpu->ax; cpu->dx=0x800; cpu->cx=64; call(cpu,0x3F00);
    vm86_expect_flag("read clears CF",cpu,VM86_CF,false); vm86_expect_u16("read AX short count",cpu->ax,19); vm86_expect_mem8("read DS:DX",cpu,0x10800,'W');
    call(cpu,0x3F00); vm86_expect_u16("EOF count zero",cpu->ax,0);
    cpu->cx=0xFFFF; cpu->dx=0xFFFE; call(cpu,0x4202);
    vm86_expect_flag("signed seek CF",cpu,VM86_CF,false); vm86_expect_u16("position low AX",cpu->ax,17); vm86_expect_u16("position high DX",cpu->dx,0);
    call(cpu,0x3E00); vm86_expect_flag("close CF",cpu,VM86_CF,false);
    call(cpu,0x3E00); vm86_expect_flag("bad handle CF",cpu,VM86_CF,true); vm86_expect_u16("bad handle AX",cpu->ax,6);
    path(cpu,"MISSING.TXT"); call(cpu,0x3D00); vm86_expect_flag("missing CF",cpu,VM86_CF,true); vm86_expect_u16("missing AX",cpu->ax,2);
    path(cpu,"HELLO.TXT"); call(cpu,0x3D03); vm86_expect_u16("invalid mode",cpu->ax,12);
    path(cpu,"NEW.TXT"); cpu->cx=0; call(cpu,0x3C00); vm86_expect_flag("create CF",cpu,VM86_CF,false); cpu->bx=cpu->ax;
    cpu->dx=0x800; cpu->cx=3; call(cpu,0x4000); vm86_expect_flag("write CF",cpu,VM86_CF,false); vm86_expect_u16("write count AX",cpu->ax,3);
    call(cpu,0x3E00); path(cpu,"NEW.TXT"); call(cpu,0x4100); vm86_expect_flag("unlink CF",cpu,VM86_CF,false);
}
static void dta(struct vm86_cpu *cpu)
{
    init(cpu); path(cpu,"*.TXT"); cpu->cx=0; call(cpu,0x4E00);
    vm86_expect_flag("FindFirst CF",cpu,VM86_CF,false);
    vm86_expect_mem8("DTA attributes at 15h",cpu,0x10095,32);
    vm86_expect_mem16("DTA size at 1Ah",cpu,0x1009A,19);
    vm86_expect_mem8("DTA name at 1Eh",cpu,0x1009E,'H');
    vm86_expect_mem8("DTA name extension",cpu,0x100A4,'T');
    vm86_expect_mem8("DTA NUL",cpu,0x100A7,0);
    cpu->dx=0x800; call(cpu,0x1A00); path(cpu,"HELLO.TXT"); cpu->cx=0; call(cpu,0x4E00);
    vm86_expect_flag("second independent DTA",cpu,VM86_CF,false);
    cpu->dx=0x80; call(cpu,0x1A00); call(cpu,0x4F00);
    vm86_expect_flag("FindNext end CF",cpu,VM86_CF,true); vm86_expect_u16("FindNext error 18",cpu->ax,18);
    cpu->dx=0x900; call(cpu,0x1A00); call(cpu,0x4F00); vm86_expect_u16("FindNext without search",cpu->ax,18);
}
static void bad_buffers(struct vm86_cpu *cpu)
{
    init(cpu); path(cpu,"HELLO.TXT"); call(cpu,0x3D00); cpu->bx=cpu->ax;
    vm86_set_seg(cpu,VM86_DS,0x8000); cpu->dx=0xFFF0; cpu->cx=64;
    /* Use a temporarily shortened mapping, independently of full test RAM. */
    uint32_t old=cpu->mem->size; cpu->mem->size=0x20000;
    call(cpu,0x3F00); vm86_expect_flag("invalid buffer CF",cpu,VM86_CF,true); vm86_expect_u16("invalid buffer error",cpu->ax,13);
    vm86_expect_u16("invalid buffer does not consume file",(uint16_t)volume.handles[0].position,0);
    cpu->mem->size=old;
}
static void program(struct vm86_cpu *cpu)
{
    (void)cpu; fixture(image); struct vm86_bios_machine *m=vm86_bios_machine_new();
    struct vm86_dos_start start={.segment=0x1000,.environment=0xF00,.parent=0x1000,.path="F:\\FILES.COM"};
    struct vm86_dos_psp psp;
    vm86_expect_u16("load FILES.COM",vm86_bios_load_dos(m,&start,corpus_files,(uint32_t)corpus_files_size,&psp),VM86_DOS_LOADED);
    vm86_expect_u16("mount independent fixture",vm86_bios_mount_fat(m,image,sizeof(image)),0);
    struct vm86_bios_plan plan={.slices=200,.steps_per_slice=250};
    vm86_expect_u16("FILES.COM exited",vm86_bios_run(m,&plan),VM86_BIOS_EXITED);
    vm86_expect_u16("FILES.COM exit code",vm86_bios_exit_code(m),0);
    const struct vm86_bios_screen *screen=vm86_bios_screen(m);
    const char *expected="W5 FAT says hello";
    for(unsigned i=0;expected[i];i++) vm86_expect_u16("file output on guest screen",vm86_bios_cell_at(screen,(uint16_t)i).character,(uint8_t)expected[i]);
    expected="W5 files: PASS";
    for(unsigned i=0;expected[i];i++) vm86_expect_u16("DOS self-check output",vm86_bios_cell_at(screen,(uint16_t)(80+i)).character,(uint8_t)expected[i]);
    vm86_expect_bool("trap flags have frames",vm86_flags_written_back()!=0,true);
    vm86_expect_u16("no dropped flag writeback",(uint16_t)vm86_flags_declined(),0);
    vm86_bios_machine_free(m);
}
static const struct vm86_test tests[]={{"DOS register contracts",registers},{"DTA ABI and independent searches",dta},{"bad guest buffer",bad_buffers},{"mounted FAT .COM",program}};
VM86_TEST_MAIN("files",tests)
