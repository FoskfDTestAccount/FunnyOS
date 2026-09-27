/*
 * The legacy 8259 programmable interrupt controllers.
 *
 * Nothing here is a driver. The PICs are disabled at boot and never
 * touched again -- but they have to be *deliberately* masked, because an
 * unmasked 8259 will still raise IRQ7 (spurious) through the IO APIC's
 * ExtINT routing and, on some firmware, keep the timer and keyboard
 * asserted in parallel with the IO APIC.
 *
 * There is no code path here to unmask anything, and that is the point:
 * on a machine with an IO APIC, the PIC is a second interrupt controller
 * that nothing is listening to.
 */
#ifndef FUNNYOS_ARCH_X86_64_PIC_H
#define FUNNYOS_ARCH_X86_64_PIC_H

#include <funnyos/arch/x86_64/io.h>

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

/* ICW1: begin initialisation, expect ICW4. */
#define PIC_ICW1_INIT 0x11
/* ICW4: 8086/88 mode. */
#define PIC_ICW4_8086 0x01
/* OCW1: interrupt mask register. */
#define PIC_OCW1      0x21

/* Mask every line on both controllers. */
static inline void pic_mask_all(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

/* Full re-initialisation, remapping the vectors and masking everything.
 *
 * The remap is not for our benefit -- with the IO APIC in use no PIC
 * interrupt is ever delivered -- but because it is the only way to move
 * the 8259s out of the way cleanly: a spurious IRQ7 arriving with the
 * default vector 0x0F would land in the middle of the CPU exception
 * range and be reported as a bogus fault. */
static inline void pic_remap_and_mask(void)
{
    outb(PIC1_COMMAND, PIC_ICW1_INIT);  io_wait();
    outb(PIC2_COMMAND, PIC_ICW1_INIT);  io_wait();

    outb(PIC1_DATA, 0x20);              io_wait();  /* master vectors 0x20.. */
    outb(PIC2_DATA, 0x28);              io_wait();  /* slave  vectors 0x28.. */

    outb(PIC1_DATA, 0x04);              io_wait();  /* slave on IRQ2 */
    outb(PIC2_DATA, 0x02);              io_wait();  /* cascade identity */

    outb(PIC1_DATA, PIC_ICW4_8086);     io_wait();
    outb(PIC2_DATA, PIC_ICW4_8086);     io_wait();

    pic_mask_all();
}

#endif /* FUNNYOS_ARCH_X86_64_PIC_H */
