/*
 * Limine boot protocol requests.
 *
 * These structures must live in the .limine_requests section, wrapped by
 * the start/end markers. The linker script groups them together. At boot
 * the bootloader scans that memory range, recognises the requests it
 * understands, and fills in the matching response pointers.
 *
 * Three constraints that must be respected:
 *   1. The variables need the `used` attribute, or the compiler will
 *      discard them as unreferenced.
 *   2. They must be placed in .limine_requests -- see linker.ld.
 *   3. They must not be const -- the bootloader writes into them.
 */
#include <funnyos/bootinfo.h>

#include <limine.h>
#include <stddef.h>

#define REQUESTS_START __attribute__((used, section(".limine_requests_start")))
#define REQUESTS       __attribute__((used, section(".limine_requests")))
#define REQUESTS_END   __attribute__((used, section(".limine_requests_end")))

/* ------------------------------------------------------------------ */
/* Request definitions                                                 */
/* ------------------------------------------------------------------ */

REQUESTS_START
static volatile uint64_t g_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

/*
 * Base revision. We request revision 3: the baseline every Limine build
 * guarantees support for, with settled HHDM and memory map semantics.
 *
 * The bootloader mutates this array:
 *   [1] receives the base revision it actually honoured
 *   [2] stays non-zero if it does not support the revision we asked for
 * So this must be validated with LIMINE_BASE_REVISION_SUPPORTED rather
 * than assumed.
 */
REQUESTS
static volatile uint64_t g_base_revision[] = LIMINE_BASE_REVISION(3);

REQUESTS
static volatile struct limine_bootloader_info_request g_bootloader_info_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID,
    .revision = 0,
};

REQUESTS
static volatile struct limine_firmware_type_request g_firmware_type_request = {
    .id = LIMINE_FIRMWARE_TYPE_REQUEST_ID,
    .revision = 0,
};

REQUESTS
static volatile struct limine_hhdm_request g_hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
};

REQUESTS
static volatile struct limine_memmap_request g_memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
};

REQUESTS
static volatile struct limine_framebuffer_request g_framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
};

REQUESTS
static volatile struct limine_executable_address_request g_exec_addr_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0,
};

REQUESTS
static volatile struct limine_rsdp_request g_rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
};

REQUESTS
static volatile struct limine_executable_cmdline_request g_cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
    .revision = 0,
};

REQUESTS_END
static volatile uint64_t g_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

/* ------------------------------------------------------------------ */
/* Accessors                                                           */
/* ------------------------------------------------------------------ */

bool bootinfo_base_revision_ok(void)
{
    return LIMINE_BASE_REVISION_SUPPORTED(g_base_revision);
}

const char *bootinfo_loader_name(void)
{
    if (!g_bootloader_info_request.response)
        return "(unknown)";
    return g_bootloader_info_request.response->name;
}

const char *bootinfo_loader_version(void)
{
    if (!g_bootloader_info_request.response)
        return "(unknown)";
    return g_bootloader_info_request.response->version;
}

uint64_t bootinfo_firmware_type(void)
{
    if (!g_firmware_type_request.response)
        return (uint64_t)-1;
    return g_firmware_type_request.response->firmware_type;
}

uint64_t bootinfo_hhdm_offset(void)
{
    if (!g_hhdm_request.response)
        return 0;
    return g_hhdm_request.response->offset;
}

uint64_t bootinfo_memmap_entry_count(void)
{
    if (!g_memmap_request.response)
        return 0;
    return g_memmap_request.response->entry_count;
}

const struct limine_memmap_entry *bootinfo_memmap_entry(uint64_t index)
{
    if (!g_memmap_request.response)
        return NULL;
    if (index >= g_memmap_request.response->entry_count)
        return NULL;
    return g_memmap_request.response->entries[index];
}

uint64_t bootinfo_memory_total_by_type(uint64_t type)
{
    if (!g_memmap_request.response)
        return 0;

    uint64_t total = 0;
    for (uint64_t i = 0; i < g_memmap_request.response->entry_count; i++) {
        const struct limine_memmap_entry *e = g_memmap_request.response->entries[i];
        if (e->type == type)
            total += e->length;
    }
    return total;
}

struct limine_framebuffer *bootinfo_framebuffer(void)
{
    if (!g_framebuffer_request.response)
        return NULL;
    if (g_framebuffer_request.response->framebuffer_count == 0)
        return NULL;
    return g_framebuffer_request.response->framebuffers[0];
}

void *bootinfo_rsdp(void)
{
    if (!g_rsdp_request.response)
        return NULL;
    return g_rsdp_request.response->address;
}

uint64_t bootinfo_kernel_physical_base(void)
{
    if (!g_exec_addr_request.response)
        return 0;
    return g_exec_addr_request.response->physical_base;
}

uint64_t bootinfo_kernel_virtual_base(void)
{
    if (!g_exec_addr_request.response)
        return 0;
    return g_exec_addr_request.response->virtual_base;
}

const char *bootinfo_cmdline(void)
{
    if (!g_cmdline_request.response || !g_cmdline_request.response->cmdline)
        return "";
    return g_cmdline_request.response->cmdline;
}
