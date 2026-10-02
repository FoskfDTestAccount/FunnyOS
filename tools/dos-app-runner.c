/* Host application probe. Runs the same freestanding interpreter/services as
 * Ring 3; this is diagnostic evidence, not a substitute for ISO acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vm86/cpu.h>
#include <vm86/dos.h>
#include <vm86/exe.h>
#include <vm86/int21.h>
#include <vm86/fat.h>
#include <vm86/host.h>
#include "../dos/bios/bios10.h"
#include "../dos/bios/bios16.h"
#include "../dos/bios/bios1a.h"
static struct bios10_state video;
static struct bios16_state keys;
static struct bios1a_state clock_state;
static struct int21_state dos;
static struct fat_volume volume;
static unsigned calls[256];static bool trace;
static void service(struct vm86_cpu *cpu,void *ctx)
{
    unsigned fn=cpu->ah;calls[fn]++;
    if(trace && calls[fn]<16) {
        fprintf(stderr,"INT21 %02x at %04x:%04x AX=%04x BX=%04x CX=%04x DX=%04x SI=%04x DI=%04x DS=%04x ES=%04x\n",fn,cpu->cs,cpu->ip,cpu->ax,cpu->bx,cpu->cx,cpu->dx,cpu->si,cpu->di,cpu->ds,cpu->es);
        if(fn==0x6c) { char p[80]; unsigned i; for(i=0;i<79;i++) { p[i]=(char)vm86_mem_read8(cpu->mem,((uint32_t)cpu->ds<<4)+(uint16_t)(cpu->si+i)); if(!p[i]) break; } p[i]=0; fprintf(stderr,"  path=%s action=%04x files=%p full=%u\n",p,cpu->dx,(void*)dos.files,(unsigned)dos.full_services); }
    }
    int21_service(cpu,ctx);
    if(trace && calls[fn]<16) fprintf(stderr," -> AX=%04x BX=%04x DX=%04x CF=%u\n",cpu->ax,cpu->bx,cpu->dx,vm86_flag_test(cpu,VM86_CF));
}
static uint8_t *read_file(const char *path,uint32_t *size)
{
    FILE *f=fopen(path,"rb");if(!f) { perror(path);exit(2); }
    fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
    if(n<=0 || n>32*1024*1024) exit(2);
    uint8_t *p=malloc((size_t)n);if(!p || fread(p,1,(size_t)n,f)!=(size_t)n) exit(2);
    fclose(f);*size=(uint32_t)n;return p;
}
static uint8_t scan_for_char(unsigned char ch, bool *shift)
{
    struct key { unsigned char ch; uint8_t scan; bool shift; };
    static const struct key keys_map[] = {
        {'1',0x02,0},{'2',0x03,0},{'3',0x04,0},{'4',0x05,0},
        {'5',0x06,0},{'6',0x07,0},{'7',0x08,0},{'8',0x09,0},
        {'9',0x0A,0},{'0',0x0B,0},{'-',0x0C,0},{'=',0x0D,0},
        {'\b',0x0E,0},{'\t',0x0F,0},{'q',0x10,0},{'w',0x11,0},
        {'e',0x12,0},{'r',0x13,0},{'t',0x14,0},{'y',0x15,0},
        {'u',0x16,0},{'i',0x17,0},{'o',0x18,0},{'p',0x19,0},
        {'[',0x1A,0},{']',0x1B,0},{'\\',0x2B,0},{'a',0x1E,0},
        {'s',0x1F,0},{'d',0x20,0},{'f',0x21,0},{'g',0x22,0},
        {'h',0x23,0},{'j',0x24,0},{'k',0x25,0},{'l',0x26,0},
        {';',0x27,0},{'\'',0x28,0},{'`',0x29,0},{'z',0x2C,0},
        {'x',0x2D,0},{'c',0x2E,0},{'v',0x2F,0},{'b',0x30,0},
        {'n',0x31,0},{'m',0x32,0},{',',0x33,0},{'.',0x34,0},
        {'/',0x35,0},{' ',0x39,0},{'\r',0x1C,0},{'\n',0x1C,0},
        {'!',0x02,1},{'@',0x03,1},{'#',0x04,1},{'$',0x05,1},
        {'%',0x06,1},{'^',0x07,1},{'&',0x08,1},{'*',0x09,1},
        {'(',0x0A,1},{')',0x0B,1},{'_',0x0C,1},{'+',0x0D,1},
        {'Q',0x10,1},{'W',0x11,1},{'E',0x12,1},{'R',0x13,1},
        {'T',0x14,1},{'Y',0x15,1},{'U',0x16,1},{'I',0x17,1},
        {'O',0x18,1},{'P',0x19,1},{'{',0x1A,1},{'}',0x1B,1},
        {'|',0x2B,1},{'A',0x1E,1},{'S',0x1F,1},{'D',0x20,1},
        {'F',0x21,1},{'G',0x22,1},{'H',0x23,1},{'J',0x24,1},
        {'K',0x25,1},{'L',0x26,1},{':',0x27,1},{'"',0x28,1},
        {'~',0x29,1},{'Z',0x2C,1},{'X',0x2D,1},{'C',0x2E,1},
        {'V',0x2F,1},{'B',0x30,1},{'N',0x31,1},{'M',0x32,1},
        {'<',0x33,1},{'>',0x34,1},{'?',0x35,1}
    };
    for (unsigned i=0;i<sizeof(keys_map)/sizeof(keys_map[0]);i++) {
        if (keys_map[i].ch==ch) { *shift=keys_map[i].shift; return keys_map[i].scan; }
    }
    *shift=false; return 0;
}
static bool read_script(const char *path, uint8_t **out, size_t *len)
{
    FILE *f=fopen(path,"rb"); if(!f) return false;
    if(fseek(f,0,SEEK_END) || ftell(f)<0) { fclose(f); return false; }
    long n=ftell(f); rewind(f); if(n>1024*1024) { fclose(f); return false; }
    uint8_t *p=malloc((size_t)n ? (size_t)n : 1); if(!p) { fclose(f); return false; }
    if(n && fread(p,1,(size_t)n,f)!=(size_t)n) { free(p); fclose(f); return false; }
    fclose(f); *out=p; *len=(size_t)n; return true;
}
static void feed_script_key(struct vm86_cpu *cpu, struct bios16_state *st, uint8_t ch)
{
    bool shift=false; uint8_t scan=scan_for_char(ch,&shift);
    if(!scan) return;
    if(shift) { bios16_key_arrived(st,0x2A); bios16_irq(cpu,st); }
    bios16_key_arrived(st,scan); bios16_irq(cpu,st);
    bios16_key_arrived(st,(uint8_t)(scan|0x80)); bios16_irq(cpu,st);
    if(shift) { bios16_key_arrived(st,0xAA); bios16_irq(cpu,st); }
}

int main(int argc,char **argv)
{
    if(argc<4) { fprintf(stderr,"usage: dos-app-runner APP FAT OUTPUT_FAT [tail] [script] [trace]\n");return 2; }
    uint32_t image_size,disk_size;uint8_t *image=read_file(argv[1],&image_size),*disk=read_file(argv[2],&disk_size);
    uint8_t *ram=calloc(1,1024*1024);struct vm86_mem mem;struct vm86_cpu cpu;
    vm86_mem_attach(&mem,ram,1024*1024);vm86_reset(&cpu,&mem);vm86_install_firmware(&cpu);
    bios10_reset(&video);bios16_reset(&keys);bios1a_reset(&clock_state);
    vm86_register_service(0x10,bios10_service,&video);
    vm86_register_service(0x16,bios16_service,&keys);
    vm86_register_service(0x09,bios16_irq,&keys);
    vm86_register_service(0x1A,bios1a_service,&clock_state);
    vm86_register_service(0x08,bios1a_irq,&clock_state);
    vm86_register_service(0x21,service,&dos);
    vm86_register_service(0x20,int21_terminate_service,&dos);
    vm86_register_service(0x23,int21_break_service,&dos);
    uint8_t *script=NULL; size_t script_len=0, script_pos=0;
    if(argc>5 && strcmp(argv[5],"trace")!=0 && !read_script(argv[5],&script,&script_len)) { perror(argv[5]); return 2; }
    trace=(argc>5 && strcmp(argv[5],"trace")==0) || (argc>6 && strcmp(argv[6],"trace")==0);
    struct vm86_dos_start start={.segment=0x1000,.environment=0xF00,.parent=0x1000,.path="F:\\APP.COM",.tail=""};
    struct vm86_dos_psp psp;char tail[128];
    int e=fat_mount(&volume,disk,disk_size);if(e) return 2;
    bool is_exe=image_size>=2 && image[0]=='M' && image[1]=='Z';
    enum vm86_dos_load_result loaded=VM86_DOS_LOADED;
    enum vm86_exe_load_result exe_loaded=VM86_EXE_LOADED;
    if(is_exe) { struct vm86_exe_image exe; exe_loaded=vm86_exe_load(&cpu,image,image_size,&start,&psp,&exe); }
    else loaded=vm86_dos_load(&cpu,image,image_size,&start,&psp);
    if((is_exe && exe_loaded!=VM86_EXE_LOADED) || (!is_exe && loaded!=VM86_DOS_LOADED)) { fprintf(stderr,"load=%u\n",is_exe ? (unsigned)exe_loaded : (unsigned)loaded);return 2; }
    int21_reset(&dos,&video,psp.segment);dos.files=&volume;dos_runtime_init(&cpu,&dos);
    e=dos_redirect(&cpu,&dos,argc>4 ? argv[4] : "",tail,sizeof(tail));if(e) { fprintf(stderr,"redirect=%d\n",e);return 2; }
    size_t n=strlen(tail);vm86_mem_write8(&mem,0x10080,(uint8_t)n);
    for(unsigned i=0;i<n;i++) vm86_mem_write8(&mem,0x10081+i,(uint8_t)tail[i]);
    vm86_mem_write8(&mem,0x10081+(uint32_t)n,13);
    enum vm86_stop stop=VM86_STOP_STEPS;
    for(unsigned i=0;i<200000;i++) {
        stop=vm86_run(&cpu,250);
        if(script_pos<script_len) feed_script_key(&cpu,&keys,script[script_pos++]);
        if(stop==VM86_STOP_EXIT || stop==VM86_STOP_FAULT || stop==VM86_STOP_BROKEN) break;
        if(i%100==0) { bios1a_advance(&clock_state,1);vm86_raise(&cpu,8); }
    }
    fprintf(stderr,"stop=%u exit=%u instructions=%llu CS:IP=%04x:%04x fault=%u\n",stop,cpu.exit_code,(unsigned long long)cpu.insn_count,cpu.cs,cpu.ip,cpu.fault);
    for(unsigned i=0;i<256;i++) if(calls[i]) fprintf(stderr,"calls %02x %u\n",i,calls[i]);
    for(unsigned row=0;row<25;row++) {
        char line[81];for(unsigned col=0;col<80;col++) line[col]=(char)vm86_mem_read8(&mem,0xB8000+(row*80+col)*2);
        line[80]=0;puts(line);
    }
    dos_handles_close_all(&dos);FILE *out=fopen(argv[3],"wb");if(!out || fwrite(disk,1,disk_size,out)!=disk_size) return 2;fclose(out);
    free(script);free(image);free(disk);free(ram);return stop==VM86_STOP_EXIT ? cpu.exit_code : 1;
}
