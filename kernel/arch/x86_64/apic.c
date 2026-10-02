#include <funnyos/arch/x86_64/apic.h>
#include <funnyos/arch/x86_64/acpi.h>
#include <funnyos/arch/x86_64/cpu.h>
#include <funnyos/arch/x86_64/pic.h>
#include <funnyos/bootinfo.h>
#include <funnyos/kprintf.h>
#include <funnyos/vmm.h>

/*
 * IA32_APIC_BASE. Bits 12-35 hold the base address, bit 11 is the global
 * enable, and bit 8 reports whether this is the boot processor.
 */
#define APIC_BASE_ADDR_MASK  0x0000000FFFFFF000ULL
#define APIC_BASE_ENABLE     (1ULL << 11)
#define APIC_BASE_IS_BSP     (1ULL << 8)

/* Address firmware has used for the local APIC since the Pentium. Used
 * only when the MSR reports nothing sensible. */
#define LAPIC_DEFAULT_PHYS   0xFEE00000ULL

/* Likewise for the IO APIC, used only when ACPI describes none. */
#define IOAPIC_DEFAULT_PHYS  0xFEC00000ULL

#define IOAPIC_MAX_INSTANCES 4
#define IOAPIC_MAX_ENTRIES   240

/* IO APIC indirect register access. */
#define IOAPIC_IOREGSEL      0x00u
#define IOAPIC_IOWIN         0x10u
#define IOAPIC_REG_ID        0x00u
#define IOAPIC_REG_VERSION   0x01u
#define IOAPIC_REG_REDIR(n)  (0x10u + 2u * (n))

/* --- MMIO mapping -------------------------------------------------- */

/*
 * Map four kilobytes of device memory and hand back a pointer to it.
 *
 * Device registers are not memory, and caching them is wrong in both
 * directions: a read that hits cache returns a stale interrupt status,
 * and a write that sits in a write-back buffer may never be issued at
 * all. So the mapping is explicitly uncached (PCD), which without PAT
 * means strongly uncacheable.
 *
 * `*fresh` reports whether a new mapping was installed. It is false when
 * a mapping already existed that could not be replaced -- in practice a
 * large HHDM page covering the device range. That is still usable, but
 * the cache type is then whatever that mapping chose, which is worth
 * knowing about.
 */
static bool map_mmio_uncached(uint64_t phys, uint64_t *virt_out, bool *fresh)
{
    uint64_t virt = bootinfo_hhdm_offset() + phys;

    if (vmm_map_page(virt, phys, VMM_KERNEL_RW | VMM_CACHE_DISABLE)) {
        *fresh = true;
    } else if (vmm_get_physical(virt) == phys) {
        *fresh = false;
    } else {
        return false;
    }

    if (vmm_get_physical(virt) != phys)
        return false;

    *virt_out = virt;
    return true;
}

/* --- Local APIC ---------------------------------------------------- */

static volatile uint32_t *g_lapic;
static uint64_t           g_lapic_phys;
static uint64_t           g_lapic_virt;
static bool               g_lapic_fresh;
static bool               g_lapic_ok;

uint32_t lapic_read(uint32_t reg)
{
    return g_lapic[reg / 4];
}

void lapic_write(uint32_t reg, uint32_t value)
{
    g_lapic[reg / 4] = value;

    /*
     * The LAPIC has no posted-write queue to drain, but the interrupt
     * controller's own documentation recommends a read-back after
     * writing a control register, and on some chipsets a write followed
     * immediately by a dependent operation is reordered. Reading the ID
     * register is the standard no-op fence.
     */
    (void)g_lapic[LAPIC_ID / 4];
}

void lapic_init(void)
{
    uint64_t apic_base = cpu_read_msr(MSR_IA32_APIC_BASE);

    g_lapic_phys = apic_base & APIC_BASE_ADDR_MASK;

    /* The MSR is the authority, but a zero base means either an ancient
     * CPU or a virtual machine that does not model the register. */
    if (g_lapic_phys == 0)
        g_lapic_phys = LAPIC_DEFAULT_PHYS;

    if (!map_mmio_uncached(g_lapic_phys, &g_lapic_virt, &g_lapic_fresh)) {
        kprintf("apic: cannot map the local APIC at %p\n", (void *)g_lapic_phys);
        return;
    }

    g_lapic = (volatile uint32_t *)g_lapic_virt;

    /*
     * Enable the APIC in software. Some firmware leaves SVR bit 8 clear,
     * in which case every LVT entry behaves as masked no matter what it
     * says -- an interrupt controller that accepts configuration and
     * delivers nothing.
     *
     * Note that this must not be skipped when the MSR already reports
     * APIC_BASE_ENABLE: that bit is the hardware enable, and this one is
     * the software enable. They are separate.
     */
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | LAPIC_SVR_VECTOR);

    /* Task priority 0 accepts every interrupt class. A non-zero value
     * masks everything at or below it, which is a very quiet way to lose
     * interrupts. */
    lapic_write(LAPIC_TPR, 0);

    /* Clear any error status left over from firmware. The error register
     * is write-to-clear and needs the companion write to latch. */
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);

    /*
     * Mask every LVT entry, including the two LINT lines.
     *
     * The bootloader masks these already, and that is not something to
     * rely on: LINT0 in ExtINT mode is how the 8259s would reach the CPU,
     * and a machine that left it unmasked would deliver PIC interrupts
     * with the PIC's own vectors -- straight into the CPU exception range.
     */
    lapic_write(LAPIC_LVT_TIMER,   LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PERF,    LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0,   LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1,   LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR,   LAPIC_LVT_MASKED);

    g_lapic_ok = true;
}

