#include <funnyos/mouse.h>
bool mouse_decode(struct mouse_decoder *d,uint8_t byte,struct mouse_event *out)
{
    if (!d->at && !(byte&8u)) return false;
    d->bytes[d->at++]=byte;
    if (d->at<3) return false;
    d->at=0;
    uint8_t h=d->bytes[0];
    /* Header sign bits, NOT an int8_t cast: the protocol carries 9 bits.
     * Overflow invalidates displacement but buttons still have meaning. */
    out->dx=(h&0xc0u) ? 0 : (int16_t)d->bytes[1]-((h&0x10u)?256:0);
    out->dy=(h&0xc0u) ? 0 : -((int16_t)d->bytes[2]-((h&0x20u)?256:0));
    out->buttons=h&7u;
    return true;
}
