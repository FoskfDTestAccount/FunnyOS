; chip_movcs -- MOV CS, r/m16, the one encoding this machine refuses
; without the chip refusing it.
;
; The only sample in the set whose expected answer is a deviation from
; what the 8086 does rather than an addition to it, and it is marked as
; such here so that it stays findable. Every other sample asserts the
; chip's behaviour: the 186 extensions add instructions the 8086 does not
; have, whereas this one takes an opcode the 8086 does have and answers
; differently.
;
; The 8086's silicon executes `8E /1` -- it writes CS and carries on
; fetching from the new CS at the old IP. The manual promises nothing
; about the encoding, and the 286 and later either treat it as invalid or
; give it another meaning, so the choices were to execute it, to refuse
; it, or to ignore it. This machine refuses it with vector 6.
;
; The reasoning lives in README section 6, which has a column for this
; machine next to the 8086 and 186 ones for exactly this kind of case:
; on the 8086 the "execute" behaviour amounts to a jump, which is
; dangerous, almost unused, and has no corresponding benefit to imitate.
;
;   mov cs, ax      with AX = 0, so executing it would leave CS = 0 and
;                   the program would carry on to the next instruction;
;   mov bx, 0x5678  reached only if CS was written;
;   hlt
;
; Expected ending: VM86_FAULT with cpu->fault = 6, the invalid-opcode
; vector. The registers are not compared: a fault is a half-product and
; pinning what AX and BX happen to hold when one is raised would pin an
; accident of the implementation rather than a property of the machine.

        bits 16
        org 0x100

        mov     ax, 0x0000
        mov     cs, ax                  ; `8e /1`
        mov     bx, 0x5678
        hlt
