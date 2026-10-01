#include <vm86/fat.h>
#include <libk/string.h>

uint16_t fat_u16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
uint32_t fat_u32(const uint8_t *p) { return fat_u16(p) | ((uint32_t)fat_u16(p+2) << 16); }
static void put16(uint8_t *p, uint16_t x) { p[0]=(uint8_t)x; p[1]=(uint8_t)(x>>8); }
static void put32(uint8_t *p, uint32_t x) { put16(p,(uint16_t)x); put16(p+2,(uint16_t)(x>>16)); }
static bool valid(const struct fat_volume *v, uint16_t c) { return c>=2 && c<v->clusters+2; }
static bool end(const struct fat_volume *v, uint16_t c) { return c >= (v->bits==12 ? 0xFF8 : 0xFFF8); }
static uint16_t next(const struct fat_volume *v, uint16_t c)
{
    uint32_t n=v->bits==12 ? c+c/2u : c*2u;
    uint16_t x=fat_u16(v->image+v->fat+n);
    return v->bits==12 ? (uint16_t)((c&1) ? x>>4 : x&0xFFF) : x;
}
static void set(struct fat_volume *v, uint16_t c, uint16_t x)
{
    uint32_t n=v->bits==12 ? c+c/2u : c*2u;
    for (unsigned i=0;i<v->copies;i++) {
        uint8_t *p=v->image+v->fat+i*v->fat_bytes+n;
        uint16_t old=fat_u16(p);
        put16(p,v->bits==16 ? x : (c&1) ? (uint16_t)((old&15)|(x<<4)) : (uint16_t)((old&0xF000)|(x&0xFFF)));
    }
}
static uint32_t address(const struct fat_volume *v, uint16_t c) { return v->data+(c-2u)*v->cluster_bytes; }
/* Validate the whole chain before mutating it. Floyd detects cycles without
 * allocation, and the step bound also protects malicious directory chains. */
