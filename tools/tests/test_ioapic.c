/* Exercise the real MMIO probe with a RAM-backed register window. Only map
 * translation is stubbed; an absent device must not become version 255. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <funnyos/vmm.h>
#include <funnyos/bootinfo.h>
#include "../../kernel/arch/x86_64/apic.c"
static uint32_t registers[1024];
uint64_t bootinfo_hhdm_offset(void) { return (uintptr_t)registers-IOAPIC_DEFAULT_PHYS; }
bool vmm_map_page(uint64_t virt,uint64_t phys,uint64_t flags)
{ (void)flags;return virt==(uintptr_t)registers && phys==IOAPIC_DEFAULT_PHYS; }
uint64_t vmm_get_physical(uint64_t virt)
{ return virt==(uintptr_t)registers ? IOAPIC_DEFAULT_PHYS : 0; }
void kprintf(const char *format,...) { (void)format; }
int main(void)
{
    registers[IOAPIC_IOWIN/4]=UINT32_MAX;
    assert(!register_ioapic(0,IOAPIC_DEFAULT_PHYS,0));
    assert(g_ioapic_count==0 && !ioapic_ready());
    registers[IOAPIC_IOWIN/4]=0;
    assert(!register_ioapic(0,IOAPIC_DEFAULT_PHYS,0));
    assert(g_ioapic_count==0 && !ioapic_ready());
    registers[IOAPIC_IOWIN/4]=(23u<<16)|0x20u;
    assert(register_ioapic(1,IOAPIC_DEFAULT_PHYS,0));
    assert(g_ioapic_count==1 && g_ioapics[0].present && g_ioapics[0].entries==24);
    puts("I/O APIC host probe: absent/zero/valid register tests passed");
}
