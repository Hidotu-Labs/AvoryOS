; Real-mode guest for the AVM single-step test, loaded at 0x1000.  The VMM turns
; single-step on, so each instruction traps (#DB) after it completes.  The out
; to COM1 is an I/O exit before its trap.  Expected debug RIPs, after each step:
; 0x1001, 0x1002, 0x1003, 0x1006, 0x1008, 0x1009.  Then the hlt.

bits 16
org 0x1000

start:
    nop
    nop
    nop
    mov dx, 0x3F8
    mov al, 'K'
    out dx, al
    hlt
