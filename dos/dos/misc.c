#include <vm86/int21.h>
#include <vm86/dos.h>
#include <vm86/fat.h>
#include <vm86/host.h>
#include <libk/string.h>
static uint32_t at(uint16_t s,uint16_t o){return ((uint32_t)s<<4)+o;}
static void error(struct vm86_cpu *c,struct int21_state *st,unsigned e)
{ st->last_error=(uint16_t)e;vm86_flag_set(c,VM86_CF,e!=0);if(e)c->ax=(uint16_t)e; }
static void new_psp(struct vm86_cpu *c,struct int21_state *st,uint16_t seg)
{
    for(unsigned i=0;i<256;i++) vm86_mem_write8(c->mem,at(seg,(uint16_t)i),vm86_mem_read8(c->mem,at(st->psp,(uint16_t)i)));
    vm86_mem_write16(c->mem,at(seg,VM86_PSP_PARENT),st->psp);
    vm86_mem_write16(c->mem,at(seg,VM86_PSP_JFT_POINTER),VM86_PSP_JFT);
    vm86_mem_write16(c->mem,at(seg,VM86_PSP_JFT_POINTER+2),seg);
}
void dos_misc_call(struct vm86_cpu *c,struct int21_state *st)
{
    switch(c->ah) {
    case 0x26: if(!st->full_services) { c->al=0;vm86_flag_set(c,VM86_CF,true);return; } new_psp(c,st,c->dx);break;
    case 0x31: {
        /* TSR keeps the PSP and the requested number of paragraphs. DOS
         * documents DX as the total resident size, including the PSP; keep
         * at least the PSP itself and resize its owning MCB before ending
         * the current invocation. The vector table remains guest memory, so
         * handlers installed by the resident program are not rewritten. */
        if(!st->full_services) { error(c,st,1);return; }
        uint16_t keep=c->dx < 0x10u ? 0x10u : c->dx, largest=0;
        int e=dos_memory_resize(c,st,st->psp,keep,&largest);
        if(e) { error(c,st,(unsigned)e); if(e==8)c->bx=largest; return; }
        st->resident_paragraphs=keep;
        st->terminated_resident=true;
        st->last_exit_code=c->al;
        vm86_flag_set(c,VM86_CF,false);
        vm86_service_exit(c,c->al);
        return;
    }
    case 0x55: if(!st->full_services) { c->al=0;vm86_flag_set(c,VM86_CF,true);return; } new_psp(c,st,c->dx);break;
    case 0x29: error(c,st,1);return;
    case 0x2A: c->cx=2026;c->dh=10;c->dl=2;c->al=5;break;
    case 0x2B: c->al=(c->cx>=1980 && c->cx<=2099 && c->dh>=1 && c->dh<=12 && c->dl>=1 && c->dl<=31) ? 0 : 0xFF;break;
    case 0x2C: { uint32_t ticks=vm86_mem_read16(c->mem,0x46C)|((uint32_t)vm86_mem_read16(c->mem,0x46E)<<16);uint32_t seconds=ticks*55u/1000u;c->ch=(uint8_t)(seconds/3600u%24u);c->cl=(uint8_t)(seconds/60u%60u);c->dh=(uint8_t)(seconds%60u);c->dl=0;break; }
    case 0x2D: c->al=(c->ch<24 && c->cl<60 && c->dh<60 && c->dl<100) ? 0 : 0xFF;break;
    case 0x33: if(c->al==0)c->dl=st->break_check;else if(c->al==1)st->break_check=c->dl!=0;else if(c->al==5)c->dl=6;else { error(c,st,1);return; }break;
    case 0x34: vm86_set_seg(c,VM86_ES,0x0800);c->bx=0;vm86_mem_write8(c->mem,0x8000,0);break;
    case 0x36: if(c->dl && c->dl!=6) { c->ax=0xFFFF;break; }if(!st->files) { c->ax=0xFFFF;break; }c->ax=(uint16_t)(st->files->cluster_bytes/512u);c->cx=512;c->dx=(uint16_t)st->files->clusters;c->bx=0;break;
    case 0x37: if(c->al==0)c->dl=st->switch_char;else if(c->al==1)st->switch_char=c->dl;else if(c->al==2)c->dl=0;else if(c->al!=3) { c->al=0xFF;break; }c->al=0;break;
    case 0x38: {
        /* US country information, DOS 2.x 34-byte layout. */
        static const uint8_t info[34]={0,0,'$',0,0,0,0,',',0,'.',0,'-',0,':',0,0,2,0,0,0,0,0,',',0};
        for(unsigned i=0;i<sizeof(info);i++) vm86_mem_write8(c->mem,at(c->ds,(uint16_t)(c->dx+i)),info[i]);
        c->bx=1;c->ax=1;break;
    }
    case 0x47: if(c->dl && c->dl!=6) { error(c,st,15);return; }vm86_mem_write8(c->mem,at(c->ds,c->si),0);break;
    case 0x4D: c->al=(uint8_t)st->last_exit_code;break;
    case 0x50: st->psp=c->bx;break;
    case 0x51: case 0x62: c->bx=st->psp;break;
    case 0x52: vm86_set_seg(c,VM86_ES,0x0800);c->bx=0x100;vm86_mem_write16(c->mem,0x80FE,st->arena_first);break;
    case 0x57: if(c->al==0) { c->cx=0;c->dx=0x5D41; }else if(c->al!=1) { error(c,st,1);return; }break;
    case 0x58: if(c->al==0)c->ax=0;else if(c->al==1 && c->bx==0) { }else { error(c,st,1);return; }break;
    case 0x59: c->ax=st->last_error;c->bh=1;c->bl=1;c->ch=1;break;
    case 0x60: {
        unsigned i;for(i=0;i<127;i++) { uint8_t b=vm86_mem_read8(c->mem,at(c->ds,(uint16_t)(c->si+i)));vm86_mem_write8(c->mem,at(c->es,(uint16_t)(c->di+i)),b);if(!b)break; }if(i==127) { error(c,st,3);return; }break;
    }
    case 0x63: if(c->al==0) { vm86_set_seg(c,VM86_DS,0x0800);c->si=0x40;vm86_mem_write16(c->mem,0x8040,0);c->al=0; }else { error(c,st,1);return; }break;
    case 0x65: {
        /* DOS 3.3+ extended country service.  The MS-DOS 4 EDLIN and
         * DEBUG builds use the Y/N validator (AL=23h), while the parser
         * also probes the five-byte country-info records (AL<20h).  Keep
         * the US/NLS values deterministic and honour the guest pointers;
         * this is deliberately bounded rather than exposing host data. */
        if(c->al==0x23) {
            c->al=(c->dl=='Y' || c->dl=='y') ? 1u :
                  (c->dl=='N' || c->dl=='n') ? 0u : 0xFFu;
        } else if(c->al==0x20) {
            if(c->dl>='a' && c->dl<='z') c->dl=(uint8_t)(c->dl-'a'+'A');
            c->al=c->dl;
        } else if(c->al==0x21) {
            uint32_t a=at(c->ds,c->dx);
            for(uint16_t i=0;i<c->cx;i++) {
                uint8_t ch=vm86_mem_read8(c->mem,a+i);
                if(ch>='a' && ch<='z') vm86_mem_write8(c->mem,a+i,(uint8_t)(ch-'a'+'A'));
            }
        } else if(c->al==0x22) {
            uint32_t a=at(c->ds,c->dx);
            for(unsigned i=0;i<127;i++) {
                uint8_t ch=vm86_mem_read8(c->mem,a+i); if(!ch) break;
                if(ch>='a' && ch<='z') vm86_mem_write8(c->mem,a+i,(uint8_t)(ch-'a'+'A'));
            }
        } else if(c->al<0x20) {
            /* The DOS 4 country table entries are five bytes.  EDLIN only
             * needs a successful probe; return the active code page and a
             * valid bounded record when ES:DI is supplied. */
            static const uint8_t record[5]={0,0,0,0,0};
            uint16_t n=c->cx<5 ? c->cx : 5;
            for(uint16_t i=0;i<n;i++) vm86_mem_write8(c->mem,at(c->es,(uint16_t)(c->di+i)),record[i]);
            c->cx=n; c->ax=1; c->bx=437;
        } else { error(c,st,1);return; }
        break;
    }
    default:error(c,st,1);return;
    }
    vm86_flag_set(c,VM86_CF,false);
}
