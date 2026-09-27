#include <funnyos/process.h>

#include <funnyos/arch/x86_64/gdt.h>
#include <funnyos/arch/x86_64/irq.h>
#include <funnyos/bootinfo.h>
#include <funnyos/heap.h>
#include <funnyos/kprintf.h>
#include <funnyos/panic.h>
#include <funnyos/pmm.h>
#include <funnyos/vmm.h>

#include <libk/string.h>

/* Provided by usermode.asm. */
int  kernel_setjmp(struct kernel_context *ctx);
void kernel_longjmp(struct kernel_context *ctx, int value)
    __attribute__((noreturn));
void usermode_enter(uint64_t entry, uint64_t stack_top, uint64_t arg)
    __attribute__((noreturn));

static uint64_t        g_kernel_pml4;
static uint64_t        g_hhdm;
static struct process *g_current;

static void process_exit_hook(void);

/* ------------------------------------------------------------------ */
/* Frame helpers                                                       */
/* ------------------------------------------------------------------ */

/*
 * Kernel-visible pointer to a physical frame.
 *
 * Frames come from the allocator as physical addresses and have to be
 * written through the direct map; there is no identity mapping to lean on.
 */
static inline void *frame_ptr(uint64_t phys)
{
    return (void *)(g_hhdm + phys);
}

/*
 * Free the frames mapped over a virtual range, without unmapping them.
 *
 * Unmapping is pointless here -- the page tables are about to be thrown
 * away wholesale -- but freeing the frames is not, and this is the only
 * place that knows which frames the process actually owns.
 */
