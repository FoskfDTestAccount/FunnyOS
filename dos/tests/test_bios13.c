/*
 * INT 13h.
 *
 * ---------------------------------------------------------------------
 * What these cases are for
 *
 * The disk service is not difficult; it is two rules and some address
 * arithmetic, and the address arithmetic has two seams in it. What makes
 * it worth a suite this long is that every mistake here is quiet. A read
 * that lands one sector off returns data -- the wrong data -- and a write
 * that only touched the caller's buffer leaves the disk alone while the
 * program that wrote it reads back what it wrote and believes it. Neither
 * shows up as a crash, and neither shows up in a test that checks "the
 * call worked".
 *
 * So the cases come in pairs of "and this one has to fail": success and
 * failure are asserted together (CF with AH), a read is checked against
 * the byte offsets it must land on rather than against "some data came
 * back", and every write is read back through a *different* buffer.
 *
 * ---------------------------------------------------------------------
 * What these cases cannot do
 *
 * They cannot go through the interrupt. The service is a plain function
 * over a CPU and a device, and the path that reaches it -- the vector
 * table, the `FE 38` trap, the stub -- belongs to the interrupt core.
 * Until that exists, these call bios13_service() directly, which is what
 * makes this suite independent of it. The end-to-end case, a program that
 * executes `INT 13h` and gets its sector, is the integration suite's.
 */
#include "harness.h"

#include "../bios/bios13.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* The machine these cases run against                                 */
/* ------------------------------------------------------------------ */

/*
 * A whole floppy, so that the geometry the disk claims and the image it
 * has agree -- a case about addressing should not be quietly testing the
 * end-of-image check at the same time.
 *
 * It is the caller's array, separate from the guest's memory, which is
 * the point of the interface: nothing in the service knows or cares that
 * this one is a static buffer in a test.
 */
static uint8_t g_image[BIOS13_FLOPPY_BYTES];

/* Two guest buffers far apart, so that a case can write from one and read
 * back into the other. A read back into the buffer that was written
 * proves nothing at all. */
#define BUF_A 0x20100u
#define BUF_B 0x30100u

/* Two patterns, so that a sector written from one and a sector written
 * from the other can be told apart wherever they land. The last byte of
 * a pattern is the complement of its seed's low byte, which is what the
 * cases below assert on. */
#define SEED_A 0xA5u
#define SEED_B 0x3Cu
#define PATTERN_END(seed) ((uint8_t)(0xFFu ^ (seed)))

static void pattern_guest(struct vm86_cpu *cpu, uint32_t linear,
                          uint32_t bytes, uint8_t seed)
{
    for (uint32_t i = 0; i < bytes; i++)
        vm86_mem_write8(cpu->mem, linear + i, (uint8_t)(i ^ seed));
}

static void expect_pattern(struct vm86_cpu *cpu, const char *what,
                           uint32_t linear, uint32_t bytes, uint8_t seed)
{
    for (uint32_t i = 0; i < bytes; i++) {
        uint8_t want = (uint8_t)(i ^ seed);

        if (vm86_mem_read8(cpu->mem, linear + i) != want) {
            /* Reported at the byte that differs rather than as one
             * failed comparison: "the buffer is wrong" and "the buffer
             * is wrong from offset 512" are different findings. */
            vm86_expect_mem8(what, cpu, linear + i, want);
            return;
        }
    }

    vm86_expect_bool(what, true, true);
}

static void fill_guest(struct vm86_cpu *cpu, uint32_t linear, uint32_t bytes,
                       uint8_t value)
{
    for (uint32_t i = 0; i < bytes; i++)
        vm86_mem_write8(cpu->mem, linear + i, value);
}

static void expect_filled(struct vm86_cpu *cpu, const char *what,
                          uint32_t linear, uint32_t bytes, uint8_t value)
{
    for (uint32_t i = 0; i < bytes; i++) {
        if (vm86_mem_read8(cpu->mem, linear + i) != value) {
            vm86_expect_mem8(what, cpu, linear + i, value);
            return;
        }
    }

    vm86_expect_bool(what, true, true);
}

/* Put ES:BX on an exact linear address. Through the setter rather than by
 * assigning the register, because the cached segment base is what the
 * service reads and it is refreshed from the dirty bitmap. */
static void point_at(struct vm86_cpu *cpu, uint32_t linear)
{
    vm86_set_seg(cpu, VM86_ES, (uint16_t)(linear >> 4));
    cpu->bx = (uint16_t)(linear & 0x0Fu);
}

/* A device with the standard geometry and an empty image. */
static void setup(struct vm86_cpu *cpu, struct bios13_disk *disk)
{
    memset(g_image, 0, sizeof(g_image));
    bios13_init(disk, g_image, sizeof(g_image));

    cpu->dl = BIOS13_DRIVE_A;
    point_at(cpu, BUF_A);
}

