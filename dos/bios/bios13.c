/*
 * INT 13h: the disk.
 *
 * See bios13.h for what this device is and for the two rules that are
 * easiest to get wrong -- errors live in the carry flag, and sectors are
 * numbered from one. What follows is the order in which a request is
 * judged, which is a decision rather than a fact, and is written down so
 * that the next person does not have to recover it from the tests.
 *
 *   1. Which drive. A drive that is not A: is 01h, before anything else
 *      is even looked at -- a drive number is a parameter, and 01h is
 *      the code for a function or parameter this device cannot take. See
 *      the note in bios13.h for why it is not 0Ch.
 *   2. How many sectors. Zero is not a count, and is 01h.
 *   3. Which sector. A cylinder, head or sector the geometry does not
 *      have is 04h, and so is a run that starts inside the image and ends
 *      past its end.
 *   4. Where in memory. A read or a write whose buffer spans a 64 KiB
 *      boundary is 09h. Verify has no buffer and skips this.
 *
 * Nothing is transferred until all four have passed, so a failed call
 * leaves the guest's buffer exactly as it was. A half-done read is worse
 * than a failed one: the caller tests the carry flag, sees a failure, and
 * then has a buffer that is neither the old contents nor the new ones.
 *
 * Every answer here comes from docs/dos-refs.md section 4 unless the
 * comment on it says otherwise. Where that section has nothing, the code
 * refuses rather than inventing -- which is why there is no 15h, and why
 * AL = 0 is an error. The report lists each one and what was decided.
 */
#include "bios13.h"

#include <stddef.h>

#include <vm86/decode.h>
#include <vm86/mem.h>

/* ------------------------------------------------------------------ */
/* Decoding an address                                                 */
/* ------------------------------------------------------------------ */

uint16_t bios13_cylinder(uint8_t ch, uint8_t cl)
{
    /*
     * The cylinder is ten bits wide and is split across two registers:
     * its low eight bits in CH, its top two in the top two bits of CL.
     * The mask is what keeps the sector, which lives in CL's low six
     * bits, out of the cylinder.
     */
    return (uint16_t)((((uint16_t)cl >> BIOS13_CL_CYLINDER_SHIFT) << 8) | ch);
}

uint8_t bios13_sector(uint8_t cl)
{
    return (uint8_t)(cl & BIOS13_CL_SECTOR_MASK);
}

uint32_t bios13_lba(const struct bios13_disk *disk, uint16_t cylinder,
                    uint8_t head, uint8_t sector)
{
    return ((uint32_t)cylinder * disk->heads + head) * disk->sectors
           + (uint32_t)(sector - 1);
}

/* ------------------------------------------------------------------ */
/* The device                                                          */
/* ------------------------------------------------------------------ */

