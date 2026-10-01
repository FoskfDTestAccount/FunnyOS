/*
 * The guest programs this image carries.
 *
 * Three of the seven in dos/corpus/bios, and one from dos/corpus/dos.
 * They are assembled by the top-level Makefile with the same nasm
 * invocation dos/Makefile uses, and embedded with tools/bin2c.py the same
 * way the init program is embedded in the kernel -- so there is one copy of
 * the bytes in the tree and it is the one the host suite runs.
 *
 * The symbol names come from those rules: vm_corpus_<file> for the BIOS
 * corpus, and vm_corpus_dos_<file> for the DOS one. The prefix differs
 * because the two are loaded differently -- the BIOS samples by the
 * convention M3 froze, and the DOS one behind a Program Segment Prefix --
 * and a directory separator cannot appear in an identifier, which is what
 * a shared rule would have needed.
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

#endif /* FUNNYOS_USER_VM_CORPUS_H */
