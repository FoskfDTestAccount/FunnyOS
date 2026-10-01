/*
 * INT 21h: the DOS function dispatcher.
 *
 * ---------------------------------------------------------------------
 * What this is, and where it sits
 *
 * A DOS program asks for everything through one vector. Printing a
 * character, reading a file, allocating memory, terminating and hooking an
 * interrupt are all `INT 21h` with a function number in AH -- there is no
 * second entry point and no syscall instruction, because the 8086 has
 * neither.
 *
 * M5-README.md section 1 rules on where a service of this size lives, and
 * this file is that ruling carried out:
 *
 *     services live in the VM process; only a real resource goes through
 *     a system call.
 *
 * So `AH=02h` -- print a character -- is entirely local. It writes the
 * guest's own video memory, which is in this process's address space, and
 * the kernel is never told. `AH=3Dh` -- open a file -- will need the host
 * at mount time; the image resource crosses the boundary then. The
 * consequence worth stating: the *large* part of this file is the
 * dispatcher, which grows to hundreds of function numbers, and the
 * *small* part crosses the boundary, which is a handful of calls that
 * will not move again.
 *
 * ---------------------------------------------------------------------
 * Where the console functions write
 *
 * Through bios10_tty(). DOS did not have its own screen code -- `AH=02h`
 * and `AH=09h` ended up in the BIOS's teletype routine, and on a real
 * machine that is literally the code they ran. Sharing one implementation
 * here is the same arrangement, and it is the only arrangement in which
 * the two cannot drift: a second copy would be a second place for CR, LF,
 * BS and TAB to be got wrong, and the day one of them was fixed the other
 * would keep the old behaviour.
 *
 * ---------------------------------------------------------------------
 * Errors go in the carry flag
 *
 * DOS 2.0 and later report failure by setting CF and putting an error
 * code in AX. It is the only way to report one, and both halves are
 * needed: an implementation that sets a code in AX and leaves CF clear is
 * worse than one that does nothing, because the caller tests CF, sees
 * success, and carries on. Every failure below goes through
 * dos_fail(), so the pair cannot come apart.
 *
 * ---------------------------------------------------------------------
 * What is here and what is not
 *
 * Here: termination (00h, 4Ch), the console output (02h, 09h), the
 * interrupt vectors (25h, 35h), the disk transfer address (1Ah, 2Fh), the
 * version (30h) and the two drive calls (0Eh, 19h) that the version and
 * the drive letters make meaningful together.
 *
 * W5 adds 3Ch..42h and 4Eh/4Fh over a FAT12/FAT16 byte-array mount.
 * W6 adds 01h/06h/07h/08h/0Ah/0Bh over the BIOS keyboard ring. Only
 * acquiring the disk resource and raw set-1 stream crosses into the host.
 * Persistent block-device writes, file sharing, FCB I/O and EXEC remain
 * outside this interface's implemented scope.
 *

 * docs/dos-refs-dos.md section 9 is the reference: every function number
 * below with its arguments, its returns and its source.
 */
#ifndef VM86_INT21_H
#define VM86_INT21_H

#include <stdint.h>
#include <stdbool.h>

#include <vm86/cpu.h>

/* ------------------------------------------------------------------ */
/* Function numbers, in AH                                             */
/* ------------------------------------------------------------------ */

/*
 * The numbers are DOS's, not a choice: a program prints a string with AH
 * set to 09h because that is what the machine it was written for answers
 * on. docs/dos-refs-dos.md section 9 has the table with a source per row.
 */
#define VM86_INT21_TERMINATE       0x00u  /* the old exit, no return code */
#define VM86_INT21_OUTPUT_CHAR     0x02u  /* DL                            */
#define VM86_INT21_OUTPUT_STRING   0x09u  /* DS:DX, '$'-terminated         */
#define VM86_INT21_DISK_RESET      0x0Du  /* flush buffers; nothing here   */
#define VM86_INT21_SELECT_DRIVE    0x0Eu  /* DL -> AL = number of drives   */
#define VM86_INT21_GET_DRIVE       0x19u  /* -> AL = current drive (0 = A) */
#define VM86_INT21_SET_DTA         0x1Au  /* DS:DX                         */
#define VM86_INT21_SET_VECTOR      0x25u  /* AL = vector, DS:DX = address  */
#define VM86_INT21_GET_DTA         0x2Fu  /* -> ES:BX                      */
#define VM86_INT21_GET_VERSION     0x30u  /* -> AL:AH, BX, CX              */
#define VM86_INT21_GET_VECTOR      0x35u  /* AL = vector -> ES:BX          */
#define VM86_INT21_TERMINATE_CODE  0x4Cu  /* AL = return code              */

/*
 * The character that ends a string handed to AH=09h, and the reason a
 * program that wants to print a literal '$' cannot use that call.
 *
 * Spelled as a character rather than as 0x24 everywhere it is used,
 * because a bare hex constant here reads like a length or a count.
 */
#define VM86_INT21_STRING_END      '$'

/* ------------------------------------------------------------------ */
/* The drive this machine has                                          */
/* ------------------------------------------------------------------ */

