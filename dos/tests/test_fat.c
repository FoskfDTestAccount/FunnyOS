#include "harness.h"
#include "fat_fixture.h"
#include <vm86/fat.h>
#include <stdlib.h>
static uint8_t image[51200];
static struct fat_volume v;
static void eq(const char *s,uint32_t a,uint32_t b) { vm86_expect_u16(s,(uint16_t)a,(uint16_t)b); vm86_expect_u16(s,(uint16_t)(a>>16),(uint16_t)(b>>16)); }
static void init(void) { fixture(image); eq("mount FAT12",fat_mount(&v,image,sizeof(image)),0); }
static void mount_bad(struct vm86_cpu *cpu)
{
    (void)cpu; init(); eq("FAT12 inferred from clusters",v.bits,12);
    image[510]=0; eq("signature",fat_mount(&v,image,sizeof(image)),13); fixture(image);
    eq("truncated medium",fat_mount(&v,image,sizeof(image)-1),13);
    image[13]=3; eq("non-power-of-two clusters",fat_mount(&v,image,sizeof(image)),13); fixture(image);
    fixture16(image+19,4); eq("metadata exceeds medium",fat_mount(&v,image,sizeof(image)),13); fixture(image);
    image[16]=0; eq("no FAT copies",fat_mount(&v,image,sizeof(image)),13); fixture(image);
    fixture16(image+17,0); eq("FAT32 root is not FAT16",fat_mount(&v,image,sizeof(image)),13);
    eq("null image",fat_mount(&v,0,0),13);
}
static void read_seek(struct vm86_cpu *cpu)
{
    (void)cpu; init(); uint16_t h; uint8_t out[80]; uint32_t n,pos;
    eq("open case-insensitive drive path",fat_open(&v,"f:\\hello.txt",0,false,0,&h),0); eq("first handle",h,5);
    eq("read",fat_read(&v,h,out,80,&n),0); eq("short read at EOF",n,19); eq("independent file bytes",memcmp(out,"W5 FAT says hello\r\n",19),0);
    eq("EOF",fat_read(&v,h,out,80,&n),0); eq("zero at EOF",n,0);
    eq("seek -2 from end",fat_seek(&v,h,2,-2,&pos),0); eq("position",pos,17);
    eq("read end",fat_read(&v,h,out,80,&n),0); eq("CR",out[0],13); eq("LF",out[1],10);
    eq("negative absolute seek rejected",fat_seek(&v,h,0,-1,&pos),1);
    eq("invalid origin",fat_seek(&v,h,3,0,&pos),1);
    eq("read-only write",fat_write(&v,h,out,1,&n),5);
    eq("close",fat_close(&v,h),0); eq("double close",fat_close(&v,h),6); eq("closed read",fat_read(&v,h,out,1,&n),6);
    eq("missing file",fat_open(&v,"MISSING.TXT",0,false,0,&h),2);
    eq("missing directory",fat_open(&v,"SUB\\HELLO.TXT",0,false,0,&h),3);
    eq("wrong drive",fat_open(&v,"A:HELLO.TXT",0,false,0,&h),15);
    eq("illegal mode",fat_open(&v,"HELLO.TXT",3,false,0,&h),12);
}
static void writes(struct vm86_cpu *cpu)
{
    (void)cpu; init(); uint16_t h; uint32_t n,pos; uint8_t in[1537],out[2048];
    for(unsigned i=0;i<sizeof(in);i++) in[i]=(uint8_t)(i%251);
    eq("create",fat_open(&v,"NEW.BIN",2,true,0,&h),0);
    eq("write across three clusters",fat_write(&v,h,in,sizeof(in),&n),0); eq("written",n,sizeof(in));
    eq("copies kept in sync",memcmp(image+512,image+1024,512),0);
    eq("rewind",fat_seek(&v,h,0,0,&pos),0); eq("read across clusters",fat_read(&v,h,out,sizeof(out),&n),0); eq("read count",n,sizeof(in)); eq("exact bytes",memcmp(in,out,n),0);
    eq("seek beyond EOF",fat_seek(&v,h,0,1800,&pos),0); eq("extend",fat_write(&v,h,in,1,&n),0);
    eq("rewind into gap",fat_seek(&v,h,0,1537,&pos),0); eq("read zero gap",fat_read(&v,h,out,264,&n),0);
    unsigned nonzero=0; for(unsigned i=0;i<263;i++) nonzero+=out[i]!=0; eq("gap zero filled",nonzero,0); eq("appended byte",out[263],in[0]);
    eq("seek truncate",fat_seek(&v,h,0,3,&pos),0); eq("zero write truncates",fat_write(&v,h,in,0,&n),0);
    eq("metadata size",fat_u32(image+v.handles[h-5].entry+28),3);
    eq("unlink open file denied",fat_unlink(&v,"NEW.BIN"),5); eq("close",fat_close(&v,h),0);
    eq("delete",fat_unlink(&v,"NEW.BIN"),0); eq("deleted file",fat_open(&v,"NEW.BIN",0,false,0,&h),2);
    eq("reuse deleted directory slot",fat_open(&v,"NEXT.BIN",1,true,0,&h),0);
    eq("write-only read denied",fat_read(&v,h,out,1,&n),5);
    eq("close",fat_close(&v,h),0);
    eq("create truncates existing",fat_open(&v,"HELLO.TXT",2,true,0,&h),0);
    eq("empty file",fat_read(&v,h,out,80,&n),0); eq("empty count",n,0);
}
static void exhaustion_corruption(struct vm86_cpu *cpu)
{
    (void)cpu; init(); uint16_t h; uint32_t n; uint8_t out[600];
    for(unsigned i=0;i<16;i++) eq("allocate independent handles",fat_open(&v,"HELLO.TXT",0,false,0,&h),0);
    eq("handle limit",fat_open(&v,"HELLO.TXT",0,false,0,&h),4);
    init(); fixture12(image,2,2); eq("cyclic chain rejected",fat_open(&v,"HELLO.TXT",0,false,0,&h),13);
    init(); fixture12(image,2,0xFF7); eq("bad cluster rejected",fat_open(&v,"HELLO.TXT",0,false,0,&h),13);
    init(); fixture32(image+1564,600); eq("size longer than chain",fat_open(&v,"HELLO.TXT",0,false,0,&h),13);
    init(); eq("create on full volume",fat_open(&v,"NEW.TXT",2,true,0,&h),0);
    for(uint16_t c=3;c<97;c++) fixture12(image,c,0xFFF);
    eq("full disk fails without allocation",fat_write(&v,h,out,1,&n),0); eq("zero bytes",n,0);
    eq("no first cluster leaked",fat_u16(image+v.handles[h-5].entry+26),0);
    eq("unchanged metadata",fat_u32(image+v.handles[h-5].entry+28),0);
    fixture12(image,95,0);
    eq("full disk permits a partial write",fat_write(&v,h,out,sizeof(out),&n),0);
    eq("one free cluster short count",n,512);
    eq("short write metadata",fat_u32(image+v.handles[h-5].entry+28),512);
    uint32_t pos;
    eq("seek past full disk EOF",fat_seek(&v,h,0,600,&pos),0);
    eq("zero-byte extension fails honestly",fat_write(&v,h,out,0,&n),39);
    eq("failed extension preserves size",fat_u32(image+v.handles[h-5].entry+28),512);
}
static void search_subdir(struct vm86_cpu *cpu)
{
    (void)cpu; init(); uint8_t pattern[11]; uint32_t i=0,off; uint16_t dir,h;
    eq("wildcard",fat_parent(&v,"F:\\*.TXT",&dir,pattern,true),0);
    eq("find",fat_find(&v,dir,&i,pattern,0,&off),0); eq("directory entry",off,1536);
    eq("no more",fat_find(&v,dir,&i,pattern,0,&off),18);
    image[1547]=1; i=0; eq("read-only included in normal search",fat_find(&v,dir,&i,pattern,0,&off),0);
    eq("readonly write open denied",fat_open(&v,"HELLO.TXT",2,false,0,&h),5);
    image[1547]=2; i=0; eq("hidden excluded",fat_find(&v,dir,&i,pattern,0,&off),18); i=0;
    eq("hidden explicitly included",fat_find(&v,dir,&i,pattern,2,&off),0);
    init(); memcpy(image+1568,"SUB        ",11); image[1579]=16; fixture16(image+1594,3); fixture12(image,3,0xFFF);
    memcpy(image+3072,"INSIDE  TXT",11); image[3083]=32; fixture16(image+3098,2); fixture32(image+3100,19);
    eq("open through directory chain",fat_open(&v,"F:\\SUB\\INSIDE.TXT",0,false,0,&h),0);
    eq("cannot open directory as file",fat_open(&v,"SUB",0,false,0,&h),5);
    eq("reject long base",fat_name("TOOLONGXX.TXT",pattern,false),3);
    eq("reject repeated dots",fat_name("A.B.C",pattern,false),3);
}
static void fat16(struct vm86_cpu *cpu)
{
    (void)cpu; uint32_t size=8192u*512; uint8_t *p=calloc(1,size); if(!p) { eq("allocation",0,1); return; }
    fixture16(p+11,512); p[13]=1; fixture16(p+14,1); p[16]=2; fixture16(p+17,32); fixture16(p+19,8192); fixture16(p+22,32); p[510]=0x55; p[511]=0xAA;
    eq("mount FAT16",fat_mount(&v,p,size),0); eq("FAT16 type",v.bits,16);
    uint16_t h; uint32_t n,pos; uint8_t out[4];
    eq("FAT16 create",fat_open(&v,"NEW.TXT",2,true,0,&h),0); eq("FAT16 write",fat_write(&v,h,(const uint8_t *)"abcd",4,&n),0);
    eq("FAT16 rewind",fat_seek(&v,h,0,0,&pos),0); eq("FAT16 read",fat_read(&v,h,out,4,&n),0); eq("FAT16 bytes",memcmp(out,"abcd",4),0);
    eq("FAT16 copies",memcmp(p+512,p+512+16384,16384),0); free(p);
}
static void fragmented(struct vm86_cpu *cpu)
{
    (void)cpu; init(); uint16_t h; uint32_t n,pos; uint8_t out[1536];
    fixture12(image,2,5); fixture12(image,5,3); fixture12(image,3,0xFFF);
    fixture32(image+1564,1200);
    memset(image+2560,'a',512); memset(image+4096,'b',512); memset(image+3072,'c',512);
    eq("open fragmented file",fat_open(&v,"HELLO.TXT",2,false,0,&h),0);
    eq("read fragmented chain",fat_read(&v,h,out,sizeof(out),&n),0);
    eq("fragmented size",n,1200);
    unsigned wrong=0;
    for(unsigned i=0;i<n;i++) wrong+=out[i]!=(i<512 ? 'a' : i<1024 ? 'b' : 'c');
    eq("noncontiguous cluster order",wrong,0);
    eq("seek into second cluster",fat_seek(&v,h,0,511,&pos),0);
    eq("rewrite across fragmented boundary",fat_write(&v,h,(const uint8_t *)"XY",2,&n),0);
    eq("first physical cluster changed",image[3071],'X');
    eq("nonadjacent physical cluster changed",image[4096],'Y');
    fixture12(image,5,2);
    eq("cycle introduced after open is rejected",fat_read(&v,h,out,1,&n),13);
}
static const struct vm86_test tests[]={
    {"BPB validation",mount_bad},{"read and signed seek",read_seek},{"writes/truncate/delete",writes},
    {"exhaustion and corrupt chains",exhaustion_corruption},{"search and subdirectories",search_subdir},{"FAT16",fat16},{"fragmented cluster chains",fragmented}
};
VM86_TEST_MAIN("fat",tests)
