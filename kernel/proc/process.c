#include <funnyos/process.h>

#include <funnyos/arch/x86_64/fpu.h>
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

bool process_kernel_stack_contains(uint64_t sp)
{
    if (!g_current || !g_current->kernel_stack)
        return false;

    uint64_t low = (uint64_t)g_current->kernel_stack;
    return sp >= low && sp < low + g_current->kernel_stack_size;
}

/*
 * Copy a flat image into a freshly mapped run of pages, zero the rest of
 * the region the caller asked for, and map the stack above it.
 *
 * Returns false with nothing leaked if any step fails.
 */
static bool build_address_space(struct process *p, const void *image,
                                size_t image_size, size_t memory_size)
{
    /*
     * Both bounds are named before anything can jump to the failure path
     * below. Declaring the stack's base down beside its loop would read
     * better, but a `goto fail` from the first loop would then arrive
     * with it holding whatever was on the stack -- and the unwind would
     * walk a range built from that, over pages that had already been
     * freed, which is a double free rather than a leak.
     */
    uint64_t memory_end = PROCESS_CODE_BASE + memory_size;
    uint64_t stack_base = PROCESS_STACK_TOP - PROCESS_STACK_SIZE;

    /* --- The program, and the tail behind it --- */
    for (uint64_t virt = PROCESS_CODE_BASE; virt < memory_end; virt += PAGE_SIZE) {
        uint64_t frame = pmm_alloc_page();
        if (!frame)
            goto fail;

        if (!vmm_map_page_in(p->pml4, virt, frame, VMM_USER_RW)) {
            pmm_free_page(frame);
            goto fail;
        }

        /*
         * Zero the whole page, then copy over the front of it. That
         * order is what makes the tail work: a page past the end of the
         * image keeps its zeroes and is never written again.
         *
         * It is a promise rather than tidiness. The one place FunnyOS
         * departs from DOS is that a program cannot see what the last
         * one left behind, and a page that skipped this would be exactly
         * that leak -- in the one routine that handles a whole
         * program's worth of memory, and silently.
         */
        uint8_t *dst = frame_ptr(frame);
        memset(dst, 0, PAGE_SIZE);

        size_t offset = (size_t)(virt - PROCESS_CODE_BASE);
        if (offset < image_size) {
            size_t chunk = image_size - offset;
            if (chunk > PAGE_SIZE)
                chunk = PAGE_SIZE;

            memcpy(dst, (const uint8_t *)image + offset, chunk);
        }
    }

    /* --- The stack --- */
    for (uint64_t virt = stack_base; virt < PROCESS_STACK_TOP; virt += PAGE_SIZE) {
        uint64_t frame = pmm_alloc_page();
        if (!frame)
            goto fail;

        if (!vmm_map_page_in(p->pml4, virt, frame, VMM_USER_RW)) {
            pmm_free_page(frame);
            goto fail;
        }

        /* Zeroed, so a program that reads an uninitialised local gets
         * zeroes rather than whatever the last process left there. */
        memset(frame_ptr(frame), 0, PAGE_SIZE);
    }

    return true;

fail:
    /*
     * Hand back what this built before it ran out.
     *
     * The frames are the caller's to free -- vmm_destroy_address_space
     * releases the page tables and deliberately nothing else -- so a
     * region abandoned half mapped would take its pages out of the
     * allocator for good. Nothing else would notice: the address space
     * still tears down cleanly, and the machine simply has less memory
     * than it should for the rest of its uptime.
     *
     * A range rather than a record of how far the loop got, because the
     * mappings are already that record: free_leaf_frames skips whatever
     * is not there.
     */
    free_leaf_frames(p->pml4, PROCESS_CODE_BASE, memory_end);
    free_leaf_frames(p->pml4, stack_base, PROCESS_STACK_TOP);

    return false;
}