/*
 * One drive, and it is F:.
 *
 * The letter is a decision and not a fact -- a machine with one diskette
 * could answer A: just as honestly -- and F: is chosen because that is
 * what the rest of this machine calls its storage: the shell's prompt is
 * `F:\>`, the environment block this loader builds says `PATH=F:\`, and
 * the program's own path in it is `F:\PSP.COM`. A DOS program that asks
 * which drive it is on and is told something else is a program that will
 * look in the wrong place for the file it was told to read, and the
 * contradiction would be between two things this machine says rather than
 * between this machine and a real one.
 *
 * The count is 6, not 1, and that is DOS's convention rather than a
 * mistake: `AH=0Eh` answers "how many logical drives are there", and a
 * machine whose highest drive is C: answers 3 even though there is no B:
 * -- the number is the range the numbering covers, not a count of media
 * that are present. A machine whose only drive is F: therefore answers 6.
 * docs/dos-refs-dos.md section 9 has the row.
 */
#define VM86_INT21_DRIVE_F         5u     /* 'F' - 'A'                    */
#define VM86_INT21_DRIVE_COUNT     6u     /* the highest drive letter, +1 */

/* ------------------------------------------------------------------ */
/* Error codes, returned in AX with the carry flag set                 */
/* ------------------------------------------------------------------ */

#define VM86_INT21_ERR_FUNCTION    0x01u  /* no such function number      */
#define VM86_INT21_ERR_NOT_FOUND   0x02u  /* file not found               */
#define VM86_INT21_ERR_PATH        0x03u  /* path not found               */
#define VM86_INT21_ERR_NO_HANDLES  0x04u  /* too many open files          */
#define VM86_INT21_ERR_ACCESS      0x05u  /* access denied                */
#define VM86_INT21_ERR_HANDLE      0x06u  /* invalid handle               */
#define VM86_INT21_ERR_INVALID_ACCESS 0x0Cu  /* bad sharing mode         */
#define VM86_INT21_ERR_DRIVE       0x0Fu  /* no such drive                */

/* ------------------------------------------------------------------ */
/* Where a program's disk transfer address starts                      */
/* ------------------------------------------------------------------ */

/*
 * The default DTA is the program's own command tail: PSP:0080h.
 *
 * That is DOS's choice and it is a slightly awkward one -- a program that
 * reads through the DTA without first setting it destroys the command
 * line it was given -- but it is the documented behaviour and a program
 * written for the real thing relies on it. docs/dos-refs-dos.md section 4
 * says the same area is both.
 */
#define VM86_INT21_DEFAULT_DTA    0x0080u

/* ------------------------------------------------------------------ */
/* The service                                                         */
/* ------------------------------------------------------------------ */

/*
 * The dispatcher's state.
 *
 * DOS-local state: the shared BIOS video, the DTA, a byte-array volume,
 * and continuation state for extended keys and a blocking line read.
 *
 * The DTA is kept as the segment and offset pair the program handed in,
 * not as a linear address. AH=2Fh has to give back exactly what 1Ah was
 * given -- a program that computes its own address out of the answer, or
 * compares it against one it saved, is comparing segments -- and
 * converting to linear and back is a way to be right about the arithmetic
 * and wrong about the answer.
 */
struct fat_volume;

struct int21_state {
    /* Where 02h and 09h write. The same video state the INT 10h service
     * uses: there is one screen and one cursor on this machine, and two
     * cursors that could disagree is the failure this sharing prevents. */
    struct bios10_state *video;

    uint16_t dta_segment;
    uint16_t dta_offset;
    struct fat_volume *files;
    uint8_t extended_key;
    bool extended_pending;
    bool line_active;
    uint16_t line_segment, line_offset;
    uint8_t line_length;
};

/*
 * Bring the dispatcher up for a machine whose program is at
 * `psp_segment`, or 0 for a machine with no program loaded.
 *
 * The PSP is a parameter because the default DTA lives inside it, which
 * is the one piece of this state that comes from the loader rather than
 * from anything here. A machine with no program gets a DTA of 0000:0000,
 * which is what a program that asks before one has been loaded --
 * impossible on a real machine, and possible here because a host test can
 * build a machine and not load anything into it -- deserves to be told.
 */
void int21_reset(struct int21_state *st, struct bios10_state *video,
                 uint16_t psp_segment);

/*
 * Service an INT 21h. `ctx` is a `struct int21_state *`.
 *
 * Register it against VM86_INT_DOS. The rest of the interface is the
 * registers: it reads AH for the function number, whatever that function
 * takes for its arguments, and writes its results back. It never touches
 * the interrupt frame.
 */
void int21_service(struct vm86_cpu *cpu, void *ctx);

/*
 * End the program, and return through the trap with the code.
 *
 * Register it against VM86_INT_TERMINATE. `ctx` is unused -- INT 20h
 * carries no return code -- and is there because the service signature
 * has one.
 *
 * It is a service of its own rather than a case inside int21_service
 * because it is a different vector. The way a program reaches it is
 * either `INT 20h` directly or a bare `RET`, which the loader turns into
 * exactly that by putting a zero word under SP -- see the loader's note
 * on why a .COM needs no exit code at all.
 */
void int21_terminate_service(struct vm86_cpu *cpu, void *ctx);
void int21_break_service(struct vm86_cpu *cpu, void *ctx);

#endif /* VM86_INT21_H */
