; W5 executable acceptance. Values and DTA offsets are DOS's ABI, not C headers.
bits 16
cpu 8086
org 100h
 mov ax,3
 int 10h
%macro dos 1
 mov ah,%1
 int 21h
 jc fail
%endmacro
 mov dx,source
 mov ax,3D00h
 int 21h
 jc fail
 mov bx,ax
 mov dx,buffer
 mov cx,64
 dos 3Fh
 cmp ax,19
 jne fail
 cmp byte [buffer],'W'
 jne fail
 mov cx,ax
 push bx
 mov bx,1
 dos 40h
 pop bx
 mov ax,4200h
 xor cx,cx
 xor dx,dx
 int 21h
 jc fail
 or ax,dx
 jnz fail
 mov cx,1
 mov dx,buffer
 dos 3Fh
 cmp byte [buffer],'W'
 jne fail
 dos 3Eh
 mov dx,target
 xor cx,cx
 dos 3Ch
 mov bx,ax
 mov dx,payload
 mov cx,13
 dos 40h
 cmp ax,13
 jne fail
 dos 3Eh
 mov dx,target
 mov ax,3D00h
 int 21h
 jc fail
 mov bx,ax
 mov dx,buffer
 mov cx,64
 dos 3Fh
 cmp ax,13
 jne fail
 push ds
 pop es
 mov si,buffer
 mov di,payload
 mov cx,13
 repe cmpsb
 jne fail
 dos 3Eh
 mov dx,dta
 dos 1Ah
 mov dx,target
 xor cx,cx
 dos 4Eh
 cmp word [dta+26],13
 jne fail
 cmp word [dta+28],0
 jne fail
 cmp byte [dta+30],'R'
 jne fail
 mov ah,4Fh
 int 21h
 jnc fail
 cmp ax,18
 jne fail
 mov dx,target
 dos 41h
 mov dx,success
 mov ah,09h
 int 21h
 mov ax,4C00h
 int 21h
fail:
 mov dx,failure
 mov ah,09h
 int 21h
 mov ax,4C01h
 int 21h
source db 'F:\HELLO.TXT',0
target db 'RESULT.TXT',0
payload db 'FAT write OK!'
success db 'W5 files: PASS',13,10,'$'
failure db 'W5 files: FAIL',13,10,'$'
buffer times 64 db 0
dta times 43 db 0