/*
 * Ask for something, the way a program does: the cylinder split across
 * CH and the top of CL, the sector in the bottom of CL.
 *
 * Writing the encoding here rather than setting an already-decoded
 * cylinder is deliberate -- the decode is part of what is under test, and
 * a helper that skipped it would test the formula against itself.
 */
static void ask(struct vm86_cpu *cpu, uint8_t function, uint16_t cylinder,
                uint8_t head, uint8_t sector, uint8_t count)
{
    cpu->ah = function;
    cpu->al = count;
    cpu->ch = (uint8_t)(cylinder & 0xFFu);
    cpu->cl = (uint8_t)(((cylinder >> 8) & 0x03u) << 6 | sector);
    cpu->dh = head;
}

/* The carry flag and the status together, every time. Half of this pair
 * is an implementation that looks like it works. */
static void expect_result(struct vm86_cpu *cpu, bool carry, uint8_t ah)
{
    vm86_expect_flag("CF", cpu, VM86_CF, carry);
    vm86_expect_u16("AH", cpu->ah, ah);
}

/* ------------------------------------------------------------------ */
/* Where a sector lives                                                */
/* ------------------------------------------------------------------ */

/*
 * Sector 1 is the first sector of the track, not the second.
 *
 * The numbering on the wire starts at one; the array starts at zero. An
 * implementation that forgets that reads and writes everything one
 * sector along, and the symptom people chase is "the boot sector is
 * empty", which sends them to look at the boot sector.
 */
static void test_sector_one_lands_at_offset_zero(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    setup(cpu, &disk);
    pattern_guest(cpu, BUF_A, 512, SEED_A);

    ask(cpu, BIOS13_FN_WRITE, 0, 0, 1, 1);
    bios13_service(cpu, &disk);

    expect_result(cpu, false, BIOS13_STATUS_OK);
    vm86_expect_u16("AL is the count transferred", cpu->al, 1);

    vm86_expect_u16("the first byte of the image", disk.image[0], SEED_A);
    vm86_expect_u16("the last byte of that sector", disk.image[511],
                    PATTERN_END(SEED_A));
    vm86_expect_u16("and nothing past it", disk.image[512], 0x00);
}

/*
 * The seam between the last sector of a track and the first of the next
 * head.
 *
 * "The next sector" means the address that follows, so the two are
 * neighbours and not the same place: sector 18 of head 0 is the
 * seventeenth sector of the disk, and the first sector of head 1 is the
 * eighteenth. Each is written with its own pattern and both are looked
 * for, one sector apart, so an address that lost the head's contribution
 * -- which would put the second write on top of the first -- is visible
 * as well as an address that is merely off by one.
 */
static void test_the_seam_from_a_track_to_the_next_head(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    setup(cpu, &disk);

    pattern_guest(cpu, BUF_A, 512, SEED_A);
    ask(cpu, BIOS13_FN_WRITE, 0, 0, 18, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);

    vm86_expect_u16("sector 18 of head 0 is the seventeenth sector",
                    disk.image[17 * 512], SEED_A);

    pattern_guest(cpu, BUF_A, 512, SEED_B);
    ask(cpu, BIOS13_FN_WRITE, 0, 1, 1, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);

    vm86_expect_u16("sector 1 of head 1 is the eighteenth",
                    disk.image[18 * 512], SEED_B);
    vm86_expect_u16("and did not land on the sector before it",
                    disk.image[17 * 512], SEED_A);
    vm86_expect_u16("or on the one after it", disk.image[19 * 512], 0x00);

    /* And the pair reads back the same way, through the other half of
     * the path. */
    fill_guest(cpu, BUF_B, 512, 0x00);
    point_at(cpu, BUF_B);

    ask(cpu, BIOS13_FN_READ, 0, 1, 1, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);

    vm86_expect_mem8("the first byte of the next head", cpu, BUF_B, SEED_B);
    expect_pattern(cpu, "the whole sector came back", BUF_B, 512, SEED_B);
}

/*
 * The seam between the last sector of the last head of a cylinder and the
 * first sector of the next cylinder. Same shape, one level up: this is
 * the pair that fails if a cylinder is not worth a whole cylinder -- the
 * first sector of cylinder 1 would land on the first sector of cylinder
 * 0 -- and it is the reason the two cases are written separately rather
 * than as one formula agreeing with itself.
 */
