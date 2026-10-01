#include <funnyos/image.h>

#include <funnyos/init_image.h>

#include <libk/string.h>

/*
 * The one image the kernel carries.
 *
 * When there is a second one -- a .COM read off a mounted volume, which is
 * what M5 is for -- it does not become a second `if` here. This is where a
 * filesystem lookup is going to be tried, and the reason the call takes a
 * name at all is so that day does not change any caller.
 */
bool image_lookup(const char *name, struct image_info *out)
{
    if (!name || !out)
        return false;

    if (strcmp(name, "funnycom") != 0)
        return false;

    out->data        = funnyos_init_image;
    out->size        = (size_t)funnyos_init_image_size;
    out->memory_size = (size_t)funnyos_init_image_mem_size;
    return true;
}
