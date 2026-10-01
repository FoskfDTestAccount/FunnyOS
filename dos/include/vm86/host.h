/*
 * The host side of the machine.
 *
 * ---------------------------------------------------------------------
 * What this layer is
 *
 * The interpreter in cpu/ and mem/ knows how to execute an 8086 and
 * nothing else. It has no idea that the program it is running is a DOS
 * program, that there is a display, or that anything called a BIOS
 * exists. Everything above that -- the interrupt vector table, the
 * services a guest calls, the run loop that decides when to deliver a
 * hardware interrupt -- is here.
 *
 * That separation is what lets the same interpreter run in three places:
 * a host unit test with a fake display, the Ring 3 process inside
 * FunnyOS, and whatever comes later.
 *
 * ---------------------------------------------------------------------
 * How a guest reaches the host
 *
 * An 8086 has no `syscall` instruction. A program asks the firmware for
 * something by executing `INT n`, which pushes a frame and jumps wherever
 * the interrupt vector table points. So the way in is the vector table,
 * and that is also the way TSRs get in -- they overwrite a vector and the
 * call goes to them instead.
 *
 * The machine therefore installs, for every vector, a four-byte stub:
 *
 *      FE 38 <vector> CF
 *
 *      FE 38   the host trap (see below)
 *      <vector> which service is being asked for
 *      CF      IRET, which returns to the caller
 *
 * and points the vector table at it. The `INT n` instruction is not
 * special-cased anywhere: it reads the table and jumps exactly as it did
 * in M3, and lands on the stub. A program that overwrites the vector gets
 * its own handler, and one that chains back to the old address lands on
 * the stub -- which is how TSR chaining comes out working without
 * anybody modelling it.
 *
 * The stub returns through its own IRET rather than through the host
 * calling back into the interpreter. That matters: the frame on the stack
 * was pushed by the guest's INT (or by whatever pushed it before chaining
 * in), and the only code that knows how to unwind it correctly is the
 * guest's own IRET.
 *
 * ---------------------------------------------------------------------
 * Why the trap is FE 38
 *
 * The trap has to be an encoding a compiler for 8086 assembly cannot
 * produce, or a program would eventually produce it by accident.
 *
 * `FE` is the byte-sized INC/DEC group. Only /0 and /1 are defined; /7 is
 * not an instruction on the 8086, the 186, or anything after, and no
 * assembler will emit it -- `inc` and `dec` have no byte form with reg=7.
 * The r/m field is zero and the mod field is zero, so the encoding carries
 * no displacement and the byte after it is unambiguous.
 *
 * The obvious alternatives are worse. `F1` (LOCK) and `D6` (SALC) are real
 * instructions on the 8086 silicon, and there are tests in this tree that
 * pin them to the invalid-opcode trap precisely because claiming them
 * would be a lie about that chip. `0F` is POP CS on the 8086 and is
 * reserved here as the 286 two-byte escape. `FE /7` costs nothing: it is
 * a hole, and this fills it.
 *
 * A guest that executes `FE 38` from ordinary code -- which it cannot
 * reach without deliberately emitting the bytes -- gets the service named
 * by the byte after it. That is not a hole in the model: a DOS program
 * could call the BIOS directly on real hardware too.
 */
#ifndef VM86_HOST_H
#define VM86_HOST_H

#include <stdbool.h>
#include <stdint.h>

#include <vm86/cpu.h>
#include <vm86/mem.h>

/* ------------------------------------------------------------------ */
/* The trap                                                            */
/* ------------------------------------------------------------------ */

#define VM86_TRAP_OPCODE   0xFEu   /* the group byte                     */
#define VM86_TRAP_MODRM    0x38u   /* reg = 7, mod = 0, r/m = 0          */
#define VM86_TRAP_IRET     0xCFu   /* what terminates every stub         */

/*
 * Where the stubs live.
 *
 * Segment 0xF000 is the ROM area on a real machine, and that is where the
 * firmware's own entry points are. It is also out of the way: a .COM
 * program is loaded at offset 0x100 and grows upward through conventional
 * memory, so anything below 0xA0000 is memory the program may legitimately
 * be using.
 *
 * Sixteen bytes per vector would be how real firmware spaces its
 * entry points, but a stub here is four bytes and the byte after it is
 * never reached, so four is enough and the whole table is 1 KiB.
 */
#define VM86_TRAP_SEGMENT  0xF000u
#define VM86_TRAP_BASE     0x0000u
#define VM86_TRAP_LINEAR   0xF0000u   /* VM86_TRAP_SEGMENT << 4 */
#define VM86_TRAP_STRIDE   4u
#define VM86_TRAP_VECTORS  256u
#define VM86_TRAP_BYTES    (VM86_TRAP_VECTORS * VM86_TRAP_STRIDE)

