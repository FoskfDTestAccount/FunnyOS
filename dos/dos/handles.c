/* DOS JFT/SFT semantics: descriptor copies refer to one open file position.
 * Device descriptors can be closed and reused; fd 0 is not intrinsically CON. */
#include <vm86/int21.h>
#include <vm86/dos.h>
#include <vm86/fat.h>
#include <vm86/host.h>
#include <libk/string.h>
#include "../bios/bios10.h"
#include "../bios/bios16.h"
static struct int21_state *handle_table(struct int21_state *st)
{ return st->handle_parent ? st->handle_parent : st; }
static struct dos_open_object *object(struct int21_state *st,uint16_t fd)
{
    struct int21_state *table=handle_table(st);
    if(fd>=DOS_JFT_MAX || st->jft[fd]>=DOS_SFT_MAX) return NULL;
    struct dos_open_object *o=&table->objects[st->jft[fd]];
    return o->refs ? o : NULL;
}
void dos_handles_reset(struct int21_state *st)
{
    st->handle_parent = NULL;
    memset(st->objects,0,sizeof(st->objects));memset(st->jft,0xFF,sizeof(st->jft));
    for(unsigned i=0;i<5;i++) {
        st->jft[i]=(uint8_t)i;st->objects[i]=(struct dos_open_object){.refs=1,.kind=1,.mode=2};
    }
}
void dos_handles_sync(struct vm86_cpu *cpu,struct int21_state *st)
{
    if(!st->psp) return;
    for(unsigned i=0;i<DOS_JFT_MAX;i++)
        vm86_mem_write8(cpu->mem,((uint32_t)st->psp<<4)+VM86_PSP_JFT+i,st->jft[i]);
}
int dos_handle_close(struct int21_state *st,uint16_t fd)
{
    struct dos_open_object *o=object(st,fd);if(!o) return 6;
    st->jft[fd]=0xFF;
    if(--o->refs==0) {
        struct int21_state *table=handle_table(st);
        if(o->kind==2 && table->files) (void)fat_close(table->files,o->handle);
        memset(o,0,sizeof(*o));
    }
    return 0;
}
void dos_handles_close_all(struct int21_state *st)
{ for(unsigned i=0;i<DOS_JFT_MAX;i++) if(object(st,(uint16_t)i)) (void)dos_handle_close(st,(uint16_t)i); }
static bool device(const char *path)
{
    const char *p=path;for(const char *q=path;*q;q++) if(*q=='\\' || *q=='/' || *q==':') p=q+1;
    return (p[0]=='C'||p[0]=='c') && (p[1]=='O'||p[1]=='o') && (p[2]=='N'||p[2]=='n') && (!p[3] || (p[3]==':' && !p[4]));
}
int dos_handle_open(struct int21_state *st,const char *path,uint8_t mode,bool create,uint16_t attr,uint16_t *fd)
{
    struct int21_state *table=handle_table(st);
    unsigned slot=0,obj=0;
    if(mode>2) return 12;
    while(slot<DOS_JFT_MAX && st->jft[slot]!=0xFF) slot++;
    while(obj<DOS_SFT_MAX && table->objects[obj].refs) obj++;
    if(slot==DOS_JFT_MAX || obj==DOS_SFT_MAX) return 4;
    struct dos_open_object o={.refs=1,.mode=mode,.kind=1};
    if(!device(path)) {
        if(!st->files) return 15;
        int e=fat_open(st->files,path,mode,create,attr,&o.handle);if(e) return e;
        o.kind=2;
    }
    table->objects[obj]=o;st->jft[slot]=(uint8_t)obj;*fd=(uint16_t)slot;return 0;
}
int dos_handle_dup(struct int21_state *st,uint16_t fd,int target,uint16_t *out)
{
    struct dos_open_object *o=object(st,fd);if(!o) return 6;
    unsigned slot=target<0 ? 0u : (unsigned)target;
    if(target<0) while(slot<DOS_JFT_MAX && st->jft[slot]!=0xFF) slot++;
    if(slot>=DOS_JFT_MAX) return target<0 ? 4 : 6;
    if(slot==fd) { *out=fd;return 0; }
    struct int21_state *table=handle_table(st);
    uint8_t id=st->jft[fd];
    if(object(st,(uint16_t)slot)) (void)dos_handle_close(st,(uint16_t)slot);
    st->jft[slot]=id;table->objects[id].refs++;*out=(uint16_t)slot;return 0;
}
int dos_handle_read(struct vm86_cpu *cpu,struct int21_state *st,uint16_t fd,uint8_t *buf,uint32_t count,uint32_t *done)
{
    *done=0;struct dos_open_object *o=object(st,fd);if(!o) return 6;
    if(o->mode==1) return 5;
    if(o->kind==2) return st->files ? fat_read(st->files,o->handle,buf,count,done) : 6;
    if(!count) return 0;
    uint16_t word;if(!bios16_take(cpu,&word)) return -1;
    buf[0]=(uint8_t)word;*done=1;return 0;
}
int dos_handle_write(struct vm86_cpu *cpu,struct int21_state *st,uint16_t fd,const uint8_t *buf,uint32_t count,uint32_t *done)
{
    *done=0;struct dos_open_object *o=object(st,fd);if(!o) return 6;
    if(o->mode==0) return 5;
    if(o->kind==2) return st->files ? fat_write(st->files,o->handle,buf,count,done) : 6;
    for(uint32_t i=0;i<count;i++) bios10_tty(cpu,st->video,buf[i]);
    *done=count;return 0;
}
void dos_output(struct vm86_cpu *cpu,struct int21_state *st,uint8_t ch)
{ uint32_t done;(void)dos_handle_write(cpu,st,1,&ch,1,&done); }
static uint32_t addr(uint16_t seg,uint16_t off,unsigned i)
{ return ((uint32_t)seg<<4)+(uint16_t)(off+i); }
static bool range(struct vm86_cpu *cpu,uint16_t seg,uint16_t off,unsigned n)
{ for(unsigned i=0;i<n;i++) if(vm86_mem_offset(cpu->mem,addr(seg,off,i))==VM86_MEM_UNMAPPED) return false;return true; }
static bool path(struct vm86_cpu *cpu,char out[128])
{ for(unsigned i=0;i<128;i++) { uint32_t a=addr(cpu->ds,cpu->dx,i);if(vm86_mem_offset(cpu->mem,a)==VM86_MEM_UNMAPPED) return false;out[i]=(char)vm86_mem_read8(cpu->mem,a);if(!out[i]) return true; }return false; }
static void transfer(struct vm86_cpu *cpu,struct int21_state *st,bool writing,int *error)
{
    unsigned count=cpu->cx,total=0;uint8_t buf[512];*error=0;
    if(!range(cpu,cpu->ds,cpu->dx,count)) { *error=13;return; }
    do {
        uint32_t n=count-total,done=0;if(n>sizeof(buf)) n=sizeof(buf);
        if(writing) {
            for(unsigned i=0;i<n;i++) buf[i]=vm86_mem_read8(cpu->mem,addr(cpu->ds,cpu->dx,total+i));
            *error=dos_handle_write(cpu,st,cpu->bx,buf,n,&done);
        } else {
            *error=dos_handle_read(cpu,st,cpu->bx,buf,n,&done);
            if(!*error) for(unsigned i=0;i<done;i++) vm86_mem_write8(cpu->mem,addr(cpu->ds,cpu->dx,total+i),buf[i]);
        }
        if(*error<0 && !total) { vm86_service_retry(cpu);return; }
        total+=done;if(*error || done<n) break;
    } while(total<count);
    if(total) *error=0;
    if(!*error) cpu->ax=(uint16_t)total;
}
static int rename_file(struct vm86_cpu *cpu, struct int21_state *st)
{
    char old_name[128], new_name[128];
    uint8_t old_short[11], new_short[11];
    uint16_t old_dir, new_dir;
    uint32_t index = 0, entry = 0;
    uint32_t old_at, new_at;
    if (!st->files || !path(cpu, old_name)) return 3;
    /* AH=56h uses DS:DX for the source and ES:DI for the destination. */
    uint16_t save_ds = cpu->ds, save_dx = cpu->dx;
    cpu->ds = cpu->es; cpu->dx = cpu->di;
    bool ok = path(cpu, new_name);
    cpu->ds = save_ds; cpu->dx = save_dx;
    if (!ok) return 3;
    int e = fat_parent(st->files, old_name, &old_dir, old_short, false);
    if (e) return e;
    e = fat_find(st->files, old_dir, &index, old_short, 0x27, &entry);
    if (e) return e == 18 ? 2 : e;
    e = fat_parent(st->files, new_name, &new_dir, new_short, false);
    if (e) return e;
    if (old_dir != new_dir) return 3;
    index = 0;
    e = fat_find(st->files, new_dir, &index, new_short, 0x37, &new_at);
    if (!e) return 80;             /* destination already exists */
    if (e != 18) return e;
    old_at = entry;
    for (unsigned i = 0; i < 11; ++i)
        st->files->image[old_at + i] = new_short[i];
    return 0;
}

