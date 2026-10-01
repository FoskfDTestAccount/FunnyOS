/*
 * The BIOS corpus replayer.
 *
 * See replay.h for what it is for, for the entry convention, and for what
 * a retry does to the stack. Two things belong here rather than there:
 * what this file assumes about modules it does not own, and how the run
 * loop decides that a program has finished.
 *
 * ---------------------------------------------------------------------
 * The assumptions, all of them in one place
 *
 * Tasks B, C and D own the headers this file includes, and this file was
 * written before two of them landed. Everything below is what their task
 * books specify, and where a task book specified less than this file
 * needs, the place is marked ASSUMPTION so it can be found rather than
 * rediscovered:
 *
 *   B  landed, and needed no assumption. `struct bios10_state` is a
 *      complete type and bios10_reset/service/active_page/cursor_cell are
 *      what the task book named.
 *
 *   D  landed, and needed one that the task book did not settle: the call
 *      that hands a byte array to the disk. It is bios13_init(disk, bytes,
 *      length), with the device a `struct bios13_disk`.
 *
 *   C  ASSUMPTION, and the only one left: bios16_reset/bios16_service/
 *      bios16_irq/bios16_key_arrived on a `struct bios16_state`, and
 *      bios1a_reset/bios1a_service/bios1a_irq/bios1a_advance on a
 *      `struct bios1a_state`. Those are the names and signatures its task
 *      book gives, and nothing beyond them is assumed.
 */
#include "replay.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if VM86_BIOS_REPLAY_HAVE_SERVICES

#include <vm86/cpu.h>
#include <vm86/display.h>
#include <vm86/dos.h>
#include <vm86/host.h>
#include <vm86/int21.h>
#include <vm86/fat.h>
#include <vm86/mem.h>

#include "../../bios/bios10.h"
#include "../../bios/bios13.h"
#include "../../bios/bios16.h"
#include "../../bios/bios1a.h"

/* ------------------------------------------------------------------ */
/* The machine                                                         */
/* ------------------------------------------------------------------ */

struct vm86_bios_machine {
    struct vm86_cpu cpu;
    struct vm86_mem mem;

    uint8_t *memory;                /* owned                       */
    uint8_t *disk;                  /* owned, VM86_BIOS_DISK_BYTES */

    /* The recording display. */
    struct vm86_bios_screen screen;
    struct vm86_display     display;

    /* One of each device, owned here and handed to the services as their
     * ctx. Nothing else may reach them. */
    struct bios10_state  video;
    struct bios16_state  keyboard;
    struct bios1a_state  clock;
    struct bios13_disk   disk_device;

    /* The DOS layer's two services: the INT 21h dispatcher, and the
     * INT 20h handler that a bare RET from a .COM arrives at. Both are
     * registered for every machine this builds -- a machine with DOS in
     * it is what this is -- and the dispatcher's default DTA is fixed up
     * by whoever loads a program, because the PSP it lives in does not
     * exist until then. */
    struct int21_state   dos21;
    struct fat_volume files;

    /* The key is delivered at most once per load; a second one would be a
     * second keystroke, which no case here is about. */
    bool key_delivered;
};

/* ------------------------------------------------------------------ */
/* The display double                                                  */
/* ------------------------------------------------------------------ */

static void record_frame(void *ctx, const uint8_t *cells, uint16_t cursor)
{
    struct vm86_bios_screen *screen = ctx;

    /* Copied, not kept: display.h says the pointer is valid only for the
     * length of the call, and a double that kept it would be a double
     * that only works because it is called at the right moment. */
    memcpy(screen->cells, cells, sizeof screen->cells);
    screen->cursor = cursor;
    screen->presents++;
}

static void present_now(struct vm86_bios_machine *machine)
{
    vm86_display_present(&machine->display,
                         bios10_active_page(&machine->cpu, &machine->video),
                         bios10_cursor_cell(&machine->cpu, &machine->video));
}

/* ------------------------------------------------------------------ */
/* Building one                                                        */
/* ------------------------------------------------------------------ */

struct vm86_bios_machine *vm86_bios_machine_new(void)
{
    struct vm86_bios_machine *machine = calloc(1, sizeof *machine);

    if (!machine)
        return NULL;

