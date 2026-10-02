#include "harness.h"
#include <vm86/exe.h>
#include <libk/string.h>
#include <vm86/dos.h>
static uint8_t file[64];
static void make_exe(void)
{
    memset(file,0,sizeof(file));
    file[0]='M';file[1]='Z';
    file[2]=0x40;file[3]=0; /* bytes in final page */
    file[4]=1;file[5]=0; /* pages */
    file[6]=1;file[7]=0; /* relocation count */
    file[8]=2;file[9]=0; /* header paragraphs = 32 bytes */
    file[0x0e]=0;file[0x0f]=0; /* initial SS */
    file[0x10]=0xfe;file[0x11]=0xff; /* SP */
    file[0x14]=0;file[0x15]=0; /* IP */
    file[0x16]=0;file[0x17]=0; /* CS */
    file[0x18]=0x1c;file[0x19]=0; /* relocation table */
    file[0x1c]=1;file[0x1d]=0; /* offset in image */
    file[0x1e]=0;file[0x1f]=0; /* segment */
    file[0x20]=0xb8;file[0x21]=0;file[0x22]=0x10; /* mov ax,1000; relocated */
}
static void loads(struct vm86_cpu *cpu)
{
    make_exe();struct vm86_dos_start start={.segment=0x1000,.environment=0xf00,.parent=0x1000,.path="F:\\T.EXE"};
    struct vm86_exe_image out;struct vm86_dos_psp psp;
    vm86_expect_u16("MZ loads",vm86_exe_load(cpu,file,sizeof(file),&start,&psp,&out),VM86_EXE_LOADED);
    vm86_expect_u16("entry CS",out.cs,0x1010);vm86_expect_u16("entry DS",out.ds,0x1000);
    vm86_expect_u16("entry SS",out.ss,0x1010);vm86_expect_u16("entry IP",out.ip,0);
    vm86_expect_u16("relocated word",vm86_mem_read16(cpu->mem,0x10101),0x2010);
    vm86_expect_u16("PSP parent",vm86_mem_read16(cpu->mem,0x10000+VM86_PSP_PARENT),0x1000);
}
static void rejects(struct vm86_cpu *cpu)
{
    struct vm86_dos_start s={.segment=0x1000,.environment=0xf00};
    struct vm86_exe_image o; struct vm86_dos_psp p;
    make_exe();file[0]='X';
    vm86_expect_u16("bad signature",vm86_exe_load(cpu,file,sizeof(file),&s,&p,&o),VM86_EXE_BAD_SIGNATURE);
    make_exe();file[2]=0;file[3]=0;
    vm86_expect_u16("truncated page",vm86_exe_load(cpu,file,sizeof(file),&s,&p,&o),VM86_EXE_TRUNCATED);
    make_exe();file[0x1c]=0x40;
    vm86_expect_u16("relocation range",vm86_exe_load(cpu,file,sizeof(file),&s,&p,&o),VM86_EXE_RELOCATION_RANGE);
}

static void rejects_header_without_mutation(struct vm86_cpu *cpu)
{
    struct vm86_dos_start s={.segment=0x1000,.environment=0xf00};
    struct vm86_exe_image o; struct vm86_dos_psp p;
    make_exe();
    file[0x18]=0x20; file[0x19]=0; /* relocation table no longer in header */
    vm86_mem_write8(cpu->mem,0x10000,0xa5);
    vm86_expect_u16("relocation table header refusal",
                     vm86_exe_load(cpu,file,sizeof(file),&s,&p,&o),
                     VM86_EXE_BAD_HEADER);
    vm86_expect_mem8("bad header leaves PSP untouched",cpu,0x10000,0xa5);

    make_exe();file[4]=0;file[5]=0;
    vm86_expect_u16("zero pages refusal",
                     vm86_exe_load(cpu,file,sizeof(file),&s,&p,&o),
                     VM86_EXE_BAD_HEADER);
    make_exe();file[2]=0x01;file[3]=0x03;
    vm86_expect_u16("oversized final page refusal",
                     vm86_exe_load(cpu,file,sizeof(file),&s,&p,&o),
                     VM86_EXE_BAD_HEADER);
}
static const struct vm86_test tests[]={ {"MZ header, image and relocation",loads}, {"malformed MZ refusal",rejects}, {"header refusal is transactional",rejects_header_without_mutation} };
VM86_TEST_MAIN("exe",tests)
