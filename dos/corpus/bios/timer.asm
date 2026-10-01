; timer -- hardware interrupt delivery, and the 08h -> 1Ch chain.
;
; Nothing else in this corpus posts an interrupt of its own; every other
; sample is the guest calling the firmware. This one is the other
; direction: the host raises IRQ0, the firmware counts it, and the program
; sees the count without ever asking.
;
; The chain it exercises is the one the machine actually has:
;
;   the host raises vector 08h        (vm86_raise, at an instruction bound)
;     the run loop delivers it        (vm86_interrupt: push, jump to IVT[8])
;       IVT[8] is the stub            (FE 38 08 CF)
;         the trap calls bios1a_irq   (the count at 0040:006C goes up)
;           which calls INT 1Ch       (vm86_interrupt again)
;             IVT[1Ch] is OURS        (this program wrote it)
;               so the handler runs  (counts into 1000:0802, then IRET)
;             IRET lands back on the 08h stub's CF
;           which IRETs to the code that was interrupted
;
; Three separate M4 claims ride on that: the trap, the 08h -> 1Ch chain,
; and a guest's own vector being the one that runs -- which is what a TSR
; does, and what M4-3 says needs no code of its own.
;
; The count lives at 0040:006C and is "ticks since midnight", incremented
; by the INT 08h handler at 18.2 Hz with 0x1800B0 ticks in a day
; (docs/dos-refs.md section 6). This program never reads it through INT 1Ah
; and never divides it, so the rate only matters in that the host must
; raise exactly as many ticks as the plan says; the counter it does read is
; its own, at 1000:0802.
;
; The program waits with `hlt` rather than spinning. With IF=1 that is a
; real wait -- M4-6 -- and the wake-up comes from the run loop delivering
; the interrupt the host raised while the processor was stopped. If the run
; loop delivers before checking whether the machine is halted, or checks
; the halt before delivery, the machine stops here forever instead of
; counting to five, and the suite reports it as "did not finish" rather
; than as a wrong screen.
;
; `sti` before the wait is not decoration. The entry convention gives
; FLAGS = 0xF002, which has IF CLEAR, unlike real DOS -- so a program that
; wants interrupts has to enable them itself, and this is the sample where
; forgetting that is fatal rather than cosmetic.
;
; Entry convention: the corpus convention, from replay.h -- a flat binary
; at 0x100 of its own segment, CS=DS=ES=SS=0x1000, SP=0xFFFE, FLAGS=0xF002.
; IF is clear at entry
; and this program sets it.
;
; Expected at HLT, with the host having raised exactly five ticks:
;   page 0, cell 0 = '5', attribute 0x07 -- the attribute the mode set's
;     clear left, because 0Eh writes no attribute byte (dos-refs section 1)
;   0040:006C = 5                 the firmware counted what was raised
;   1000:0802 = 5                 our own handler was called as often

        bits 16
        org 0x100

        ; Mode 3 first: 0Eh writes no attribute byte, so without a mode
        ; set the digit lands on a page whose attributes are all 0x00 and
        ; nobody can see it. See hello.asm.
        mov     ax, 0x0003
        int     0x10

        ; The user timer hook: 08h calls 1Ch when it has finished its own
        ; work.
        ;
        ; The table is at address zero and this program is not, so the hook
        ; takes a segment: an offset stored as a bare number would be read
        ; out of the program's own segment, which holds the program. The
        ; handler's segment is CS, and this is the one place a sample has
        ; to say so.
        xor     ax, ax
        mov     es, ax
        mov     word [es:0x1C*4], tick
        mov     ax, cs
        mov     word [es:0x1C*4+2], ax

        sti

.wait:
        hlt
        mov     ax, [0x0802]
        cmp     ax, 5
        jb      .wait

        cli

        ; say how many we counted
        mov     al, [0x0802]
        add     al, '0'
        mov     ah, 0x0E
        mov     bh, 0x00
        mov     bl, 0x07
        int     0x10

        cli
        hlt

tick:
        inc     word [0x0802]
        iret
