#include <funnyos/arch/x86_64/gdt.h>

#include <stddef.h>
#include <stdint.h>

#include <libk/string.h>

/* ------------------------------------------------------------------ */
/* Descriptor layout                                                   */
/* ------------------------------------------------------------------ */

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_high;   /* flags (high nibble) | limit[16:19] */
    uint8_t  base_high;
} __attribute__((packed));

/* A TSS descriptor is 16 bytes, so it covers two consecutive GDT slots. */
struct tss_descriptor {
    struct gdt_entry low;
    uint32_t         base_upper;
    uint32_t         reserved;
} __attribute__((packed));

/*
 * The table is modelled explicitly rather than as a flat array of entries,
 * because the TSS descriptor is twice the width of every other entry.
 * Spelling that out keeps the slot numbering obvious.
 *
 *   slot 0 -> entries[0]   null
 *   slot 1 -> entries[1]   kernel code
 *   slot 2 -> entries[2]   kernel data
 *   slot 3 -> entries[3]   user code
 *   slot 4 -> entries[4]   user data
 *   slot 5,6 -> tss        TSS descriptor (16 bytes)
 */
struct gdt_table {
    struct gdt_entry      entries[5];
    struct tss_descriptor tss;
} __attribute__((packed));

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/*
 * Long mode TSS. Only rsp0 and the IST pointers are used by 64-bit code.
 *
 * iomap_base is set past the end of the structure to declare that no I/O
 * permission bitmap is present. That placement is deliberate: if it named
 * valid memory, the CPU would read that memory as a permission bitmap and
 * Ring 3 code could issue in/out without faulting.
 */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

/* ------------------------------------------------------------------ */
/* Storage                                                             */
/* ------------------------------------------------------------------ */

static struct gdt_table g_gdt;
static struct tss       g_tss;
static struct gdtr      g_gdtr;

/*
 * Stacks selected by the IST mechanism. They must be separate from the
 * normal kernel stack: the entire point is to have a stack that is known
 * good even when the current one is not.
 */
static uint8_t g_df_stack[16384] __attribute__((aligned(16)));
static uint8_t g_nmi_stack[16384] __attribute__((aligned(16)));

extern void gdt_load(const struct gdtr *gdtr);
extern void gdt_load_cs(void);
extern void tss_load(uint16_t selector);

/* ------------------------------------------------------------------ */
/* Entry construction                                                  */
/* ------------------------------------------------------------------ */

static void gdt_set_entry(struct gdt_entry *e, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t flags)
{
    e->limit_low        = (uint16_t)(limit & 0xFFFF);
    e->base_low         = (uint16_t)(base & 0xFFFF);
    e->base_mid         = (uint8_t)((base >> 16) & 0xFF);
    e->access           = access;
    e->flags_limit_high = (uint8_t)(((limit >> 16) & 0x0F) | (flags & 0xF0));
    e->base_high        = (uint8_t)((base >> 24) & 0xFF);
}

void gdt_init(void)
{
    memset(&g_gdt, 0, sizeof(g_gdt));
    memset(&g_tss, 0, sizeof(g_tss));

    /*
     * Code and data descriptors in long mode carry base 0 and an ignored
     * limit, so the interesting bits are:
     *
     *   access 0x9A / 0x92   present, DPL 0, code/data, readable/writable
     *   access 0xFA / 0xF2   same, but DPL 3 for user mode
     *   flags  0xA0          granularity set, L=1 (64-bit), D/B=0
     *                        D/B must be 0 whenever L is 1
     *   flags  0xC0          data segments: granularity set, D/B=1
     */
    gdt_set_entry(&g_gdt.entries[0], 0, 0, 0x00, 0x00);   /* null */
    gdt_set_entry(&g_gdt.entries[1], 0, 0, 0x9A, 0xA0);   /* kernel code */
    gdt_set_entry(&g_gdt.entries[2], 0, 0, 0x92, 0xC0);   /* kernel data */
    gdt_set_entry(&g_gdt.entries[3], 0, 0, 0xFA, 0xA0);   /* user code */
    gdt_set_entry(&g_gdt.entries[4], 0, 0, 0xF2, 0xC0);   /* user data */

    /* Type 0x9 is "64-bit TSS (available)". The granularity flag stays
     * clear, so the limit is measured in bytes. */
    uint64_t tss_base  = (uint64_t)&g_tss;
    uint32_t tss_limit = sizeof(g_tss) - 1;

    gdt_set_entry(&g_gdt.tss.low, (uint32_t)tss_base, tss_limit, 0x89, 0x00);
    g_gdt.tss.base_upper = (uint32_t)(tss_base >> 32);
    g_gdt.tss.reserved   = 0;

    /* Declare the absence of an I/O permission bitmap. */
    g_tss.iomap_base = sizeof(g_tss);

    /* ist[] is 0-based here but the IDT names IST1..IST7, so IST1 is
     * ist[0]. Stacks grow downwards, hence the end pointer. */
    g_tss.ist[IST_DOUBLE_FAULT - 1] = (uint64_t)(g_df_stack + sizeof(g_df_stack));
    g_tss.ist[IST_NMI - 1]          = (uint64_t)(g_nmi_stack + sizeof(g_nmi_stack));

    g_gdtr.limit = sizeof(g_gdt) - 1;
    g_gdtr.base  = (uint64_t)&g_gdt;

    gdt_load(&g_gdtr);

    /* CS can only change through a far transfer, and the selector we
     * arrived with (0x28, from Limine's GDT) means something different in
     * this table. */
    gdt_load_cs();

    tss_load(GDT_TSS);
}

void tss_set_kernel_stack(uint64_t rsp0)
{
    g_tss.rsp0 = rsp0;
}

uint64_t tss_get_kernel_stack(void)
{
    return g_tss.rsp0;
}

uint64_t tss_address(void)
{
    return (uint64_t)&g_tss;
}
