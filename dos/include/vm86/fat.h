/* FAT12/FAT16 superfloppy volumes. Pure byte-array backend; no host I/O.
 * Errors are DOS extended error codes. Paths are absolute 8.3 paths on F:.
 * The mutable image belongs to the VM; writes do not persist across runs. */
#ifndef VM86_FAT_H
#define VM86_FAT_H
#include <stdint.h>
#include <stdbool.h>
#define FAT_HANDLES 16
struct fat_handle { bool used; uint8_t mode; uint32_t entry, position; };
struct fat_volume {
    uint8_t *image;
    uint32_t size, fat, fat_bytes, root, data, clusters, cluster_bytes;
    uint16_t root_entries;
    uint8_t copies, bits;
    struct fat_handle handles[FAT_HANDLES];
};
int fat_mount(struct fat_volume *v, uint8_t *image, uint32_t size);
int fat_name(const char *text, uint8_t name[11], bool pattern);
int fat_parent(struct fat_volume *v, const char *path, uint16_t *dir,
               uint8_t name[11], bool pattern);
int fat_entry(struct fat_volume *v, uint16_t dir, uint32_t index, uint32_t *offset);
int fat_open(struct fat_volume *v, const char *path, uint8_t mode, bool create,
             uint16_t attr, uint16_t *handle);
int fat_close(struct fat_volume *v, uint16_t handle);
int fat_read(struct fat_volume *v, uint16_t handle, uint8_t *out,
             uint32_t count, uint32_t *done);
int fat_write(struct fat_volume *v, uint16_t handle, const uint8_t *in,
              uint32_t count, uint32_t *done);
int fat_seek(struct fat_volume *v, uint16_t handle, uint8_t origin,
             int32_t offset, uint32_t *position);
int fat_unlink(struct fat_volume *v, const char *path);
int fat_find(struct fat_volume *v, uint16_t dir, uint32_t *index,
             const uint8_t pattern[11], uint8_t attr, uint32_t *entry);
uint16_t fat_u16(const uint8_t *p);
uint32_t fat_u32(const uint8_t *p);
#endif
