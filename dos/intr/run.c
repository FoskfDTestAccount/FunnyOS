/*
 * The run loop: the thing that decides what happens next.
 *
 * ---------------------------------------------------------------------
 * What this file is for
 *
 * vm86_step() executes one instruction and says what it did. It knows
 * nothing about interrupts -- it cannot, because a hardware interrupt is
 * not an instruction and is not caused by one. Something has to sit above
 * it, look at the machine between instructions, and decide whether to
 * deliver an interrupt or to execute the next instruction.
 *
 * That is all this is, and the *order* of the four things it does is the
 * whole of its correctness. They are:
 *
 *   1. The shadow (cpu->intr_shadow), then delivery.
 *      A nonzero shadow means an instruction is in the grace period after
 *      STI or after a load of SS: the count is decremented and delivery
 *      is skipped, but the next instruction still runs. Once the shadow
 *      is zero, a pending interrupt the guest can take is delivered --
 *      and delivering it retires no instruction, so the budget does not
 *      move.
 *
 *   2. Halted, with nothing that could wake it: stop and say so.
 *
 *   3. Out of budget: stop and say so.
 *
 *   4. Execute one instruction.
 *
 * ---------------------------------------------------------------------
 * Why the order is not negotiable
 *
 * Delivery before the halt check, because `hlt` means "wait for an
 * interrupt" and not "stop". A machine that returned as soon as HLT
 * executed would never wake up: the interrupt it is waiting for is
 * exactly the thing this loop would have delivered. So HLT sets the flag
 * and returns, the loop comes back round to step 1, and *that* decides
 * whether the machine was woken or whether it is genuinely stopped. The
 * `sti; hlt` idle loop depends on this: STI sets the shadow, the next
 * boundary eats the shadow and executes the HLT, and the boundary after
 * that delivers and clears the halted flag.
 *
 * Delivery before the budget check, because `steps` of zero is a
 * meaningful request -- "is anything waiting that should run, without
 * executing anything" -- and a budget check at the top of the loop would
 * skip the delivery that request is asking for.
 *
 * The shadow before both, because it is the only state in the loop that
 * changes without an instruction executing, and it is what makes the
 * instruction after an STI special.
 *
 * ---------------------------------------------------------------------
 * What happens to a fault
 *
 * vm86_step() reports an exception rather than delivering it, because the
 * instruction layer has no way to know whether anybody wants it. This
 * loop knows: if the vector still points at the firmware's stub then the
 * guest never hooked it, there is no handler to run, and the machine
 * stops with the vector left in cpu->fault for the caller. If the guest
 * did hook it, the exception is delivered like any other interrupt.
 *
 * There is exactly one exception raised by the machine and a great many
 * that a guest can hook, and this is the rule that tells them apart
 * without any per-vector table.
 */
#include <vm86/host.h>

/* ------------------------------------------------------------------ */
/* The loop                                                            */
/* ------------------------------------------------------------------ */

enum vm86_stop vm86_run(struct vm86_cpu *cpu, uint64_t steps)
{
    /* Instructions retired, which is what `steps` counts. Delivering an
     * interrupt is not one: an interrupt is not an instruction, and
     * charging it to the budget would make a machine under a steady
     * interrupt load run fewer instructions per slice than one that is
     * idle. */
    uint64_t retired = 0;

    for (;;) {
        /*
         * 1. The shadow, then delivery.
         *
         * A nonzero shadow is the instruction or two after STI or after a
         * load of SS, during which the guest does not recognise
         * interrupts. The count is decremented and the instruction is
         * still executed -- the grace is over "the next instruction", not
         * over "the next time round".
         *
         * vm86_interruptible() answers whether IF is set, the shadow is
         * clear and something is pending. The vector is then looked up
         * only when that says yes, so a disagreement between the
         * predicate and the bitmap costs a missed interrupt rather than a
         * delivery of vector 0xFF.
         *
         * vm86_next_pending() answers with the *lowest* vector, and that
         * is a priority order rather than a convention: the 8259 presents
         * its lines to the processor in a fixed sequence and the
         * firmware's handlers are numbered to match. Delivering whichever
         * bit a scan happened to reach first would put the timer behind
         * the keyboard whenever both were pending, and the symptom -- a
         * clock that runs slow under load -- would be blamed on the
         * clock.
         */
        if (cpu->intr_shadow == 0) {
            int vector = vm86_interruptible(cpu) ? vm86_next_pending(cpu) : -1;

            if (vector >= 0) {
                vm86_clear_pending(cpu, (uint8_t)vector);

                /*
                 * Waking the machine is what HLT is for, and it is done
                 * here rather than on the HLT path on purpose: reaching
                 * this line is the definition of "something woke it", so
                 * there is no second place that could disagree about
                 * whether a halted machine is runnable again.
                 */
                cpu->halted = false;

                vm86_interrupt(cpu, (uint8_t)vector);

                /* No instruction retired, so the budget has not moved. */
                continue;
            }
        }

        /*
         * 2. Halted, and step 1 found nothing that could wake it.
         *
         * This is the terminal case for `cli; hlt`, which is how a program
         * here ends: with IF clear, a maskable interrupt cannot wake the
         * machine, so there is nothing left that could make progress. The
         * caller is told rather than spun at.
         */
        if (cpu->halted)
            return VM86_STOP_HALT;

        /*
         * 3. Out of budget, with the machine still runnable.
         *
         * Not an ending: the caller asked for a bounded slice so that it
         * can refresh a display or poll a keyboard, and this is that
         * slice ending. Called again, the machine carries on.
         */
        if (retired >= steps)
            return VM86_STOP_STEPS;

        /*
         * 4. Spend one boundary of the grace period.
         *
         * Here, and not up with the delivery check, because the grace
         * belongs to the instruction that is about to run and the two
         * must not come apart. A slice that ends between them would
         * otherwise eat the grace without executing anything, and the
         * next call would deliver an interrupt in the middle of the
         * window -- which for `sti; hlt` means the machine is woken by an
         * interrupt that the HLT then waits for a second time, and an
         * idle loop stops instead of sleeping. It is the same failure the
         * grace exists to prevent, arriving by a different route.
         */
        if (cpu->intr_shadow != 0)
            cpu->intr_shadow--;

        /* 5. One instruction. */
        enum vm86_result result = vm86_step(cpu);

        retired++;

        switch (result) {
        case VM86_CONTINUE:
            break;

        case VM86_HALT:
            /*
             * Not an ending, and not a return. The machine is halted;
             * whether that means stopped or merely waiting is decided at
             * the top of the next pass, where a pending interrupt is
             * looked for. Returning here is the bug this whole ordering
             * exists to avoid: it loses the wake-up in every `sti / hlt`
             * idle loop, and the program that hangs is an operating
             * system's shell rather than a test.
             */
            break;

        case VM86_FAULT: {
            /*
             * Read the vector now. vm86_step() clears cpu->fault at the
             * start of every instruction, so it describes the step just
             * taken and nothing else -- deferring this read to the next
             * pass would read a field that had already been reset.
             */
            uint8_t vector = cpu->fault;

            if (vm86_vector_is_stub(cpu, vector)) {
                /*
                 * The guest never installed a handler for this one, so
                 * there is nobody to deliver it to and no sense in
                 * continuing. Stopping is what M3 did with every fault
                 * and is what the tests for divide error expect; the
                 * vector is left in cpu->fault for the caller to report.
                 */
                return VM86_STOP_FAULT;
            }

            /* The guest hooked it, so it is that handler's problem --
             * delivered exactly as a hardware interrupt is, because on
             * this machine an exception reaching a handler is the same
             * event. */
            vm86_interrupt(cpu, vector);
            break;
        }

        case VM86_INTERNAL_ERROR:
            /*
             * Something on the host is broken: the opcode tables did not
             * merge. No instruction executed and nothing is wrong with
             * the guest, which is why this is its own stop rather than a
             * halt -- a caller that reads every stop as the program
             * ending would take a broken emulator for a clean run.
             */
            return VM86_STOP_BROKEN;

        case VM86_EXIT:
            /*
             * The program ended itself: it executed an INT whose handler
             * asked to terminate. The code is already on the processor,
             * put there by vm86_service_exit(), and the caller reads it
             * from cpu->exit_code.
             *
             * It is deliberately not VM86_HALT. HLT is a guest waiting
             * for an interrupt and this is a guest that is finished; the
             * two are told apart here, once, rather than by every caller
             * wondering whether a halt meant an exit.
             */
            return VM86_STOP_EXIT;
        }
    }
}