static void test_the_seam_from_a_head_to_the_next_cylinder(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    setup(cpu, &disk);

    pattern_guest(cpu, BUF_A, 512, SEED_A);
    ask(cpu, BIOS13_FN_WRITE, 0, 1, 18, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);

    vm86_expect_u16("the last sector of cylinder 0", disk.image[35 * 512],
                    SEED_A);

    pattern_guest(cpu, BUF_A, 512, SEED_B);
    ask(cpu, BIOS13_FN_WRITE, 1, 0, 1, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);

    vm86_expect_u16("the first sector of cylinder 1 is one further on",
                    disk.image[36 * 512], SEED_B);
    vm86_expect_u16("the cylinder's worth of sectors in between is intact",
                    disk.image[35 * 512], SEED_A);
    vm86_expect_u16("and nothing landed past it", disk.image[37 * 512],
                    0x00);

    fill_guest(cpu, BUF_B, 512, 0x00);
    point_at(cpu, BUF_B);

    ask(cpu, BIOS13_FN_READ, 1, 0, 1, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);

    expect_pattern(cpu, "the whole sector came back", BUF_B, 512, SEED_B);
}

/*
 * The cylinder's ninth and tenth bits.
 *
 * They live in the top two bits of CL, and on a 1.44 MB floppy they are
 * zero on every request the machine can serve -- eighty cylinders fit in
 * eight bits -- so no amount of working code shows them. That is why the
 * decoder is called directly here: a case that went through the service
 * would be refused as out of range whichever way its cylinder had been
 * decoded, and would pass against an implementation that drops the two
 * bits entirely.
 */
static void test_the_cylinder_has_two_bits_in_the_sector_register(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    setup(cpu, &disk);

    /* CL = 0x41: sector 1, and bit 8 of the cylinder set. 300 is 0x12C,
     * so its low eight bits are 2Ch and its ninth bit is the one CL
     * carries. */
    vm86_expect_u16("cylinder 300 from CH=2Ch CL=41h",
                    bios13_cylinder(0x2C, 0x41), 300);
    vm86_expect_u16("and its sector", bios13_sector(0x41), 1);

    /* CL = 0x82: sector 2, and the tenth bit set rather than the ninth,
     * so the two are distinguishable from each other. */
    vm86_expect_u16("cylinder 512 from CH=00h CL=82h",
                    bios13_cylinder(0x00, 0x82), 0x0200);
    vm86_expect_u16("and its sector", bios13_sector(0x82), 2);

    /* And a request the floppy itself can serve: the bits are there and
     * zero, which is the case that hides the bug. */
    vm86_expect_u16("an ordinary cylinder", bios13_cylinder(0x4F, 0x12), 79);
    vm86_expect_u16("whose sector is not the cylinder",
                    bios13_sector(0x12), 18);

    /* The two halves of the split are each wrong on their own, so this
     * also pins which register holds which. */
    vm86_expect_u16("the low byte is CH", bios13_cylinder(0x01, 0x00), 1);
    vm86_expect_u16("the high bits are CL's",
                    bios13_cylinder(0x00, 0x40), 0x0100);
}

/*
 * And the same decode reached through the service, on a disk that really
 * does have cylinder 300. The geometry is a field, so a test can describe
 * a device that is not a floppy without a second implementation of the
 * address arithmetic appearing anywhere.
 */
static void test_the_service_reaches_a_high_cylinder(struct vm86_cpu *cpu)
{
    /* Cylinder 300, head 0, sector 1 is sector 10800; one sector on top
     * of that is the smallest image that can serve it. */
    static uint8_t big[10801 * BIOS13_SECTOR_BYTES];
    struct bios13_disk disk;

    memset(big, 0, sizeof(big));
    bios13_init(&disk, big, sizeof(big));
    disk.cylinders = 1024;      /* ten bits' worth, so the decode matters */

    cpu->dl = BIOS13_DRIVE_A;
    point_at(cpu, BUF_A);

    pattern_guest(cpu, BUF_A, 512, SEED_A);
    ask(cpu, BIOS13_FN_WRITE, 300, 0, 1, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);

    vm86_expect_u16("cylinder 300 sector 1 begins here",
                    big[10800 * 512], SEED_A);
    vm86_expect_u16("its last byte", big[10801 * 512 - 1],
                    PATTERN_END(SEED_A));
    vm86_expect_u16("and nothing just before it", big[10800 * 512 - 1], 0x00);
}

/* ------------------------------------------------------------------ */
/* The request the disk cannot serve                                   */
/* ------------------------------------------------------------------ */

/*
 * Past the end of the image, with the buffer written first so that a
 * half-done read is visible: a read that fills some of the sectors and
 * then fails leaves the caller with neither the old contents nor the new
 * ones, and the caller has already been told the call failed.
 */
