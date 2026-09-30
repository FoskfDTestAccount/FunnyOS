/*
 * CPU exception handling.
 *
 * M1 exists to make faults legible. Before this, any fault became a triple
 * fault and a silent reboot; the goal here is that a fault instead names
 * itself, points at the instruction that caused it, and dumps enough state
 * to work out why.
 *
 * Device interrupts arrive through the same entry stub and pass through
 * here on their way to their registered handler. They are tried first:
 * once drivers exist, a claimed vector is the common case, while every
 * path below this point is terminal.
 */
#include <funnyos/arch/x86_64/cpu.h>
#include <funnyos/arch/x86_64/irq.h>
#include <funnyos/kprintf.h>
#include <funnyos/panic.h>
#include <funnyos/process.h>

#include <stdint.h>
#include <stddef.h>

/* --- Names -------------------------------------------------------- */

static const char *exception_name(uint64_t vector)
{
    static const char *const names[32] = {
        "#DE Divide Error",
        "#DB Debug",
        "NMI Interrupt",
        "#BP Breakpoint",
        "#OF Overflow",
        "#BR BOUND Range Exceeded",
        "#UD Invalid Opcode",
        "#NM Device Not Available",
        "#DF Double Fault",
        "Coprocessor Segment Overrun (reserved)",
        "#TS Invalid TSS",
        "#NP Segment Not Present",
        "#SS Stack-Segment Fault",
        "#GP General Protection Fault",
        "#PF Page Fault",
        "Reserved",
        "#MF x87 Floating-Point Exception",
        "#AC Alignment Check",
        "#MC Machine Check",
        "#XM SIMD Floating-Point Exception",
        "#VE Virtualization Exception",
        "#CP Control Protection Exception",
        "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
        "#HV Hypervisor Injection Exception",
        "#VC VMM Communication Exception",
        "#SX Security Exception",
        "Reserved",
    };

    if (vector < 32)
        return names[vector];
    return "(unknown)";
}

/* --- Decoders ----------------------------------------------------- */

/*
 * Page fault error code bits, Intel SDM Vol 3A, 4.7.
 *     bit 0  P   : 0 = page not present, 1 = protection violation
 *     bit 1  W/R : 0 = read, 1 = write
 *     bit 2  U/S : 0 = supervisor, 1 = user
 *     bit 3  RSVD: reserved bit set in a paging structure
 *     bit 4  I/D : instruction fetch
 */
static void print_page_fault_cause(uint64_t ec)
{
    kprintf("               %s, %s, %s mode",
            (ec & 0x01) ? "protection violation" : "page not present",
            (ec & 0x02) ? "write" : "read",
            (ec & 0x04) ? "user" : "kernel");

    if (ec & 0x08)
        kprintf(", reserved bit set in paging structure");
    if (ec & 0x10)
        kprintf(", instruction fetch");
    kprintf("\n");
}

static void print_rflags(uint64_t f)
{
    kprintf("  RFLAGS     : 0x%016llx  [", (unsigned long long)f);

    /* Only the flags a reader would actually reason about. */
    if (f & (1u << 0))  kprintf(" CF");
    if (f & (1u << 2))  kprintf(" PF");
    if (f & (1u << 4))  kprintf(" AF");
    if (f & (1u << 6))  kprintf(" ZF");
    if (f & (1u << 7))  kprintf(" SF");
    if (f & (1u << 8))  kprintf(" TF");
    if (f & (1u << 9))  kprintf(" IF");
    if (f & (1u << 10)) kprintf(" DF");
    if (f & (1u << 11)) kprintf(" OF");
    if (f & (1u << 14)) kprintf(" NT");
    if (f & (1u << 16)) kprintf(" RF");
    if (f & (1u << 18)) kprintf(" AC");
    if (f & (1u << 21)) kprintf(" ID");

    kprintf(" ]\n");
}

static void print_segment(const char *label, uint64_t selector)
{
    kprintf("  %s         : 0x%04llx  (index %llu, RPL %llu)\n", label,
            (unsigned long long)selector,
            (unsigned long long)(selector >> 3),
            (unsigned long long)(selector & 3));
}

/* --- Entry point -------------------------------------------------- */

void isr_dispatch(struct interrupt_frame *f);

