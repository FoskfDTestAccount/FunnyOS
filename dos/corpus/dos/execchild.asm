; M6 EXEC child acceptance program.
bits 16
cpu 8086
org 100h
    mov dx, child_msg
    mov ah, 09h
    int 21h
    mov ax, 4C00h
    int 21h
child_msg db 'M6 EXEC child PASS',13,10,'$'
