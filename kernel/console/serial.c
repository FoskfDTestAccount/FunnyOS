#include <funnyos/serial.h>
#include <funnyos/arch/x86_64/io.h>

#define COM1_BASE 0x3F8

/* 16550 register offsets (with DLAB cleared) */
#define UART_DATA        0 /* receive/transmit buffer */
#define UART_IER         1 /* interrupt enable */
#define UART_FCR         2 /* FIFO control (write) */
#define UART_LCR         3 /* line control */
#define UART_MCR         4 /* modem control */
#define UART_LSR         5 /* line status */

/* With DLAB set, offsets 0 and 1 alias the divisor latch */
#define UART_DLL         0
#define UART_DLM         1

#define UART_LCR_DLAB    0x80 /* divisor latch access bit */
#define UART_LCR_8N1     0x03 /* 8 data bits, no parity, 1 stop bit */

#define UART_FCR_ENABLE  0x01
#define UART_FCR_CLEAR   0x06 /* clear both receive and transmit FIFOs */
#define UART_FCR_TRIGGER 0xC0 /* 14-byte trigger threshold */

#define UART_MCR_DTR_RTS 0x03

#define UART_LSR_THRE    0x20 /* transmit holding register empty */
#define UART_LSR_TEMT    0x40 /* transmitter empty */

static bool g_serial_ready = false;

void serial_init(void)
{
    outb(COM1_BASE + UART_IER, 0x00);              /* mask all UART interrupts */
    outb(COM1_BASE + UART_LCR, UART_LCR_DLAB);     /* enable DLAB */
    outb(COM1_BASE + UART_DLL, 0x01);              /* divisor low byte = 1 */
    outb(COM1_BASE + UART_DLM, 0x00);              /* divisor high byte = 0 */
                                                   /* => 115200 / 1 = 115200 baud */
    outb(COM1_BASE + UART_LCR, UART_LCR_8N1);      /* clear DLAB, select 8N1 */
    outb(COM1_BASE + UART_FCR,
         UART_FCR_ENABLE | UART_FCR_CLEAR | UART_FCR_TRIGGER);
    outb(COM1_BASE + UART_MCR, UART_MCR_DTR_RTS);

    /* Loopback self-test: a byte written must read back unchanged.
     * This is how we confirm a UART is actually present rather than
     * silently discarding everything we send. */
    outb(COM1_BASE + UART_MCR, 0x1E);              /* enable loopback */
    outb(COM1_BASE + UART_DATA, 0xAE);
    bool loopback_ok = (inb(COM1_BASE + UART_DATA) == 0xAE);
    outb(COM1_BASE + UART_MCR, UART_MCR_DTR_RTS);  /* back to normal mode */

    g_serial_ready = loopback_ok;
}

bool serial_is_ready(void)
{
    return g_serial_ready;
}

void serial_putc(char c)
{
    /* Wait for the transmit holding register to drain */
    while ((inb(COM1_BASE + UART_LSR) & UART_LSR_THRE) == 0)
        ;

    /* Emit CR alongside LF: both terminals and QEMU's stdio backend
     * expect CRLF line endings. */
    if (c == '\n')
        outb(COM1_BASE + UART_DATA, '\r');

    outb(COM1_BASE + UART_DATA, (uint8_t)c);
}

void serial_write(const char *s)
{
    while (*s)
        serial_putc(*s++);
}
