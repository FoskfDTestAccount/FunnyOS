; Segment register reload for the GDT switch.
;
; These live in assembly because C cannot express them: loading CS requires
; a far return, and loading the data segment registers has to happen in a
; specific order relative to it.

bits 64

section .text

global gdt_load
global gdt_load_cs
global tss_load

; void gdt_load(const struct gdtr *gdtr)
;   rdi = pointer to a 10-byte { uint16_t limit; uint64_t base; } structure
gdt_load:
    lgdt [rdi]
    ret

; void gdt_load_cs(void)
;
; Reload CS with GDT_KERNEL_CODE. The only way to change CS is a far
; transfer, so push the target selector and offset and use retfq.
;
; The data segment registers are reloaded here too. In 64-bit mode their
; bases are ignored, but they must still hold valid selectors rather than
; stale ones from the previous GDT -- a stale selector becomes a problem
; the moment anything inspects it, and mixing descriptors across tables is
; a subtle way to get a #GP far from its cause.
gdt_load_cs:
    push qword 0x08                 ; GDT_KERNEL_CODE
    lea rax, [rel .reload_cs]
    push rax
    retfq

.reload_cs:
    mov ax, 0x10                    ; GDT_KERNEL_DATA
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    ret

; void tss_load(uint16_t selector)
;   di = TSS selector
tss_load:
    ltr di
    ret
