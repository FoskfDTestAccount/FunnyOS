/*
 * FunnyOS kernel entry point.
 *
 * What this verifies, in order:
 *
 *   1. Limine loaded the kernel into 64-bit long mode and jumped to
 *      _start, and the stack the kernel built for itself is usable.
 *   2. The output channels work: serial (captured headlessly by the test
 *      harness) and the framebuffer console (what a real screen shows).
 *   3. The boot protocol requests parsed correctly: memory map, HHDM,
 *      framebuffer, firmware type, kernel image address.
 *   4. Descriptor tables are installed, so a CPU fault is reported rather
 *      than becoming a triple fault and a silent reboot. This is the whole
 *      point of M1.
 *
 * A deliberate fault can be injected by booting with the command line
 * option `selftest=fault`, which exercises the exception path on demand
 * instead of waiting for a real bug.
 */
#include <funnyos/bootinfo.h>
#include <funnyos/fb.h>
#include <funnyos/kprintf.h>
#include <funnyos/panic.h>
#include <funnyos/serial.h>

#include <funnyos/arch/x86_64/acpi.h>
#include <funnyos/arch/x86_64/apic.h>
#include <funnyos/arch/x86_64/gdt.h>
#include <funnyos/arch/x86_64/idt.h>
#include <funnyos/arch/x86_64/io.h>
#include <funnyos/arch/x86_64/irq.h>
#include <funnyos/arch/x86_64/timer.h>

#include <funnyos/heap.h>
#include <funnyos/pmm.h>
#include <funnyos/vmm.h>

#include <stdbool.h>
#include <libk/string.h>

extern char __kernel_end[];

/* Scratch address for the mapping self-test below. Far above the kernel
 * image and outside both the heap range and the HHDM. */
#define MEMTEST_VADDR 0xffffffffd0000000ULL

/*
 * Below the kernel and outside the HHDM, so it is guaranteed unmapped
 * under base revision 3: the unconditional identity map of the low 4 GiB
 * is dropped at that revision. Chosen to be recognisable in a register
 * dump at a glance.
 */
#define FAULT_TEST_ADDRESS 0x00000000DEADB000ULL

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

/* Print a value in megahertz with three decimals, without floating point:
 * the kernel is compiled with -mgeneral-regs-only and has no FPU state to
 * save. */
static void print_mhz(uint64_t hz)
{
    kprintf("%llu.%03llu MHz",
            (unsigned long long)(hz / 1000000ull),
            (unsigned long long)((hz % 1000000ull) / 1000ull));
}

static void print_interrupt_info(void)
{
    kprintf("\n[interrupts]\n");

    if (acpi_madt_found()) {
        kprintf("  ACPI           : MADT parsed, %llu IO APIC(s)\n",
                (unsigned long long)acpi_ioapic_count());
    } else {
        kprintf("  ACPI           : no usable MADT, using default addresses\n");
    }

    if (lapic_ready()) {
        kprintf("  Local APIC     : id %u, v%u, base %p (mapped uncached)\n",
                (unsigned)lapic_id(), (unsigned)lapic_version(),
                (void *)lapic_base_physical());
    } else {
        kprintf("  Local APIC     : NOT AVAILABLE\n");
    }

    if (ioapic_ready()) {
        kprintf("  I/O APIC       : id %u, v%u, base %p, %u lines\n",
                (unsigned)ioapic_id(), (unsigned)ioapic_version(),
                (void *)ioapic_address(), (unsigned)ioapic_entry_count());
    } else {
        kprintf("  I/O APIC       : NOT AVAILABLE\n");
    }

    /* Historical note rather than a live component: the 8259 pair is
     * remapped out of the exception vector range and masked, and nothing
     * ever unmasks it. */
    kprintf("  Legacy PIC     : remapped to 0x20/0x28 and masked\n");
}

static void print_timer_info(void)
{
    kprintf("\n[timer]\n");

    if (!timer_ready()) {
        kprintf("  Source         : none, no time base available\n");
        return;
    }

    kprintf("  Source         : LAPIC timer, %u Hz periodic on vector %u\n",
            (unsigned)timer_hz(), (unsigned)IRQ_VECTOR_TIMER);
    kprintf("  Calibration    : %llu counts/s at divide 16 (PIT reference)\n",
            (unsigned long long)timer_lapic_hz());
    kprintf("  Tick period    : %llu counts\n",
            (unsigned long long)(timer_lapic_hz() / timer_hz()));

    kprintf("  Timestamp ctr  : ");
    print_mhz(timer_tsc_hz());
    kprintf("\n");
}

/*
 * Exercise the memory subsystem end to end.
 *
 * This runs on every boot rather than only under a test flag: it is fast,
 * side-effect free once it unwinds, and the failure it guards against --
 * page tables that map nothing, or a heap that hands out memory twice --
 * would otherwise not show up until much later, in code far from the
 * cause.
 */
