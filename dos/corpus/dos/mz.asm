bits 16
org 0
mz_header:
    dw 'MZ'
    dw file_end-$$                       ; bytes in final page
    dw 1                                ; pages
    dw 0                                ; relocations
    dw 2                                ; header paragraphs
    dw 0,0                              ; min/max allocation
    dw 0                                ; initial SS
    dw 0fffeh                           ; initial SP
    dw 0                                ; checksum
    dw 0                                ; initial IP
    dw 0                                ; initial CS
    dw 1ch                              ; relocation table
    dw 0                                ; overlay
    times 0x20-($-$$) db 0
start:
    mov ax,cs
    mov ds,ax
    mov dx,msg-0x20
    mov ah,09h
    int 21h
    mov ax,4c00h
    int 21h
msg db 'M6 MZ loader PASS',13,10,'$'
file_end:
