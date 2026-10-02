#ifndef FUNNYOS_PS2_H
#define FUNNYOS_PS2_H
#include <stdbool.h>
#include <stdint.h>
typedef void (*ps2_sink)(uint8_t byte);
void ps2_set_sink(bool aux, ps2_sink sink);
void ps2_dispatch(uint8_t status, uint8_t byte);
void ps2_drain(void);
bool ps2_command(uint8_t command);
bool ps2_write(uint8_t byte);
bool ps2_read(uint8_t *byte);
bool ps2_mouse_command(uint8_t command);
#endif
