/* Guest-visible DOS MCB arena. Chain validation precedes all edits; sizes are
 * paragraphs excluding the MCB itself. Arena ends before the VGA aperture. */
#include <vm86/int21.h>
#include <vm86/dos.h>
#include <vm86/mem.h>
static uint32_t at(uint16_t s,unsigned o){return ((uint32_t)s<<4)+o;}
static uint16_t size(struct vm86_cpu *c,uint16_t s){return vm86_mem_read16(c->mem,at(s,3));}
static uint16_t owner(struct vm86_cpu *c,uint16_t s){return vm86_mem_read16(c->mem,at(s,1));}
static uint8_t type(struct vm86_cpu *c,uint16_t s){return vm86_mem_read8(c->mem,at(s,0));}
static void block(struct vm86_cpu *c,uint16_t s,uint8_t t,uint16_t o,uint16_t n)
{ vm86_mem_write8(c->mem,at(s,0),t);vm86_mem_write16(c->mem,at(s,1),o);vm86_mem_write16(c->mem,at(s,3),n); }
static int validate(struct vm86_cpu *c,struct int21_state *st)
{
    if(!st->arena_first || !st->arena_top) return 7;
    uint32_t s=st->arena_first;
    for(unsigned i=0;i<65536;i++) {
        if(s>=st->arena_top) return 7;
        uint8_t t=type(c,(uint16_t)s);uint32_t next=s+size(c,(uint16_t)s)+1u;
        if(next>st->arena_top || (t!='M' && t!='Z')) return 7;
        if(t=='Z') return next==st->arena_top ? 0 : 7;
        s=next;
    }
    return 7;
}
static void coalesce(struct vm86_cpu *c,struct int21_state *st)
{
    uint16_t s=st->arena_first;
    while(type(c,s)=='M') {
        uint16_t n=(uint16_t)(s+size(c,s)+1u);
        if(!owner(c,s) && !owner(c,n)) block(c,s,type(c,n),0,(uint16_t)(size(c,s)+size(c,n)+1u));
        else s=n;
    }
}
static int locate(struct vm86_cpu *c,struct int21_state *st,uint16_t segment,uint16_t *out)
{
    int e=validate(c,st);if(e) return e;
    uint16_t s=st->arena_first;
    for(;;) { if((uint16_t)(s+1u)==segment) { *out=s;return 0; }if(type(c,s)=='Z') return 9;s=(uint16_t)(s+size(c,s)+1u); }
}
void dos_runtime_init(struct vm86_cpu *c,struct int21_state *st)
{
    st->full_services=true;st->version=0x0004;
    st->arena_first=(uint16_t)(st->psp-1u);st->arena_top=0xA000;
    block(c,st->arena_first,'Z',st->psp,(uint16_t)(st->arena_top-st->psp));
    vm86_mem_write16(c->mem,at(st->psp,VM86_PSP_VERSION),st->version);
    dos_handles_sync(c,st);
}
int dos_memory_alloc(struct vm86_cpu *c,struct int21_state *st,uint16_t n,uint16_t o,uint16_t *segment,uint16_t *largest)
{
    int e=validate(c,st);if(e) return e;coalesce(c,st);*largest=0;
    uint16_t s=st->arena_first,found=0;
    for(;;) {
        uint16_t v=size(c,s);if(!owner(c,s)) { if(v>*largest) *largest=v;if(!found && v>=n) found=s; }
        if(type(c,s)=='Z') break;
        s=(uint16_t)(s+v+1u);
    }
    if(!n || !found) return 8;
    uint16_t v=size(c,found);uint8_t t=type(c,found);
    if(v>n) { uint16_t rest=(uint16_t)(found+n+1u);block(c,rest,t,0,(uint16_t)(v-n-1u));t='M'; }
    block(c,found,t,o,n);*segment=(uint16_t)(found+1u);return 0;
}
int dos_memory_free(struct vm86_cpu *c,struct int21_state *st,uint16_t segment)
{
    uint16_t s;int e=locate(c,st,segment,&s);if(e) return e;if(!owner(c,s)) return 9;
    block(c,s,type(c,s),0,size(c,s));coalesce(c,st);return 0;
}
int dos_memory_block_size(struct vm86_cpu *c,struct int21_state *st,
                          uint16_t segment,uint16_t *paragraphs)
{
    uint16_t s; int e=locate(c,st,segment,&s);
    if(e) return e;
    if(!owner(c,s)) return 9;
    if(paragraphs) *paragraphs=size(c,s);
    return 0;
}
int dos_memory_resize(struct vm86_cpu *c,struct int21_state *st,uint16_t segment,uint16_t n,uint16_t *largest)
{
    uint16_t s;int e=locate(c,st,segment,&s);if(e) return e;if(!owner(c,s)) return 9;
    coalesce(c,st);uint16_t v=size(c,s);uint8_t t=type(c,s);
    if(t=='M') {
        uint16_t next=(uint16_t)(s+v+1u);
        if(!owner(c,next)) { v=(uint16_t)(v+1u+size(c,next));t=type(c,next); }
    }
    *largest=v;if(n>v) return 8;
    uint16_t o=owner(c,s);
    if(n<v) { block(c,(uint16_t)(s+n+1u),t,0,(uint16_t)(v-n-1u));t='M'; }
    block(c,s,t,o,n);coalesce(c,st);
    if(segment==st->psp) vm86_mem_write16(c->mem,at(segment,2),(uint16_t)(segment+n));
    return 0;
}
void dos_memory_call(struct vm86_cpu *c,struct int21_state *st)
{
    if(!st->full_services) { c->al=0;vm86_flag_set(c,VM86_CF,true);return; }
    uint16_t seg=0,largest=0;int e;
    if(c->ah==0x48) { e=dos_memory_alloc(c,st,c->bx,st->psp,&seg,&largest);if(!e)c->ax=seg; }
    else if(c->ah==0x49) e=dos_memory_free(c,st,c->es);
    else e=dos_memory_resize(c,st,c->es,c->bx,&largest);
    st->last_error=(uint16_t)e;vm86_flag_set(c,VM86_CF,e!=0);if(e) { c->ax=(uint16_t)e;if(e==8)c->bx=largest; }
}