static void test_a_read_past_the_image_leaves_the_buffer_alone(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    setup(cpu, &disk);

    /* Four sectors of disk. The geometry still says eighteen to a track,
     * so it is the image's length and not the sector number that has to
     * refuse this. */
    disk.size = 4 * BIOS13_SECTOR_BYTES;

    fill_guest(cpu, BUF_A, 1024, 0x5A);

    ask(cpu, BIOS13_FN_READ, 0, 0, 5, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, true, BIOS13_STATUS_NOT_FOUND);
    vm86_expect_u16("AL, no sectors moved", cpu->al, 0);
    expect_filled(cpu, "the buffer is untouched", BUF_A, 1024, 0x5A);

    /* A run that starts inside the image and ends past its end is the
     * same failure: sectors 4 and 5 of a four-sector image. */
    ask(cpu, BIOS13_FN_READ, 0, 0, 4, 2);
    bios13_service(cpu, &disk);
    expect_result(cpu, true, BIOS13_STATUS_NOT_FOUND);
    expect_filled(cpu, "still untouched", BUF_A, 1024, 0x5A);

    /* The three sectors it does have are all readable, which is what
     * makes the two failures above about the end rather than about the
     * request being malformed. */
    ask(cpu, BIOS13_FN_READ, 0, 0, 4, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);
}

static void test_a_write_past_the_image_leaves_it_alone(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    setup(cpu, &disk);
    disk.size = 2 * BIOS13_SECTOR_BYTES;

    disk.image[0]    = 0x11;
    disk.image[1023] = 0x22;

    /* Sectors 2 and 3 of a two-sector image: the first would fit and the
     * second would not. A write path that copies sector by sector and
     * stops when it runs out has already destroyed the first one. */
    pattern_guest(cpu, BUF_A, 512, SEED_A);
    ask(cpu, BIOS13_FN_WRITE, 0, 0, 2, 2);
    bios13_service(cpu, &disk);

    expect_result(cpu, true, BIOS13_STATUS_NOT_FOUND);
    vm86_expect_u16("the image is unchanged", disk.image[0], 0x11);
    vm86_expect_u16("all of it", disk.image[1023], 0x22);
}

/*
 * A cylinder, head or sector the geometry does not have.
 *
 * The geometry is shrunk rather than the request grown, so that the
 * refusal is the geometry's and not the image's -- the opposite of the
 * case above, and the two together are what say that both are checked.
 */
static void test_a_request_the_geometry_does_not_have(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    setup(cpu, &disk);
    disk.cylinders = 2;
    disk.heads     = 1;
    disk.sectors   = 9;

    ask(cpu, BIOS13_FN_READ, 2, 0, 1, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, true, BIOS13_STATUS_NOT_FOUND);

    ask(cpu, BIOS13_FN_READ, 0, 1, 1, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, true, BIOS13_STATUS_NOT_FOUND);

    ask(cpu, BIOS13_FN_READ, 0, 0, 10, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, true, BIOS13_STATUS_NOT_FOUND);

    /* The last sector the geometry does have, to show where the edge is. */
    ask(cpu, BIOS13_FN_READ, 1, 0, 9, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);
}

/*
 * Sector zero.
 *
 * The numbering starts at one, so zero is not the first sector -- it is
 * not a sector. An implementation that subtracts one from whatever it
 * was handed underflows and reads the byte before the sector it was
 * asked for, which for sector zero is somewhere before the image.
 */
static void test_sector_zero_is_not_a_sector(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    setup(cpu, &disk);
    fill_guest(cpu, BUF_A, 512, 0x5A);

    ask(cpu, BIOS13_FN_READ, 0, 0, 0, 1);
    bios13_service(cpu, &disk);

    expect_result(cpu, true, BIOS13_STATUS_NOT_FOUND);
    expect_filled(cpu, "the buffer is untouched", BUF_A, 512, 0x5A);
}

/* ------------------------------------------------------------------ */
/* The buffer in the guest's memory                                    */
/* ------------------------------------------------------------------ */

/*
 * A transfer may not span two 64 KiB pages, because the controller's DMA
 * channel counts within one and carries no way to say "and the next one".
 *
 * Both sides of the edge: a run that ends exactly on the boundary is
 * inside one page and is allowed, and one paragraph further is not. A
 * check written as `>=` or with the length included at the wrong end
 * passes the second and fails the first.
 */
static void test_a_buffer_may_not_span_a_page(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    setup(cpu, &disk);

    /* ES:BX = 2000:FE00. The last byte is the page's last byte. */
    vm86_set_seg(cpu, VM86_ES, 0x2000);
    cpu->bx = 0xFE00;

    ask(cpu, BIOS13_FN_READ, 0, 0, 1, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);

    /* One sector more and it is two pages, which is refused... */
    ask(cpu, BIOS13_FN_READ, 0, 0, 1, 2);
    bios13_service(cpu, &disk);
    expect_result(cpu, true, BIOS13_STATUS_DMA_BOUNDARY);

    /* ...and a single sector one paragraph along is past the boundary on
     * its own. */
    cpu->bx = 0xFF00;
    fill_guest(cpu, 0x2FF00, 512, 0x5A);

    ask(cpu, BIOS13_FN_READ, 0, 0, 1, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, true, BIOS13_STATUS_DMA_BOUNDARY);
    expect_filled(cpu, "the buffer is untouched", 0x2FF00, 512, 0x5A);

    /* A write is refused for the same reason, and does not reach the
     * disk. */
    pattern_guest(cpu, 0x2FF00, 512, SEED_A);
    ask(cpu, BIOS13_FN_WRITE, 0, 0, 1, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, true, BIOS13_STATUS_DMA_BOUNDARY);
    vm86_expect_u16("the image is untouched", disk.image[0], 0x00);

    /* And a run longer than the 128 sectors the sources give as the top
     * of AL is refused here rather than by a limit of its own. It is the
     * same fact either way -- 129 sectors are more than a DMA page can
     * carry -- and answering a question the hardware answers with 09h
     * with a code invented for it would disagree with the hardware for no
     * gain. */
    vm86_set_seg(cpu, VM86_ES, 0x2000);
    cpu->bx = 0x0000;

    ask(cpu, BIOS13_FN_READ, 0, 0, 1, 129);
    bios13_service(cpu, &disk);
    expect_result(cpu, true, BIOS13_STATUS_DMA_BOUNDARY);
}

