/*
 * The 8086 interpreter running inside FunnyOS.
 *
 * Two entry points, both of which the kernel reaches through the shell's
 * startup argument: the self-test, and the acceptance case with the screen
 * left standing for a photograph.
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

/*
 * Run the acceptance case and leave the guest's page on the screen until a
 * key arrives.
 *
 * The self-test gives the screen back when it is done, so there is no
 * moment at which a screendump could be taken of it. This is that moment:
 * one program, one page, up and unchanging until somebody types. See
 * tools/run-screen-test.sh.
 */
int vm_screen_hold(void);

#endif /* FUNNYOS_USER_VM_H */