static int chain(struct fat_volume *v, uint16_t first, uint32_t *length)
{
    *length=0;
    if (!first) return 0;
    uint16_t c=first, slow=first, fast=first;
    while (valid(v,c)) {
        if (++*length>v->clusters) return 13;
        c=next(v,c);
        if (end(v,c)) return 0;
        if (!valid(v,c)) return 13;
        slow=valid(v,slow) ? next(v,slow) : slow;
        for (unsigned i=0;i<2;i++) fast=valid(v,fast) ? next(v,fast) : fast;
        if (valid(v,fast) && slow==fast) return 13;
    }
    return 13;
}
int fat_mount(struct fat_volume *v, uint8_t *image, uint32_t size)
{
    memset(v,0,sizeof(*v));
    if (!image || size<512 || image[510]!=0x55 || image[511]!=0xAA) return 13;
    uint32_t b=fat_u16(image+11), spc=image[13], reserved=fat_u16(image+14);
    uint32_t copies=image[16], roots=fat_u16(image+17), sectors=fat_u16(image+19);
    uint32_t f=fat_u16(image+22);
    if (!sectors) sectors=fat_u32(image+32);
    if (b<512 || b>4096 || (b&(b-1)) || !spc || spc>128 || (spc&(spc-1)) ||
        !reserved || !copies || copies>2 || !roots || !f || !sectors) return 13;
    uint64_t root=(uint64_t)reserved+copies*f, data=root+((roots*32u+b-1)/b);
    if ((uint64_t)sectors*b>size || data>=sectors) return 13;
    uint32_t clusters=(uint32_t)((sectors-data)/spc);
    if (!clusters || clusters>=65525) return 13; /* FAT32 is not DOS 3.30 */
    unsigned bits=clusters<4085 ? 12 : 16;
    /* Reserved cluster values must never be allocated. */
    if (clusters+1 >= (bits==12 ? 0xFF0u : 0xFFF0u)) return 13;
    uint64_t need=bits==12 ? ((uint64_t)(clusters+2)*3+1)/2 : (uint64_t)(clusters+2)*2;
    if (need>(uint64_t)f*b) return 13;
    v->image=image; v->size=sectors*b; v->fat=reserved*b; v->fat_bytes=f*b;
    v->root=(uint32_t)root*b; v->data=(uint32_t)data*b;
    v->clusters=clusters; v->cluster_bytes=b*spc; v->root_entries=(uint16_t)roots;
    v->copies=(uint8_t)copies; v->bits=(uint8_t)bits;
    return 0;
}
static char upper(char c) { return c>='a' && c<='z' ? (char)(c-'a'+'A') : c; }
int fat_name(const char *text, uint8_t name[11], bool pattern)
{
    memset(name,' ',11);
    unsigned pos=0, limit=8; bool dot=false, any=false;
    for (;*text;text++) {
        char c=upper(*text);
        if (c=='.') { if(dot) return 3; dot=true; pos=8; limit=11; continue; }
        if (c=='*' && pattern) { while(pos<limit) name[pos++]='?'; while(text[1] && text[1]!='.') text++; any=true; continue; }
        if (pos>=limit || (unsigned char)c<33 || c=='/' || c=='\\' || c==':' || c=='"' || c=='<' || c=='>' || c=='|' || c=='+' || c=='=' || c==';' || c==',' || c=='[' || c==']' || c=='*' || (c=='?' && !pattern)) return 3;
        name[pos++]=(uint8_t)c; any=true;
    }
    return any && name[0]!=' ' ? 0 : 3;
}
int fat_entry(struct fat_volume *v, uint16_t dir, uint32_t index, uint32_t *offset)
{
    if (!v || !v->image) return 15;
    if (!dir) { if(index>=v->root_entries) return 18; *offset=v->root+index*32u; return 0; }
    uint32_t length; int e=chain(v,dir,&length); if(e) return e;
    uint32_t per=v->cluster_bytes/32u, skip=index/per;
    if (skip>=length) return 18;
    while(skip--) dir=next(v,dir);
    *offset=address(v,dir)+(index%per)*32u;
    return 0;
}
static int lookup(struct fat_volume *v,uint16_t dir,const uint8_t name[11],uint32_t *off)
{
    for(uint32_t i=0;;i++) {
        int e=fat_entry(v,dir,i,off); if(e) return e==18 ? 2 : e;
        uint8_t *p=v->image+*off;
        if(!p[0]) return 2;
        if(p[0]==0xE5 || p[11]==0x0F || (p[11]&8)) continue;
        if(!memcmp(p,name,11)) return 0;
    }
}
int fat_parent(struct fat_volume *v,const char *path,uint16_t *dir,uint8_t name[11],bool pattern)
{
    if(!v || !v->image) return 15;
    if(!path || !*path) return 3;
    if(path[1]==':') { if(upper(path[0])!='F') return 15; path+=2; }
    *dir=0;
    while(*path=='/' || *path=='\\') path++;
    for (;;) {
        char part[13]; unsigned n=0;
        while(*path && *path!='/' && *path!='\\') { if(n==12) return 3; part[n++]=*path++; }
        part[n]=0;
        if(!*path) return fat_name(part,name,pattern);
        while(*path=='/' || *path=='\\') path++;
        if(!strcmp(part,".")) continue;
        uint32_t off;
        if(!strcmp(part,"..")) {
            if(!*dir) continue;
            uint8_t parent[11]={' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '}; parent[0]='.'; parent[1]='.';
            int e=lookup(v,*dir,parent,&off); if(e) return 3;
        } else {
            int e=fat_name(part,name,false); if(e) return e;
            e=lookup(v,*dir,name,&off); if(e) return e==2 ? 3 : e;
        }
        if(!(v->image[off+11]&16)) return 3;
        *dir=fat_u16(v->image+off+26);
    }
}
static void release(struct fat_volume *v,uint16_t c)
{
    while(valid(v,c)) { uint16_t n=next(v,c); set(v,c,0); c=n; }
}
int fat_open(struct fat_volume *v,const char *path,uint8_t mode,bool create,uint16_t attr,uint16_t *handle)
{
    if(mode>2 || (create && (attr&~0x27u))) return 12;
    unsigned h; for(h=0;h<FAT_HANDLES && v->handles[h].used;h++); if(h==FAT_HANDLES) return 4;
    uint16_t dir; uint8_t name[11]; uint32_t off=0;
    int e=fat_parent(v,path,&dir,name,false); if(e) return e;
    e=lookup(v,dir,name,&off);
    if(e && e!=2) return e;
    if(e==2) {
        if(!create) return 2;
        for(uint32_t i=0;;i++) {
            e=fat_entry(v,dir,i,&off); if(e) return e==18 ? 5 : e;
            if(!v->image[off] || v->image[off]==0xE5) break;
        }
        memset(v->image+off,0,32); memcpy(v->image+off,name,11); v->image[off+11]=(uint8_t)attr;
    } else {
        uint8_t a=v->image[off+11]; if((a&16) || ((a&1) && (create || mode))) return 5;
        uint32_t len; e=chain(v,fat_u16(v->image+off+26),&len); if(e) return e;
        if(fat_u32(v->image+off+28)>(uint64_t)len*v->cluster_bytes) return 13;
        if(create) {
            for(unsigned i=0;i<FAT_HANDLES;i++) if(v->handles[i].used && v->handles[i].entry==off) return 5;
            release(v,fat_u16(v->image+off+26)); put16(v->image+off+26,0); put32(v->image+off+28,0); v->image[off+11]=(uint8_t)attr;
        }
    }
    v->handles[h]=(struct fat_handle){true,mode,off,0}; *handle=(uint16_t)(h+5); return 0;
}
static struct fat_handle *get(struct fat_volume *v,uint16_t h) { return v && v->image && h>=5 && h<5+FAT_HANDLES && v->handles[h-5].used ? &v->handles[h-5] : 0; }
int fat_close(struct fat_volume *v,uint16_t h) { struct fat_handle *f=get(v,h); if(!f) return 6; f->used=false; return 0; }
static uint16_t at(struct fat_volume *v,uint16_t first,uint32_t pos) { for(uint32_t i=pos/v->cluster_bytes;i && valid(v,first);i--) first=next(v,first); return first; }
int fat_read(struct fat_volume *v,uint16_t h,uint8_t *out,uint32_t count,uint32_t *done)
{
    *done=0; struct fat_handle *f=get(v,h); if(!f) return 6; if(f->mode==1) return 5;
    uint8_t *p=v->image+f->entry; uint32_t size=fat_u32(p+28), length;
    int e=chain(v,fat_u16(p+26),&length); if(e || size>(uint64_t)length*v->cluster_bytes) return 13;
    if(f->position>=size) return 0;
    if(count>size-f->position) count=size-f->position;
    uint16_t c=at(v,fat_u16(p+26),f->position);
    while(*done<count) {
        uint32_t within=f->position%v->cluster_bytes, n=v->cluster_bytes-within;
        if(n>count-*done) n=count-*done;
        memcpy(out+*done,v->image+address(v,c)+within,n); *done+=n; f->position+=n; c=next(v,c);
    }
    return 0;
}
static uint16_t allocate(struct fat_volume *v)
{
    for(uint32_t c=2;c<v->clusters+2;c++) if(!next(v,(uint16_t)c)) {
        set(v,(uint16_t)c,v->bits==12 ? 0xFFF : 0xFFFF); memset(v->image+address(v,(uint16_t)c),0,v->cluster_bytes); return (uint16_t)c;
    }
    return 0;
}
int fat_write(struct fat_volume *v,uint16_t h,const uint8_t *in,uint32_t count,uint32_t *done)
{
    *done=0; struct fat_handle *f=get(v,h); if(!f) return 6; if(!f->mode) return 5;
    uint8_t *p=v->image+f->entry; uint16_t first=fat_u16(p+26); uint32_t length;
    int e=chain(v,first,&length); if(e) return e;
    uint64_t target=(uint64_t)f->position+count;
    if(target>0xFFFFFFFFu) return 39;
    uint32_t needed=(uint32_t)((target+v->cluster_bytes-1)/v->cluster_bytes);
    /* Preflight capacity: failures must not leak clusters or partial metadata. */
    if(needed>length) {
        uint32_t free=0; for(uint32_t c=2;c<v->clusters+2;c++) if(!next(v,(uint16_t)c)) free++;
        if(needed-length>free) {
            if(!count) return 39;
            uint64_t capacity=(uint64_t)(length+free)*v->cluster_bytes;
            if(f->position>=capacity) return 0;
            count=(uint32_t)(capacity-f->position);
            target=(uint64_t)f->position+count;
            needed=(uint32_t)((target+v->cluster_bytes-1)/v->cluster_bytes);
        }
    }
    uint16_t last=first; for(uint32_t i=1;i<length;i++) last=next(v,last);
    while(length<needed) { uint16_t c=allocate(v); if(length) set(v,last,c); else { first=c; put16(p+26,c); } last=c; length++; }
    if(!count) {
        if(!needed) { release(v,first); put16(p+26,0); }
        else { last=at(v,first,f->position-1); uint16_t tail=next(v,last); set(v,last,v->bits==12 ? 0xFFF : 0xFFFF); release(v,tail); }
    }
    /* Zero the seek gap, including the tail of an already allocated cluster. */
    uint32_t old=fat_u32(p+28);
    for(uint32_t pos=old;pos<f->position;) {
        uint16_t c=at(v,first,pos); uint32_t within=pos%v->cluster_bytes, n=v->cluster_bytes-within;
        if(n>f->position-pos) n=f->position-pos;
        memset(v->image+address(v,c)+within,0,n); pos+=n;
    }
    if(count) {
        uint16_t c=at(v,first,f->position);
        while(*done<count) {
            uint32_t within=f->position%v->cluster_bytes, n=v->cluster_bytes-within;
            if(n>count-*done) n=count-*done;
            memcpy(v->image+address(v,c)+within,in+*done,n); *done+=n; f->position+=n; c=next(v,c);
        }
    }
    if(!count || f->position>old) put32(p+28,f->position);
    p[11]|=0x20; return 0;
}
int fat_seek(struct fat_volume *v,uint16_t h,uint8_t origin,int32_t offset,uint32_t *position)
{
    struct fat_handle *f=get(v,h); if(!f) return 6; if(origin>2) return 1;
    int64_t base=origin==0 ? 0 : origin==1 ? f->position : fat_u32(v->image+f->entry+28);
    int64_t n=base+offset; if(n<0 || n>0xFFFFFFFFll) return 1;
    f->position=(uint32_t)n; *position=f->position; return 0;
}
int fat_unlink(struct fat_volume *v,const char *path)
{
    uint16_t dir; uint8_t name[11]; uint32_t off; int e=fat_parent(v,path,&dir,name,false); if(e) return e;
    e=lookup(v,dir,name,&off); if(e) return e;
    if(v->image[off+11]&0x11) return 5;
    for(unsigned i=0;i<FAT_HANDLES;i++) if(v->handles[i].used && v->handles[i].entry==off) return 5;
    uint32_t length; e=chain(v,fat_u16(v->image+off+26),&length); if(e) return e;
    release(v,fat_u16(v->image+off+26)); v->image[off]=0xE5; return 0;
}
int fat_find(struct fat_volume *v,uint16_t dir,uint32_t *index,const uint8_t pattern[11],uint8_t attr,uint32_t *entry)
{
    for(;;) {
        int e=fat_entry(v,dir,(*index)++,entry); if(e) return e;
        uint8_t *p=v->image+*entry; if(!p[0]) return 18;
        if(p[0]==0xE5 || p[11]==0x0F) continue;
        if(attr==8 ? !(p[11]&8) : ((p[11]&8) || ((p[11]&0x16)&~attr))) continue;
        bool match=true; for(unsigned j=0;j<11;j++) if(pattern[j]!='?' && pattern[j]!=p[j]) match=false;
        if(match) return 0;
    }
}