/* ------------------------------------------------------------------ */
/* The write path                                                      */
/* ------------------------------------------------------------------ */

/*
 * The one case that can tell a write from a copy.
 *
 * Reading back into the buffer that was written passes whether or not the
 * write reached the image, because that buffer holds the data either way.
 * So the read goes somewhere else, and the second buffer is filled with a
 * value the data is not.
 */
static void test_a_write_is_read_back_through_another_buffer(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    setup(cpu, &disk);
    pattern_guest(cpu, BUF_A, 512, SEED_A);

    ask(cpu, BIOS13_FN_WRITE, 0, 0, 1, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);

    /* The image, not just the buffer. */
    vm86_expect_u16("the first byte reached the disk", disk.image[0], SEED_A);
    vm86_expect_u16("and the last", disk.image[511], PATTERN_END(SEED_A));

    fill_guest(cpu, BUF_B, 512, 0xFF);
    point_at(cpu, BUF_B);

    ask(cpu, BIOS13_FN_READ, 0, 0, 1, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);

    expect_pattern(cpu, "what came back is what was written", BUF_B, 512,
                   SEED_A);
}

/*
 * And the write it made is the one a later read finds at another address,
 * so that "the image changed" is not satisfied by the image changing
 * anywhere.
 */
static void test_a_write_lands_where_its_address_says(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    setup(cpu, &disk);
    pattern_guest(cpu, BUF_A, 512, SEED_A);

    ask(cpu, BIOS13_FN_WRITE, 0, 1, 3, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);

    /* Head 1, sector 3: one whole track and two sectors in. */
    vm86_expect_u16("at head 1 sector 3", disk.image[(18 + 2) * 512], SEED_A);
    vm86_expect_u16("and nowhere else", disk.image[0], 0x00);
}

/* ------------------------------------------------------------------ */
/* AL                                                                  */
/* ------------------------------------------------------------------ */

/*
 * AL = 0 is refused.
 *
 * The folklore answer is eighteen sectors, "which is what a real floppy
 * BIOS does". It was written into the first draft of this task and then
 * looked up: docs/dos-refs.md section 4 gives the documented range as 1
 * to 128 and says the eighteen is not in any of its sources. A memory
 * does not get to decide what a program sees.
 *
 * So the choice is between refusing, moving nothing and reporting
 * success, and inventing a count. This case is written against all
 * three:
 *
 *   - the call must fail, which rules out the silent no-op;
 *   - the buffer it pointed at must be untouched, which rules out the
 *     eighteen sectors being filled into a buffer a caller asking for
 *     no sectors had no reason to make that large;
 *   - a call that names one sector must still work, which rules out
 *     "everything is refused".
 */
static void test_al_zero_is_refused(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    setup(cpu, &disk);

    /* Eighteen sectors' worth, so that an implementation which decided
     * AL = 0 meant eighteen has somewhere to put them and is caught
     * doing it. */
    fill_guest(cpu, BUF_A, 18 * BIOS13_SECTOR_BYTES, 0x5A);

    ask(cpu, BIOS13_FN_READ, 0, 0, 1, 0);
    bios13_service(cpu, &disk);

    expect_result(cpu, true, BIOS13_STATUS_BAD_COMMAND);
    vm86_expect_u16("AL, no sectors moved", cpu->al, 0);
    expect_filled(cpu, "nothing was read into the buffer", BUF_A,
                  18 * BIOS13_SECTOR_BYTES, 0x5A);

    /* A write is refused in the same place, before anything moves in
     * either direction. */
    pattern_guest(cpu, BUF_A, 512, SEED_A);
    ask(cpu, BIOS13_FN_WRITE, 0, 0, 2, 0);
    bios13_service(cpu, &disk);
    expect_result(cpu, true, BIOS13_STATUS_BAD_COMMAND);
    vm86_expect_u16("the image is untouched", disk.image[512], 0x00);

    /* And a count that means something is served, so none of the above
     * is passing because the call was malformed. */
    ask(cpu, BIOS13_FN_READ, 0, 0, 1, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);
}

