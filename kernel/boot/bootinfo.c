/*
 * Limine 引导协议请求
 *
 * 这些结构体必须放在 .limine_requests 段内，且被 start/end 标记包裹。
 * 链接脚本负责把它们聚到一起。引导器在启动时扫描这段内存，
 * 找到它认识的请求并填充对应的 response 指针。
 *
 * 三个必须遵守的约束：
 *   1. 变量必须带 used 属性——否则编译器会认为无人引用而删除
 *   2. 变量必须位于 .limine_requests 段——见 linker.ld
 *   3. 变量不能是 const——引导器要往里写数据
 */
#include <funnyos/bootinfo.h>

#include <limine.h>
#include <stddef.h>

#define REQUESTS_START __attribute__((used, section(".limine_requests_start")))
#define REQUESTS       __attribute__((used, section(".limine_requests")))
#define REQUESTS_END   __attribute__((used, section(".limine_requests_end")))

/* ------------------------------------------------------------------ */
/* 请求定义                                                            */
/* ------------------------------------------------------------------ */

REQUESTS_START
static volatile uint64_t g_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

/*
 * 基版本。我们请求版本 3 —— 这是 Limine 保证支持的基线，
 * 且 HHDM 与内存映射语义已经稳定。
 *
 * 引导器会修改这个数组：
 *   [1] 被写入它实际使用的基版本
 *   [2] 若它不支持我们请求的版本则保持非 0
 * 因此必须用 LIMINE_BASE_REVISION_SUPPORTED 检查，不能假定成功。
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

REQUESTS_END
static volatile uint64_t g_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

/* ------------------------------------------------------------------ */
/* 访问接口                                                            */
/* ------------------------------------------------------------------ */

bool bootinfo_base_revision_ok(void)
{
    return LIMINE_BASE_REVISION_SUPPORTED(g_base_revision);
}

const char *bootinfo_loader_name(void)
{
    if (!g_bootloader_info_request.response)
        return "(未知)";
    return g_bootloader_info_request.response->name;
}

const char *bootinfo_loader_version(void)
{
    if (!g_bootloader_info_request.response)
        return "(未知)";
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
