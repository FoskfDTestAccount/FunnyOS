; FunnyOS x86-64 内核入口
;
; Limine 已完成的准备工作（见 limine-protocol.md "Machine State at Entry"）：
;   - 已进入 64 位长模式，分页已开启（PG/PAE/LME/LMA 置位）
;   - CS = 0x28（64 位代码段），DS/ES/SS/FS/GS = 0x30（64 位数据段）
;   - IF 已清除，DF 已清除
;   - 内核已映射到高半区 0xffffffff80000000 以上
;   - A20 已打开，传统 PIC 全部屏蔽
;
; Limine 未做的、必须由内核自己做的：
;   - 没有设置栈指针（rsp 值未定义）  <-- 本文件的第一件事
;   - 没有加载 IDT（rev5 以下状态未定义；rev5+ 为 base 0/limit 0）
;   - 没有建立 GDT 之外的任务状态段

bits 64

section .text
global _start
extern kmain

_start:
    ; 立刻建立自己的栈。在此之前不能调用任何函数，也不能触发任何中断。
    mov rsp, stack_top

    ; 对齐栈帧并清掉 rbp，方便后续补栈回溯
    and rsp, ~0xF
    xor rbp, rbp

    ; 进入 C 世界
    call kmain

    ; kmain 正常情况不会返回。若返回则停在这里。
.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
stack_bottom:
    resb 65536                  ; 64 KiB 内核栈（M0 足够，M1 接中断时会重新评估）
stack_top:
