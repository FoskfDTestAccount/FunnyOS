/*
 * An independent audit of dos/bios/bios13.{c,h} -- INT 13h.
 *
 * ---------------------------------------------------------------------
 * What this file is, and how it was written
 *
 * Task D wrote that module and its own suite. This is a second pair of
 * eyes: the cases below were derived from docs/dos-refs.md section 4 (the
 * project's own second-hand reference for the service) and from the
 * geometry and buffer rules in bios13.h, and then run against the
 * implementation. Where an expectation here disagrees with
 * test_bios13.c, that is the interesting part and it is written up in
 * docs/tasks/M4-bios13-audit-report.md.
 *
 * The things section 4 does not settle are NOT asserted here, because a
 * case whose expected value is still being argued about cannot fail
 * usefully. The two that matter are `08h`'s BL and ES:DI -- see the
 * report. Everything asserted below has a source.
 *
 * ---------------------------------------------------------------------
 * Why the seam cases look the way they do
 *
 * "The next sector after the last one on a track is the first sector of
 * the next head" is a statement about two *adjacent* sectors, not about
 * one address. A case that wrote to (0,0,18) and read back from (0,1,1)
 * would be asking whether two different places are the same place, and it
 * would fail against a correct implementation. So each seam is checked by
 * writing distinct patterns at the two addresses and asserting that they
 * land in adjacent parts of the image.
 *
 * ---------------------------------------------------------------------
 * Numbers used below
 *
 * The standard geometry is 80 cylinders, 2 heads, 18 sectors, so
 *
 *     lba = (cylinder * 2 + head) * 18 + (sector - 1)
 *
 * and the values that appear over and over are 17/18 (the track-to-head
 * seam) and 35/36 (the head-to-cylinder seam). They are written out as
 * literals with the arithmetic beside them rather than recomputed, so
 * that a mistake in the arithmetic here shows up as a failure rather than
 * as two copies of the same mistake agreeing.
 */
#include "harness.h"

#include <stdio.h>

#include <vm86/firmware.h>
#include <vm86/host.h>

#include "../bios/bios13.h"

#define SECTOR 512u

/*
 * A test image, and the disk that describes it.
 *
 * These are static rather than automatic because they are tens of
 * kilobytes, and because the harness runs each case with the guest's
 * memory already attached -- the image is the caller's memory, not the
 * guest's, which is the whole of what bios13.h means by "the array is the
 * caller's".
 */
static uint8_t g_image[128u * SECTOR];        /* 128 sectors: lba 0..127 */
static uint8_t g_big[600u * SECTOR];          /* for cylinders past 255  */
static uint8_t g_short[8u * SECTOR];          /* a disk shorter than it says */
static uint8_t g_round[288u * SECTOR];        /* 8 cylinders x 2 x 18    */

/* ------------------------------------------------------------------ */
/* Plumbing                                                            */
/* ------------------------------------------------------------------ */

/*
 * Where guest buffers go. Both are ordinary guest RAM: past the vector
 * table and the data area, below the video window, and far from the
 * loader's 0x100.
 */
#define BUF_A_SEG 0x1000u
#define BUF_A_LIN 0x10000u
#define BUF_B_SEG 0x2000u
#define BUF_B_LIN 0x20000u

static void reset_image(void)
{
    for (unsigned i = 0; i < sizeof(g_image); i++)
        g_image[i] = 0;

    for (unsigned i = 0; i < sizeof(g_big); i++)
        g_big[i] = 0;

    for (unsigned i = 0; i < sizeof(g_short); i++)
        g_short[i] = 0;

    for (unsigned i = 0; i < sizeof(g_round); i++)
        g_round[i] = 0;
}

static void fill_guest(struct vm86_cpu *cpu, uint32_t linear,
                       uint8_t value, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; i++)
        vm86_mem_write8(cpu->mem, linear + i, value);
}

static bool guest_is(struct vm86_cpu *cpu, uint32_t linear,
                     uint8_t value, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; i++) {
        if (vm86_mem_read8(cpu->mem, linear + i) != value)
            return false;
    }

    return true;
}

static bool image_is(const uint8_t *image, uint32_t offset,
                     uint8_t value, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; i++) {
        if (image[offset + i] != value)
            return false;
    }

    return true;
}