/* ------------------------------------------------------------------ */
/* Services                                                            */
/* ------------------------------------------------------------------ */

/*
 * A host service: the thing on the other end of an `INT n`.
 *
 * It is handed the processor, from which it reads its arguments out of
 * the guest's registers and memory, and writes its results back the same
 * way. It must not touch the interrupt frame on the stack -- that belongs
 * to the stub's IRET, which runs after this returns.
 *
 * `ctx` is whatever the registrar wanted it to be: the display state, the
 * keyboard queue, the disk image. It is per-vector rather than global so
 * that a host unit test can give each service a fake device and the real
 * machine can give each one the real thing, without either knowing about
 * the other.
 *
 * The signature has no way to say "the program has ended". It gains one
 * in M5, when INT 20h and INT 21h/4Ch exist and something needs to end a
 * program; the compiler will then point at every service that has to
 * decide, which is what the -Wswitch discipline on enum vm86_result is
 * for. Nothing in M4 needs it: a program here ends by halting.
 */
typedef void (*vm86_service_fn)(struct vm86_cpu *cpu, void *ctx);

/*
 * Give a vector a host service. Passing NULL removes it.
 *
 * The registry is process-wide rather than per-machine. There is one
 * firmware in the machine, and a machine built for a test is built one at
 * a time; threading a table through every call to save a global would
 * cost more than it buys. `vm86_clear_services()` returns it to the state
 * a fresh process is in, which is what a suite calls between cases.
 *
 * Registering for a vector that is never called costs nothing. Calling a
 * vector with no service reaches the stub, finds nobody, and the IRET
 * returns -- which is what firmware does for a vector it has no handler
 * for, and is deliberately not an error.
 */
void vm86_register_service(uint8_t vector, vm86_service_fn fn, void *ctx);

/* Forget every registered service. */
void vm86_clear_services(void);

/*
 * Called by the opcode dispatcher when the host trap executes.
 *
 * Exposed because the trap is reached through the FE group handler rather
 * than the merged table, and whoever owns that handler has to call this.
 * It reads the service byte, calls the service, and leaves the
 * instruction pointer on the stub's IRET so that the next step executes
 * it. Always returns VM86_CONTINUE: the guest is still running.
 */
enum vm86_result vm86_host_trap(struct vm86_cpu *cpu, uint8_t opcode);

/*
 * Ask for the instruction being executed to run again.
 *
 * For a service that cannot answer yet -- the keyboard buffer is empty,
 * so a blocking read has nothing to give back. Returning anyway would
 * tell the caller it got an answer, with whatever happened to be left in
 * the accumulator, and a program that waits for a keypress would carry on
 * with a phantom one.
 *
 * So the interrupt is undone and the instruction is re-executed: the
 * frame the INT pushed is popped, and the instruction pointer is rewound
 * to the instruction itself. The guest then really does wait -- with
 * interrupts live -- and a keyboard interrupt arriving in the meantime
 * runs its handler between one attempt and the next, filling the buffer
 * that the next attempt reads.
 *
 * That is what a blocking BIOS call does on real hardware: it spins with
 * interrupts enabled. The only difference is that the spinning happens in
 * the run loop, where the host still gets a look in, instead of inside a
 * service that would never return.
 *
 * Only correct for a service reached through an INT, which is what a
 * blocking read is. A service reached by a chained far call has a frame
 * on the stack that this did not put there, and retrying would consume
 * it; a service that might be reached either way has to decide for itself
 * whether to use this.
 */
void vm86_service_retry(struct vm86_cpu *cpu);

/* ------------------------------------------------------------------ */
/* The interrupt vector table                                          */
/* ------------------------------------------------------------------ */

/*
 * Bring the machine up the way firmware does.
 *
 * Fills the BIOS data area with what this machine has, builds the vector
 * table, and registers the two services that describe the machine to
 * itself -- equipment (11h) and memory size (12h). Call it once, after
 * the CPU has been reset and its memory attached, and before registering
 * anything else.
 *
 * Deliberately separate from vm86_reset(), which resets a *processor*.
 * This sets up a machine, and a host unit test that wants a bare
 * processor with no firmware on it should be able to have one.
 */
void vm86_install_firmware(struct vm86_cpu *cpu);