static bool run_memory_selftest(void)
{
    bool ok = true;

    /* --- Frames and page tables --- */
    uint64_t frame = pmm_alloc_page();
    if (!frame) {
        kprintf("    frames      : FAILED, no frame available\n");
        return false;
    }

    if (!vmm_map_page(MEMTEST_VADDR, frame, VMM_KERNEL_RW)) {
        kprintf("    mapping     : FAILED to map %p\n", (void *)MEMTEST_VADDR);
        pmm_free_page(frame);
        return false;
    }

    /*
     * Write at both ends of the page. A single write would not notice a
     * mapping that only covers part of the frame.
     */
    volatile uint64_t *page = (volatile uint64_t *)MEMTEST_VADDR;
    page[0]   = 0x0123456789ABCDEFULL;
    page[511] = 0xFEDCBA9876543210ULL;

    if (page[0] != 0x0123456789ABCDEFULL ||
        page[511] != 0xFEDCBA9876543210ULL) {
        kprintf("    mapping     : FAILED, data did not round-trip\n");
        ok = false;
    }

    if (vmm_get_physical(MEMTEST_VADDR) != frame) {
        kprintf("    reverse map : FAILED, virt resolves to the wrong frame\n");
        ok = false;
    }

    vmm_unmap_page(MEMTEST_VADDR);

    if (vmm_is_mapped(MEMTEST_VADDR)) {
        kprintf("    unmap       : FAILED, still mapped after unmap\n");
        ok = false;
    }

    pmm_free_page(frame);

    /* --- Heap --- */
    uint8_t *small = kmalloc(100);
    uint8_t *large = kmalloc(4000);
    uint8_t *big   = kmalloc(70000);   /* forces a growth step */

    if (!small || !large || !big) {
        kprintf("    heap alloc  : FAILED (small=%p large=%p big=%p)\n",
                (void *)small, (void *)large, (void *)big);
        ok = false;
    } else {
        memset(small, 0xAA, 100);
        memset(large, 0xBB, 4000);
        memset(big,   0xCC, 70000);

        if (small[0] != 0xAA || small[99] != 0xAA ||
            large[0] != 0xBB || large[3999] != 0xBB ||
            big[0] != 0xCC || big[69999] != 0xCC) {
            kprintf("    heap write  : FAILED, data did not round-trip\n");
            ok = false;
        }

        /* Distinct allocations must not overlap. */
        if (small == large || small == big || large == big) {
            kprintf("    heap alloc  : FAILED, allocations overlap\n");
            ok = false;
        }

        kfree(small);
        kfree(large);
        kfree(big);
    }

    /* --- Coalescing --- */
    size_t before = heap_largest_free();
    uint8_t *a = kmalloc(4096);
    uint8_t *b = kmalloc(4096);
    uint8_t *c = kmalloc(4096);
    if (a && b && c) {
        kfree(b);
        kfree(a);
        kfree(c);
        if (heap_largest_free() < before) {
            kprintf("    coalescing  : FAILED, largest free block shrank\n");
            ok = false;
        }
    } else {
        ok = false;
    }

    return ok;
}

