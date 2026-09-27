#include <funnyos/arch/x86_64/acpi.h>
#include <funnyos/bootinfo.h>
#include <funnyos/kprintf.h>
#include <funnyos/pmm.h>
#include <funnyos/vmm.h>

#include <libk/string.h>

#define ACPI_MAX_IOAPICS 4
#define ACPI_ISA_IRQ_COUNT 16

/* --- Table layouts ------------------------------------------------- */

struct rsdp {
    char     signature[8];      /* "RSD PTR " */
    uint8_t  checksum;          /* over the first 20 bytes */
    char     oem_id[6];
    uint8_t  revision;          /* 0 = ACPI 1.0, >= 2 = has XSDT */
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct madt {
    struct sdt_header hdr;
    uint32_t lapic_address;
    uint32_t flags;
    /* variable-length entries follow */
} __attribute__((packed));

struct madt_entry {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

#define MADT_TYPE_IOAPIC        1
#define MADT_TYPE_ISA_OVERRIDE  2
#define MADT_TYPE_LAPIC_OVERRIDE 5

struct madt_ioapic {
    struct madt_entry hdr;
    uint8_t  id;
    uint8_t  reserved;
    uint32_t address;
    uint32_t gsi_base;
} __attribute__((packed));

struct madt_isa_override {
    struct madt_entry hdr;
    uint8_t  bus;        /* 0 = ISA */
    uint8_t  source;     /* the ISA IRQ being overridden */
    uint32_t gsi;        /* the global system interrupt it becomes */
    uint16_t flags;
} __attribute__((packed));

struct madt_lapic_override {
    struct madt_entry hdr;
    uint16_t reserved;
    uint64_t address;
} __attribute__((packed));

/* --- State --------------------------------------------------------- */

static bool     g_found;
static uint64_t g_lapic_address;

static struct acpi_ioapic_info g_ioapics[ACPI_MAX_IOAPICS];
static size_t                  g_ioapic_count;

static struct acpi_isa_override g_overrides[ACPI_ISA_IRQ_COUNT];

/* --- Helpers ------------------------------------------------------- */

/*
 * Make a chunk of firmware memory readable.
 *
 * ACPI tables live in ACPI-reclaimable or ACPI-NVS memory, and base
 * revision 3 of the boot protocol only guarantees that usable memory,
 * bootloader-reclaimable memory and the kernel image are mapped. So
 * firmware can hand back a perfectly valid table pointer that faults on
 * first touch.
 *
 * Limine returns ACPI pointers inside the HHDM, which makes the physical
 * address a subtraction away, and any page that is missing can be mapped
 * on demand.
 */
static bool ensure_readable(const void *ptr, size_t len)
{
    uint64_t hhdm  = bootinfo_hhdm_offset();
    uint64_t first = (uint64_t)ptr & ~(PAGE_SIZE - 1);
    uint64_t last  = ((uint64_t)ptr + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t page = first; page < last; page += PAGE_SIZE) {
        if (vmm_is_mapped(page))
            continue;

        /* Only HHDM addresses have an obvious physical counterpart. A
         * pointer below the HHDM would be a physical address that the
         * caller should have rebased already. */
        if (page < hhdm)
            return false;

        if (!vmm_map_page(page, page - hhdm, VMM_KERNEL_RW))
            return false;
    }

    return true;
}

/*
 * Rebase an address that came out of a table.
 *
 * Addresses stored *inside* ACPI tables are physical, so they need the
 * HHDM offset added. Accept one that is already virtual rather than
 * adding the offset twice, which is what a bootloader that helpfully
 * fixed up the tables would produce.
 */
static uint64_t rebase(uint64_t raw)
{
    uint64_t hhdm = bootinfo_hhdm_offset();
    return raw >= hhdm ? raw : hhdm + raw;
}

static bool checksum_ok(const void *table, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)table;
    uint8_t sum = 0;

    for (size_t i = 0; i < len; i++)
        sum = (uint8_t)(sum + bytes[i]);