/*
 * Load a request into the registers, the way a program does.
 *
 * The cylinder is split here exactly as docs/dos-refs.md section 4 says
 * the *program* splits it, which is the same packing 08h hands back --
 * that is the point of the encoding, and one case below feeds an 08h
 * answer straight into this.
 */
static void request(struct vm86_cpu *cpu, uint8_t function, uint8_t count,
                    uint16_t cylinder, uint8_t head, uint8_t sector,
                    uint8_t drive, uint16_t seg, uint16_t bx)
{
    cpu->ah = function;
    cpu->al = count;
    cpu->ch = (uint8_t)(cylinder & 0xFFu);
    cpu->cl = (uint8_t)((((cylinder >> 8) & 0x03u) << 6) | (sector & 0x3Fu));
    cpu->dh = head;
    cpu->dl = drive;
    cpu->bx = bx;

    vm86_set_seg(cpu, VM86_ES, seg);
    vm86_flush_segments(cpu);
}

/* The two flags that have to be set together, read together. */
static void expect_outcome(const char *what, struct vm86_cpu *cpu,
                           bool failed, uint16_t status)
{
    char detail[96];

    snprintf(detail, sizeof(detail), "%s: CF", what);
    vm86_expect_bool(detail, vm86_flag_test(cpu, VM86_CF), failed);

    snprintf(detail, sizeof(detail), "%s: AH", what);
    vm86_expect_u16(detail, cpu->ah, status);
}

/*
 * A sector number, compared as the thirty-two-bit value it is.
 *
 * The harness's own comparison is sixteen bits, and an lba that came back
 * 65536 too large would be truncated to the number the case is looking
 * for -- a passing assertion about a decoder that is wrong. The failure
 * message carries the number for the same reason.
 */
static void expect_lba(const char *what, uint32_t got, uint32_t want)
{
    char detail[128];

    snprintf(detail, sizeof(detail), "%s: wanted %u, got %u",
             what, (unsigned)want, (unsigned)got);

    vm86_expect_bool(detail, got == want, true);
}

/* ------------------------------------------------------------------ */
/* Decoding an address, from section 4's own formula                   */
/* ------------------------------------------------------------------ */

/*
 * Section 4 gives the packing as
 *
 *     CX := ((cylinder and 255) shl 8) or ((cylinder and 768) shr 2)
 *           or sector
 *
 * so the low eight bits are CH and the top two are CL's top two. The
 * pairs below are chosen to separate the two top bits from each other:
 * 256 is bit 8 alone, 512 is bit 9 alone, and 300 has bit 8 set with bit
 * 9 clear while 0x41's neighbour 0x81 has the same low byte -- a decoder
 * that dropped either bit passes one of these and fails another.
 */
static void test_the_cylinder_decoder(struct vm86_cpu *cpu)
{
    (void)cpu;

    vm86_expect_u16("low byte from CH, high bits zero",
                    bios13_cylinder(0x01, 0x00), 1);
    vm86_expect_u16("bit 8 of the cylinder is CL bit 6",
                    bios13_cylinder(0x00, 0x40), 0x100);
    vm86_expect_u16("bit 8, low byte set",
                    bios13_cylinder(0x2C, 0x41), 300);
    vm86_expect_u16("bit 9 of the cylinder is CL bit 7",
                    bios13_cylinder(0x00, 0x80), 0x200);
    vm86_expect_u16("bit 9, low byte clear",
                    bios13_cylinder(0x00, 0x82), 512);
    vm86_expect_u16("both top bits",
                    bios13_cylinder(0xFF, 0xC0), 0x3FF);
}

static void test_the_sector_decoder_ignores_the_cylinder_bits(
        struct vm86_cpu *cpu)
{
    (void)cpu;

    vm86_expect_u16("sector 1 alone", bios13_sector(0x01), 1);
    vm86_expect_u16("sector 18 alone", bios13_sector(0x12), 18);
    vm86_expect_u16("sector 18 with the cylinder bits on top",
                    bios13_sector(0xD2), 18);
    vm86_expect_u16("the largest sector six bits hold",
                    bios13_sector(0x3F), 63);
}