void bios13_init(struct bios13_disk *disk, uint8_t *image, uint32_t size)
{
    disk->image       = image;
    disk->size        = size;
    disk->cylinders   = BIOS13_FLOPPY_CYLINDERS;
    disk->heads       = BIOS13_FLOPPY_HEADS;
    disk->sectors     = BIOS13_FLOPPY_SECTORS;
    disk->last_status = BIOS13_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

/*
 * Success and failure are one call each so that the pair -- CF with AH,
 * CF with AL -- is written once. They were two lines at every return
 * until one of them was missed.
 */
static void bios13_ok(struct vm86_cpu *cpu, struct bios13_disk *disk)
{
    disk->last_status = BIOS13_STATUS_OK;
    cpu->ah           = BIOS13_STATUS_OK;
    vm86_flag_set(cpu, VM86_CF, false);
}

static void bios13_fail(struct vm86_cpu *cpu, struct bios13_disk *disk,
                        uint8_t status)
{
    disk->last_status = status;
    cpu->ah           = status;

    /* No sectors moved, so the count of them is zero. Leaving the
     * requested count standing would tell a caller that ignores CF that
     * the transfer happened. */
    cpu->al = 0;

    vm86_flag_set(cpu, VM86_CF, true);
}

/* ------------------------------------------------------------------ */
/* Reading the request                                                 */
/* ------------------------------------------------------------------ */

struct bios13_request {
    uint32_t lba;      /* the first sector, in the image's numbering */
    uint32_t bytes;    /* how many bytes the whole run is             */
    uint32_t address;  /* where in the guest it goes, for reads/writes */
    uint8_t  count;    /* sectors, after AL = 0 has been resolved     */
};

/*
 * Everything up to the transfer, including all three judgements above.
 * Returns a status; anything but OK means the caller reports it and
 * transfers nothing.
 */
static uint8_t bios13_check(const struct bios13_disk *disk,
                            struct vm86_cpu *cpu, bool uses_dma,
                            struct bios13_request *request)
{
    uint8_t  drive    = cpu->dl;
    uint16_t cylinder = bios13_cylinder(cpu->ch, cpu->cl);
    uint8_t  sector   = bios13_sector(cpu->cl);
    uint8_t  head     = cpu->dh;
    uint8_t  count    = cpu->al;

    if (drive != BIOS13_DRIVE_A)
        return BIOS13_STATUS_BAD_COMMAND;

    /*
     * A call that asks for no sectors is refused rather than answered.
     * The alternatives are a silent no-op, which leaves the caller unable
     * to tell a successful call from a call that did nothing, and
     * inventing a count -- the folklore says eighteen -- which fills a
     * buffer that a caller asking for nothing has no reason to have made
     * that large. See the note on bios13_service in the header.
     */
    if (count == 0)
        return BIOS13_STATUS_BAD_COMMAND;

    /* A drive letter is not the only thing that has to match: an image
     * attached to drive A: can still be shorter than a floppy, and a
     * request for a sector it does not have is the same failure as one
     * for a sector the geometry does not have. */
    if (sector == 0 ||
        sector > disk->sectors ||
        head >= disk->heads ||
        cylinder >= disk->cylinders)
        return BIOS13_STATUS_NOT_FOUND;

    request->count = count;
    request->lba   = bios13_lba(disk, cylinder, head, sector);
    request->bytes = (uint32_t)count * BIOS13_SECTOR_BYTES;

    /*
     * One sector's worth past the end is past the end. The comparison is
     * in bytes and in 64-bit arithmetic so that a run of 255 sectors at
     * a high cylinder cannot wrap its way to looking short.
     *
     * It is also the only bounds check there is, which is not something
     * this line shows. bios13_transfer() indexes disk->image[] directly
     * and tests nothing, so what this returns on failure is not the
     * interesting part: the interesting part is that it is what keeps a
     * read and a write inside the caller's array at all. Deleting it does
     * not lose an error code -- the transfer runs off the end of somebody
     * else's buffer. The audit that tried it got a segmentation fault
     * rather than a failing case (docs/tasks/M4-bios13-audit-report.md,
     * section 8).
     *
     * So anyone who relaxes this comparison -- a different unit, 32-bit
     * arithmetic, "warn and carry on" -- has to give bios13_transfer a
     * bound of its own first.
     */
    if ((uint64_t)request->lba * BIOS13_SECTOR_BYTES + request->bytes >
        disk->size)
        return BIOS13_STATUS_NOT_FOUND;

    if (!uses_dma) {
        request->address = 0;
        return BIOS13_STATUS_OK;
    }

    request->address = vm86_linear(cpu, VM86_ES, cpu->bx);

    /*
     * The controller's DMA channel counts within a 64 KiB page and
     * carries no way to say "and the next page too", so a buffer that
     * spans two of them is refused rather than split. The test is on the
     * low sixteen bits of the address: a run that ends exactly on the
     * boundary is inside one page and is allowed.
     */
    if (((request->address & 0xFFFFu) + request->bytes) > 0x10000u)
        return BIOS13_STATUS_DMA_BOUNDARY;

    return BIOS13_STATUS_OK;
}

/*
 * Move the sectors. Every address and length here has already been
 * checked, so this cannot fail and cannot run off either end.
 *
 * That sentence is the whole of this function's bounds checking, and it
 * is why the end-of-image comparison in bios13_check() is load-bearing
 * for memory safety rather than only for the status it returns. See the
 * note on it there.
 */
static void bios13_transfer(struct vm86_cpu *cpu, struct bios13_disk *disk,
                            const struct bios13_request *request, bool write)
{
    uint32_t offset = request->lba * BIOS13_SECTOR_BYTES;

    for (uint32_t i = 0; i < request->bytes; i++) {
        if (write)
            disk->image[offset + i] =
                vm86_mem_read8(cpu->mem, request->address + i);
        else
            vm86_mem_write8(cpu->mem, request->address + i,
                            disk->image[offset + i]);
    }
}

/* ------------------------------------------------------------------ */
/* The service                                                         */
/* ------------------------------------------------------------------ */

void bios13_service(struct vm86_cpu *cpu, void *ctx)
{
    struct bios13_disk *disk = (struct bios13_disk *)ctx;
    uint8_t function         = cpu->ah;

    /*
     * Reset first, before the no-disk test, because resetting is the one
     * thing that works on a machine with no disk: there is no controller
     * to reset, so there is nothing to fail. What it does do is forget
     * the last error, which is what a program that resets and retries is
     * asking for.
     */
    if (function == BIOS13_FN_RESET) {
        if (disk != NULL)
            disk->last_status = BIOS13_STATUS_OK;

        cpu->ah = BIOS13_STATUS_OK;
        vm86_flag_set(cpu, VM86_CF, false);
        return;
    }

    if (disk == NULL) {
        cpu->ah = BIOS13_STATUS_BAD_COMMAND;
        cpu->al = 0;
        vm86_flag_set(cpu, VM86_CF, true);
        return;
    }

    /*
     * Status: hand back what the last call stored, and nothing else. It
     * is the one function that does not update the remembered status --
     * reporting it must not overwrite it -- and the carry flag is set
     * to match, so a caller that checks only CF gets the same answer as
     * one that reads AH.
     */
    if (function == BIOS13_FN_STATUS) {
        cpu->ah = disk->last_status;
        vm86_flag_set(cpu, VM86_CF, disk->last_status != BIOS13_STATUS_OK);
        return;
    }

    switch (function) {
    case BIOS13_FN_READ:
    case BIOS13_FN_WRITE:
    case BIOS13_FN_VERIFY: {
        bool     write  = function == BIOS13_FN_WRITE;
        bool     verify = function == BIOS13_FN_VERIFY;
        uint8_t  status;

        struct bios13_request request;

        /* Verify takes no buffer, so the boundary that matters to it is
         * the sector's, not the guest's. Asking it to check a DMA
         * address would invent a failure the hardware does not have. */
        status = bios13_check(disk, cpu, !verify, &request);

        if (status != BIOS13_STATUS_OK) {
            bios13_fail(cpu, disk, status);
            return;
        }

        if (!verify)
            bios13_transfer(cpu, disk, &request, write);

        cpu->al = request.count;
        bios13_ok(cpu, disk);
        return;
    }

    case BIOS13_FN_PARAMS: {
        if (cpu->dl != BIOS13_DRIVE_A) {
            bios13_fail(cpu, disk, BIOS13_STATUS_BAD_COMMAND);
            return;
        }

        /*
         * The three numbers are maxima, and they are the largest value
         * each field can hold -- not how many there are. Cylinders and
         * heads are counted from zero, so theirs are one less than the
         * count; sectors are numbered from one, so that one is not. A
         * program that sizes the disk from this answer multiplies
         * (cylinders + 1) by (heads + 1) by sectors, and an answer that
         * subtracted one from the sector, or failed to subtract one from
         * the heads, gives it a disk that is one row or one track short.
         */
        uint16_t max_cylinder = (uint16_t)(disk->cylinders - 1);

        cpu->ch = (uint8_t)(max_cylinder & 0xFFu);
        cpu->cl = (uint8_t)(((max_cylinder >> 8) & 0x03u)
                                << BIOS13_CL_CYLINDER_SHIFT
                            | disk->sectors);
        cpu->dh = (uint8_t)(disk->heads - 1);

        /* DL becomes the number of drives, which is one. It was the drive
         * number on the way in and a program is not entitled to both.
         * BL is the drive type, from dos-refs.md section 4.
         *
         * ES:DI is supposed to point at the drive's eleven-byte parameter
         * table and is left alone. That is a known gap rather than an
         * oversight: pointing it at a table means deciding where in guest
         * memory that table lives, which is an interface decision (on the
         * hardware it is in ROM and IVT[1Eh] points at it), and nothing
         * in M4 reads it. A made-up address would replace a stated gap
         * with something that looks finished. */
        cpu->dl = BIOS13_FLOPPY_DRIVES;
        cpu->bl = BIOS13_DRIVE_TYPE_1_44M;

        bios13_ok(cpu, disk);
        return;
    }

    default:
        /*
         * Anything else is not a function this machine has, and says so
         * rather than returning success. A program that probes for a
         * capability by calling it and looking at the carry flag is
         * entitled to the real answer, and a silent success makes it
         * think it found one.
         *
         * 15h lands here on purpose. It is a real function, and the
         * answer a program gets -- no such function -- sends it to the
         * road this machine does have, which is 08h. See bios13.h.
         */
        bios13_fail(cpu, disk, BIOS13_STATUS_BAD_COMMAND);
        return;
    }
}
