/*
 * 串口驱动（16550 UART，COM1）
 *
 * 为什么 M0 就需要串口：QEMU 可以无头运行并把串口接到 stdout，
 * 这让内核输出可以被脚本捕获和断言。在帧缓冲控制台做好之前
 * （以及之后用于内核诊断），串口是唯一可靠的观测通道。
 */
#ifndef FUNNYOS_SERIAL_H
#define FUNNYOS_SERIAL_H

#include <stdbool.h>

/* 初始化 COM1。必须在任何输出之前调用。 */
void serial_init(void);

/* 输出单个字符。未初始化时行为未定义。 */
void serial_putc(char c);

/* 输出以 NUL 结尾的字符串 */
void serial_write(const char *s);

/* 串口是否已初始化并就绪 */
bool serial_is_ready(void);

#endif /* FUNNYOS_SERIAL_H */