/*
 * Write the stubs and build the vector table.
 *
 * Every one of the 256 vectors is pointed at its own stub, including the
 * ones with no service. That is what real firmware does: the table is
 * full, and an unexpected interrupt returns instead of running off into
 * whatever happens to be at address zero.
 *
 * A sub-step of vm86_install_firmware(), exposed because a test that
 * wants only the table should not have to take the data area with it.
 * Writing into guest memory, so it needs cpu->mem set.
 */
void vm86_install_ivt(struct vm86_cpu *cpu);

/*
 * Is this vector still pointed at the stub we installed?
 *
 * True means the guest never hooked it. That is the question the run loop
 * asks before delivering an exception: a guest with a handler for divide
 * error wants the vector delivered to it, and one without one wants the
 * machine to stop rather than spin. Doing it by comparison rather than by
 * watching writes to the first kilobyte of memory is the same answer for
 * a good deal less machinery.
 */
bool vm86_vector_is_stub(const struct vm86_cpu *cpu, uint8_t vector);

/* ------------------------------------------------------------------ */
/* Interrupts                                                          */
/* ------------------------------------------------------------------ */

/*
 * Push an interrupt frame and vector through the table.
 *
 * This is what `INT n` does, and it is a function because three things
 * have to agree on it: the INT instruction, the run loop delivering a
 * hardware interrupt, and the run loop delivering an exception. They
 * differ in when they happen, not in what they do, and the moment they
 * are written twice they stop matching.
 *
 * It does not check whether the interrupt is allowed. Both callers decide
 * that for themselves, because they decide it differently -- a software
 * INT is never gated on IF, a hardware one always is.
 */
void vm86_interrupt(struct vm86_cpu *cpu, uint8_t vector);

/*
 * Raise a hardware interrupt. Set it pending; it is delivered at the next
 * instruction boundary at which the guest can take it.
 *
 * Raising a vector that is already pending does nothing: a line that is
 * asserted while its previous interrupt has not been taken yet is one
 * interrupt, not two. That is how the 8259 works and is what keeps a
 * device that is flooded from building an unbounded backlog the guest can
 * never catch up with.
 */
void vm86_raise(struct vm86_cpu *cpu, uint8_t vector);

/*
 * Whether a pending interrupt would be recognized right now: IF is set,
 * no instruction is in the shadow of STI or of a load of SS, and
 * something is pending.
 */
bool vm86_interruptible(const struct vm86_cpu *cpu);

/* Drop a pending vector. Used by the run loop after it delivers one. */
void vm86_clear_pending(struct vm86_cpu *cpu, uint8_t vector);

/* ------------------------------------------------------------------ */
/* The run loop                                                        */
/* ------------------------------------------------------------------ */

/*
 * Why the machine stopped.
 *
 * This is not `enum vm86_result`. That enum answers "what did this one
 * instruction do", and it is the right answer for one instruction. Most
 * of what a caller wants to know is about the run -- did the program
 * finish, did it fault, did it use up its budget -- and folding those
 * into the per-instruction enum would make a caller sort out "HALT
 * because HLT executed" from "HALT because a service ended the program"
 * every time.
 */
enum vm86_stop {
    /* The budget ran out with the machine still runnable. Not an ending:
     * call again. */
    VM86_STOP_STEPS = 0,

    /* HLT, with nothing pending that could wake it. Terminal when the
     * guest has interrupts disabled, which is what a program ending with
     * `cli; hlt` relies on. */
    VM86_STOP_HALT,

    /* An exception the guest has no handler for. cpu->fault says which. */
    VM86_STOP_FAULT,

    /* The host is broken: the opcode tables did not merge, so nothing in
     * this run reached the handler it should have. This is never the
     * guest's fault and must not be reported as though it were. */
    VM86_STOP_BROKEN,
};

/*
 * Run until something stops the machine or `steps` instructions retire.
 *
 * The loop is the one DESIGN.md 3.5 describes, with the pieces the 8086
 * actually has:
 *
 *   1. If an interrupt is pending and the guest can take it, deliver it.
 *      A machine that is halted is woken by this rather than by anything
 *      else, which is the whole point of `sti; hlt`.
 *   2. Otherwise execute one instruction.
 *   3. Repeat.
 *
 * A step budget rather than an open-ended call, because the caller is a
 * user process with a display to refresh and a keyboard to poll. Bounded
 * slices are how it stays responsive, and the budget is the caller's
 * business rather than a constant here.
 *
 * Returns when the machine stops, or VM86_STOP_STEPS when the budget is
 * used up with the guest still runnable. `steps` of zero is a legal way
 * to ask "is anything pending that should run" without executing
 * anything.
 */
enum vm86_stop vm86_run(struct vm86_cpu *cpu, uint64_t steps);

#endif /* VM86_HOST_H */