/*
 * The three seams, as arithmetic.
 *
 * Sector 18 is the last on a track and sector 1 is the first of the next,
 * so those two are neighbours in the image -- as are the last sector of
 * the last head and the first sector of the next cylinder. Writing these
 * as four literals is what makes "one off" visible.
 */
static void test_the_seams_are_adjacent(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    (void)cpu;

    bios13_init(&disk, g_image, sizeof(g_image));

    expect_lba("the first sector is sector 1",
               bios13_lba(&disk, 0, 0, 1), 0);
    expect_lba("last sector of track 0, head 0",
               bios13_lba(&disk, 0, 0, 18), 17);
    expect_lba("first sector of the next head is the next one",
               bios13_lba(&disk, 0, 1, 1), 18);
    expect_lba("last sector of head 1",
               bios13_lba(&disk, 0, 1, 18), 35);
    expect_lba("after the last head comes the next cylinder",
               bios13_lba(&disk, 1, 0, 1), 36);
    expect_lba("and the last sector of the disk",
               bios13_lba(&disk, 79, 1, 18), 2879);
}

/* ------------------------------------------------------------------ */
/* The seams, through the service                                      */
/* ------------------------------------------------------------------ */

static void test_a_read_crosses_the_head_seam(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    reset_image();
    bios13_init(&disk, g_image, sizeof(g_image));

    /* Two sectors, told apart: 0x11 at the last of head 0, 0x22 at the
     * first of head 1. */
    for (uint32_t i = 0; i < SECTOR; i++) {
        g_image[17u * SECTOR + i] = 0x11;
        g_image[18u * SECTOR + i] = 0x22;
    }

    fill_guest(cpu, BUF_A_LIN, 0xEE, 2u * SECTOR);

    request(cpu, BIOS13_FN_READ, 2, 0, 0, 18, BIOS13_DRIVE_A,
            BUF_A_SEG, 0);
    bios13_service(cpu, &disk);

    expect_outcome("read across the track-to-head seam", cpu, false, 0x00);
    vm86_expect_u16("two sectors moved", cpu->al, 2);

    vm86_expect_bool("the last sector of the track arrived first",
                     guest_is(cpu, BUF_A_LIN, 0x11, SECTOR), true);
    vm86_expect_bool("and the next head's first sector right after it",
                     guest_is(cpu, BUF_A_LIN + SECTOR, 0x22, SECTOR), true);
}

static void test_a_read_crosses_the_cylinder_seam(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    reset_image();
    bios13_init(&disk, g_image, sizeof(g_image));

    for (uint32_t i = 0; i < SECTOR; i++) {
        g_image[35u * SECTOR + i] = 0x33;
        g_image[36u * SECTOR + i] = 0x44;
    }

    fill_guest(cpu, BUF_A_LIN, 0xEE, 2u * SECTOR);

    request(cpu, BIOS13_FN_READ, 2, 0, 1, 18, BIOS13_DRIVE_A,
            BUF_A_SEG, 0);
    bios13_service(cpu, &disk);

    expect_outcome("read across the head-to-cylinder seam", cpu, false, 0x00);

    vm86_expect_bool("the last sector of the cylinder arrived first",
                     guest_is(cpu, BUF_A_LIN, 0x33, SECTOR), true);
    vm86_expect_bool("and cylinder 1 head 0 sector 1 right after it",
                     guest_is(cpu, BUF_A_LIN + SECTOR, 0x44, SECTOR), true);
}

/*
 * The top two bits of CL, through the service.
 *
 * A standard floppy cannot show these: eighty cylinders fit in eight bits,
 * so a request that uses them fails the range check whichever way it was
 * decoded. The disk's geometry is a field precisely so that this case can
 * exist -- and it is the case that proves the service uses the decoder
 * rather than merely that the decoder is right.
 *
 * The geometry is set by hand to 1024 cylinders, one head, one sector, so
 * that cylinder N is image offset N * 512 and the arithmetic in the
 * assertion is the cylinder number itself.
 */
