; Real-mode guest for the AVM ioeventfd test.  Assembled with nasm -f bin and
; loaded at guest-physical 0x1000.
;
; The VMM registers three ioeventfds before running this guest:
;   port 0x600, any data     - 'X' is written here, and the VMM must not see it
;   port 0x601, data 'Y' only - 'N' misses and exits to the VMM, 'Y' matches
;   address 0xD0000000       - an MMIO write that hits, so the VMM must not see it
; Expected: the VMM receives only 'N' (on the port) and 'K' (on COM1).  The
; a32 prefix gives the 32-bit MMIO address in real mode.

bits 16
org 0x1000

start:
    cli
    mov sp, 0x3000

    mov dx, 0x600
    mov al, 'X'
    out dx, al

    mov dx, 0x601
    mov al, 'N'
    out dx, al
    mov al, 'Y'
    out dx, al

    mov al, 'M'
    a32 mov [0xD0000000], al

    mov dx, 0x3F8
    mov al, 'K'
    out dx, al
    hlt