static int file_time(struct vm86_cpu *cpu, struct int21_state *st)
{
    struct dos_open_object *o = object(st, cpu->bx);
    if (!o || o->kind != 2 || !st->files || o->handle >= FAT_HANDLES ||
        !st->files->handles[o->handle].used)
        return 6;
    uint32_t e = st->files->handles[o->handle].entry;
    if (cpu->al == 0) {
        cpu->cx = fat_u16(st->files->image + e + 22);
        cpu->dx = fat_u16(st->files->image + e + 24);
        return 0;
    }
    if (cpu->al == 1) {
        st->files->image[e + 22] = (uint8_t)cpu->cx;
        st->files->image[e + 23] = (uint8_t)(cpu->cx >> 8);
        st->files->image[e + 24] = (uint8_t)cpu->dx;
        st->files->image[e + 25] = (uint8_t)(cpu->dx >> 8);
        return 0;
    }
    return 1;
}

void dos_handle_call(struct vm86_cpu *cpu,struct int21_state *st)
{
    uint8_t fn=cpu->ah;char text[128];int e=0;uint16_t fd=0;
    if(fn==0x3F || fn==0x40) { transfer(cpu,st,fn==0x40,&e);if(e<0) return; }
    else if(fn==0x6C) {
        uint16_t save=cpu->dx;cpu->dx=cpu->si;
        if(!path(cpu,text)) e=3;
        cpu->dx=save;
        uint8_t name[11];uint16_t dir;uint32_t index=0,entry=0;
        int exists=!e && st->files ? fat_parent(st->files,text,&dir,name,false) : 15;
        if(!exists) exists=fat_find(st->files,dir,&index,name,0x37,&entry);
        unsigned action=exists ? ((save>>4)&15u) : (save&15u);
        if(!e && exists!=0 && exists!=18 && exists!=2) e=exists;
        if(!e && (action>2 || (!exists && action==0) || (exists && action!=1))) e=exists ? 2 : 80;
        if(!e) {
            e=dos_handle_open(st,text,(uint8_t)(cpu->bx&3),exists || action==2,cpu->cx,&fd);
            if(!e) { cpu->ax=fd;cpu->cx=exists ? 2 : action==2 ? 3 : 1; }
        }
    }
    else if(fn==0x3C || fn==0x3D || fn==0x41 || fn==0x43) {
        if(!path(cpu,text)) e=3;
        else if(fn==0x41) e=st->files ? fat_unlink(st->files,text) : 15;
        else if(fn==0x43) {
            uint8_t name[11];uint16_t dir;uint32_t index=0,entry;
            e=st->files ? fat_parent(st->files,text,&dir,name,false) : 15;
            if(!e) e=fat_find(st->files,dir,&index,name,0x37,&entry);
            if(e==18) e=2;
            if(!e) {
                if(cpu->al==0) cpu->cx=st->files->image[entry+11];
                else if(cpu->al==1 && !(cpu->cx&~0x27u)) st->files->image[entry+11]=(uint8_t)cpu->cx;
                else e=1;
            }
        } else {
            e=(fn==0x3D && (cpu->al&0x78)) ? 12 : dos_handle_open(st,text,fn==0x3C ? 2 : cpu->al&7,fn==0x3C,cpu->cx,&fd);
            if(!e) cpu->ax=fd;
        }
    } else if(fn==0x3E) e=dos_handle_close(st,cpu->bx);
    else if(fn==0x56) e=rename_file(cpu,st);
    else if(fn==0x57) e=file_time(cpu,st);
    else if(fn==0x45 || fn==0x46) { e=dos_handle_dup(st,cpu->bx,fn==0x45 ? -1 : cpu->cx,&fd);if(!e && fn==0x45) cpu->ax=fd; }
    else if(fn==0x44) {
        struct dos_open_object *o=object(st,cpu->bx);
        if(!o) e=6;
        else if(cpu->al==0) cpu->dx=o->kind==1 ? 0x80D3 : 0;
        else if(cpu->al==1 && o->kind==1) { /* raw/cooked device flags do not change queue ownership */ }
        else if(cpu->al==6 || cpu->al==7) {
            uint16_t word;cpu->al=(o->kind==2 || cpu->al==7 || bios16_peek(cpu,&word)) ? 0xFF : 0;
        } else e=1;
    } else if(fn==0x42) {
        struct dos_open_object *o=object(st,cpu->bx);uint32_t pos;
        if(!o) e=6;else if(o->kind!=2) e=1;
        else { e=fat_seek(st->files,o->handle,cpu->al,(int32_t)(((uint32_t)cpu->cx<<16)|cpu->dx),&pos);if(!e) { cpu->ax=(uint16_t)pos;cpu->dx=(uint16_t)(pos>>16); } }
    }
    st->last_error=(uint16_t)e;vm86_flag_set(cpu,VM86_CF,e!=0);if(e) cpu->ax=(uint16_t)e;
    dos_handles_sync(cpu,st);
}
/* Redirection belongs to the DOS launcher, not PSP argument text. Validate
 * syntax first; on error the caller discards this run's private volume. */
