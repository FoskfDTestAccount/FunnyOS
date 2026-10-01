/*
 * INT 13h: the disk, one sector at a time.
 *
 * ---------------------------------------------------------------------
 * What this is, and what it deliberately is not
 *
 * A block device: a linear array of bytes plus the geometry that says how
 * a cylinder, head and sector name a place in it. It has no idea what is
 * stored in those bytes. There is no file system here and there is not
 * meant to be one -- FAT is M5, and a version of this that understood it
 * would hide exactly the addressing mistakes this layer exists to get
 * right, by making "the read returned something plausible" pass for "the
 * read returned the right thing".
 *
 * The array is the caller's. It can be a static buffer in a test, a
 * region the kernel owns, or a file mapped in later. Nothing here
 * allocates, opens or closes anything.
 *
 * ---------------------------------------------------------------------
 * Errors are reported in the carry flag
 *
 * A successful call clears CF and leaves AH at zero. A failed call sets
 * CF and puts a status code in AH. Both halves are required, and the
 * second is the one that gets forgotten: an implementation that writes a
 * status into AH but leaves CF clear is worse than one that does
 * nothing, because the caller tests CF, sees success, and carries on with
 * whatever was already in the buffer. Every error path here goes through
 * one function so that the pair cannot come apart.
 *
 * ---------------------------------------------------------------------
 * Sectors are numbered from one
 *
 * CH holds the low eight bits of the cylinder, CL holds the sector in its
 * low six bits and the top two bits of the cylinder in its top two. The
 * sector is not a zero-based index: sector 1 is the first sector of the
 * track, so an implementation that forgets the "- 1" reads sector 2's
 * data when asked for sector 1, and -- the symptom that sends people
 * looking in the wrong place -- cannot read the boot sector at all.
 *
 * ---------------------------------------------------------------------
 * The device the caller attaches
 *
 * One 1.44 MB floppy is what this machine has: eighty cylinders, two
 * heads, eighteen sectors of 512 bytes. The image is whatever the caller
 * passes and may be shorter than a whole floppy, which is what a test
 * wants; a request that reaches past its end fails rather than reading
 * whatever follows it in host memory.
 */
#ifndef VM86_BIOS13_H
#define VM86_BIOS13_H

#include <stdbool.h>
#include <stdint.h>

#include <vm86/cpu.h>

/* ------------------------------------------------------------------ */
/* The geometry this machine has                                       */
/* ------------------------------------------------------------------ */

#define BIOS13_SECTOR_BYTES     512u

#define BIOS13_FLOPPY_CYLINDERS 80u
#define BIOS13_FLOPPY_HEADS     2u
#define BIOS13_FLOPPY_SECTORS   18u

/* 1 474 560 bytes: the size of the image `tools/mkfloppy.py` builds and
 * the size of the floppy this machine claims to have. */
#define BIOS13_FLOPPY_BYTES     (BIOS13_FLOPPY_CYLINDERS * \
                                 BIOS13_FLOPPY_HEADS * \
                                 BIOS13_FLOPPY_SECTORS * \
                                 BIOS13_SECTOR_BYTES)

#define BIOS13_FLOPPY_TOTAL_SECTORS \
    (BIOS13_FLOPPY_CYLINDERS * BIOS13_FLOPPY_HEADS * BIOS13_FLOPPY_SECTORS)

/* The drive numbers. Only the first floppy exists: 80h is a hard disk and
 * this machine has none, so asking for one is an error rather than a
 * quietly-served floppy -- a program looking for C: that gets A:'s
 * contents does something much stranger than fail. */
#define BIOS13_DRIVE_A          0x00u
#define BIOS13_DRIVE_C          0x80u
#define BIOS13_FLOPPY_DRIVES    1u      /* how many DL=0..7Fh drives exist */

/* ------------------------------------------------------------------ */
/* Function numbers                                                    */
/* ------------------------------------------------------------------ */

/*
 * The numbers are the PC's, from docs/dos-refs.md section 4, not a
 * choice: a program asks for a read with AH=02h because that is what the
 * hardware it was written for answers on.
 *
 * 15h is listed there among the functions but its *returns* are not
 * documented in anything this repository has, and it is deliberately not
 * implemented -- see bios13_service().
 */
#define BIOS13_FN_RESET         0x00u
#define BIOS13_FN_STATUS        0x01u
#define BIOS13_FN_READ          0x02u
#define BIOS13_FN_WRITE         0x03u
#define BIOS13_FN_VERIFY        0x04u
#define BIOS13_FN_PARAMS        0x08u
#define BIOS13_FN_TYPE          0x15u   /* named, not implemented */

/* ------------------------------------------------------------------ */
/* Status codes                                                        */
/* ------------------------------------------------------------------ */

#define BIOS13_STATUS_OK            0x00u
#define BIOS13_STATUS_BAD_COMMAND   0x01u   /* no such function, or a
                                             * request this device cannot
                                             * carry out                 */
#define BIOS13_STATUS_NOT_FOUND     0x04u   /* no such sector             */
#define BIOS13_STATUS_DMA_BOUNDARY  0x09u   /* the buffer spans two pages */
#define BIOS13_STATUS_NO_DRIVE      0x0Cu   /* no such drive              */