    machine->memory = calloc(1, VM86_BIOS_MEMORY);
    machine->disk   = calloc(1, VM86_BIOS_DISK_IMAGE_BYTES);

    if (!machine->memory || !machine->disk) {
        free(machine->memory);
        free(machine->disk);
        free(machine);
        return NULL;
    }

    machine->display.present = record_frame;
    machine->display.ctx     = &machine->screen;

    vm86_mem_attach(&machine->mem, machine->memory, VM86_BIOS_MEMORY);

    return machine;
}

void vm86_bios_machine_free(struct vm86_bios_machine *machine)
{
    if (!machine)
        return;

    free(machine->memory);
    free(machine->disk);
    free(machine);
}

uint8_t *vm86_bios_disk(struct vm86_bios_machine *machine)
{
    return machine->disk;
}

/*
 * Power the machine on: no program in it, firmware installed, services
 * registered, screen cleared.
 *
 * Split out of vm86_bios_load because there are now two ways to put a
 * program into a machine and only one way to build the machine. The order
 * within it is the order the real hardware does it in -- POST builds the
 * vector table, then something loads a program -- and it used to be
 * load-bearing: under the old convention a program went to linear 0x100
 * with CS = 0, which is inside the table, so installing the firmware
 * second buried the program's first bytes under its own vector entries.
 * The program has a segment of its own now; see VM86_BIOS_LOAD_SEGMENT.
 *
 * The services go in after the firmware, so that the two the firmware owns
 * itself -- equipment and memory size -- are not unregistered by a clear.
 * The registry is process-wide (see trap.c), so powering on is also what
 * makes sure the entries belong to THIS machine's devices and not to a
 * previous case's.
 */
static void power_on(struct vm86_bios_machine *machine)
{
    struct vm86_cpu *cpu = &machine->cpu;

    vm86_mem_clear(&machine->mem);
    vm86_reset(cpu, &machine->mem);

    vm86_clear_services();
    vm86_install_firmware(cpu);

    memset(&machine->screen, 0, sizeof machine->screen);
    machine->key_delivered = false;

    bios10_reset(&machine->video);
    bios16_reset(&machine->keyboard);
    bios1a_reset(&machine->clock);
    bios13_init(&machine->disk_device, machine->disk,
                VM86_BIOS_DISK_IMAGE_BYTES);

    /*
     * The DOS dispatcher comes up with no program behind it -- segment
     * zero, offset zero -- and vm86_bios_load_dos() gives it the real PSP
     * once there is one. That is not a placeholder to be tidied away
     * later: a DTA is *inside* the program's own segment, so the number
     * does not exist until the loader has chosen one.
     */
    int21_reset(&machine->dos21, &machine->video, 0u);

    vm86_register_service(VM86_INT_VIDEO,         bios10_service,
                          &machine->video);
    vm86_register_service(VM86_INT_KEYBOARD_BIOS, bios16_service,
                          &machine->keyboard);
    vm86_register_service(VM86_INT_KEYBOARD,      bios16_irq,
                          &machine->keyboard);
    vm86_register_service(VM86_INT_TIME,          bios1a_service,
                          &machine->clock);
    vm86_register_service(VM86_INT_TIMER,         bios1a_irq,
                          &machine->clock);
    vm86_register_service(VM86_INT_DISK,          bios13_service,
                          &machine->disk_device);
    vm86_register_service(VM86_INT_CTRL_BREAK, int21_break_service, NULL);
    vm86_register_service(VM86_INT_DOS,           int21_service,
                          &machine->dos21);
    vm86_register_service(VM86_INT_TERMINATE,     int21_terminate_service,
                          NULL);
}

void vm86_bios_load(struct vm86_bios_machine *machine,
                    const uint8_t *image, uint16_t image_size)
{
    struct vm86_cpu *cpu = &machine->cpu;

    power_on(machine);

    for (uint16_t i = 0; i < image_size; i++)
        vm86_mem_write8(&machine->mem,
                        VM86_BIOS_LOAD_LINEAR + VM86_BIOS_LOAD_OFFSET + i,
                        image[i]);

    vm86_set_seg(cpu, VM86_CS, VM86_BIOS_LOAD_SEGMENT);
    vm86_set_seg(cpu, VM86_DS, VM86_BIOS_LOAD_SEGMENT);
    vm86_set_seg(cpu, VM86_ES, VM86_BIOS_LOAD_SEGMENT);
    vm86_set_seg(cpu, VM86_SS, VM86_BIOS_LOAD_SEGMENT);
    vm86_flush_segments(cpu);

    cpu->ip    = VM86_BIOS_LOAD_OFFSET;
    cpu->sp    = VM86_BIOS_STACK_TOP;
    cpu->flags = VM86_BIOS_INITIAL_FLAGS;
}