static void test_a_cylinder_past_255_reaches_the_image(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    reset_image();
    bios13_init(&disk, g_big, sizeof(g_big));
    disk.cylinders = 1024;
    disk.heads     = 1;
    disk.sectors   = 1;

    static const uint16_t cylinders[] = { 256, 300, 512 };
    static const uint8_t  marks[]     = { 0xAA, 0xBB, 0xCC };

    for (unsigned i = 0; i < 3; i++) {
        request(cpu, BIOS13_FN_WRITE, 1, cylinders[i], 0, 1, BIOS13_DRIVE_A,
                BUF_A_SEG, 0);

        fill_guest(cpu, BUF_A_LIN, marks[i], SECTOR);
        bios13_service(cpu, &disk);

        char detail[80];
        snprintf(detail, sizeof(detail), "write to cylinder %u", cylinders[i]);
        expect_outcome(detail, cpu, false, 0x00);
    }

    for (unsigned i = 0; i < 3; i++) {
        char detail[80];
        snprintf(detail, sizeof(detail),
                 "cylinder %u landed at offset %u",
                 cylinders[i], (unsigned)cylinders[i] * SECTOR);

        vm86_expect_bool(detail,
                         image_is(g_big, (uint32_t)cylinders[i] * SECTOR,
                                  marks[i], SECTOR), true);
    }
}

/* ------------------------------------------------------------------ */
/* CF and AH are one answer, not two                                   */
/* ------------------------------------------------------------------ */

/*
 * Both flags start out wrong on purpose.
 *
 * The harness resets the machine, so CF is zero and a successful call
 * would pass a check on CF even if the service never touched it. Setting
 * CF before the call is the only way to ask "did this service clear it"
 * rather than "was it already clear".
 */
static void test_success_sets_cf_and_ah_together(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    reset_image();
    bios13_init(&disk, g_image, sizeof(g_image));

    cpu->flags |= VM86_CF;      /* left over from a previous failure */
    cpu->ah = 0xAB;             /* and so is this */

    request(cpu, BIOS13_FN_READ, 1, 0, 0, 1, BIOS13_DRIVE_A, BUF_A_SEG, 0);
    bios13_service(cpu, &disk);

    expect_outcome("a successful read clears both", cpu, false, 0x00);
}

static void test_failure_sets_cf_and_ah_together(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    reset_image();
    bios13_init(&disk, g_image, sizeof(g_image));

    cpu->flags &= (uint16_t)~VM86_CF;   /* left over from a success */
    cpu->ah = 0x00;

    request(cpu, BIOS13_FN_READ, 1, 0, 0, 99, BIOS13_DRIVE_A, BUF_A_SEG, 0);
    bios13_service(cpu, &disk);

    expect_outcome("a read past the geometry sets both", cpu, true, 0x04);
}

/* ------------------------------------------------------------------ */
/* A failed call changes nothing                                       */
/* ------------------------------------------------------------------ */

/*
 * Every rejection reason, and after each one the buffer has to hold
 * exactly what it held before. A half-done read is worse than a failed
 * one: the caller sees the failure and then has a buffer that is neither
 * the old contents nor the new.
 */
static void test_a_rejected_read_leaves_the_buffer(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    reset_image();
    bios13_init(&disk, g_image, sizeof(g_image));

    /* Something on the image, so a partial transfer would be visible. */
    for (unsigned i = 0; i < sizeof(g_image); i++)
        g_image[i] = 0x5A;

    struct {
        const char *what;
        uint8_t  count;
        uint16_t cylinder;
        uint8_t  head, sector, drive;
        uint16_t seg, bx;
        uint16_t status;
    } cases[] = {
        { "a drive that is not A:",      1, 0, 0,  1, 0x80, BUF_A_SEG, 0, 0x01 },
        { "no sectors at all",           0, 0, 0,  1, 0x00, BUF_A_SEG, 0, 0x01 },
        { "sector zero",                 1, 0, 0,  0, 0x00, BUF_A_SEG, 0, 0x04 },
        { "a sector the track has not",  1, 0, 0, 19, 0x00, BUF_A_SEG, 0, 0x04 },
        { "a head the geometry has not", 1, 0, 2,  1, 0x00, BUF_A_SEG, 0, 0x04 },
        { "a cylinder the geometry has not",
                                         1, 80, 0, 1, 0x00, BUF_A_SEG, 0, 0x04 },
        /* lba 126 is the last whole sector the image has: the first
         * sector here is legal by the geometry and by the image, and the
         * run's *end* is not. */
        { "a run that ends past the image",
                                         4, 3, 1,  1, 0x00, BUF_A_SEG, 0, 0x04 },
        { "a buffer that spans two pages",
                                         2, 0, 0,  1, 0x00, 0x2000, 0xFE00, 0x09 },
        { "a buffer starting in the last page",
                                         1, 0, 0,  1, 0x00, 0x2000, 0xFF00, 0x09 },
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fill_guest(cpu, BUF_A_LIN, 0xEE, 4u * SECTOR);

        request(cpu, BIOS13_FN_READ, cases[i].count, cases[i].cylinder,
                cases[i].head, cases[i].sector, cases[i].drive,
                cases[i].seg, cases[i].bx);
        bios13_service(cpu, &disk);

        expect_outcome(cases[i].what, cpu, true, cases[i].status);

        char detail[96];
        snprintf(detail, sizeof(detail), "%s: the buffer is untouched",
                 cases[i].what);
        vm86_expect_bool(detail, guest_is(cpu, BUF_A_LIN, 0xEE, 4u * SECTOR),
                         true);
    }
}

