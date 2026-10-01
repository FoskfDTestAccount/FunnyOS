/*
 * The guest programs this image carries.
 *
 * Two of the seven in dos/corpus/bios. They are assembled by the top-level
 * Makefile with the same nasm invocation dos/Makefile uses, and embedded
 * with tools/bin2c.py the same way the init program is embedded in the
 * kernel -- so there is one copy of the bytes in the tree and it is the
 * one the host suite runs.
 *
 * The symbol names come from that rule: vm_corpus_<file>.
 */
#ifndef FUNNYOS_USER_VM_CORPUS_H
#define FUNNYOS_USER_VM_CORPUS_H

#define VM_CORPUS_DECLARE(sym)                     \
    extern const unsigned char sym[];              \
    extern const unsigned long sym##_size;

VM_CORPUS_DECLARE(vm_corpus_hello)
VM_CORPUS_DECLARE(vm_corpus_direct)
VM_CORPUS_DECLARE(vm_corpus_timer)

#endif /* FUNNYOS_USER_VM_CORPUS_H */
