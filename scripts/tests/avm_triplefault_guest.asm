; Real-mode guest for the AVM triple fault test, loaded at 0x1000.  The VMM
; gives it an empty IDT, so INT3 cannot be delivered: the fault becomes a double
; fault and then a triple fault, which the VMM sees as a shutdown exit.

bits 16
org 0x1000

start:
    int3
