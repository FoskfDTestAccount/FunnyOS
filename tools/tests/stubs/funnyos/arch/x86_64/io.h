#ifndef DESKTOP_TEST_IO_H
#define DESKTOP_TEST_IO_H
#include <stdbool.h>
#include <stdint.h>
extern bool test_interrupts;
static inline void outb(uint16_t p,uint8_t b) { (void)p;(void)b; }
static inline uint8_t inb(uint16_t p) { (void)p;return 0; }
static inline void cpu_halt(void) {}
static inline void interrupts_enable(void) { test_interrupts=true; }
static inline void interrupts_disable(void) { test_interrupts=false; }
static inline bool interrupts_enabled(void) { return test_interrupts; }
#endif