enum vm86_dos_load_result
vm86_bios_load_dos(struct vm86_bios_machine *machine,
                   const struct vm86_dos_start *start,
                   const uint8_t *image, uint32_t image_size,
                   struct vm86_dos_psp *out)
{
    power_on(machine);

    enum vm86_dos_load_result result =
        vm86_dos_load(&machine->cpu, image, image_size, start, out);

    /*
     * The dispatcher's default DTA is inside the PSP, so it cannot be set
     * until the loader has said which segment that is. Only on success:
     * a refused load left no program for a transfer address to belong to,
     * and pointing the DTA at a segment nothing was written to would be
     * an answer that looks like one.
     */
    if (result == VM86_DOS_LOADED)
        int21_reset(&machine->dos21, &machine->video, out->segment);

    return result;
}

/* ------------------------------------------------------------------ */
/* The host loop                                                       */
/* ------------------------------------------------------------------ */

enum vm86_bios_stop vm86_bios_run(struct vm86_bios_machine *machine,
                                  const struct vm86_bios_plan *plan)
{
    for (uint32_t slice = 0; slice < plan->slices; slice++) {
        enum vm86_stop stop = vm86_run(&machine->cpu, plan->steps_per_slice);

        if (stop == VM86_STOP_BROKEN)
            return VM86_BIOS_BROKEN;

        if (stop == VM86_STOP_FAULT)
            return VM86_BIOS_FAULT;

        /*
         * The display is refreshed after every slice and not only at the
         * end, because a program that writes 0xB8000 directly never passes
         * through a service that could refresh it. Rendering on the host's
         * schedule is the whole point of the display interface, and doing
         * it anywhere else is how the direct-write sample would come to
         * depend on the BIOS.
         */
        present_now(machine);

        /*
         * A halt ends the program only when the guest has interrupts
         * turned off: `cli; hlt` is terminal because nothing can wake it.
         * With IF set the processor is waiting -- M4-6 -- and the way out
         * is to give it something to wake for, which is what the clock and
         * the keyboard below are for.
         */
        if (stop == VM86_STOP_HALT && !vm86_flag_test(&machine->cpu, VM86_IF))
            return VM86_BIOS_HALTED;

        /*
         * The program ended itself. The page is presented first, the same
         * way the halt above is: a DOS program's last act is often to
         * print something, and a screen that stopped one frame short of
         * the exit would be a screen missing the line the exit was
         * about. The code travels out separately -- see the note on
         * VM86_BIOS_EXITED for why it is not folded into the outcome.
         */
        if (stop == VM86_STOP_EXIT)
            return VM86_BIOS_EXITED;

        if (plan->ms_per_slice != 0) {
            uint32_t ticks = bios1a_advance(&machine->clock,
                                            plan->ms_per_slice);

            /* One raise per tick: a vector raised while it is already
             * pending stays raised once (host.h), so driving this from
             * the count rather than from the elapsed time is what makes
             * the guest's count equal the plan's. */
            for (uint32_t i = 0; i < ticks; i++)
                vm86_raise(&machine->cpu, VM86_INT_TIMER);
        }

        if (!machine->key_delivered && plan->key_at_slice != 0 &&
            slice + 1u >= plan->key_at_slice) {
            bios16_key_arrived(&machine->keyboard, plan->key_scancode);
            vm86_raise(&machine->cpu, VM86_INT_KEYBOARD);
            machine->key_delivered = true;
        }
    }

    present_now(machine);
    return VM86_BIOS_UNFINISHED;
}

/* ------------------------------------------------------------------ */
/* Reading it back                                                     */
/* ------------------------------------------------------------------ */