bool lapic_ready(void)            { return g_lapic_ok; }
uint64_t lapic_base_physical(void) { return g_lapic_phys; }
uint64_t lapic_base_virtual(void)  { return g_lapic_virt; }
uint32_t lapic_id(void)            { return g_lapic_ok ? (lapic_read(LAPIC_ID) >> 24) : 0; }
uint32_t lapic_version(void)       { return g_lapic_ok ? (lapic_read(LAPIC_VERSION) & 0xFF) : 0; }

void lapic_eoi(void)
{
    if (g_lapic_ok)
        g_lapic[LAPIC_EOI / 4] = 0;
}

/* --- I/O APIC ------------------------------------------------------ */

struct ioapic_instance {
    bool     present;
    uint8_t  id;
    uint32_t gsi_base;
    uint32_t entries;
    uint64_t phys;
    uint64_t virt;
};

static struct ioapic_instance g_ioapics[IOAPIC_MAX_INSTANCES];
static size_t                 g_ioapic_count;
static bool                   g_ioapic_ok;

static uint32_t ioapic_read(const struct ioapic_instance *io, uint32_t reg)
{
    volatile uint32_t *base = (volatile uint32_t *)io->virt;
    base[IOAPIC_IOREGSEL / 4] = reg;
    return base[IOAPIC_IOWIN / 4];
}

static void ioapic_write(const struct ioapic_instance *io, uint32_t reg,
                         uint32_t value)
{
    volatile uint32_t *base = (volatile uint32_t *)io->virt;
    base[IOAPIC_IOREGSEL / 4] = reg;
    base[IOAPIC_IOWIN / 4]     = value;
}

static void ioapic_write_redirection(const struct ioapic_instance *io,
                                     uint32_t index, uint64_t entry)
{
    ioapic_write(io, IOAPIC_REG_REDIR(index),     (uint32_t)(entry & 0xFFFFFFFFu));
    ioapic_write(io, IOAPIC_REG_REDIR(index) + 1, (uint32_t)(entry >> 32));
}

static uint64_t ioapic_read_redirection(const struct ioapic_instance *io,
                                        uint32_t index)
{
    uint64_t low  = ioapic_read(io, IOAPIC_REG_REDIR(index));
    uint64_t high = ioapic_read(io, IOAPIC_REG_REDIR(index) + 1);
    return (high << 32) | low;
}

/*
 * Register one IO APIC and map its register window.
 *
 * The version register also reports how many redirection entries there
 * are, which differs between parts (16 on early ones, 24 on most, 120 on
 * some server chipsets). Discovering the count rather than assuming 24 is
 * what keeps `max_gsi` honest.
 */
static bool register_ioapic(uint8_t id, uint32_t phys, uint32_t gsi_base)
{
    if (g_ioapic_count >= IOAPIC_MAX_INSTANCES)
        return false;

    struct ioapic_instance *io = &g_ioapics[g_ioapic_count];

    bool fresh;
    if (!map_mmio_uncached(phys, &io->virt, &fresh))
        return false;

    io->id       = id;
    io->gsi_base = gsi_base;
    io->phys     = phys;

    uint32_t version = ioapic_read(io, IOAPIC_REG_VERSION);
    /* Mapping an address does not prove that a device exists there. VBox
     * with I/O APIC disabled returns all ones at this MMIO address; treating
     * that as version 255 / 256 lines falsely advertises working ISA IRQs. */
    if (version == 0 || version == UINT32_MAX) {
        kprintf("apic: no usable IO APIC at %p (version %x); "
                "enable I/O APIC in the VM settings\n", (void *)io->phys,
                (unsigned)version);
        return false;
    }
    io->present = true;
    io->entries = ((version >> 16) & 0xFF) + 1;

    if (io->entries > IOAPIC_MAX_ENTRIES)
        io->entries = IOAPIC_MAX_ENTRIES;

    g_ioapic_count++;
    return true;
}

