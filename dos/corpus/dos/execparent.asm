; M6 EXEC parent acceptance program. The VM runs the child synchronously,
; restores this PSP/CPU, and makes AH=4Dh report the child's exit code.
bits 16
cpu 8086
org 100h
    mov dx, child_path
    mov ax, 4B00h
    push ds
    pop es
    mov bx, params
    int 21h
    jc fail
    mov ah, 4Dh
    int 21h
    or al, al
    jnz fail
    mov dx, parent_ok
    mov ah, 09h
    int 21h
    mov ax, 4C00h
    int 21h
fail:
    mov dx, parent_fail
    mov ah, 09h
    int 21h
    mov ax, 4C01h
    int 21h
child_path db 'F:\EXECHILD.COM',0
parent_ok db 'M6 EXEC parent PASS',13,10,'$'
parent_fail db 'M6 EXEC parent FAIL',13,10,'$'
; Minimal EXEC parameter block. The current implementation inherits the
; caller's command tail and environment, but still validates the ABI pointer.
params dw 0
       dd 0
       dd 0
