#include <funnyos/arch/x86_64/fpu.h>
#include <funnyos/arch/x86_64/cpu.h>
#include <funnyos/kprintf.h>

/* CR0, Intel SDM Vol 3A, 2.5. */
#define CR0_MP (1ULL << 1)   /* monitor coprocessor */
#define CR0_EM (1ULL << 2)   /* emulation */
#define CR0_TS (1ULL << 3)   /* task switched */
#define CR0_NE (1ULL << 5)   /* numeric error */

/* CR4. */
#define CR4_OSFXSR     (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)
#define CR4_OSXSAVE    (1ULL << 18)

/* CPUID leaf 1, ECX. */
#define CPUID_ECX_AVX (1u << 28)

static bool g_ready;
static bool g_avx;

bool fpu_init(void)
{
    struct cpu_cpuid_result id = cpu_cpuid(1, 0);
    g_avx = (id.ecx & CPUID_ECX_AVX) != 0;

    uint64_t cr0 = cpu_read_cr0();
    uint64_t cr4 = cpu_read_cr4();

    /*
     * CR0.EM = 0. While set, every x87 instruction raises #NM so that
     * software can emulate a coprocessor. There is a real one here, and
     * leaving this bit set means the FPU is unreachable for no reason.
     */
    cr0 &= ~CR0_EM;

    /*
     * CR0.TS = 0. This is the lazy-switching bit: set, it makes the first
     * floating point instruction after a task switch trap, so the OS can
     * defer the state save until something actually uses it. This kernel
     * switches eagerly and has no scheduler yet, so leaving it set would
     * produce a #NM on the first instruction of every program -- and
     * clearing it is the honest statement that there is no lazy path here
     * to get wrong.
     */
    cr0 &= ~CR0_TS;

    cr0 |= CR0_MP;   /* paired with TS; harmless when TS is clear */
    cr0 |= CR0_NE;   /* x87 errors arrive as #MF, not through the PIC */

    cpu_write_cr0(cr0);

    /*
     * CR4.OSFXSR. Without it, SSE instructions raise #UD. The processor is
     * entitled to make that assumption: an OS which does not set this has
     * said it cannot preserve XMM state, and running SSE anyway would
     * corrupt it silently. Setting the bit is the promise that the save
     * and restore in this file actually happen.
     */
    cr4 |= CR4_OSFXSR;

    /* CR4.OSXMMEXCPT: unmasked SSE exceptions are delivered as #XM rather
     * than the catch-all #UD. */
    cr4 |= CR4_OSXMMEXCPT;

    /*
     * CR4.OSXSAVE is deliberately left clear, even though most processors
     * that have AVX would set it happily.
     *
     * Setting it enables the XSAVE family, and with it YMM registers --
     * which FXSAVE does not save. This kernel's state image is 512 bytes
     * of FXSAVE, so AVX state would be exactly the thing the flag exists
     * to protect and would not be protected. Leaving the bit clear means
     * AVX instructions raise #UD instead, which is a loud failure rather
     * than a quiet one.
     *
     * Nothing here is compiled to use AVX, so this costs nothing today. It
     * becomes a real decision the day some piece of this system wants it.
     */
    cr4 &= ~CR4_OSXSAVE;

    cpu_write_cr4(cr4);

    /*
     * Read back rather than assume. A control register write that did not
     * take is the kind of thing a hypervisor can do, and finding out here
     * is much cheaper than finding out as a mystery fault in a program
     * that happens to use a double.
     */
    if (cpu_read_cr0() & (CR0_EM | CR0_TS)) {
        kprintf("fpu: CR0 still reports EM or TS after writing it\n");
        return false;
    }

    if ((cpu_read_cr4() & (CR4_OSFXSR | CR4_OSXMMEXCPT)) !=
        (CR4_OSFXSR | CR4_OSXMMEXCPT)) {
        kprintf("fpu: CR4 did not take OSFXSR and OSXMMEXCPT\n");
        return false;
    }

    if (cpu_read_cr4() & CR4_OSXSAVE) {
        kprintf("fpu: CR4.OSXSAVE is set and would not clear; AVX state "
                "would not be saved\n");
        return false;
    }

    g_ready = true;
    return true;
}

bool fpu_ready(void)      { return g_ready; }
bool fpu_avx_present(void) { return g_avx; }