/*
 * 02h ("address mark not found") and 10h ("uncorrectable CRC error") are
 * deliberately absent: both name a failure of the controller's own read
 * of the track, and there is no controller here to fail that way.
 * Nothing in this file produces them.
 */

/* The slice of AH that CL splits between the sector number and the top
 * of the cylinder. Named because the shift is the mistake, not the mask.
 * dos-refs.md section 4 gives the packing as
 *
 *     CX := ((cylinder and 255) shl 8) or ((cylinder and 768) shr 2)
 *           or sector
 *
 * which is this, written the other way round. */
#define BIOS13_CL_SECTOR_MASK   0x3Fu
#define BIOS13_CL_CYLINDER_SHIFT 6

/*
 * How many sectors one call may ask for, from dos-refs.md section 4: the
 * documented range is 1 to 128, and 128 is what fits in a 64 KiB DMA
 * page.
 *
 * Only the bottom of that range is enforced, in bios13_check. A call for
 * more than 128 sectors is refused by the boundary rule on its own for
 * reads and writes, with the status the hardware uses (09h), and a
 * second check in front of it would answer the same question with a
 * different code. There is deliberately no constant here for the top of
 * the range: a limit that nothing enforces reads like one that does.
 */

/* ------------------------------------------------------------------ */
/* The device                                                          */
/* ------------------------------------------------------------------ */

/*
 * One disk, owned by whoever built it.
 *
 * The geometry is a field rather than a constant so that a test can
 * describe a device that is not the standard floppy -- a huge one, to
 * check that a cylinder number above 255 decodes -- without a second
 * implementation of the address arithmetic appearing next to this one.
 */
struct bios13_disk {
    uint8_t *image;      /* the bytes, owned by the caller */
    uint32_t size;       /* how many of them there are */

    uint16_t cylinders;
    uint8_t  heads;
    uint8_t  sectors;    /* per track; the largest sector number */

    /*
     * What the last call reported, which is what AH=01h hands back.
     * Successful calls store zero here rather than leaving the previous
     * failure standing: "the last operation" means the last one, and a
     * program that checks 01h after a call that worked has to see that it
     * worked.
     */
    uint8_t  last_status;
};

/* Attach an image and give the disk the standard floppy geometry. */
void bios13_init(struct bios13_disk *disk, uint8_t *image, uint32_t size);

/*
 * The service. Register it for 13h and pass the disk as the context:
 *
 *     vm86_register_service(VM86_INT_DISK, bios13_service, &disk);
 *
 * A NULL context is a machine with no disk: every function except the
 * reset reports 0Ch, which is what a caller should see.
 *
 * On AL = 0: refused, with CF set and 01h in AH.
 *
 * The folklore answer is eighteen sectors, and it was written into the
 * first draft of this task before anyone looked it up. docs/dos-refs.md
 * section 4 gives the documented range as 1 to 128 and is explicit that
 * the eighteen has no source in anything it found -- so it is a memory,
 * not a fact, and it does not get to decide what a program sees.
 *
 * Refusing is the choice because the alternatives fail quietly. Moving
 * nothing and reporting success leaves the caller with a buffer it
 * believes was filled. Moving eighteen sectors fills a buffer that a
 * caller asking for "no sectors" -- if it meant anything at all -- has no
 * reason to have made that large, so a wrong guess corrupts memory
 * instead of returning an error. A refusal is visible and costs a
 * program that relied on the folklore a loud failure rather than a
 * silent one.
 *
 * On 15h: not implemented, and it reports 01h like any other function
 * this machine does not have. That is a decision, not an omission.
 * docs/dos-refs.md section 4 lists the function but nothing in this
 * repository says what it *returns* -- not the type codes, not what the
 * pair of registers means. Implementing it from memory would let a
 * program take a path we made up; refusing it sends the program down the
 * other road, which the same section does document: 08h, whose answers
 * this file already gives.
 */
void bios13_service(struct vm86_cpu *cpu, void *ctx);

/* ------------------------------------------------------------------ */
/* Decoding an address                                                 */
/* ------------------------------------------------------------------ */

/*
 * The cylinder a CH/CL pair names, and the sector CL names on its own.
 *
 * Exposed because the top two bits of CL are the part of this encoding
 * that a floppy cannot show: eighty cylinders fit in eight bits, so those
 * two are zero on every request a real disk will ever see, and a decoder
 * that drops them works perfectly until something asks for a cylinder
 * past 255. A test cannot reach that case through the service on a
 * standard floppy -- the request fails as out of range first, whichever
 * way it was decoded -- so the decoder is what has to be tested.
 */
uint16_t bios13_cylinder(uint8_t ch, uint8_t cl);
uint8_t  bios13_sector(uint8_t cl);

/*
 * Where a cylinder, head and sector land in the image.
 *
 * `sector` is one-based, as it is on the wire; it must be at least one.
 * The formula is the one the hardware implies:
 *
 *     lba = ((cylinder * heads) + head) * sectors + (sector - 1)
 *
 * with the head varying fastest and the sector within the track fastest
 * of all. Nothing here wraps: the caller has already checked that the
 * three values are ones this disk has.
 */
uint32_t bios13_lba(const struct bios13_disk *disk, uint16_t cylinder,
                    uint8_t head, uint8_t sector);

#endif /* VM86_BIOS13_H */
