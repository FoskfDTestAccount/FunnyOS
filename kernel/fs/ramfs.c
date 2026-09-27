#include <funnyos/ramfs.h>

#include <stdbool.h>

#include <libk/string.h>

/*
 * File contents.
 *
 * Plain newlines rather than the CRLF a DOS text file would carry. The
 * console treats '\n' as a full return-and-advance, so CRLF would work
 * too, but writing one here would be pretending this is something it is
 * not -- the FAT layer in M5 is where line endings become a real question
 * and get a real answer.
 */
static const char readme_txt[] =
    "FunnyOS -- a native x86-64 operating system with a built-in 8086\n"
    "virtual machine, whose purpose is running 16-bit DOS programs.\n"
    "\n"
    "This file is not stored anywhere. The filesystem you are reading it\n"
    "from is a table of string literals compiled into the kernel, because\n"
    "there is no disk driver yet. `dir` and `type` are real commands\n"
    "working on real file data; only the storage is pretend.\n"
    "\n"
    "What works so far:\n"
    "  - Limine boot on BIOS and UEFI, into 64-bit long mode\n"
    "  - GDT, TSS, a 256-vector IDT, and CPU faults reported rather\n"
    "    than becoming a silent reboot\n"
    "  - Physical frames, page tables, a kernel heap, all self-tested\n"
    "    on every boot\n"
    "  - ACPI, the local and I/O APICs, and a calibrated 100 Hz timer\n"
    "  - A PS/2 keyboard and a console line discipline\n"
    "  - Processes in Ring 3, with their own address spaces, talking to\n"
    "    the kernel through system calls\n"
    "\n"
    "What does not work yet: everything after that. The 8086 interpreter,\n"
    "the BIOS and DOS services it will call, and the FAT filesystem it\n"
    "will read from. See docs/DESIGN.md for the plan.\n";

static const char notes_txt[] =
    "Notes on what is deliberately unusual here.\n"
    "\n"
    "The framebuffer is write-only. Limine maps it as write-combining\n"
    "memory, and reads from WC memory bypass the cache entirely -- one\n"
    "uncached transaction per byte. The obvious scroll implementation is\n"
    "to move the framebuffer up a row, and written that way this console\n"
    "took about sixty seconds to print a boot log. Screen contents live\n"
    "in a cached character array instead, and the framebuffer is only\n"
    "ever written.\n"
    "\n"
    "The timer's frequency is measured, not assumed. The LAPIC timer\n"
    "counts against a clock whose rate varies by CPU model, so the\n"
    "kernel times one interval against the 8254 PIT's crystal -- the one\n"
    "frequency on a PC that really is a constant -- and calibrates the\n"
    "timestamp counter in the same window.\n"
    "\n"
    "Interrupt controller addresses come from the ACPI tables rather\n"
    "than from constants. They default to the same values on essentially\n"
    "every machine ever built, which is exactly why guessing works right\n"
    "up until the machine where it does not.\n";

static const char hello_c[] =
    "/* The traditional first program.\n"
    " *\n"
    " * This is a file here, not a program the shell can run: there is no\n"
    " * compiler on this side of the system call boundary, and the 8086\n"
    " * interpreter that will eventually run .COM files does not exist\n"
    " * yet. It is included because a programming environment without a\n"
    " * hello world in it feels unfinished, and because in a few\n"
    " * milestones this will be running.\n"
    " */\n"
    "#include <stdio.h>\n"
    "\n"
    "int main(void)\n"
    "{\n"
    "    printf(\"Hello, world.\\n\");\n"
    "    return 0;\n"
    "}\n";

/*
 * The directory. Names are stored in the form DOS would use -- uppercase,
 * 8.3 -- and lookups are case-insensitive, so `type readme.txt` finds
 * README.TXT. That mismatch is the normal state of affairs on a DOS
 * system and pretending otherwise would break the habit before it is
 * even formed.
 */
static const struct ramfs_file g_files[] = {
    { "README.TXT", readme_txt, sizeof(readme_txt) - 1 },
    { "NOTES.TXT",  notes_txt,  sizeof(notes_txt)  - 1 },
    { "HELLO.C",    hello_c,    sizeof(hello_c)    - 1 },
};

#define FILE_COUNT ((int)(sizeof(g_files) / sizeof(g_files[0])))

int ramfs_count(void)
{
    return FILE_COUNT;
}

const struct ramfs_file *ramfs_at(int index)
{
    if (index < 0 || index >= FILE_COUNT)
        return NULL;
    return &g_files[index];
}

/* Uppercase a byte. ASCII only, which is all a name can contain here. */
static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static bool name_matches(const char *stored, const char *wanted)
{
    for (;;) {
        char a = upper(*stored++);
        char b = upper(*wanted++);

        if (a != b)
            return false;
        if (a == '\0')
            return true;
    }
}

const struct ramfs_file *ramfs_find(const char *name)
{
    if (!name)
        return NULL;

    /* A DOS program would be entitled to type a drive letter or a leading
     * slash; neither means anything here, so both are skipped rather than
     * being an error. */
    while (*name == '/' || *name == '\\')
        name++;

    if (name[0] != '\0' && name[1] == ':')
        name += 2;

    for (int i = 0; i < FILE_COUNT; i++) {
        if (name_matches(g_files[i].name, name))
            return &g_files[i];
    }

    return NULL;
}
