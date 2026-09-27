/*
 * FunnyOS kernel entry point.
 *
 * M0 goal: prove the boot chain works end to end. Four things are
 * verified here:
 *   1. Limine loaded the kernel correctly and jumped to _start in 64-bit
 *      long mode.
 *   2. The stack the kernel built for itself is usable, so C code runs.
 *   3. The serial output channel works. QEMU can capture it headlessly,
 *      which is what makes the automated test possible.
 *   4. Limine requests were parsed correctly and memory map /
 *      framebuffer information is readable.
 */
#include <funnyos/bootinfo.h>
#include <funnyos/kprintf.h>
#include <funnyos/panic.h>
#include <funnyos/serial.h>

#include <libk/string.h>

extern char __kernel_end[];

/* Translate the firmware type enum into a human-readable name. */
static const char *firmware_type_name(uint64_t type)
{
    switch (type) {
    case LIMINE_FIRMWARE_TYPE_X86BIOS: return "x86 BIOS (legacy)";
    case LIMINE_FIRMWARE_TYPE_EFI32:   return "UEFI 32-bit";
    case LIMINE_FIRMWARE_TYPE_EFI64:   return "UEFI 64-bit";
    default:                           return "(unknown)";
    }
}

static void print_memory_summary(void)
{
    uint64_t usable = bootinfo_memory_total_by_type(LIMINE_MEMMAP_USABLE);

    kprintf("  Usable memory  : %llu MiB (%llu bytes)\n",
            (unsigned long long)(usable / (1024 * 1024)),
            (unsigned long long)usable);

    /* Summarise memory map entries by type, which doubles as a check
     * that the map parsed correctly rather than merely being present. */
    static const struct {
        uint64_t    type;
        const char *name;
    } types[] = {
        { LIMINE_MEMMAP_USABLE,                 "usable" },
        { LIMINE_MEMMAP_RESERVED,               "reserved" },
        { LIMINE_MEMMAP_ACPI_RECLAIMABLE,       "ACPI reclaimable" },
        { LIMINE_MEMMAP_ACPI_NVS,               "ACPI NVS" },
        { LIMINE_MEMMAP_BAD_MEMORY,             "bad memory" },
        { LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE, "bootloader reclaimable" },
        { LIMINE_MEMMAP_EXECUTABLE_AND_MODULES, "kernel and modules" },
        { LIMINE_MEMMAP_FRAMEBUFFER,            "framebuffer" },
    };

    kprintf("  Memmap entries : %llu\n",
            (unsigned long long)bootinfo_memmap_entry_count());
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        uint64_t bytes = bootinfo_memory_total_by_type(types[i].type);
        if (bytes == 0)
            continue;
        kprintf("    %-24s %8llu KiB\n",
                types[i].name, (unsigned long long)(bytes / 1024));
    }
}

static void print_framebuffer_info(void)
{
    struct limine_framebuffer *fb = bootinfo_framebuffer();
    if (!fb) {
        kprintf("  Framebuffer    : not available\n");
        return;
    }

    kprintf("  Framebuffer    : %llux%llu, %u bpp, pitch=%llu\n",
            (unsigned long long)fb->width,
            (unsigned long long)fb->height,
            (unsigned)fb->bpp,
            (unsigned long long)fb->pitch);
    kprintf("  FB address     : %p (virtual)\n", fb->address);
    kprintf("  Pixel format   : model=%u R:%u@%u G:%u@%u B:%u@%u\n",
            fb->memory_model,
            fb->red_mask_size,   fb->red_mask_shift,
            fb->green_mask_size, fb->green_mask_shift,
            fb->blue_mask_size,  fb->blue_mask_shift);
}

void kmain(void)
{
    /* Serial must come first: it is the channel for every diagnostic
     * message that follows. */
    serial_init();

    kprintf("\n");
    kprintf("==================================================\n");
    kprintf("  FunnyOS -- x86-64 kernel, M0 boot verification\n");
    kprintf("==================================================\n");
    kprintf("\n");

    /*
     * First check: does the bootloader support the base revision we
     * requested? If not, none of the response pointers can be trusted
     * and we must stop immediately.
     */
    if (!bootinfo_base_revision_ok()) {
        panic("Limine does not support requested boot protocol base revision 3.\n"
              "Use a more complete Limine build, or lower the requested revision.");
    }

    kprintf("[boot]\n");
    kprintf("  Bootloader     : %s %s\n",
            bootinfo_loader_name(), bootinfo_loader_version());
    kprintf("  Firmware       : %s\n", firmware_type_name(bootinfo_firmware_type()));
    kprintf("  Base revision  : 3 (confirmed supported)\n");

    kprintf("\n[kernel image]\n");
    kprintf("  Physical base  : %p\n", (void *)bootinfo_kernel_physical_base());
    kprintf("  Virtual base   : %p\n", (void *)bootinfo_kernel_virtual_base());
    kprintf("  Image end      : %p\n", (void *)__kernel_end);

    kprintf("\n[memory]\n");
    kprintf("  HHDM offset    : %p\n", (void *)bootinfo_hhdm_offset());
    print_memory_summary();

    kprintf("\n[display]\n");
    print_framebuffer_info();

    /*
     * Report the serial loopback result last. Even when loopback fails,
     * everything above still made it out -- which means the transmit
     * direction works and only receive is broken. That distinction is
     * itself diagnostically useful.
     */
    kprintf("\n[serial]\n");
    kprintf("  COM1 (115200 8N1): %s\n",
            serial_is_ready() ? "loopback self-test passed"
                              : "transmit works, loopback self-test failed");

    kprintf("\n");
    kprintf("==================================================\n");
    kprintf("  M0 boot verification PASSED. Halting.\n");
    kprintf("==================================================\n");

    /* M0 stops here. M1 will install the IDT, page tables and physical
     * memory manager at this point. */
    kprintf("\nHalting...\n");
    __asm__ volatile("cli");
    for (;;)
        __asm__ volatile("hlt");
}