static void test_a_rejected_write_leaves_the_image(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    reset_image();
    bios13_init(&disk, g_image, sizeof(g_image));

    for (unsigned i = 0; i < sizeof(g_image); i++)
        g_image[i] = 0x5A;

    fill_guest(cpu, BUF_A_LIN, 0xEE, SECTOR);

    request(cpu, BIOS13_FN_WRITE, 1, 79, 1, 18, BIOS13_DRIVE_A,
            BUF_A_SEG, 0);
    bios13_service(cpu, &disk);

    expect_outcome("a write past the image", cpu, true, 0x04);
    vm86_expect_bool("the image is untouched",
                     image_is(g_image, 0, 0x5A, sizeof(g_image)), true);
}

/* ------------------------------------------------------------------ */
/* Writing, and reading it back somewhere else                         */
/* ------------------------------------------------------------------ */

/*
 * The bug this is shaped to catch is a write that only touches the guest
 * buffer -- or one that lands at the wrong offset. Reading back into the
 * *same* buffer would pass against an implementation that did nothing at
 * all, so the read goes to a second buffer that was filled with
 * something else first, and the image is inspected directly as well.
 */
static void test_a_write_reaches_the_image_and_comes_back(
        struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    reset_image();
    bios13_init(&disk, g_image, sizeof(g_image));

    fill_guest(cpu, BUF_A_LIN, 0x77, SECTOR);

    /* Cylinder 1, head 1, sector 5 -> (1*2 + 1) * 18 + 4 = 58. */
    request(cpu, BIOS13_FN_WRITE, 1, 1, 1, 5, BIOS13_DRIVE_A,
            BUF_A_SEG, 0);
    bios13_service(cpu, &disk);
    expect_outcome("the write", cpu, false, 0x00);

    vm86_expect_bool("it landed at the offset the address names",
                     image_is(g_image, 58u * SECTOR, 0x77, SECTOR), true);

    fill_guest(cpu, BUF_B_LIN, 0x99, SECTOR);

    request(cpu, BIOS13_FN_READ, 1, 1, 1, 5, BIOS13_DRIVE_A, BUF_B_SEG, 0);
    bios13_service(cpu, &disk);
    expect_outcome("and the read back", cpu, false, 0x00);

    vm86_expect_bool("the second buffer has what was written",
                     guest_is(cpu, BUF_B_LIN, 0x77, SECTOR), true);
}

/* ------------------------------------------------------------------ */
/* 08h describes this disk, and its answer feeds 02h                   */
/* ------------------------------------------------------------------ */

/*
 * Section 4 says the 08h answer is laid out the same way 02h's request
 * is, "so that a program can feed one straight into the other". That is
 * testable, and it is a stronger statement than either half alone: it
 * asks the two decoders to agree with each other and with the image.
 *
 * The geometry is set to something that is not the standard floppy, so
 * that an 08h that reported the constants rather than the disk is caught.
 */