/*
 * Every success clears the carry and zeroes AH.
 *
 * Both halves, after a failure has set them: an implementation that only
 * ever sets the status leaves the carry set through a good read, and the
 * caller throws the data away.
 */
static void test_success_clears_the_carry_and_the_status(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    setup(cpu, &disk);

    ask(cpu, BIOS13_FN_READ, 0, 0, 19, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, true, BIOS13_STATUS_NOT_FOUND);

    ask(cpu, BIOS13_FN_READ, 0, 0, 1, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);
    vm86_expect_u16("AL is the count", cpu->al, 1);
}

/* ------------------------------------------------------------------ */
/* Remembering what happened                                           */
/* ------------------------------------------------------------------ */

static void test_the_status_call_remembers_the_last_error(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    setup(cpu, &disk);

    ask(cpu, BIOS13_FN_READ, 0, 0, 19, 1);
    bios13_service(cpu, &disk);

    cpu->ah = BIOS13_FN_STATUS;
    bios13_service(cpu, &disk);
    expect_result(cpu, true, BIOS13_STATUS_NOT_FOUND);

    /* After something that worked it is zero, not the old failure: the
     * last operation means the last one. */
    ask(cpu, BIOS13_FN_READ, 0, 0, 1, 1);
    bios13_service(cpu, &disk);

    cpu->ah = BIOS13_FN_STATUS;
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);

    /* And asking does not change it: reporting the status must not
     * overwrite the status. */
    cpu->ah = BIOS13_FN_STATUS;
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);
}

static void test_reset_forgets_the_last_error(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    setup(cpu, &disk);

    ask(cpu, BIOS13_FN_READ, 0, 0, 19, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, true, BIOS13_STATUS_NOT_FOUND);

    /* There is no controller to reset, so this reports success and
     * forgets the error -- which is what a program that resets and
     * retries is asking for. */
    cpu->ah = BIOS13_FN_RESET;
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);

    cpu->ah = BIOS13_FN_STATUS;
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);
}

/* ------------------------------------------------------------------ */
/* Functions and drives this machine does not have                     */
/* ------------------------------------------------------------------ */

/* Several of them, so that this is about the default rather than about
 * one case that happened to be handled. Every one is given an otherwise
 * perfect request, so only the function number can be the reason. */
static void test_an_unimplemented_function_says_so(struct vm86_cpu *cpu)
{
    static const uint8_t unimplemented[] = {
        0x05, 0x06, 0x07, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x10, 0x11, 0x12, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A,
        0x7F, 0xFE, 0xFF,
    };

    struct bios13_disk disk;

    setup(cpu, &disk);

    for (size_t i = 0; i < sizeof(unimplemented) / sizeof(unimplemented[0]); i++) {
        /* Each one named, because twenty-two identical failures say
         * nothing about which of them was the odd one out. */
        char what[64];

        ask(cpu, unimplemented[i], 0, 0, 1, 1);
        bios13_service(cpu, &disk);

        snprintf(what, sizeof(what), "function %02Xh clears the carry",
                 unimplemented[i]);
        vm86_expect_flag(what, cpu, VM86_CF, true);

        snprintf(what, sizeof(what), "function %02Xh reports the status",
                 unimplemented[i]);
        vm86_expect_u16(what, cpu->ah, BIOS13_STATUS_BAD_COMMAND);
    }

    /* A program that probes for a capability by calling it and looking at
     * the carry flag is entitled to the truth, and the disk still works
     * afterwards. */
    ask(cpu, BIOS13_FN_READ, 0, 0, 1, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);
}

/*
 * A drive that is not there, and a function that is not there, are the
 * same kind of answer: a parameter this device cannot take, and a
 * function it does not have, are both 01h (docs/dos-refs.md section 4).
 *
 * That they are the *same* code is asserted rather than assumed, because
 * this is where the machine used to answer 0Ch -- which section 4 gives
 * to "the media type was not found", a statement about a medium, and this
 * machine has no medium to be wrong about.
 */
