/*
 * FunnyOS 内核链接脚本
 *
 * 关键约束（来自 Limine 引导协议）：
 *   - 内核必须链接到地址空间高半区 0xffffffff80000000 及以上
 *   - .limine_requests 段必须存在，且被 start/end 标记包裹
 *   - 每个段单独分配 program header，以便引导器设置正确的 MMU 权限
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    limine_requests PT_LOAD;
    text            PT_LOAD;
    rodata          PT_LOAD;
    data            PT_LOAD;
}

SECTIONS
{
    /* 高半区起点。Limine 规定内核必须落在最高 2 GiB 内 */
    . = 0xffffffff80000000;

    /* 引导请求区。KEEP 防止 --gc-sections 把它优化掉 */
    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        *(.data .data.*)
        *(.got .got.*)
    } :data

    /* .bss 归入 data 的 program header，但标记为 NOBITS 不占文件空间 */
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /* 这两个段在某些宿主机上会引发问题，丢弃 */
    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
    }

    /* 供内核自检使用的链接期符号 */
    . = ALIGN(4096);
    __kernel_end = .;
}
