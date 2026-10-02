#include <funnyos/mouse.h>
#include <funnyos/ps2.h>
#include <funnyos/arch/x86_64/io.h>
#include <funnyos/arch/x86_64/irq.h>
#include <funnyos/arch/x86_64/apic.h>
#define MOUSE_QUEUE 64u
static struct mouse_decoder g_decoder;
static struct mouse_event g_events[MOUSE_QUEUE];
static volatile unsigned g_head,g_tail;
static uint64_t g_dropped;
static bool g_ready;
static void receive(uint8_t byte)
{
    struct mouse_event e;
    if (!mouse_decode(&g_decoder,byte,&e)) return;
    unsigned next=(g_head+1u)%MOUSE_QUEUE;
    if (next==g_tail) {
        /* Resynchronize button state after overflow; don't keep a stuck drag. */
        g_dropped++; g_tail=g_head; e.dx=e.dy=0;
    }
    g_events[g_head]=e; g_head=next;
}
static void mouse_irq(struct interrupt_frame *frame,void *ctx)
{ (void)frame; (void)ctx; ps2_drain(); }
bool mouse_init(void)
{
    if (!lapic_ready() || !ioapic_ready()) return false;
    uint8_t config=0; bool got_config=false, registered=false;
    if (!ps2_command(0xad)) return false;
    ps2_drain();
    if (!ps2_command(0x20) || !ps2_read(&config)) goto done;
    got_config=true;
    if (!ps2_command(0x60) || !ps2_write((config|2u)&~0x20u) ||
        !ps2_command(0xa8)) goto done;
    if (!ps2_mouse_command(0xf6) || !ps2_mouse_command(0xf4)) goto done;
    registered=irq_register(IRQ_VECTOR_BASE+12u,mouse_irq,0);
    if (!registered) goto done;
    ps2_set_sink(true,receive);
    ioapic_route_isa(12u,IRQ_VECTOR_BASE+12u);
    g_ready=true;
done:
    if (!g_ready && got_config) {
        (void)ps2_command(0xa7);
        (void)ps2_command(0x60); (void)ps2_write(config);
    }
    if (!ps2_command(0xae)) g_ready=false;
    if (!g_ready && registered) (void)irq_unregister(IRQ_VECTOR_BASE+12u);
    return g_ready;
}
bool mouse_ready(void) { return g_ready; }
uint64_t mouse_dropped(void) { return g_dropped; }
bool mouse_poll(struct mouse_event *out)
{
    bool enabled=interrupts_enabled(); interrupts_disable();
    bool available=g_tail!=g_head;
    if (available) { *out=g_events[g_tail]; g_tail=(g_tail+1u)%MOUSE_QUEUE; }
    if (enabled) interrupts_enable();
    return available;
}
