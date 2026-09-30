; illegal_ff7 -- an FF group encoding that names no operation.
;
; Opcode FF is a group of eight, and seven of them are things: INC, DEC,
; CALL near, CALL far, JMP near, JMP far, PUSH. The eighth has no
; definition on any x86 -- there has never been an `/7` in this group --
; so the correct answer is the invalid-opcode exception, vector 6, and
; not "whatever the nearest case in the switch was".
;
; That failure mode is the reason this sample exists, and it is the shape
; of a group-encoding mistake rather than a single-opcode one: a
; `switch (mr.reg)` with seven cases and no default falls through to
; whatever is written last, so `FF /7` quietly becomes a far call or a
; push. The program that used it -- a CPU probe, most likely, or an
; assembler that emitted a byte it should not have -- gets a plausible
; answer instead of a refusal, and the bug is invisible until something
; depends on the refusal.
;
; The instruction before the bad encoding is there so the sample is not
; three bytes long and so it is visible that the machine really ran
; before it stopped. The registers are not compared -- the state an
; instruction leaves behind when it faults is a half-product, as the
; fault case's own comment in replay.h says -- so what this sample
; asserts is the ending and the vector.
;
; Expected ending: VM86_FAULT with cpu->fault = 6.

        bits 16
        org 0x100

        mov     ax, 0x1234
        db 0xff, 0x38                   ; FF /7, mod=00 r/m=000: no such op
        mov     bx, 0x5678              ; never reached
        hlt