static void test_a_drive_that_is_not_the_floppy(struct vm86_cpu *cpu)
{
    static const uint8_t others[] = { 0x01, 0x02, 0x7F, 0x80, 0x81, 0xFF };
    struct bios13_disk disk;

    setup(cpu, &disk);

    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        /* An otherwise perfect request: only the drive number is wrong. */
        ask(cpu, BIOS13_FN_READ, 0, 0, 1, 1);
        cpu->dl = others[i];
        bios13_service(cpu, &disk);

        expect_result(cpu, true, BIOS13_STATUS_BAD_COMMAND);

        /* 08h is the same answer about the same drive. */
        cpu->ah = BIOS13_FN_PARAMS;
        bios13_service(cpu, &disk);
        expect_result(cpu, true, BIOS13_STATUS_BAD_COMMAND);
    }

    /* Which is the code an unknown function gets, and not 0Ch. */
    cpu->dl = BIOS13_DRIVE_C;
    ask(cpu, BIOS13_FN_READ, 0, 0, 1, 1);
    bios13_service(cpu, &disk);

    uint8_t from_the_drive = cpu->ah;

    ask(cpu, 0xFE, 0, 0, 1, 1);
    bios13_service(cpu, &disk);

    vm86_expect_u16("a missing drive and a missing function agree",
                    cpu->ah, from_the_drive);
    vm86_expect_u16("and neither of them is 0Ch", from_the_drive, 0x01);

    /* Drive A still answers, so the loop above is not passing because
     * the service refuses everything. */
    ask(cpu, BIOS13_FN_READ, 0, 0, 1, 1);
    cpu->dl = BIOS13_DRIVE_A;
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);
}

static void test_a_machine_with_no_disk(struct vm86_cpu *cpu)
{
    ask(cpu, BIOS13_FN_READ, 0, 0, 1, 1);
    bios13_service(cpu, NULL);
    expect_result(cpu, true, BIOS13_STATUS_BAD_COMMAND);

    cpu->ah = BIOS13_FN_PARAMS;
    bios13_service(cpu, NULL);
    expect_result(cpu, true, BIOS13_STATUS_BAD_COMMAND);

    /* Reset is the exception, and deliberately: there is no controller to
     * reset, so there is nothing that can fail. */
    cpu->ah = BIOS13_FN_RESET;
    bios13_service(cpu, NULL);
    expect_result(cpu, false, BIOS13_STATUS_OK);
}

/* ------------------------------------------------------------------ */
/* What the drive says about itself                                    */
/* ------------------------------------------------------------------ */

/*
 * The three numbers 08h returns are maxima, and they are the largest
 * value each field can hold rather than how many of each there are.
 *
 * Cylinders and heads are counted from zero, so theirs are one less than
 * the count. Sectors are numbered from one, so this one is not: eighteen
 * sectors to a track means the largest sector number is eighteen. A
 * program sizes the disk as (cylinders + 1) x (heads + 1) x sectors, so
 * a subtraction in the wrong place costs it a track, and one that is
 * missing costs it everything past the end of the first cylinder.
 */
static void test_the_drive_parameters_are_maxima(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    setup(cpu, &disk);

    cpu->ah = BIOS13_FN_PARAMS;
    cpu->dl = BIOS13_DRIVE_A;
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);

    vm86_expect_u16("CH, the largest cylinder", cpu->ch, 79);
    vm86_expect_u16("CL's top two bits, its ninth and tenth",
                    (uint16_t)(cpu->cl >> 6), 0);
    vm86_expect_u16("DH, the largest head", cpu->dh, 1);
    vm86_expect_u16("CL's low six bits, the largest sector number",
                    (uint16_t)(cpu->cl & 0x3F), 18);
    vm86_expect_u16("DL, how many drives there are",
                    cpu->dl, BIOS13_FLOPPY_DRIVES);
    vm86_expect_u16("BL, what kind of drive it is",
                    cpu->bl, BIOS13_DRIVE_TYPE_1_44M);

    /* ES:DI is the parameter table, and is deliberately not set -- see
     * the note in bios13.c. Asserted so that "left alone" is a fact
     * rather than an assumption: the segment is whatever the caller
     * had. */
    vm86_set_seg(cpu, VM86_ES, 0x1234);
    cpu->di = 0x5678;
    cpu->ah = BIOS13_FN_PARAMS;
    cpu->dl = BIOS13_DRIVE_A;
    bios13_service(cpu, &disk);
    vm86_expect_u16("ES is not repointed", cpu->es, 0x1234);
    vm86_expect_u16("DI is not repointed", cpu->di, 0x5678);

    /* The capacity a program computes from those four fields is the size
     * of the image attached here, which is the only check that says the
     * answer describes this disk rather than a plausible one. */
    uint32_t cylinders = ((uint32_t)(cpu->ch | ((cpu->cl >> 6) << 8))) + 1;
    uint32_t heads     = (uint32_t)cpu->dh + 1;
    uint32_t sectors   = (uint32_t)(cpu->cl & 0x3F);

    vm86_expect_bool("the capacity it computes is the image",
                     cylinders * heads * sectors * BIOS13_SECTOR_BYTES
                         == disk.size,
                     true);
}

/*
 * 15h is a real function and this machine does not have it.
 *
 * docs/dos-refs.md section 4 lists it among the disk functions and then
 * says nothing about what it returns -- not the type codes, not what the
 * pair of registers holds. Answering from memory would let a program take
 * a path whose data was invented here; answering 01h sends it down the
 * road the same section does document, which is 08h.
 *
 * The two halves of this case are that it refuses, and that the fallback
 * it is sending the program to actually answers.
 */
