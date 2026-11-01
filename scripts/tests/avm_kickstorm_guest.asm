; Real-mode guest for the AVM kick stress test, loaded at 0x1000.  It writes to
; port 0x3F9 forever, so every iteration is an exit to the VMM, and the VMM can
; kick it at any time.  The VMM also toggles a memslot from another thread.

bits 16
org 0x1000

start:
    mov dx, 0x3F9
loop:
    out dx, al
    jmp loop