int dos_redirect(struct vm86_cpu *cpu,struct int21_state *st,const char *tail,char *clean,unsigned capacity)
{
    unsigned used=0;const char *p=tail ? tail : "";
    while(*p) {
        if(*p!='<' && *p!='>') { if(used+1>=capacity) return 13;clean[used++]=*p++;continue; }
        bool writing=*p++=='>';bool append=writing && *p=='>';if(append) p++;
        while(*p==' ' || *p=='\t') p++;
        char name[128];unsigned n=0;
        while(*p && *p!=' ' && *p!='\t' && *p!='<' && *p!='>') { if(n+1>=sizeof(name)) return 3;name[n++]=*p++; }
        name[n]=0;if(!n) return 3;
        uint16_t fd,out;int e=dos_handle_open(st,name,writing ? 2 : 0,writing && !append,0,&fd);
        if(e && append && e==2) e=dos_handle_open(st,name,2,true,0,&fd);
        if(e) return e;
        if(append) { uint32_t pos;struct dos_open_object *o=object(st,fd);e=fat_seek(st->files,o->handle,2,0,&pos); }
        if(!e) e=dos_handle_dup(st,fd,writing ? 1 : 0,&out);
        (void)dos_handle_close(st,fd);if(e) return e;
    }
    while(used && (clean[used-1]==' ' || clean[used-1]=='\t')) used--;
    clean[used]=0;dos_handles_sync(cpu,st);return 0;
}