const struct vm86_bios_screen *vm86_bios_screen(
    const struct vm86_bios_machine *machine)
{
    return &machine->screen;
}

const uint8_t *vm86_bios_memory(const struct vm86_bios_machine *machine)
{
    return machine->memory;
}

uint32_t vm86_bios_memory_size(void)
{
    return VM86_BIOS_MEMORY;
}

const struct vm86_cpu *vm86_bios_cpu(const struct vm86_bios_machine *machine)
{
    return &machine->cpu;
}

uint16_t vm86_bios_exit_code(const struct vm86_bios_machine *machine)
{
    return machine->cpu.exit_code;
}

struct vm86_bios_cell vm86_bios_cell_at(const struct vm86_bios_screen *screen,
                                        uint16_t cell)
{
    struct vm86_bios_cell result;

    result.character = screen->cells[cell * 2u];
    result.attribute = screen->cells[cell * 2u + 1u];

    return result;
}

#else /* !VM86_BIOS_REPLAY_HAVE_SERVICES */

/*
 * No firmware in this tree yet. Every entry point still exists and every
 * one of them says so, so that the suite builds, runs, reports that it
 * could not run, and passes -- rather than failing to compile and stopping
 * four other people's `make test`.
 */

struct vm86_bios_machine {
    int unused;
};

struct vm86_bios_machine *vm86_bios_machine_new(void)
{
    return NULL;
}

void vm86_bios_machine_free(struct vm86_bios_machine *machine)
{
    (void)machine;
}

uint8_t *vm86_bios_disk(struct vm86_bios_machine *machine)
{
    (void)machine;
    return NULL;
}

void vm86_bios_load(struct vm86_bios_machine *machine,
                    const uint8_t *image, uint16_t image_size)
{
    (void)machine;
    (void)image;
    (void)image_size;
}

enum vm86_dos_load_result
vm86_bios_load_dos(struct vm86_bios_machine *machine,
                   const struct vm86_dos_start *start,
                   const uint8_t *image, uint32_t image_size,
                   struct vm86_dos_psp *out)
{
    (void)machine;
    (void)start;
    (void)image;
    (void)image_size;
    (void)out;

    /* The DOS loader itself is always there -- dos/dos/psp.c is in the
     * core, not behind the firmware check -- so this refusal is not about
     * the loader. It is that a DOS program with no firmware behind it
     * cannot print, which is the whole of what W3's acceptance asks for.
     * Saying so here is better than letting the suite run and fail on an
     * empty screen. */
    return VM86_DOS_NO_SUCH_SEGMENT;
}

enum vm86_bios_stop vm86_bios_run(struct vm86_bios_machine *machine,
                                  const struct vm86_bios_plan *plan)
{
    (void)machine;
    (void)plan;
    return VM86_BIOS_NO_FIRMWARE;
}

const struct vm86_bios_screen *vm86_bios_screen(
    const struct vm86_bios_machine *machine)
{
    (void)machine;
    return NULL;
}

const uint8_t *vm86_bios_memory(const struct vm86_bios_machine *machine)
{
    (void)machine;
    return NULL;
}

uint32_t vm86_bios_memory_size(void)
{
    return VM86_BIOS_MEMORY;
}

const struct vm86_cpu *vm86_bios_cpu(const struct vm86_bios_machine *machine)
{
    (void)machine;
    return NULL;
}

uint16_t vm86_bios_exit_code(const struct vm86_bios_machine *machine)
{
    (void)machine;
    return 0;
}

struct vm86_bios_cell vm86_bios_cell_at(const struct vm86_bios_screen *screen,
                                        uint16_t cell)
{
    struct vm86_bios_cell result = { 0, 0 };

    (void)screen;
    (void)cell;
    return result;
}

#endif /* VM86_BIOS_REPLAY_HAVE_SERVICES */

int vm86_bios_mount_fat(struct vm86_bios_machine *m,uint8_t *image,uint32_t size)
{
    int e=fat_mount(&m->files,image,size);
    if(!e) m->dos21.files=&m->files;
    return e;
}
void vm86_bios_feed_key(struct vm86_bios_machine *m,uint8_t scan)
{
    bios16_key_arrived(&m->keyboard,scan);
    vm86_raise(&m->cpu,VM86_INT_KEYBOARD);
}
