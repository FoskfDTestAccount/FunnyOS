/* DOS FCB records use guest ABI bytes, never host structure packing. Each
 * opened FCB carries an opaque token in DOS-reserved bytes; copied FCBs retain
 * identity. DTA belongs to the caller and is checked before I/O mutates state. */
#include <vm86/int21.h>
#include <vm86/dos.h>
#include <vm86/fat.h>
#include <libk/string.h>
static uint32_t addr(uint16_t s,uint16_t o){return ((uint32_t)s<<4)+o;}
static uint8_t r8(struct vm86_cpu *c,uint32_t a){return vm86_mem_read8(c->mem,a);}
static uint16_t r16(struct vm86_cpu *c,uint32_t a){return vm86_mem_read16(c->mem,a);}
static uint32_t r32(struct vm86_cpu *c,uint32_t a){return r16(c,a)|((uint32_t)r16(c,a+2)<<16);}
static void w8(struct vm86_cpu *c,uint32_t a,uint8_t v){vm86_mem_write8(c->mem,a,v);}
static void w16(struct vm86_cpu *c,uint32_t a,uint16_t v){vm86_mem_write16(c->mem,a,v);}
static void w32(struct vm86_cpu *c,uint32_t a,uint32_t v){w16(c,a,(uint16_t)v);w16(c,a+2,(uint16_t)(v>>16));}
static bool range(struct vm86_cpu *c,uint32_t a,unsigned n)
{ return a<=c->mem->size && n<=c->mem->size-a; }
static uint32_t fcb(struct vm86_cpu *c)
{ uint32_t a=addr(c->ds,c->dx);return r8(c,a)==0xFF ? a+7 : a; }
static int slot(struct vm86_cpu *c,struct int21_state *st,uint32_t a)
{
    if(r16(c,a+24)!=0xFBCB) return -1;
    uint16_t token=r16(c,a+26);
    for(unsigned i=0;i<20;i++) if(st->fcb[i].used && st->fcb[i].token==token) return (int)i;
    return -1;
}
static bool name(struct vm86_cpu *c,uint32_t a,char path[20],bool wildcard)
{
    unsigned n=0;uint8_t drive=r8(c,a);
    if(drive && drive!=6) return false;
    path[n++]='F';path[n++]=':';path[n++]='\\';
    for(unsigned i=0;i<8;i++) { uint8_t b=r8(c,a+1+i);if(b!=' ') { if(!wildcard && (b=='?' || b=='*')) return false;path[n++]=(char)b; } }
    bool ext=false;for(unsigned i=0;i<3;i++) if(r8(c,a+9+i)!=' ') ext=true;
    if(ext) { path[n++]='.';for(unsigned i=0;i<3;i++) { uint8_t b=r8(c,a+9+i);if(b!=' ') path[n++]=(char)b; } }
    path[n]=0;return n>3;
}
static int entry(struct vm86_cpu *c,struct int21_state *st,uint32_t a,uint32_t *out)
{
    char p[20];uint8_t nm[11];uint16_t dir;uint32_t index=0;
    if(!st->files || !name(c,a,p,true)) return 15;
    int e=fat_parent(st->files,p,&dir,nm,true);if(e)return e;
    return fat_find(st->files,dir,&index,nm,0x27,out);
}
static void metadata(struct vm86_cpu *c,struct int21_state *st,uint32_t a,uint32_t e)
{
    w32(c,a+16,fat_u32(st->files->image+e+28));w16(c,a+20,fat_u16(st->files->image+e+24));w16(c,a+22,fat_u16(st->files->image+e+22));
}
static void open_fcb(struct vm86_cpu *c,struct int21_state *st,uint32_t a,bool create)
{
    char p[20];uint16_t fd;unsigned i=0;uint32_t e;
    while(i<20 && st->fcb[i].used)i++;
    if(i==20 || !name(c,a,p,false)) { c->al=0xFF;return; }
    int err=dos_handle_open(st,p,2,create,0,&fd);
    if(err) { c->al=0xFF;return; }
    st->fcb[i].used=true;st->fcb[i].fd=fd;st->fcb[i].token=st->fcb_next++;
    if(!st->fcb_next)st->fcb_next=1;
    w16(c,a+24,0xFBCB);w16(c,a+26,st->fcb[i].token);w16(c,a+12,0);w16(c,a+14,128);w8(c,a+32,0);
    if(!entry(c,st,a,&e))metadata(c,st,a,e);
    w8(c,a,6);c->al=0;
}
static uint32_t record(struct vm86_cpu *c,uint32_t a)
{ return (uint32_t)r16(c,a+12)*128u+r8(c,a+32); }
static void set_record(struct vm86_cpu *c,uint32_t a,uint32_t rec)
{ w16(c,a+12,(uint16_t)(rec/128u));w8(c,a+32,(uint8_t)(rec%128u)); }
static uint32_t random_record(struct vm86_cpu *c,uint32_t a)
{ uint32_t v=r32(c,a+33);return r16(c,a+14)>=64 ? v&0xFFFFFFu : v; }
static void transfer(struct vm86_cpu *c,struct int21_state *st,uint32_t a,uint8_t fn)
{
    int s=slot(c,st,a);if(s<0) { c->al=1;return; }
    uint16_t len=r16(c,a+14);if(!len) { c->al=1;return; }
    bool random=fn==0x21 || fn==0x22 || fn==0x27 || fn==0x28;
    bool writing=fn==0x15 || fn==0x22 || fn==0x28;
    bool block=fn==0x27 || fn==0x28;
    uint32_t rec=random ? random_record(c,a) : record(c,a);
    uint32_t number=block ? c->cx : 1;
    uint32_t dta=addr(st->dta_segment,st->dta_offset),completed=0;
    uint8_t result=0,buf[512];uint16_t fd=st->fcb[s].fd;
    struct dos_open_object *o=&st->objects[st->jft[fd]];
    if(o->kind!=2) { c->al=1;return; }
    if((uint64_t)number*len>0x10000u-st->dta_offset || !range(c,dta,number*len)) { c->al=2;if(block)c->cx=0;return; }
    if((uint64_t)rec*len>0x7FFFFFFFu) { c->al=1;return; }
    uint32_t pos;int e=fat_seek(st->files,o->handle,0,(int32_t)(rec*len),&pos);
    if(e) { c->al=1;return; }
    for(uint32_t j=0;j<number;j++) {
        uint32_t total=0;
        while(total<len) {
            uint32_t n=len-total,done=0;if(n>sizeof(buf))n=sizeof(buf);
            if(writing)for(unsigned k=0;k<n;k++)buf[k]=r8(c,dta+j*len+total+k);
            e=writing ? dos_handle_write(c,st,fd,buf,n,&done) : dos_handle_read(c,st,fd,buf,n,&done);
            if(!writing && !e)for(unsigned k=0;k<done;k++)w8(c,dta+j*len+total+k,buf[k]);
            total+=done;if(e || done<n)break;
        }
        if(e || (!total && len)) { result=1;break; }
        completed++;
        if(total<len) {
            if(!writing)for(uint32_t k=total;k<len;k++)w8(c,dta+j*len+k,0);
            result=writing ? 1 : 3;break;
        }
    }
    if(!random || block)set_record(c,a,rec+completed);
    else set_record(c,a,rec);
    if(block) { w32(c,a+33,rec+completed);c->cx=(uint16_t)completed; }
    if(writing) { uint32_t en;if(!entry(c,st,a,&en))metadata(c,st,a,en); }
    c->al=result;
}
static bool delimiter(uint8_t b)
{ return !b || b==13 || b==' ' || b=='\t' || b==',' || b==';' || b=='=' || b=='+' || b=='/' || b=='\\' || b=='"' || b=='[' || b==']' || b=='<' || b=='>' || b=='|'; }
static void parse(struct vm86_cpu *c)
{
    uint8_t flags=c->al;uint16_t off=c->si;uint32_t dest=addr(c->es,c->di);bool wild=false;
    if(flags&1)while(r8(c,addr(c->ds,off))==' ' || r8(c,addr(c->ds,off))=='\t' || r8(c,addr(c->ds,off))==',' || r8(c,addr(c->ds,off))==';' || r8(c,addr(c->ds,off))=='=')off++;
    if(!(flags&2))w8(c,dest,0);
    if(!(flags&4))for(unsigned i=0;i<8;i++)w8(c,dest+1+i,' ');
    if(!(flags&8))for(unsigned i=0;i<3;i++)w8(c,dest+9+i,' ');
    uint8_t b=r8(c,addr(c->ds,off));
    if(r8(c,addr(c->ds,(uint16_t)(off+1)))==':') {
        if(b>='a' && b<='z')b-=32;
        if(b<'A' || b>'Z') { c->al=0xFF;c->si=off;return; }
        w8(c,dest,(uint8_t)(b-'A'+1));off+=2;
    }
    unsigned component=0,index=0;
    for(unsigned seen=0;seen<127;seen++) {
        b=r8(c,addr(c->ds,off));if(delimiter(b))break;
        if(b=='.') { component=1;index=0;off++;continue; }
        if(b>='a' && b<='z')b-=32;
        unsigned max=component ? 3 : 8,base=component ? 9 : 1;
        if(b=='*') { wild=true;while(index<max)w8(c,dest+base+index++,'?'); }
        else if(index<max) { if(b=='?')wild=true;w8(c,dest+base+index++,b); }
        off++;
    }
    c->si=off;c->al=wild ? 1 : 0;
}
static void find(struct vm86_cpu *c,struct int21_state *st,uint32_t a,bool first)
{
    uint32_t d=addr(st->dta_segment,st->dta_offset),index=0,e;uint16_t dir=0;uint8_t nm[11];bool ext=r8(c,addr(c->ds,c->dx))==0xFF;
    if(!st->files || !range(c,d,ext ? 44 : 37)) { c->al=0xFF;return; }
    if(first) { char p[20];if(!name(c,a,p,true) || fat_parent(st->files,p,&dir,nm,true)) { c->al=0xFF;return; } }
    else {
        uint32_t base=ext ? d+7 : d;if(r16(c,base+24)!=0xFCBF) { c->al=0xFF;return; }
        for(unsigned i=0;i<11;i++)nm[i]=r8(c,base+1+i);
        dir=r16(c,base+26);index=r32(c,base+28);
    }
    if(fat_find(st->files,dir,&index,nm,ext ? r8(c,a-1) : 0,&e)) { c->al=0xFF;return; }
    uint32_t base=ext ? d+7 : d;
    if(ext) { for(unsigned i=0;i<7;i++)w8(c,d+i,0);w8(c,d,0xFF);w8(c,d+6,st->files->image[e+11]); }
    for(unsigned i=0;i<37;i++)w8(c,base+i,0);
    w8(c,base,6);for(unsigned i=0;i<11;i++)w8(c,base+1+i,st->files->image[e+i]);
    metadata(c,st,base,e);w16(c,base+24,0xFCBF);w16(c,base+26,dir);w32(c,base+28,index);
    /* Preserve the pattern for FindNext in caller FCB DOS-reserved bytes. */
    for(unsigned i=0;i<11;i++)w8(c,base+1+i,nm[i]);
    c->al=0;
}
void dos_fcb_call(struct vm86_cpu *c,struct int21_state *st)
{
    if(!st->full_services) { c->al=0;vm86_flag_set(c,VM86_CF,true);return; }
    uint8_t fn=c->ah;uint32_t a=fcb(c);
    if(fn==0x29) { parse(c);return; }
    if(!range(c,a,37)) { c->al=0xFF;return; }
    switch(fn) {
    case 0x0F: case 0x16:open_fcb(c,st,a,fn==0x16);break;
    case 0x10: { int s=slot(c,st,a);if(s<0)c->al=0xFF;else { (void)dos_handle_close(st,st->fcb[s].fd);st->fcb[s].used=false;w16(c,a+24,0);c->al=0; }break; }
    case 0x11:case 0x12:find(c,st,a,fn==0x11);break;
    case 0x13: { char p[20];c->al=(!st->files || !name(c,a,p,false) || fat_unlink(st->files,p)) ? 0xFF : 0;break; }
    case 0x14:case 0x15:case 0x21:case 0x22:case 0x27:case 0x28:transfer(c,st,a,fn);break;
    case 0x23: { uint32_t e;uint16_t len=r16(c,a+14);if(!len)len=128;if(entry(c,st,a,&e))c->al=0xFF;else { uint32_t n=fat_u32(st->files->image+e+28);w32(c,a+33,(n+len-1u)/len);c->al=0; }break; }
    case 0x24:w32(c,a+33,record(c,a));break;
    case 0x17: { uint32_t e;char p[20];uint8_t nm[11];if(entry(c,st,a,&e) || !name(c,a+16,p,false) || fat_name(p+3,nm,false))c->al=0xFF;else { memcpy(st->files->image+e,nm,11);c->al=0; }break; }
    default:c->al=0xFF;break;
    }
    dos_handles_sync(c,st);
}
