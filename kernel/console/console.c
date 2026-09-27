#include <funnyos/console.h>
#include <funnyos/kbd.h>
#include <funnyos/kprintf.h>

void console_prompt(const char *text)
{
    kprintf("%s", text);
}

size_t console_read_line(char *buf, size_t size)
{
    size_t length = 0;

    if (size == 0)
        return 0;

    for (;;) {
        int key = kbd_getchar();

        switch (key) {
        case KEY_NONE:
            /*
             * kbd_getchar only returns KEY_NONE when interrupts are
             * disabled, because there is nothing to wake it otherwise.
             * Returning an empty line lets the caller make progress
             * instead of spinning forever on a key that cannot arrive.
             */
            buf[0] = '\0';
            return 0;

        case KEY_ENTER:
            buf[length] = '\0';
            kprintf("\n");
            return length;

        case KEY_BACKSPACE:
            if (length == 0)
                break;   /* nothing to erase; do not eat the prompt */

            length--;
            /* Move back, overwrite with a space, move back again. A bare
             * "\b" would leave the old character on screen. */
            kprintf("\b \b");
            break;

        /* Arrows and the rest are meaningless without a line editor, and
         * silently inserting a control byte into the buffer would be
         * worse than ignoring them. */
        default:
            if (key < 0x100 && length + 1 < size) {
                buf[length++] = (char)key;
                kprintf("%c", (char)key);
            }
            break;
        }
    }
}