void kmain(void)
{
    /* Serial comes first: it is the channel of last resort, and it still
     * works even if the framebuffer turns out to be unusable. */
    serial_init();

    /* Then the visible console. A failure here is not fatal -- output
     * simply stays serial-only, which the summary below reports. */
    bool fb_ok = fb_init();

    /*
     * Descriptor tables go before anything else that can fault. Loading the
     * IDT is what turns a CPU exception from a silent triple fault into a
     * handler that can explain itself.
     *
     * Order matters: the IDT entries name a code selector, and the IST
     * stacks live in the TSS, so the GDT has to be in place first.
     */
    gdt_init();
    idt_init();

    /*
     * Then memory, in dependency order: the frame allocator reads the
     * bootloader's memory map, the page table code needs frames for any
     * new table, and the heap needs both to grow itself.
     */
    pmm_init();
    vmm_init();
    heap_init();

    /*
     * Interrupt controllers last, because everything above them needs
     * memory to be working: finding the LAPIC means mapping its registers,
     * and mapping needs page tables, which need frames.
     *
     * The order within this group is a dependency chain. The handler table
     * comes first so a driver can claim a vector. ACPI comes next because
     * it is where firmware says where the controllers are. The LAPIC is
     * then enabled -- and it has to be, because the IO APIC delivers
     * *through* it. Only then can the IO APIC be programmed. The timer
     * needs both: the LAPIC to tick, and the PIT as a reference to
     * calibrate against.
     */
    irq_init();
    acpi_init();
    lapic_init();
    ioapic_init();
    timer_init();

    kprintf("\n");
    kprintf("==================================================\n");
    kprintf("  FunnyOS -- x86-64 kernel\n");
    kprintf("  boot chain and kernel infrastructure check\n");
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

    kprintf("\n[cpu]\n");
    kprintf("  GDT/TSS        : installed, TSS at %p\n", (void *)tss_address());
    kprintf("  IDT            : 256 vectors installed\n");
    kprintf("  Fault handling : active (#DF and NMI on dedicated IST stacks)\n");

    kprintf("\n[memory]\n");
    kprintf("  HHDM offset    : %p\n", (void *)bootinfo_hhdm_offset());
    print_memory_summary();

    kprintf("\n[page frames]\n");
    kprintf("  Page size      : %llu bytes\n", (unsigned long long)PAGE_SIZE);
    kprintf("  Total frames   : %llu\n", (unsigned long long)pmm_total_pages());
    kprintf("  Free frames    : %llu (%llu MiB)\n",
            (unsigned long long)pmm_free_frame_count(),
            (unsigned long long)(pmm_free_frame_count() * PAGE_SIZE / (1024 * 1024)));
    kprintf("  In use         : %llu frames\n",
            (unsigned long long)pmm_used_pages());

    kprintf("\n[kernel heap]\n");
    kprintf("  Mapped         : %llu KiB\n",
            (unsigned long long)(heap_capacity() / 1024));
    kprintf("  In use         : %llu bytes\n",
            (unsigned long long)heap_in_use());
    kprintf("  Largest free   : %llu bytes\n",
            (unsigned long long)heap_largest_free());

    kprintf("\n[display]\n");
    print_framebuffer_info();

    print_interrupt_info();
    print_timer_info();

    /*
     * Report the output channels last. Even when the serial loopback
     * check fails, everything above still made it out -- which means the
     * transmit direction works and only receive is broken. That
     * distinction is itself diagnostically useful.
     */
    kprintf("\n[console]\n");
    kprintf("  Text console   : %s\n",
            fb_ok ? "active (8x16 cell grid)"
                  : "unavailable, output is serial only");
    kprintf("  COM1 (115200 8N1): %s\n",
            serial_is_ready() ? "loopback self-test passed"
                              : "transmit works, loopback self-test failed");

    kprintf("\n[self-test]\n");
    bool mem_ok = run_memory_selftest();
    kprintf("  Memory subsystem: %s\n",
            mem_ok ? "all checks passed" : "FAILURES, see above");

    kprintf("\n");
    kprintf("==================================================\n");
    kprintf("  Boot verification %s.\n", mem_ok ? "PASSED" : "FAILED");
    kprintf("==================================================\n");

    /*
     * Opt-in fault injection. Booting with `selftest=fault` on the command
     * line exercises the exception path deliberately, so the diagnostic
     * machinery can be tested without waiting for a real bug to appear.
     * Normal boots never reach this.
     */
    const char *cmdline = bootinfo_cmdline();
    if (strstr(cmdline, "selftest=fault")) {
        kprintf("\n[fault self-test]\n");
        kprintf("  Command line requested fault injection.\n");
        kprintf("  Reading from unmapped address %p ...\n",
                (void *)FAULT_TEST_ADDRESS);

        volatile uint64_t *unmapped = (volatile uint64_t *)FAULT_TEST_ADDRESS;
        (void)*unmapped;    /* the fault handler takes over from here */

        /* Not reached: the page fault handler does not return. */
        panic("fault injection did not fault at %p", (void *)FAULT_TEST_ADDRESS);
    }

    /*
     * From here on the kernel is interrupt-driven. Enabling interrupts is
     * the last step of every bring-up sequence, and deliberately so:
     * before this line, an interrupt arriving would find handlers
     * registered against a half-initialised machine.
     */
    kprintf("\n[timer check]\n");
    interrupts_enable();
    kprintf("  Interrupts     : enabled\n");

    uint32_t measured_x10 = 0;
    bool timer_ok = timer_selftest(&measured_x10);

    if (!timer_ready()) {
        kprintf("  Tick rate      : SKIPPED (no time base)\n");
    } else {
        kprintf("  Tick rate      : %s (%u.%u Hz measured against the TSC)\n",
                timer_ok ? "PASS" : "FAIL",
                (unsigned)(measured_x10 / 10), (unsigned)(measured_x10 % 10));
    }

    kprintf("  Interrupt count: %llu\n",
            (unsigned long long)irq_total_count());

    kprintf("\nThis output is mirrored to COM1. The kernel has no scheduler\n");
    kprintf("yet, so it idles here by design, ticking at %u Hz.\n",
            (unsigned)timer_hz());

    for (;;)
        __asm__ volatile("hlt");
}