void isr_dispatch(struct interrupt_frame *f)
{
    /* A registered device interrupt completes here and the interrupted
     * code resumes. Everything past this point is fatal to something. */
    if (irq_dispatch(f))
        return;

    /*
     * A fault raised by Ring 3 code belongs to that program.
     *
     * This is the whole reason programs run in Ring 3, and it is the one
     * place FunnyOS deliberately departs from DOS: a wild pointer ends
     * the program that made it, and nothing else. The kernel reports
     * what happened and unwinds -- it does not dump its own registers
     * and it does not halt.
     *
     * The check is on the saved code segment's privilege level, which is
     * the architectural answer to "where was this raised from" and the
     * only one that cannot be spoofed by the program.
     */
    if ((f->cs & 3) == 3) {
        kprintf("\n*** FunnyOS: program fault ***\n");
        kprintf("  Process     : faulted in Ring 3\n");
        kprintf("  Vector      : %llu  %s\n",
                (unsigned long long)f->vector, exception_name(f->vector));
        kprintf("  Error code  : 0x%016llx\n",
                (unsigned long long)f->error_code);
        if (f->vector == 14) {
            kprintf("  CR2         : 0x%016llx  (faulting address)\n",
                    (unsigned long long)cpu_read_cr2());
        }
        kprintf("  RIP         : 0x%016llx\n", (unsigned long long)f->rip);
        kprintf("  RSP         : 0x%016llx\n", (unsigned long long)f->rsp);
        kprintf("  The kernel is unaffected; ending the process.\n");

        /* Never returns. */
        process_abort_on_fault(f->vector);
    }

    kprintf("\n");
    kprintf("==================== CPU EXCEPTION ====================\n");
    kprintf("  Vector     : %llu  %s\n",
            (unsigned long long)f->vector, exception_name(f->vector));

    if (f->vector == 8) {
        kprintf("               This is a double fault: a fault occurred while\n");
        kprintf("               handling a fault. The usual cause is a broken or\n");
        kprintf("               exhausted stack. This handler runs on its own IST\n");
        kprintf("               stack, which is why it got to run at all.\n");
    }

    kprintf("  Error code : 0x%016llx\n", (unsigned long long)f->error_code);
    if (f->vector == 14)
        print_page_fault_cause(f->error_code);
    else if (f->vector == 13 && f->error_code != 0)
        kprintf("               selector/index 0x%llx\n",
                (unsigned long long)(f->error_code & 0xFFF8));

    if (f->vector == 14)
        kprintf("  CR2        : 0x%016llx  (faulting address)\n",
                (unsigned long long)cpu_read_cr2());

    kprintf("\n");
    kprintf("  RIP        : 0x%016llx\n", (unsigned long long)f->rip);
    print_segment("CS", f->cs);
    print_segment("SS", f->ss);
    print_rflags(f->rflags);
    kprintf("  RSP        : 0x%016llx\n", (unsigned long long)f->rsp);

    kprintf("\n");
    kprintf("  CR0        : 0x%016llx\n", (unsigned long long)cpu_read_cr0());
    kprintf("  CR3        : 0x%016llx\n", (unsigned long long)cpu_read_cr3());
    kprintf("  CR4        : 0x%016llx\n", (unsigned long long)cpu_read_cr4());

    kprintf("\n");
    kprintf("  RAX %016llx  RBX %016llx\n",
            (unsigned long long)f->rax, (unsigned long long)f->rbx);
    kprintf("  RCX %016llx  RDX %016llx\n",
            (unsigned long long)f->rcx, (unsigned long long)f->rdx);
    kprintf("  RSI %016llx  RDI %016llx\n",
            (unsigned long long)f->rsi, (unsigned long long)f->rdi);
    kprintf("  RBP %016llx  RSP %016llx\n",
            (unsigned long long)f->rbp, (unsigned long long)f->rsp);
    kprintf("  R8  %016llx  R9  %016llx\n",
            (unsigned long long)f->r8, (unsigned long long)f->r9);
    kprintf("  R10 %016llx  R11 %016llx\n",
            (unsigned long long)f->r10, (unsigned long long)f->r11);
    kprintf("  R12 %016llx  R13 %016llx\n",
            (unsigned long long)f->r12, (unsigned long long)f->r13);
    kprintf("  R14 %016llx  R15 %016llx\n",
            (unsigned long long)f->r14, (unsigned long long)f->r15);

    kprintf("=======================================================\n");

    panic("unhandled CPU exception %llu (%s)",
          (unsigned long long)f->vector, exception_name(f->vector));
}