static void test_08h_describes_the_disk_and_feeds_02h(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    reset_image();
    bios13_init(&disk, g_round, sizeof(g_round));
    disk.cylinders = 8;
    disk.heads     = 2;
    disk.sectors   = 18;

    request(cpu, BIOS13_FN_PARAMS, 0, 0, 0, 0, BIOS13_DRIVE_A, 0, 0);
    bios13_service(cpu, &disk);

    expect_outcome("08h", cpu, false, 0x00);
    vm86_expect_u16("CH is the largest cylinder's low byte", cpu->ch, 7);
    vm86_expect_u16("CL's low six bits are the largest sector",
                    bios13_sector(cpu->cl), 18);
    vm86_expect_u16("CL's top two bits are the cylinder's",
                    bios13_cylinder(0x00, cpu->cl), 0);
    vm86_expect_u16("DH is the largest head", cpu->dh, 1);
    vm86_expect_u16("DL is how many drives there are", cpu->dl, 1);

    /* The largest address 08h just described, read through 02h. */
    uint8_t ch = cpu->ch;
    uint8_t cl = cpu->cl;
    uint8_t dh = cpu->dh;

    /* The last sector the answer describes: cylinder 7, head 1, sector
     * 18 -> (7*2 + 1) * 18 + 17 = 287. The arithmetic is spelled out
     * here rather than taken from bios13_lba() so that a mistake in the
     * decoder cannot make this case agree with itself. */
    g_round[287u * SECTOR] = 0x42;

    fill_guest(cpu, BUF_A_LIN, 0xEE, SECTOR);

    request(cpu, BIOS13_FN_READ, 1, bios13_cylinder(ch, cl), dh,
            bios13_sector(cl), BIOS13_DRIVE_A, BUF_A_SEG, 0);
    bios13_service(cpu, &disk);

    expect_outcome("the largest address 08h described is readable", cpu,
                   false, 0x00);
    vm86_expect_u16("at the first byte of the last sector",
                    vm86_mem_read8(cpu->mem, BUF_A_LIN), 0x42);
}

/* ------------------------------------------------------------------ */
/* The geometry is not the image                                       */
/* ------------------------------------------------------------------ */

/*
 * bios13.h says the image may be shorter than the disk it describes, and
 * that a request reaching past its end fails. That is a different
 * question from the geometry: the address is legal and the sector is not
 * there.
 */
static void test_a_short_image_fails_past_its_end(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    reset_image();
    bios13_init(&disk, g_short, sizeof(g_short));   /* eight sectors */

    request(cpu, BIOS13_FN_READ, 1, 0, 0, 8, BIOS13_DRIVE_A, BUF_A_SEG, 0);
    bios13_service(cpu, &disk);
    expect_outcome("the eighth sector is there", cpu, false, 0x00);

    fill_guest(cpu, BUF_A_LIN, 0xEE, SECTOR);

    request(cpu, BIOS13_FN_READ, 1, 0, 0, 9, BIOS13_DRIVE_A, BUF_A_SEG, 0);
    bios13_service(cpu, &disk);
    expect_outcome("the ninth is past the end of the image", cpu, true, 0x04);

    vm86_expect_u16("and its low half too",
                    vm86_flag_test(cpu, VM86_CF), true);
}

/* ------------------------------------------------------------------ */
/* 01h, 00h, and functions that are not there                          */
/* ------------------------------------------------------------------ */

static void test_01h_reports_and_keeps_the_last_status(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    reset_image();
    bios13_init(&disk, g_image, sizeof(g_image));

    request(cpu, BIOS13_FN_READ, 1, 0, 0, 99, BIOS13_DRIVE_A, BUF_A_SEG, 0);
    bios13_service(cpu, &disk);
    expect_outcome("the failing read", cpu, true, 0x04);

    request(cpu, BIOS13_FN_STATUS, 0, 0, 0, 0, 0, 0, 0);
    bios13_service(cpu, &disk);
    expect_outcome("01h reports the failure", cpu, true, 0x04);

    /* Twice, because a 01h that stored what it reported would answer
     * differently the second time only if the answer changed. */
    request(cpu, BIOS13_FN_STATUS, 0, 0, 0, 0, 0, 0, 0);
    bios13_service(cpu, &disk);
    expect_outcome("and still reports it afterwards", cpu, true, 0x04);

    request(cpu, BIOS13_FN_RESET, 0, 0, 0, 0, 0, 0, 0);
    bios13_service(cpu, &disk);
    expect_outcome("reset", cpu, false, 0x00);

    request(cpu, BIOS13_FN_STATUS, 0, 0, 0, 0, 0, 0, 0);
    bios13_service(cpu, &disk);
    expect_outcome("01h now reports success", cpu, false, 0x00);
}

