/*
 * 引导信息的统一访问接口
 *
 * 内核其余部分通过这里读取 Limine 递交的信息，不直接接触
 * limine.h 的请求结构体。这样将来若更换引导器，只需重写实现。
 */
#ifndef FUNNYOS_BOOTINFO_H
#define FUNNYOS_BOOTINFO_H

#include <stdbool.h>
#include <stdint.h>

#include <limine.h>

/* 引导协议基版本是否被引导器支持。必须在启动早期检查。 */
bool bootinfo_base_revision_ok(void);

/* 引导器名称与版本 */
const char *bootinfo_loader_name(void);
const char *bootinfo_loader_version(void);

/* 固件类型，取值 LIMINE_FIRMWARE_TYPE_* */
uint64_t bootinfo_firmware_type(void);

/* 高半区直接映射（HHDM）的虚拟地址偏移。
 * 物理地址 phy 对应的虚拟地址是 hhdm_offset + phy。 */
uint64_t bootinfo_hhdm_offset(void);

/* 物理内存映射 */
uint64_t bootinfo_memmap_entry_count(void);
const struct limine_memmap_entry *bootinfo_memmap_entry(uint64_t index);

/* 按类型统计可用物理内存总量（字节） */
uint64_t bootinfo_memory_total_by_type(uint64_t type);

/* 首选帧缓冲，无可用帧缓冲时返回 NULL */
struct limine_framebuffer *bootinfo_framebuffer(void);

/* 内核被加载到的物理基址 */
uint64_t bootinfo_kernel_physical_base(void);

/* 内核的虚拟基址 */
uint64_t bootinfo_kernel_virtual_base(void);

#endif /* FUNNYOS_BOOTINFO_H */
