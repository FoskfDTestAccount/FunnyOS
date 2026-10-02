#ifndef FUNNYOS_TERMINAL_H
#define FUNNYOS_TERMINAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <funnyos/process.h>
#include <funnyos/kbd.h>
#include <funnyos/mouse.h>
#define TERMINAL_MAX 8u
bool terminal_enabled(void);
void terminal_enable(void);
unsigned terminal_create(void);
void terminal_destroy(unsigned id);
unsigned terminal_visible(void);
bool terminal_exists(unsigned id);
bool terminal_pending(unsigned id);
bool terminal_work_pending(void);
bool terminal_take_new(void);
unsigned terminal_take_close(void);
void terminal_feedback(const char *text);
void terminal_poll(void);
void terminal_keyboard(uint8_t byte,bool extended,int key,const struct kbd_state *state);
int terminal_key_poll(const struct process *p);
void terminal_write(const struct process *p,const char *text,size_t size);
void terminal_clear(const struct process *p);
bool terminal_focus(const struct process *p);
bool terminal_guest_acquire(const struct process *p);
bool terminal_guest_present(const struct process *p,const uint8_t *cells,unsigned cols,uint16_t cursor);
void terminal_guest_release(const struct process *p);
bool terminal_mouse_poll(const struct process *p,struct mouse_event *event);
bool terminal_raw_acquire(const struct process *p);
int terminal_raw_poll(const struct process *p);
bool terminal_raw_release(const struct process *p);
bool terminal_busy(unsigned id);
int terminal_control(const struct process *p,unsigned operation);
void terminal_panic(void);
/* Cooperative foreground sessions; no arbitrary Ring-3 CPU preemption. */
void terminal_sessions_run(struct process *first) __attribute__((noreturn));
void process_session_yield(bool waiting);
#endif
