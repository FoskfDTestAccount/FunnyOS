#ifndef FUNNYOS_MOUSE_H
#define FUNNYOS_MOUSE_H
#include <stdbool.h>
#include <stdint.h>
struct mouse_event { int16_t dx, dy; uint8_t buttons; };
struct mouse_decoder { uint8_t bytes[3], at; };
bool mouse_decode(struct mouse_decoder *d,uint8_t byte,struct mouse_event *out);
bool mouse_init(void);
bool mouse_ready(void);
bool mouse_poll(struct mouse_event *out);
uint64_t mouse_dropped(void);
#endif
