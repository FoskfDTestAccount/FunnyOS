/* MZ executable loader.  Header validation is deliberately completed before
 * the PSP/image is written: a refused image must not leave a half-loaded
 * process behind. */
#include <vm86/exe.h>
#include <vm86/mem.h>
#include <libk/string.h>

#define MZ_HEADER_MIN 28u

static uint16_t u16(const uint8_t *p)
{ return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

static uint32_t at(uint16_t s, uint16_t o)
{ return ((uint32_t)s << 4) + o; }

static bool inside(const struct vm86_mem *m, uint32_t a, uint32_t n)
{ return a <= m->size && n <= m->size - a; }

static bool add_segment(uint16_t base, uint16_t delta, uint16_t *out)
{
    uint32_t v = (uint32_t)base + delta;
    if (v > 0xffffu) return false;
    *out = (uint16_t)v;
    return true;
}

enum vm86_exe_load_result
vm86_exe_load(struct vm86_cpu *cpu, const uint8_t *file, uint32_t file_size,
              const struct vm86_dos_start *start, struct vm86_dos_psp *psp,
              struct vm86_exe_image *out)
{
    if (!cpu || !file || !start || file_size < MZ_HEADER_MIN ||
        u16(file) != 0x5a4d)
        return VM86_EXE_BAD_SIGNATURE;

    uint16_t last = u16(file + 2);
    uint16_t pages = u16(file + 4);
    uint16_t relocs = u16(file + 6);
    uint16_t header_paras = u16(file + 8);
    uint16_t minalloc = u16(file + 0x0a);
    uint16_t rel_off = u16(file + 0x18);

    /* A zero-page image, a partial page larger than a page, or a header
     * extending beyond the file is not an MZ image. */
    if (!pages || last > 512u || header_paras < 2u ||
        (uint32_t)header_paras * 16u > file_size)
        return VM86_EXE_BAD_HEADER;

    uint32_t disk_size = (uint32_t)(pages - 1u) * 512u +
                         (last ? last : 512u);
    uint32_t header_bytes = (uint32_t)header_paras * 16u;
    if (disk_size < header_bytes || disk_size > file_size)
        return VM86_EXE_TRUNCATED;

    /* Relocations are part of the header.  Checking this before loading is
     * important because otherwise a bad table can leave PSP and image bytes
     * visible after a failed call. */
    if (rel_off < MZ_HEADER_MIN ||
        (uint32_t)rel_off + (uint32_t)relocs * 4u > header_bytes ||
        (uint32_t)rel_off + (uint32_t)relocs * 4u > disk_size)
        return VM86_EXE_BAD_HEADER;

    uint32_t body = disk_size - header_bytes;
    uint32_t image_paras = (body + 15u) / 16u + minalloc;
    if (image_paras > 0xffefu)
        return VM86_EXE_TOO_LARGE;

    uint16_t load_segment;
    if (!add_segment(start->segment, 0x10u, &load_segment))
        return VM86_EXE_TOO_LARGE;
    uint32_t load_at = at(load_segment, 0);
    uint64_t allocation_bytes = (uint64_t)image_paras * 16u;
    if (allocation_bytes > 0xffffffffu ||
        !inside(cpu->mem, at(start->segment, 0), VM86_PSP_BYTES) ||
        !inside(cpu->mem, load_at, (uint32_t)allocation_bytes))
        return VM86_EXE_TOO_LARGE;

    uint16_t cs, ss;
    if (!add_segment(load_segment, u16(file + 0x16), &cs) ||
        !add_segment(load_segment, u16(file + 0x0e), &ss))
        return VM86_EXE_TOO_LARGE;
    uint16_t ip = u16(file + 0x14);
    uint16_t sp = u16(file + 0x10);
    if (sp < 2u || !inside(cpu->mem, at(ss, (uint16_t)(sp - 2u)), 2u))
        return VM86_EXE_TOO_LARGE;

    for (uint16_t i = 0; i < relocs; ++i) {
        const uint8_t *r = file + rel_off + (uint32_t)i * 4u;
        uint32_t offset = (uint32_t)u16(r) + ((uint32_t)u16(r + 2) << 4);
        if (offset + 2u > body)
            return VM86_EXE_RELOCATION_RANGE;
    }

    struct vm86_dos_start com = *start;
    /* Reuse the PSP/environment path, then replace the one-byte COM image. */
    static const uint8_t ret_image[] = { 0xc3 };
    enum vm86_dos_load_result r =
        vm86_dos_load(cpu, ret_image, sizeof(ret_image), &com, psp);
    if (r != VM86_DOS_LOADED)
        return VM86_EXE_ENVIRONMENT;

    uint32_t image_offset = vm86_mem_offset(cpu->mem, load_at);
    if (image_offset == VM86_MEM_UNMAPPED)
        return VM86_EXE_TOO_LARGE;
    memset(cpu->mem->ram + image_offset, 0, (size_t)allocation_bytes);
    memcpy(cpu->mem->ram + image_offset, file + header_bytes, body);

    for (uint16_t i = 0; i < relocs; ++i) {
        const uint8_t *r = file + rel_off + (uint32_t)i * 4u;
        uint32_t offset = (uint32_t)u16(r) + ((uint32_t)u16(r + 2) << 4);
        uint32_t a = load_at + offset;
        uint16_t value = vm86_mem_read16(cpu->mem, a);
        vm86_mem_write16(cpu->mem, a, (uint16_t)(value + load_segment));
    }

    vm86_set_seg(cpu, VM86_CS, cs);
    /* DOS enters an EXE with DS and ES pointing at its PSP, not at CS. */
    vm86_set_seg(cpu, VM86_DS, start->segment);
    vm86_set_seg(cpu, VM86_ES, start->segment);
    vm86_set_seg(cpu, VM86_SS, ss);
    vm86_flush_segments(cpu);
    cpu->ip = ip;
    cpu->sp = sp;
    cpu->flags = VM86_FLAG_ALWAYS_SET | VM86_IF;
    vm86_mem_write16(cpu->mem, at(ss, (uint16_t)(sp - 2u)), 0);

    if (out) {
        *out = (struct vm86_exe_image){
            start->segment, load_segment, cs, start->segment, start->segment,
            ss, ip, sp, body, image_paras, relocs
        };
    }
    return VM86_EXE_LOADED;
}
