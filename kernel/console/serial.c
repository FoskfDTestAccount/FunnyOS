#include <funnyos/serial.h>
#include <funnyos/arch/x86_64/io.h>

#define COM1_BASE 0x3F8

/* 16550 寄存器偏移（DLAB=0） */
#define UART_DATA        0 /* 收发数据 */
#define UART_IER         1 /* 中断使能 */
#define UART_FCR         2 /* FIFO 控制（写） */
#define UART_LCR         3 /* 线路控制 */
#define UART_MCR         4 /* 调制解调器控制 */
#define UART_LSR         5 /* 线路状态 */

/* DLAB=1 时偏移 0/1 变为除数锁存器 */
#define UART_DLL         0
#define UART_DLM         1

#define UART_LCR_DLAB    0x80 /* 除数锁存器访问位 */
#define UART_LCR_8N1     0x03 /* 8 位数据，无校验，1 位停止位 */

#define UART_FCR_ENABLE  0x01
#define UART_FCR_CLEAR   0x06 /* 清空收发 FIFO */
#define UART_FCR_TRIGGER 0xC0 /* 14 字节触发门限 */

#define UART_MCR_DTR_RTS 0x03

#define UART_LSR_THRE    0x20 /* 发送保持寄存器空 */
#define UART_LSR_TEMT    0x40 /* 发送器空 */

static bool g_serial_ready = false;

void serial_init(void)
{
    outb(COM1_BASE + UART_IER, 0x00);              /* 关闭全部串口中断 */
    outb(COM1_BASE + UART_LCR, UART_LCR_DLAB);     /* 打开 DLAB */
    outb(COM1_BASE + UART_DLL, 0x01);              /* 除数低字节 = 1 */
    outb(COM1_BASE + UART_DLM, 0x00);              /* 除数高字节 = 0 */
                                                   /* => 115200 / 1 = 115200 波特 */
    outb(COM1_BASE + UART_LCR, UART_LCR_8N1);      /* 关 DLAB，设为 8N1 */
    outb(COM1_BASE + UART_FCR,
         UART_FCR_ENABLE | UART_FCR_CLEAR | UART_FCR_TRIGGER);
    outb(COM1_BASE + UART_MCR, UART_MCR_DTR_RTS);

    /* 回环自检：写入的字节应当能被读回。用来确认端口真的存在。 */
    outb(COM1_BASE + UART_MCR, 0x1E);              /* 回环模式 */
    outb(COM1_BASE + UART_DATA, 0xAE);
    bool loopback_ok = (inb(COM1_BASE + UART_DATA) == 0xAE);
    outb(COM1_BASE + UART_MCR, UART_MCR_DTR_RTS);  /* 恢复正常模式 */

    g_serial_ready = loopback_ok;
}

bool serial_is_ready(void)
{
    return g_serial_ready;
}

void serial_putc(char c)
{
    /* 等待发送保持寄存器变空 */
    while ((inb(COM1_BASE + UART_LSR) & UART_LSR_THRE) == 0)
        ;

    /* 换行时补一个回车：终端和 QEMU 的 stdio 后端都需要 CRLF */
    if (c == '\n')
        outb(COM1_BASE + UART_DATA, '\r');

    outb(COM1_BASE + UART_DATA, (uint8_t)c);
}

void serial_write(const char *s)
{
    while (*s)
        serial_putc(*s++);
}
