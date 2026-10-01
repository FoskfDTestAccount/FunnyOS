/*
 * The guest programs this image carries.
 *
 * Three of the seven in dos/corpus/bios, and two from dos/corpus/dos.
 * They are assembled by the top-level Makefile with the same nasm
 * invocation dos/Makefile uses, and embedded with tools/bin2c.py the same
 * way the init program is embedded in the kernel -- so there is one copy of
 * the bytes in the tree and it is the one the host suite runs.
 *
 * The symbol names come from those rules: vm_corpus_<file> for the BIOS
 * corpus, and vm_corpus_dos_<file> for the DOS one. The prefix differs
 * because the two are loaded differently -- the BIOS samples by the
 * convention M3 froze, and the DOS ones behind a Program Segment Prefix --
 * and a directory separator cannot appear in an identifier, which is what
 * a shared rule would have needed.
 *
 * Which programs are here and which are not is the top-level Makefile's
 * choice, and it says why: the BIOS corpus carries the three that are
 * about a machine rather than about registers, and the DOS corpus carries
 * both of its samples because each is the acceptance for something the
 * host suite cannot see -- the screen.
 */
#ifndef FUNNYOS_USER_VM_CORPUS_H
#define FUNNYOS_USER_VM_CORPUS_H

#define VM_CORPUS_DECLARE(sym)                     \
    extern const unsigned char sym[];              \
    extern const unsigned long sym##_size;

VM_CORPUS_DECLARE(vm_corpus_hello)
VM_CORPUS_DECLARE(vm_corpus_direct)
VM_CORPUS_DECLARE(vm_corpus_timer)

/* Loaded the way DOS loads one. See dos/include/vm86/dos.h. */
VM_CORPUS_DECLARE(vm_corpus_dos_psp)

/* And one that talks to DOS rather than reading its own memory. See
 * dos/include/vm86/int21.h. */
VM_CORPUS_DECLARE(vm_corpus_dos_int21)

extern const unsigned char vm_corpus_dos_files[], vm_corpus_dos_keys[];
extern const unsigned long vm_corpus_dos_files_size, vm_corpus_dos_keys_size;

#endif /* FUNNYOS_USER_VM_CORPUS_H */