static void test_functions_this_machine_does_not_have(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    reset_image();
    bios13_init(&disk, g_image, sizeof(g_image));

    static const uint8_t missing[] = { 0x05, 0x0A, 0x10, 0x15, 0x19, 0x99 };

    for (unsigned i = 0; i < sizeof(missing); i++) {
        fill_guest(cpu, BUF_A_LIN, 0xEE, SECTOR);

        request(cpu, missing[i], 1, 0, 0, 1, BIOS13_DRIVE_A, BUF_A_SEG, 0);
        bios13_service(cpu, &disk);

        char detail[64];
        snprintf(detail, sizeof(detail), "AH=0x%02X", missing[i]);
        expect_outcome(detail, cpu, true, 0x01);

        snprintf(detail, sizeof(detail), "AH=0x%02X moves nothing",
                 missing[i]);
        vm86_expect_bool(detail, guest_is(cpu, BUF_A_LIN, 0xEE, SECTOR), true);
    }
}

static void test_a_machine_with_no_disk(struct vm86_cpu *cpu)
{
    request(cpu, BIOS13_FN_RESET, 0, 0, 0, 0, 0, 0, 0);
    bios13_service(cpu, NULL);
    expect_outcome("reset works with no disk", cpu, false, 0x00);

    request(cpu, BIOS13_FN_READ, 1, 0, 0, 1, BIOS13_DRIVE_A, BUF_A_SEG, 0);
    bios13_service(cpu, NULL);
    expect_outcome("a read does not", cpu, true, 0x01);

    request(cpu, BIOS13_FN_PARAMS, 0, 0, 0, 0, BIOS13_DRIVE_A, 0, 0);
    bios13_service(cpu, NULL);
    expect_outcome("nor does 08h", cpu, true, 0x01);
}

/* ------------------------------------------------------------------ */
/* 04h has no buffer                                                   */
/* ------------------------------------------------------------------ */

/*
 * Verify judges the address and nothing else, because it has no buffer to
 * check and no memory to compare against. Both halves matter: a verify
 * that reused the read path's page check would invent a failure the
 * hardware does not have, and one that compared against ES:BX would fail
 * for every caller that did not set it.
 */
static void test_verify_does_not_use_a_buffer(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    reset_image();
    bios13_init(&disk, g_image, sizeof(g_image));

    fill_guest(cpu, BUF_A_LIN, 0xEE, 2u * SECTOR);

    /* A buffer that a read would refuse: it spans two pages. */
    request(cpu, BIOS13_FN_VERIFY, 2, 0, 0, 1, BIOS13_DRIVE_A,
            0x2000, 0xFE00);
    bios13_service(cpu, &disk);
    expect_outcome("verify ignores the buffer", cpu, false, 0x00);

    vm86_expect_bool("and touches nothing",
                     guest_is(cpu, BUF_A_LIN, 0xEE, 2u * SECTOR), true);

    /* The address it does judge is the sector's. */
    request(cpu, BIOS13_FN_VERIFY, 1, 0, 0, 99, BIOS13_DRIVE_A, 0, 0);
    bios13_service(cpu, &disk);
    expect_outcome("verify still judges the sector", cpu, true, 0x04);
}

/* ------------------------------------------------------------------ */
/* 08h and the drive-type byte, which three sources disagreed about       */
/* ------------------------------------------------------------------ */

/*
 * THREE SOURCES DISAGREED, AND ONE OF THEM WAS THIS CASE'S OWN READING OF
 * THE MODULE.
 *
 *   docs/dos-refs.md section 4      said BL comes back as the drive type,
 *                                   that 04h is the 1.44M floppy, and that
 *                                   it was implemented.
 *   bios13_service()                did not write BL -- on the tree this
 *                                   audit was written against, which was
 *                                   cut before the commit that made the
 *                                   reference file right.
 *   docs/tasks/M4-D-report.md 5.7   said leaving it was deliberate.
 *
 * The ruling went the module's way -- and the module had already gone
 * that way. BL is a byte whose value is a fact about this machine, like
 * the DL beside it; ES:DI is not, because pointing it at a parameter table
 * means deciding where eleven bytes live in guest memory, which is an
 * interface decision.
 *
 * While the question was open the case pinned the opposite, on purpose:
 * that way whichever ruling came, the change would be a visible one rather
 * than a silent one. It is why the failure it produced named all three
 * sources -- going red said which source had won, not merely which number
 * had changed.
 *
 * ES:DI is still not asserted, because nothing disagrees about it: section
 * 4 lists the parameter table among the things this machine has not done,
 * and bios13 does not write it either.
 */