void ioapic_init(void)
{
    /*
     * Take the 8259s out of the picture first. They are remapped out of
     * the exception vector range and masked, so that a stray spurious
     * IRQ7 from an unclaimed PIC cannot be mistaken for a CPU fault.
     */
    pic_remap_and_mask();

    if (acpi_ioapic_count() > 0) {
        for (size_t i = 0; i < acpi_ioapic_count(); i++) {
            const struct acpi_ioapic_info *info = acpi_ioapic(i);
            register_ioapic(info->id, info->address, info->gsi_base);
        }
    } else {
        /* No ACPI, or a MADT with no IO APIC in it. Fall back to the
         * address every PC has used for thirty years. */
        register_ioapic(0, IOAPIC_DEFAULT_PHYS, 0);
    }

    if (g_ioapic_count == 0) {
        kprintf("apic: no IO APIC could be mapped; external interrupts are dead\n");
        return;
    }

    g_ioapic_ok = true;
    ioapic_mask_all();
}

bool ioapic_ready(void)              { return g_ioapic_ok; }
uint32_t ioapic_id(void)             { return g_ioapic_ok ? g_ioapics[0].id : 0; }
uint32_t ioapic_version(void)        { return g_ioapic_ok ? (ioapic_read(&g_ioapics[0], IOAPIC_REG_VERSION) & 0xFF) : 0; }
uint64_t ioapic_address(void)        { return g_ioapic_ok ? g_ioapics[0].phys : 0; }
uint32_t ioapic_entry_count(void)    { return g_ioapic_ok ? g_ioapics[0].entries : 0; }

uint32_t ioapic_max_gsi(void)
{
    uint32_t max = 0;
    for (size_t i = 0; i < g_ioapic_count; i++) {
        uint32_t top = g_ioapics[i].gsi_base + g_ioapics[i].entries;
        if (top > max)
            max = top;
    }
    return max;
}

static struct ioapic_instance *ioapic_for_gsi(uint32_t gsi)
{
    for (size_t i = 0; i < g_ioapic_count; i++) {
        uint32_t base = g_ioapics[i].gsi_base;
        if (gsi >= base && gsi < base + g_ioapics[i].entries)
            return &g_ioapics[i];
    }
    return NULL;
}

void ioapic_mask_gsi(uint32_t gsi, bool masked)
{
    struct ioapic_instance *io = ioapic_for_gsi(gsi);
    if (!io)
        return;

    uint32_t index = gsi - io->gsi_base;
    uint64_t entry = ioapic_read_redirection(io, index);

    if (masked)
        entry |= (1ULL << 16);
    else
        entry &= ~(1ULL << 16);

    ioapic_write_redirection(io, index, entry);
}

void ioapic_mask_all(void)
{
    for (size_t i = 0; i < g_ioapic_count; i++) {
        for (uint32_t e = 0; e < g_ioapics[i].entries; e++)
            ioapic_write_redirection(&g_ioapics[i], e, 1ULL << 16);
    }
}

void ioapic_route_gsi(uint32_t gsi, uint8_t vector, bool level_triggered,
                      bool active_low)
{
    struct ioapic_instance *io = ioapic_for_gsi(gsi);
    if (!io || !g_lapic_ok)
        return;

    /*
     * Redirection entry layout, Intel SDM Vol 3A, 11.6.1:
     *   bits  0-7  vector
     *   bits  8-10 delivery mode (000 = fixed)
     *   bit  11    destination mode (0 = physical)
     *   bit  13    polarity (0 = active high, 1 = active low)
     *   bit  15    trigger mode (0 = edge, 1 = level)
     *   bit  16    mask
     *   bits 56-63 destination APIC ID
     */
    uint64_t entry = vector;

    if (active_low)
        entry |= 1ULL << 13;
    if (level_triggered)
        entry |= 1ULL << 15;

    entry |= (uint64_t)lapic_id() << 56;

    /* Program the entry while still masked, then unmask. Unmasking first
     * would let a pending line fire with a half-written vector. */
    ioapic_write_redirection(io, gsi - io->gsi_base, entry | (1ULL << 16));
    ioapic_write_redirection(io, gsi - io->gsi_base, entry);
}

void ioapic_route_isa(unsigned isa_irq, uint8_t vector)
{
    uint32_t gsi = isa_irq;

    /*
     * Identity for most lines, but firmware is allowed to move any of
     * them. The usual override in the wild sends the timer to GSI 2
     * because the 8254 is wired there on some chipsets; the keyboard's
     * override is rarer but entirely legitimate.
     */
    bool level = false;
    bool low   = false;

    const struct acpi_isa_override *ov = acpi_isa_override(isa_irq);
    if (ov->present) {
        gsi   = ov->gsi;
        level = ov->level_triggered;
        low   = ov->active_low;
    }

    ioapic_route_gsi(gsi, vector, level, low);
}