static void free_leaf_frames(uint64_t pml4, uint64_t start, uint64_t end)
{
    for (uint64_t virt = start; virt < end; virt += PAGE_SIZE) {
        uint64_t phys = vmm_get_physical_in(pml4, virt);
        if (phys)
            pmm_free_page(phys);
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void process_init(void)
{
    g_kernel_pml4 = vmm_pml4_physical();
    g_hhdm        = bootinfo_hhdm_offset();

    /*
     * The hook that actually ends a process.
     *
     * SYS_EXIT cannot return to the program that called it, and it cannot
     * simply not return either -- it is inside an interrupt handler, and
     * the only way out of one is the iretq at the bottom of the entry
     * stub. So it records the intent and this runs afterwards, with the
     * interrupt already acknowledged, and unwinds to whoever called
     * process_run.
     */
    irq_set_post_hook(process_exit_hook);
}

uint64_t process_current_pml4(void)
{
    return g_current ? g_current->pml4 : g_kernel_pml4;
}

struct open_file *process_open_files(void)
{
    return g_current ? g_current->files : NULL;
}

/*
 * Copy a flat image into a freshly mapped run of pages, and map the
 * stack above it.
 *
 * Returns false with nothing leaked if any step fails.
 */
static bool build_address_space(struct process *p, const void *image,
                                size_t size)
{
    /* --- The program --- */
    uint64_t image_end = PROCESS_CODE_BASE + size;

    for (uint64_t virt = PROCESS_CODE_BASE; virt < image_end; virt += PAGE_SIZE) {
        uint64_t frame = pmm_alloc_page();
        if (!frame)
            return false;

        if (!vmm_map_page_in(p->pml4, virt, frame, VMM_USER_RW)) {
            pmm_free_page(frame);
            return false;
        }

        uint8_t *dst = frame_ptr(frame);
        memset(dst, 0, PAGE_SIZE);

        size_t offset = (size_t)(virt - PROCESS_CODE_BASE);
        size_t chunk  = size - offset;
        if (chunk > PAGE_SIZE)
            chunk = PAGE_SIZE;

        memcpy(dst, (const uint8_t *)image + offset, chunk);
    }

    /* --- The stack --- */
    uint64_t stack_base = PROCESS_STACK_TOP - PROCESS_STACK_SIZE;

    for (uint64_t virt = stack_base; virt < PROCESS_STACK_TOP; virt += PAGE_SIZE) {
        uint64_t frame = pmm_alloc_page();
        if (!frame)
            return false;

        if (!vmm_map_page_in(p->pml4, virt, frame, VMM_USER_RW)) {
            pmm_free_page(frame);
            return false;
        }

        /* Zeroed, so a program that reads an uninitialised local gets
         * zeroes rather than whatever the last process left there. */
        memset(frame_ptr(frame), 0, PAGE_SIZE);
    }

    return true;
}

struct process *process_create(const char *name, const void *image, size_t size)
{
    if (!image || size == 0)
        return NULL;

    struct process *p = kmalloc(sizeof(*p));
    if (!p)
        return NULL;
    memset(p, 0, sizeof(*p));
    p->name = name;

    p->pml4 = vmm_create_address_space();
    if (!p->pml4) {
        kprintf("process: cannot create an address space\n");
        kfree(p);
        return NULL;
    }

    p->kernel_stack_size = PROCESS_KERNEL_STACK_SIZE;
    p->kernel_stack      = kmalloc(p->kernel_stack_size);
    if (!p->kernel_stack) {
        kprintf("process: cannot allocate a kernel stack\n");
        vmm_destroy_address_space(p->pml4);
        kfree(p);
        return NULL;
    }

    if (!build_address_space(p, image, size)) {
        kprintf("process: cannot build the address space (out of memory?)\n");
        vmm_destroy_address_space(p->pml4);
        kfree(p->kernel_stack);
        kfree(p);
        return NULL;
    }

    p->entry      = PROCESS_CODE_BASE;
    p->stack_top  = PROCESS_STACK_TOP;
    p->image_size = size;

    return p;
}

void process_destroy(struct process *p)
{
    if (!p)
        return;

    /* The image starts at a fixed base and the stack sits directly below
     * the top. Both are freed by range, which is exact rather than
     * approximate: this is the same layout build_address_space created,
     * and the image size is recorded at load time for precisely this. */
    free_leaf_frames(p->pml4, PROCESS_CODE_BASE,
                     PROCESS_CODE_BASE + p->image_size);
    free_leaf_frames(p->pml4, PROCESS_STACK_TOP - PROCESS_STACK_SIZE,
                     PROCESS_STACK_TOP);

    vmm_destroy_address_space(p->pml4);

    if (p->kernel_stack)
        kfree(p->kernel_stack);

    kfree(p);
}

/* ------------------------------------------------------------------ */
/* Running                                                             */
/* ------------------------------------------------------------------ */

int process_run(struct process *p, uint64_t arg)
{
    if (!p)
        return -1;

    g_current = p;

    /*
     * setjmp's return value is used only to tell the first call from a
     * resumed one, never to carry the exit code.
     *
     * The exit code can legitimately be zero, and so can setjmp's first
     * return, so passing one through the other makes "the process exited
     * successfully" indistinguishable from "the process has not started
     * yet" -- and the caller re-enters Ring 3 instead of returning. The
     * code travels in the structure instead, and the longjmp value is a
     * constant whose only job is to be non-zero.
     */
    if (kernel_setjmp(&p->resume) == 0) {
        /*
         * Land interrupts from Ring 3 on this process's kernel stack.
         * Until this point the CPU has been using whatever stack the
         * kernel was on, which a user program is entitled to clobber.
         */
        tss_set_kernel_stack((uint64_t)p->kernel_stack + p->kernel_stack_size);

        vmm_switch_to(p->pml4);

        /* Leaves through iretq; the next code to run is the program's. */
        usermode_enter(p->entry, p->stack_top, arg);
    }

    /* Resumed here by process_exit_hook, which has already switched back
     * to the kernel's address space. */
    g_current = NULL;
    return p->exit_code;
}

void process_request_exit(int code)
{
    if (!g_current)
        return;

    g_current->exited    = true;
    g_current->exit_code = code;
}

void process_abort_on_fault(uint64_t vector)
{
    if (!g_current)
        panic("CPU exception %llu with no process running: this is the "
              "kernel's own fault, not a program's",
              (unsigned long long)vector);

    /*
     * A fault in Ring 3 is a program's problem and stays one.
     *
     * This is the promise DESIGN.md makes when it says FunnyOS departs
     * from DOS on exactly one point: a program that goes wrong stops
     * being a program, not a machine. Turning a wild pointer into a
     * reboot is what DOS did, and it is the thing being fixed here.
     *
     * Unlike the exit path this does not need the interrupt post-hook:
     * an exception is not a device interrupt, so there is nothing to
     * acknowledge, and the fault handler is already on the way out.
     */
    vmm_switch_to(g_kernel_pml4);

    /* The code is in the structure; the longjmp value only has to be
     * non-zero. See process_run. */
    g_current->exit_code = (int)(PROCESS_EXIT_FAULT_BASE + vector);
    kernel_longjmp(&g_current->resume, 1);
}

static void process_exit_hook(void)
{
    if (!g_current || !g_current->exited)
        return;

    /*
     * Clear the flag before unwinding, not after.
     *
     * Control returns to the kernel with interrupts enabled, and the
     * address space and the current process are not torn down until
     * process_run gets around to it. A timer tick arriving in that window
     * would find g_current still set and the exit flag still raised, and
     * unwind a second time from a context that has already been resumed.
     * Clearing it here closes the window rather than relying on the
     * second unwind landing somewhere harmless.
     */
    g_current->exited = false;

    vmm_switch_to(g_kernel_pml4);
    kernel_longjmp(&g_current->resume, 1);
}