static void test_08h_reports_the_drive_type(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    reset_image();
    bios13_init(&disk, g_image, sizeof(g_image));

    request(cpu, BIOS13_FN_PARAMS, 0, 0, 0, 0, BIOS13_DRIVE_A, 0, 0);

    /* After the request, not before: the helper loads BX as the buffer
     * offset, and a sentinel set beforehand would be overwritten by it. */
    cpu->bl = 0xAB;

    bios13_service(cpu, &disk);

    expect_outcome("08h", cpu, false, 0x00);

    vm86_expect_u16("BL is the 1.44M drive type, as section 4 says",
                    cpu->bl, 0x04);
}

/* ------------------------------------------------------------------ */
/* The drive number is a parameter                                     */
/* ------------------------------------------------------------------ */

/*
 * 80h is a hard disk and this machine has none. Section 4's 01h covers
 * "a function number or a parameter it cannot take", and a drive number
 * is a parameter -- which is why this is 01h and not 0Ch. That ruling is
 * worth a case of its own because the two codes are both plausible and
 * only one of them is in section 4's list.
 */
static void test_a_drive_that_is_not_there(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    reset_image();
    bios13_init(&disk, g_image, sizeof(g_image));

    static const uint8_t drives[] = { 0x01, 0x02, 0x7F, 0x80, 0x81 };

    for (unsigned i = 0; i < sizeof(drives); i++) {
        request(cpu, BIOS13_FN_READ, 1, 0, 0, 1, drives[i], BUF_A_SEG, 0);
        bios13_service(cpu, &disk);

        char detail[64];
        snprintf(detail, sizeof(detail), "DL=0x%02X", drives[i]);
        expect_outcome(detail, cpu, true, 0x01);
    }

    /* And 08h answers the same way, because two answers to one question
     * is the failure this ruling exists to avoid. */
    request(cpu, BIOS13_FN_PARAMS, 0, 0, 0, 0, 0x80, 0, 0);
    bios13_service(cpu, &disk);
    expect_outcome("08h for a drive that is not there", cpu, true, 0x01);
}

/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    { "the cylinder decoder",             test_the_cylinder_decoder },
    { "the sector decoder",               test_the_sector_decoder_ignores_the_cylinder_bits },
    { "the seams are adjacent",           test_the_seams_are_adjacent },
    { "a read crosses the head seam",     test_a_read_crosses_the_head_seam },
    { "a read crosses the cylinder seam", test_a_read_crosses_the_cylinder_seam },
    { "a cylinder past 255 reaches the image",
                                          test_a_cylinder_past_255_reaches_the_image },
    { "success sets CF and AH together",  test_success_sets_cf_and_ah_together },
    { "failure sets CF and AH together",  test_failure_sets_cf_and_ah_together },
    { "a rejected read leaves the buffer", test_a_rejected_read_leaves_the_buffer },
    { "a rejected write leaves the image", test_a_rejected_write_leaves_the_image },
    { "a write reaches the image",        test_a_write_reaches_the_image_and_comes_back },
    { "08h describes the disk and feeds 02h",
                                          test_08h_describes_the_disk_and_feeds_02h },
    { "08h reports the drive type",       test_08h_reports_the_drive_type },
    { "a short image fails past its end", test_a_short_image_fails_past_its_end },
    { "01h reports and keeps the status", test_01h_reports_and_keeps_the_last_status },
    { "functions this machine does not have",
                                          test_functions_this_machine_does_not_have },
    { "a machine with no disk",           test_a_machine_with_no_disk },
    { "verify does not use a buffer",     test_verify_does_not_use_a_buffer },
    { "a drive that is not there",        test_a_drive_that_is_not_there },
};

VM86_TEST_MAIN("bios13_audit", tests)
