/*
 * FunnyOS 内核入口
 *
 * M0 目标：证明引导闭环成立。
 * 具体验证四件事：
 *   1. Limine 把内核正确加载进 64 位长模式并跳转到 _start
 *   2. 内核自建的栈可用，C 代码能正常运行
 *   3. 串口输出通道可用（QEMU 可无头捕获，便于自动化测试）
 *   4. Limine 请求被正确解析，内存映射/帧缓冲信息可读
 */
#include <funnyos/bootinfo.h>
#include <funnyos/kprintf.h>
#include <funnyos/panic.h>
#include <funnyos/serial.h>

#include <libk/string.h>

extern char __kernel_end[];

/* 把固件类型枚举翻译成可读字符串 */
static const char *firmware_type_name(uint64_t type)
{
    switch (type) {
    case LIMINE_FIRMWARE_TYPE_X86BIOS: return "x86 BIOS (传统)";
    case LIMINE_FIRMWARE_TYPE_EFI32:   return "UEFI 32-bit";
    case LIMINE_FIRMWARE_TYPE_EFI64:   return "UEFI 64-bit";
    default:                           return "(未知)";
    }
}

static void print_memory_summary(void)
{
    uint64_t usable = bootinfo_memory_total_by_type(LIMINE_MEMMAP_USABLE);

    kprintf("  可用内存      : %llu MiB (%llu 字节)\n",
            (unsigned long long)(usable / (1024 * 1024)),
            (unsigned long long)usable);

    /* 内存映射条目按类型汇总，便于确认解析正确 */
    static const struct {
        uint64_t    type;
        const char *name;
    } types[] = {
        { LIMINE_MEMMAP_USABLE,                 "可用" },
        { LIMINE_MEMMAP_RESERVED,               "保留" },
        { LIMINE_MEMMAP_ACPI_RECLAIMABLE,       "ACPI 可回收" },
        { LIMINE_MEMMAP_ACPI_NVS,               "ACPI NVS" },
        { LIMINE_MEMMAP_BAD_MEMORY,             "坏内存" },
        { LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE, "引导器可回收" },
        { LIMINE_MEMMAP_EXECUTABLE_AND_MODULES, "内核与模块" },
        { LIMINE_MEMMAP_FRAMEBUFFER,            "帧缓冲" },
    };

    kprintf("  内存映射条目  : %llu\n",
            (unsigned long long)bootinfo_memmap_entry_count());
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        uint64_t bytes = bootinfo_memory_total_by_type(types[i].type);
        if (bytes == 0)
            continue;
        kprintf("    %-16s %8llu KiB\n",
                types[i].name, (unsigned long long)(bytes / 1024));
    }
}

static void print_framebuffer_info(void)
{
    struct limine_framebuffer *fb = bootinfo_framebuffer();
    if (!fb) {
        kprintf("  帧缓冲        : 不可用\n");
        return;
    }

    kprintf("  帧缓冲        : %llux%llu, %u bpp, pitch=%llu\n",
            (unsigned long long)fb->width,
            (unsigned long long)fb->height,
            (unsigned)fb->bpp,
            (unsigned long long)fb->pitch);
    kprintf("  帧缓冲地址    : %p (虚拟)\n", fb->address);
    kprintf("  像素格式      : 内存模型=%u R:%u@%u G:%u@%u B:%u@%u\n",
            fb->memory_model,
            fb->red_mask_size,   fb->red_mask_shift,
            fb->green_mask_size, fb->green_mask_shift,
            fb->blue_mask_size,  fb->blue_mask_shift);
}

void kmain(void)
{
    /* 串口必须最先初始化——它是接下来所有诊断输出的通道 */
    serial_init();

    kprintf("\n");
    kprintf("==================================================\n");
    kprintf("  FunnyOS —— x86-64 内核，M0 引导闭环验证\n");
    kprintf("==================================================\n");
    kprintf("\n");

    /*
     * 首要检查：引导器是否支持我们请求的基版本。
     * 若否，后续所有 response 指针都不可信，必须立刻停下。
     */
    if (!bootinfo_base_revision_ok()) {
        panic("Limine 不支持请求的引导协议基版本 3。\n"
              "请使用更完整的 Limine 构建，或降低请求的基版本号。");
    }

    kprintf("[引导]\n");
    kprintf("  引导器        : %s %s\n",
            bootinfo_loader_name(), bootinfo_loader_version());
    kprintf("  固件类型      : %s\n", firmware_type_name(bootinfo_firmware_type()));
    kprintf("  协议基版本    : 3 (已确认支持)\n");

    kprintf("\n[内核映像]\n");
    kprintf("  物理基址      : %p\n", (void *)bootinfo_kernel_physical_base());
    kprintf("  虚拟基址      : %p\n", (void *)bootinfo_kernel_virtual_base());
    kprintf("  映像结束      : %p\n", (void *)__kernel_end);

    kprintf("\n[内存]\n");
    kprintf("  HHDM 偏移     : %p\n", (void *)bootinfo_hhdm_offset());
    print_memory_summary();

    kprintf("\n[显示]\n");
    print_framebuffer_info();

    /*
     * 串口回环自检结果放在最后报告。
     * 注意：即使回环失败，上面的输出也已经打出——说明发送方向是好的，
     * 只有接收方向有问题。这条信息本身就有诊断价值。
     */
    kprintf("\n[串口]\n");
    kprintf("  COM1 (115200 8N1) : %s\n",
            serial_is_ready() ? "回环自检通过" : "发送可用，回环自检失败");

    kprintf("\n");
    kprintf("==================================================\n");
    kprintf("  M0 引导闭环验证通过。内核进入停机状态。\n");
    kprintf("==================================================\n");

    /* M0 到此为止。M1 会在这里接入 IDT、页表与物理内存管理。 */
    kprintf("\n正在停机……\n");
    __asm__ volatile("cli");
    for (;;)
        __asm__ volatile("hlt");
}
