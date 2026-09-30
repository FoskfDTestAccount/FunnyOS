/*
 * The 8086 interpreter running inside FunnyOS.
 *
 * One entry point. The kernel starts the shell with `vm=1` on its command
 * line, which reaches this as the process's startup argument -- the same
 * one-value mode mechanism the M2 self-tests use. There is no argument
 * vector yet and this does not need one.
 */
#ifndef FUNNYOS_USER_VM_H
#define FUNNYOS_USER_VM_H

/*
 * Run every guest case and assert the terminal state of each.
 *
 * Returns 0 when every case ended the way the manuals say it should and
 * left behind the state they say it should, and 1 otherwise. That value
 * becomes the process's exit code, so it travels back to the kernel and
 * the QEMU test reads it there as well as in this function's output.
 */
int vm_selftest(void);

#endif /* FUNNYOS_USER_VM_H */
