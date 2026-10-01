; W6 checks BIOS scan/ASCII pairs, a peek that does not consume, and DOS input.
bits 16
cpu 8086
org 100h
 mov ax,3
 int 10h
%macro read_pair 1
 mov ah,0
 int 16h
 cmp ax,%1
 jne fail
 mov dx,pair_ok
 mov ah,9
 int 21h
%endmacro
 mov ah,1
 int 16h
 jnz fail
 mov ah,6
 mov dl,0FFh
 int 21h
 jnz fail
 mov dx,ready
 mov ah,9
 int 21h
.wait:
 mov ah,1
 int 16h
 jz .wait
 cmp ax,1E61h
 jne fail
 read_pair 1E61h
 read_pair 3042h
 read_pair 4800h
 read_pair 3B00h
 mov dx,dos_ready
 mov ah,9
 int 21h
 mov ah,0Bh
 int 21h
 ; input availability itself is host-tested; no timing-dependent assertion.
 mov ah,1
 int 21h
 cmp al,'c'
 jne fail
 mov dx,line
 mov ah,0Ah
 int 21h
 cmp byte [line+1],1
 jne fail
 cmp byte [line+2],'e'
 jne fail
 cmp byte [line+3],13
 jne fail
 mov dx,success
 mov ah,9
 int 21h
 mov ax,4C00h
 int 21h
fail:
 mov dx,failure
 mov ah,9
 int 21h
 mov ax,4C01h
 int 21h
ready db 'W6 BIOS ready',13,10,'$'
pair_ok db 'W6 pair: PASS',13,10,'$'
dos_ready db 'W6 DOS ready',13,10,'$'
success db 13,10,'W6 keys: PASS',13,10,'$'
failure db 'W6 keys: FAIL',13,10,'$'
line db 8,0
 times 8 db 0