    return sum == 0;
}

static bool signature_is(const char *sig, const char *want)
{
    return memcmp(sig, want, 4) == 0;
}

/* --- MADT parsing -------------------------------------------------- */

static void parse_madt(struct madt *madt)
{
    g_lapic_address = madt->lapic_address;

    /*
     * Walk the entry list. Every entry carries its own length, so an
     * entry type this kernel has never heard of is skipped by its own
     * length field rather than by guessing -- which is what makes an
     * unparsed type harmless instead of fatal.
     */
    const uint8_t *base   = (const uint8_t *)madt;
    const uint8_t *end    = base + madt->hdr.length;
    const uint8_t *cursor = base + sizeof(struct madt);

    while (cursor + sizeof(struct madt_entry) <= end) {
        const struct madt_entry *entry = (const struct madt_entry *)cursor;

        if (entry->length < sizeof(struct madt_entry) ||
            cursor + entry->length > end)
            break;   /* malformed; stop rather than run off the table */

        switch (entry->type) {
        case MADT_TYPE_IOAPIC:
            if (g_ioapic_count < ACPI_MAX_IOAPICS) {
                const struct madt_ioapic *io = (const struct madt_ioapic *)entry;
                g_ioapics[g_ioapic_count].id       = io->id;
                g_ioapics[g_ioapic_count].address  = io->address;
                g_ioapics[g_ioapic_count].gsi_base = io->gsi_base;
                g_ioapic_count++;
            }
            break;

        case MADT_TYPE_ISA_OVERRIDE: {
            const struct madt_isa_override *ov = (const struct madt_isa_override *)entry;
            if (ov->source < ACPI_ISA_IRQ_COUNT) {
                struct acpi_isa_override *slot = &g_overrides[ov->source];

                /*
                 * Flags bits 0-1 are polarity, bits 2-3 are trigger mode.
                 * Zero means "bus default", and the ISA bus default is
                 * active high, edge triggered -- which is exactly what the
                 * IO APIC already assumes, so an all-zero flags field
                 * needs no special handling.
                 */
                slot->present         = true;
                slot->gsi             = ov->gsi;
                slot->active_low      = ((ov->flags >> 0) & 3) == 3;
                slot->level_triggered = ((ov->flags >> 2) & 3) == 3;
            }
            break;
        }

        case MADT_TYPE_LAPIC_OVERRIDE: {
            const struct madt_lapic_override *lo =
                (const struct madt_lapic_override *)entry;
            /* Takes precedence over the address in the MADT header. */
            g_lapic_address = lo->address;
            break;
        }

        default:
            break;
        }

        cursor += entry->length;
    }
}

static struct madt *find_madt(void)
{
    void    *rsdp_ptr = bootinfo_rsdp();
    uint64_t hhdm     = bootinfo_hhdm_offset();

    if (!rsdp_ptr)
        return NULL;

    /* The bootloader is supposed to hand back a virtual address, but a
     * physical one is cheap to accept and impossible to distinguish by
     * inspection: both are plausible 64-bit values. Anything below the
     * HHDM can only be physical. */
    uint64_t rsdp_addr = (uint64_t)rsdp_ptr;
    if (rsdp_addr < hhdm)
        rsdp_addr += hhdm;

    if (!ensure_readable((void *)rsdp_addr, sizeof(struct rsdp)))
        return NULL;

    struct rsdp *rsdp = (struct rsdp *)rsdp_addr;
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0)
        return NULL;
    if (!checksum_ok(rsdp, 20))
        return NULL;

    /* ACPI 1.0 has no XSDT, only the 32-bit RSDT. */
    bool     use_xsdt = rsdp->revision >= 2 && rsdp->xsdt_address != 0;
    uint64_t root     = rebase(use_xsdt ? rsdp->xsdt_address
                                        : (uint64_t)rsdp->rsdt_address);

    if (!ensure_readable((void *)root, sizeof(struct sdt_header)))
        return NULL;

    struct sdt_header *root_hdr = (struct sdt_header *)root;
    if (!signature_is(root_hdr->signature, use_xsdt ? "XSDT" : "RSDT"))
        return NULL;
    if (!ensure_readable((void *)root, root_hdr->length))
        return NULL;
    if (!checksum_ok((void *)root, root_hdr->length))
        return NULL;

    size_t   entry_size = use_xsdt ? 8 : 4;
    size_t   count      = (root_hdr->length - sizeof(struct sdt_header)) / entry_size;
    uint8_t *entries    = (uint8_t *)root + sizeof(struct sdt_header);

    for (size_t i = 0; i < count; i++) {
        uint64_t raw = use_xsdt
                     ? ((uint64_t *)entries)[i]
                     : (uint64_t)((uint32_t *)entries)[i];
        uint64_t addr = rebase(raw);

        if (!ensure_readable((void *)addr, sizeof(struct sdt_header)))
            continue;

        struct sdt_header *hdr = (struct sdt_header *)addr;
        if (!signature_is(hdr->signature, "APIC"))
            continue;

        if (!ensure_readable((void *)addr, hdr->length))
            continue;
        if (!checksum_ok((void *)addr, hdr->length))
            continue;

        return (struct madt *)addr;
    }

    return NULL;
}

/* --- Public interface ---------------------------------------------- */

void acpi_init(void)
{
    struct madt *madt = find_madt();

    if (!madt) {
        g_found = false;
        return;
    }

    parse_madt(madt);
    g_found = true;
}

bool acpi_madt_found(void)
{
    return g_found;
}

uint64_t acpi_lapic_address(void)
{
    return g_lapic_address;
}

size_t acpi_ioapic_count(void)
{
    return g_ioapic_count;
}

const struct acpi_ioapic_info *acpi_ioapic(size_t index)
{
    if (index >= g_ioapic_count)
        return NULL;
    return &g_ioapics[index];
}

const struct acpi_isa_override *acpi_isa_override(unsigned isa_irq)
{
    static const struct acpi_isa_override none = { false, 0, false, false };

    if (isa_irq >= ACPI_ISA_IRQ_COUNT)
        return &none;
    return &g_overrides[isa_irq];
}

const struct acpi_ioapic_info *acpi_ioapic_for_gsi(uint32_t gsi)
{
    for (size_t i = 0; i < g_ioapic_count; i++) {
        uint32_t base = g_ioapics[i].gsi_base;
        /* An IO APIC owns a range of GSIs; its width is bounded by the
         * number of redirection entries it reports, but a generous 24 is
         * enough to disambiguate in a machine with several of them. */
        if (gsi >= base && gsi < base + 256)
            return &g_ioapics[i];
    }
    return NULL;
}