static void test_the_extended_drive_type_is_refused(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    setup(cpu, &disk);

    /* Something recognisable in the registers a working 15h would have
     * answered in, so that "left alone" is visible rather than assumed. */
    cpu->dl = BIOS13_DRIVE_A;
    cpu->cx = 0xDEAD;
    cpu->dh = 0xBE;

    cpu->ah = BIOS13_FN_TYPE;
    bios13_service(cpu, &disk);

    expect_result(cpu, true, BIOS13_STATUS_BAD_COMMAND);
    vm86_expect_u16("CX is left alone", cpu->cx, 0xDEAD);
    vm86_expect_u16("DH is left alone", cpu->dh, 0xBE);

    /* The road it falls back to: a program that takes the 01h at face
     * value and calls 08h gets the drive described. */
    cpu->ah = BIOS13_FN_PARAMS;
    cpu->dl = BIOS13_DRIVE_A;
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);
    vm86_expect_u16("CH", cpu->ch, 79);
    vm86_expect_u16("DH", cpu->dh, 1);
    vm86_expect_u16("CL's low six bits", (uint16_t)(cpu->cl & 0x3F), 18);
}

/* ------------------------------------------------------------------ */
/* Verify                                                              */
/* ------------------------------------------------------------------ */

/*
 * Verify checks the address and nothing else.
 *
 * There is no CRC here to check, because there is no controller to have
 * failed one -- so this reports what it can know: that the sector is one
 * the disk has. It moves nothing, and the report says so; a version that
 * silently did less than a program assumes would be worse than one that
 * said it could not.
 */
static void test_verify_checks_the_address_and_moves_nothing(struct vm86_cpu *cpu)
{
    struct bios13_disk disk;

    setup(cpu, &disk);
    fill_guest(cpu, BUF_A, 512, 0x5A);

    ask(cpu, BIOS13_FN_VERIFY, 0, 0, 1, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);
    vm86_expect_u16("AL is the count", cpu->al, 1);
    expect_filled(cpu, "the buffer is not touched", BUF_A, 512, 0x5A);

    /* The address is still judged. */
    ask(cpu, BIOS13_FN_VERIFY, 0, 0, 19, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, true, BIOS13_STATUS_NOT_FOUND);

    disk.size = BIOS13_SECTOR_BYTES;
    ask(cpu, BIOS13_FN_VERIFY, 0, 0, 2, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, true, BIOS13_STATUS_NOT_FOUND);

    /* There is no buffer, so a crossing is not its business. */
    vm86_set_seg(cpu, VM86_ES, 0x2000);
    cpu->bx = 0xFF00;
    ask(cpu, BIOS13_FN_VERIFY, 0, 0, 1, 1);
    bios13_service(cpu, &disk);
    expect_result(cpu, false, BIOS13_STATUS_OK);
}

/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    { "sector 1 is at offset 0", test_sector_one_lands_at_offset_zero },
    { "the last sector of a track is followed by the next head",
      test_the_seam_from_a_track_to_the_next_head },
    { "the last sector of a cylinder is followed by the next cylinder",
      test_the_seam_from_a_head_to_the_next_cylinder },
    { "the cylinder has two bits in CL",
      test_the_cylinder_has_two_bits_in_the_sector_register },
    { "the service reaches a cylinder above 255",
      test_the_service_reaches_a_high_cylinder },
    { "a read past the image leaves the buffer alone",
      test_a_read_past_the_image_leaves_the_buffer_alone },
    { "a write past the image leaves it alone",
      test_a_write_past_the_image_leaves_it_alone },
    { "a request the geometry does not have",
      test_a_request_the_geometry_does_not_have },
    { "sector zero is not a sector", test_sector_zero_is_not_a_sector },
    { "a buffer may not span a 64 KiB page",
      test_a_buffer_may_not_span_a_page },
    { "a write is read back through another buffer",
      test_a_write_is_read_back_through_another_buffer },
    { "a write lands where its address says",
      test_a_write_lands_where_its_address_says },
    { "AL = 0 is refused", test_al_zero_is_refused },
    { "success clears the carry and the status",
      test_success_clears_the_carry_and_the_status },
    { "the status call remembers the last error",
      test_the_status_call_remembers_the_last_error },
    { "reset forgets the last error",
      test_reset_forgets_the_last_error },
    { "an unimplemented function says so",
      test_an_unimplemented_function_says_so },
    { "a drive that is not the floppy",
      test_a_drive_that_is_not_the_floppy },
    { "a machine with no disk", test_a_machine_with_no_disk },
    { "the drive parameters are maxima",
      test_the_drive_parameters_are_maxima },
    { "15h is refused, and 08h is the road it falls back to",
      test_the_extended_drive_type_is_refused },
    { "verify checks the address and moves nothing",
      test_verify_checks_the_address_and_moves_nothing },
};

VM86_TEST_MAIN("bios13", tests)
