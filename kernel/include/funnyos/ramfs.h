/*
 * A filesystem made of string literals.
 *
 * There is no disk driver yet, and a shell that cannot list or read
 * anything is not a shell. So the first filesystem is the files
 * themselves, compiled into the kernel: enough to make `dir` and `type`
 * real commands rather than placeholders, and enough to shake the file
 * interface into shape before there is anything more interesting behind
 * it.
 *
 * It is read-only by construction, and that is not a limitation being
 * worked around -- a table of `const char[]` has nowhere to put a write.
 * FAT on a real image replaces this in M5, and the interface here is
 * shaped so that it can: names, sizes and contents are what a FAT
 * directory entry also has, and nothing above this layer knows the
 * difference.
 */
#ifndef FUNNYOS_RAMFS_H
#define FUNNYOS_RAMFS_H

#include <stddef.h>

/* Longest name this filesystem will store, including the terminator.
 * Matches DIRENT_NAME_MAX so an entry never has to be truncated on its
 * way out to a program. */
#define RAMFS_NAME_MAX 32

struct ramfs_file {
    const char *name;
    const char *data;
    unsigned    size;      /* bytes, excluding the terminator */
};

/* Number of files. */
int ramfs_count(void);

/* Entry by position, or NULL past the end. Used to enumerate. */
const struct ramfs_file *ramfs_at(int index);

/*
 * Look a name up. Case-insensitive, because that is what a DOS program
 * and everyone who has ever used one expects, and because the alternative
 * is a shell where `type readme.txt` fails on a file named README.TXT.
 */
const struct ramfs_file *ramfs_find(const char *name);

#endif /* FUNNYOS_RAMFS_H */
