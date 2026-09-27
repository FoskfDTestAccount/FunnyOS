#include <funnyos/arch/x86_64/irq.h>
#include <funnyos/arch/x86_64/apic.h>
#include <funnyos/kprintf.h>

#include <stddef.h>

struct irq_slot {
    irq_handler_fn fn;
    void          *ctx;
};

static struct irq_slot g_handlers[256];
static uint64_t       g_counts[256];
static uint64_t       g_total;
static irq_post_hook_fn g_post_hook;

void irq_init(void)
{
    for (int i = 0; i < 256; i++) {
        g_handlers[i].fn  = NULL;
        g_handlers[i].ctx = NULL;
        g_counts[i]       = 0;
    }
    g_total     = 0;
    g_post_hook = NULL;
}

void irq_set_post_hook(irq_post_hook_fn hook)
{
    g_post_hook = hook;
}

bool irq_register(uint8_t vector, irq_handler_fn fn, void *ctx)
{
    if (vector < IRQ_VECTOR_BASE || !fn)
        return false;
    if (g_handlers[vector].fn)
        return false;   /* already claimed */

    g_handlers[vector].fn  = fn;
    g_handlers[vector].ctx = ctx;
    return true;
}

bool irq_unregister(uint8_t vector)
{
    if (vector < IRQ_VECTOR_BASE || !g_handlers[vector].fn)
        return false;

    g_handlers[vector].fn  = NULL;
    g_handlers[vector].ctx = NULL;
    return true;
}

bool irq_dispatch(struct interrupt_frame *frame)
{
    uint8_t vector = (uint8_t)frame->vector;

    if (vector < IRQ_VECTOR_BASE)
        return false;   /* CPU exception; isr.c owns these */

    g_counts[vector]++;
    g_total++;

    struct irq_slot slot = g_handlers[vector];

    if (!slot.fn) {
        /*
         * Nobody claimed this vector. That used to be the normal state --
         * the bootloader starts with every LVT entry masked, so an
         * unclaimed vector firing means a device was routed to the local
         * APIC and then forgotten about.
         *
         * Report it and carry on rather than panicking: a spurious or
         * unexpected device interrupt is a bug, but not one worth taking
         * the machine down for. Repeat offenders are visible because the
         * message names the vector and the count keeps climbing.
         */
        kprintf("irq: unhandled interrupt on vector %u (count %llu)\n",
                (unsigned)vector, (unsigned long long)g_counts[vector]);

        lapic_eoi();
        return true;
    }

    slot.fn(frame, slot.ctx);

    lapic_eoi();

    /* The handler may have asked to leave this context entirely. */
    if (g_post_hook)
        g_post_hook();

    return true;
}

uint64_t irq_count(uint8_t vector)
{
    return g_counts[vector];
}

uint64_t irq_total_count(void)
{
    return g_total;
}