struct process *process_create(const char *name, const void *image,
                               size_t image_size, size_t memory_size)
{
    if (!image || image_size == 0)
        return NULL;

    /*
     * The two layouts that cannot exist, refused before anything has been
     * allocated -- and refused as argument errors, not as a shortage of
     * memory, because that is what they are.
     *
     * The second is compared as a distance rather than by adding to
     * PROCESS_CODE_BASE, so that a size near SIZE_MAX is refused instead
     * of wrapping round to something that would look small enough. See
     * the note on PROCESS_STACK_TOP for why a region reaching the stack
     * is not a failure the loader would otherwise get to notice.
     */
    if (memory_size < image_size) {
        kprintf("process: %llu bytes of memory is less than the %llu-byte "
                "image going into it\n",
                (unsigned long long)memory_size,
                (unsigned long long)image_size);
        return NULL;
    }

    if (memory_size > (PROCESS_STACK_TOP - PROCESS_STACK_SIZE) - PROCESS_CODE_BASE) {
        kprintf("process: %llu bytes of memory would reach the stack at %p\n",
                (unsigned long long)memory_size,
                (void *)(PROCESS_STACK_TOP - PROCESS_STACK_SIZE));
        return NULL;
    }

    struct process *p = kmalloc(sizeof(*p));
    if (!p)
        return NULL;
    memset(p, 0, sizeof(*p));

    /*
     * The name is copied, not borrowed.
     *
     * Every caller today passes a string literal, so storing the pointer
     * would work and go on working -- until a caller passes a buffer that
     * is gone by the time anybody reads the name back. A structure that
     * keeps the bytes cannot have that argument with itself.
     */
    if (name) {
        size_t length = strlen(name);
        if (length >= sizeof p->name)
            length = sizeof p->name - 1;
        memcpy(p->name, name, length);
        p->name[length] = '\0';
    }

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

    /*
     * Floating point state. The heap hands back 16-byte aligned blocks --
     * its header is 32 bytes and its base is page aligned -- which is
     * exactly what FXSAVE requires, but that is a property of another
     * file, so it is checked rather than assumed.
     */
    p->fpu_state = kmalloc(FPU_STATE_SIZE);
    if (!p->fpu_state) {
        kprintf("process: cannot allocate floating point state\n");
        vmm_destroy_address_space(p->pml4);
        kfree(p->kernel_stack);
        kfree(p);
        return NULL;
    }

    if (((uintptr_t)p->fpu_state & (FPU_STATE_ALIGN - 1)) != 0) {
        kprintf("process: floating point state is not %u-byte aligned\n",
                (unsigned)FPU_STATE_ALIGN);
        vmm_destroy_address_space(p->pml4);
        kfree(p->fpu_state);
        kfree(p->kernel_stack);
        kfree(p);
        return NULL;
    }

    /* A program starts with the architectural default: x87 reset, all
     * exceptions masked, round to nearest. Restoring this before the first
     * instruction is what makes its first floating point result depend on
     * its code and not on whatever ran before it. */
    fpu_init_state(p->fpu_state);

    if (!build_address_space(p, image, image_size, memory_size)) {
        kprintf("process: cannot build the address space (out of memory?)\n");
        vmm_destroy_address_space(p->pml4);
        kfree(p->fpu_state);
        kfree(p->kernel_stack);
        kfree(p);
        return NULL;
    }

    p->entry       = PROCESS_CODE_BASE;
    p->stack_top   = PROCESS_STACK_TOP;
    p->memory_size = memory_size;

    return p;
}

void process_destroy(struct process *p)
{
    if (!p)
        return;

    /* The program's memory starts at a fixed base and runs for as long as
     * the process was given -- which is not the length of its image, and
     * the difference is a whole tail of frames. Freeing by the image
     * length instead would leave those frames mapped to nothing and out
     * of the allocator, and only for a process that was given a tail,
     * which is to say only the one the emulator needs. The stack sits
     * directly below its top. Both are freed by range, which is exact
     * rather than approximate: this is the same layout
     * build_address_space created. */
    free_leaf_frames(p->pml4, PROCESS_CODE_BASE,
                     PROCESS_CODE_BASE + p->memory_size);
    free_leaf_frames(p->pml4, PROCESS_STACK_TOP - PROCESS_STACK_SIZE,
                     PROCESS_STACK_TOP);

    vmm_destroy_address_space(p->pml4);

    if (p->fpu_state)
        kfree(p->fpu_state);

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

    /*
     * Everything this function is about to overwrite, saved before it is
     * overwritten.
     *
     * With one program in the machine, "the current process" could be set
     * on entry and cleared on the way out and nothing could tell the
     * difference. That stops being true the moment a program can start
     * another one: the child runs with g_current pointing at itself, and
     * when it ends the parent is still there -- clearing the pointer
     * announces that it is not. Four pieces of machine state have the same
     * problem, and they are saved and restored as a set because they
     * describe the same machine:
     *
     *   g_current            who is running
     *   the current PML4     what it can see
     *   the TSS rsp0         where its next Ring 3 interrupt will land
     *   the vector registers what its arithmetic is in the middle of
     *
     * The third is the one that bites hardest. rsp0 left pointing at a
     * child's kernel stack after the child has been destroyed means the
     * parent's next interrupt pushes its frame onto freed memory -- which
     * is silent until the allocator hands that memory to somebody else.
     *
     * The fourth is not a context switch yet and is not treated as one:
     * the kernel touches no vector register (see fpu.h), so saving the
     * caller's state here captures exactly what the caller had when it
     * called in, with nothing of the kernel's mixed in.
     */
    struct process *previous       = g_current;
    uint64_t        previous_pml4  = process_current_pml4();
    uint64_t        previous_stack = tss_get_kernel_stack();

    if (previous)
        fpu_save(previous->fpu_state);

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

        /*
         * Hand the program its floating point state, and take it back
         * afterwards.
         *
         * This is the seed of a context switch. With one process at a time
         * the two calls are a round trip through the same memory and
         * change nothing; with two, they are the difference between the
         * programs keeping their own arithmetic and sharing one set of
         * registers by accident.
         *
         * Nothing here needs to happen around interrupts: the kernel is
         * compiled without vector registers, so a handler cannot disturb
         * them in the first place. See fpu.h.
         */
        fpu_restore(p->fpu_state);

        /* Leaves through iretq; the next code to run is the program's. */
        usermode_enter(p->entry, p->stack_top, arg);
    }

    /* The process has stopped, however it stopped. Whatever it left in the
     * floating point registers is its own until the next time it runs. */
    fpu_save(p->fpu_state);

    /*
     * Put the machine back the way the caller left it. See the save above
     * for why each of these is here. The order matters once: the address
     * space has to be the caller's before the caller resumes in it.
     *
     * Control arrives here from process_exit_hook, which has already
     * switched to the kernel's address space -- so the switch below is
     * always from the kernel's tables to the caller's, never out of a
     * child's that is about to be destroyed.
     */
    if (previous)
        fpu_restore(previous->fpu_state);

    vmm_switch_to(previous_pml4);
    tss_set_kernel_stack(previous_stack);
    g_current = previous;

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
