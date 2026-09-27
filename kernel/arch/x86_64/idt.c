#include <funnyos/arch/x86_64/idt.h>
#include <funnyos/arch/x86_64/gdt.h>

#include <stddef.h>
#include <stdint.h>

#include <libk/string.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;          /* bits 2:0 = IST index, remaining bits zero */
    uint8_t  type_attr;    /* present | DPL | zero | gate type */
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry g_idt[IDT_VECTOR_COUNT];
static struct idtr      g_idtr;

extern void  idt_load(const struct idtr *idtr);
extern void *isr_stub_table[IDT_VECTOR_COUNT];

void idt_set_handler(uint8_t vector, void (*handler)(void), uint8_t ist,
                     uint8_t dpl)
{
    struct idt_entry *e = &g_idt[vector];
    uint64_t addr = (uint64_t)handler;

    e->offset_low  = (uint16_t)(addr & 0xFFFF);
    e->selector    = GDT_KERNEL_CODE;
    e->ist         = ist & 0x07;
    e->type_attr   = (uint8_t)(0x80 |                 /* present */
                               ((dpl & 0x03) << 5) |  /* DPL */
                               0x0E);                 /* 64-bit interrupt gate */
    e->offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    e->offset_high = (uint32_t)(addr >> 32);
    e->zero        = 0;
}

void idt_init(void)
{
    memset(&g_idt, 0, sizeof(g_idt));

    /*
     * Vectors 0..255 all have a dedicated stub generated in isr.asm, and
     * the addresses come through as a table. Installing them in a loop
     * keeps the C side free of 256 symbol declarations.
     */
    for (int i = 0; i < IDT_VECTOR_COUNT; i++)
        idt_set_handler((uint8_t)i, (void (*)(void))isr_stub_table[i], 0, 0);

    /*
     * A double fault means a fault occurred while handling a fault, which
     * in practice usually means the stack is unusable. Give it a dedicated
     * IST stack so the handler gets to report something, instead of the
     * CPU escalating to a triple fault and rebooting with no diagnostic.
     *
     * NMI gets one for the same reason: it can arrive at any moment,
     * including while the normal stack is mid-update.
     *
     * DPL stays 0 for both, so user code cannot raise them with INT n.
     */
    idt_set_handler(2, (void (*)(void))isr_stub_table[2], IST_NMI, 0);          /* NMI */
    idt_set_handler(8, (void (*)(void))isr_stub_table[8], IST_DOUBLE_FAULT, 0); /* #DF */

    g_idtr.limit = sizeof(g_idt) - 1;
    g_idtr.base  = (uint64_t)&g_idt;

    idt_load(&g_idtr);
}
