; key -- a blocking keyboard read, which is the one call that can wait.
;
; INT 16h AH=00h means "give me a keystroke, and if there is not one yet,
; wait until there is", and it returns the scan code in AH and the ASCII in
; AL (docs/dos-refs.md section 5). On real hardware that wait is a spin with
; interrupts enabled, and the keystroke arrives from IRQ1 in the middle of
; it. A single-threaded emulator cannot spin inside a service -- the host
; would never get the processor back and the key would never be delivered
; -- so the machine does something else that looks the same from the guest's
; side: the trap is re-run (vm86_service_retry), the run loop returns at the
; end of its slice, the host feeds the key and raises 09h, and the guest's
; read is answered by the next attempt.
;
; Which is why this sample is the acceptance for that mechanism and not just
; for the keyboard: what it has to show is that the guest WAITS. A service
; that answers a blocking read with whatever was left in the accumulator
; finishes immediately and prints a character nobody typed, and this program
; cannot tell the difference from inside -- but the suite can, because it
; looks at the machine after several slices in which no key was fed and
; requires that the program has not finished and that the screen is still
; untouched.
;
; What the stack does while it waits is NOT what the older note in
; host.h's neighbourhood described. A retry under the current trap does not
; pop the interrupt frame and re-execute the INT; it re-runs the trap and
; leaves the frame where the guest's INT put it (see vm86_service_retry in
; host.h and trap.c). So while the program is waiting, SP is six below
; where it started -- 0xFFF8 -- and stays there, and the stub's IRET unwinds
; it once, when the read finally succeeds. The suite asserts SP == 0xFFF8
; while waiting and SP == 0xFFFE after the echo.
;
; The key the host feeds is the scan code for 'A' with no shift held --
; 0x1E in set 1 -- and the character echoed is therefore a lowercase 'a',
; which is what the standard BIOS table gives for an unshifted letter.
; Nothing here decodes a scan code; that is INT 09h's job. It is also the
; one part of the keyboard with no entry in docs/dos-refs.md, which says so
; in its section 9: the table is a few hundred rows and belongs to the
; implementation, so the expected character is task C's decision and is
; written down here as one.
;
; `sti` is required for the same reason it is in timer.asm: the entry
; convention gives IF clear, and without it the retries could never be
; interrupted and the wait would be permanent.
;
; Entry convention: the corpus convention, from replay.h -- a flat binary
; at 0x100 of its own segment, CS=DS=ES=SS=0x1000, SP=0xFFFE, FLAGS=0xF002.
; IF is clear at entry
; and this program sets it.
;
; Expected at HLT, with the host having fed scancode 0x1E after several
; slices in which it fed nothing:
;   before the key: not halted, SP = 0xFFF8 (the INT's frame is still up),
;     and the screen untouched -- no present has shown a character
;   page 0, cell 0 = 'a', attribute 0x07, cursor at cell 1
;   SP = 0xFFFE once more: the stub's IRET took the frame back

        bits 16
        org 0x100

        ; Mode 3 first: 0Eh writes no attribute byte, so without a mode
        ; set the echoed character lands on a page of attribute 0x00 and
        ; nobody can see it. See hello.asm.
        mov     ax, 0x0003
        int     0x10

        sti

        mov     ah, 0x00                ; read a key, waiting if necessary
        int     0x16

        mov     ah, 0x0E                ; echo the character in AL
        mov     bh, 0x00
        mov     bl, 0x07
        int     0x10

        cli
        hlt
