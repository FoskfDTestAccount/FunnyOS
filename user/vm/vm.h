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

/*
 * The same thing for the DOS loader: one .COM, loaded behind a Program
 * Segment Prefix, read back off the screen.
 *
 * Separate from the one above because it is a different claim. That one is
 * about a program appearing on a display; this one is about a program that
 * was *loaded the way DOS loads one* appearing there -- the PSP it reads,
 * the four segment registers pointed at it, the tail it was given, and all
 * of it on a screen rather than only in a host test's comparison.
 */
int vm_psp_hold(void);

#endif /* FUNNYOS_USER_VM_H */
