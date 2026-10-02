/* Both ISA IRQs drain ONE output buffer. Never discard the other port. */
#include <funnyos/ps2.h>
#include <funnyos/arch/x86_64/io.h>
static ps2_sink g_sinks[2];
void ps2_set_sink(bool aux, ps2_sink sink) { g_sinks[aux ? 1 : 0] = sink; }
void ps2_dispatch(uint8_t status, uint8_t byte)
{
    if (!(status & 1u) || (status & 0xc0u)) return; /* parity/timeout */
    ps2_sink sink = g_sinks[(status & 0x20u) ? 1 : 0];
    if (sink) sink(byte);
}
void ps2_drain(void)
{
    for (unsigned n = 0; n < 1024; n++) {
        uint8_t status = inb(0x64);
        if (!(status & 1u)) break;
        ps2_dispatch(status, inb(0x60));
    }
}
static bool ready(bool output)
{
    for (unsigned n = 0; n < 100000; n++) {
        uint8_t s = inb(0x64);
        if (output ? (s & 1u) : !(s & 2u)) return true;
    }
    return false;
}
bool ps2_command(uint8_t c) { if (!ready(false)) return false; outb(0x64,c); return true; }
bool ps2_write(uint8_t c) { if (!ready(false)) return false; outb(0x60,c); return true; }
bool ps2_read(uint8_t *b) { if (!ready(true)) return false; *b=inb(0x60); return true; }
bool ps2_mouse_command(uint8_t command)
{
    for (unsigned retry=0; retry<3; retry++) {
        if (!ps2_command(0xd4) || !ps2_write(command)) return false;
        for (unsigned n=0; n<100000; n++) {
            uint8_t s=inb(0x64);
            if (!(s&1u)) continue;
            uint8_t b=inb(0x60);
            if (!(s&0x20u)) { ps2_dispatch(s,b); continue; }
            if (s&0xc0u) continue;
            if (b==0xfa) return true;
            if (b==0xfe) break;
            /* Non-ACK AUX bytes cannot be mistaken for keyboard input. */
        }
    }
    return false;
}
