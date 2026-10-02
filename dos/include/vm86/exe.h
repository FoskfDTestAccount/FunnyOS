/* MZ executable loader.  The loader is deliberately separate from the COM
 * PSP builder: an EXE has a file header, relocation records, and two entry
 * segments, but it still enters through the same DOS PSP contract. */
#ifndef VM86_EXE_H
#define VM86_EXE_H
#include <stdint.h>
#include <vm86/dos.h>

enum vm86_exe_load_result {
    VM86_EXE_LOADED = 0,
    VM86_EXE_BAD_SIGNATURE,
    VM86_EXE_BAD_HEADER,
    VM86_EXE_TRUNCATED,
    VM86_EXE_TOO_LARGE,
    VM86_EXE_RELOCATION_RANGE,
    VM86_EXE_ENVIRONMENT,
};
struct vm86_exe_image {
    uint16_t psp_segment, load_segment;
    uint16_t cs, ds, es, ss, ip, sp;
    uint32_t image_bytes, image_paragraphs;
    uint16_t relocations;
};
enum vm86_exe_load_result vm86_exe_load(struct vm86_cpu *cpu,
    const uint8_t *file, uint32_t file_size,
    const struct vm86_dos_start *start, struct vm86_dos_psp *psp,
    struct vm86_exe_image *out);
#endif
