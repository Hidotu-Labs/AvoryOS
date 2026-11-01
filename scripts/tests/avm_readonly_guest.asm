; Real-mode guest for the AVM read-only memory test, loaded at 0x1000.  The
; VMM maps 0x100000 read-only with 'R' as its first byte.  The guest reads the
; byte, writes 'X' to it, and reads it again.  Each read is written to COM1, so
; the expected output is "RRRK".  The write must reach the VMM as an MMIO exit
; and must not change the byte, so the reads after it are still 'R'.  The a32
; prefix gives the 32-bit address in real mode.

bits 16
org 0x1000

start:
    a32 mov al, [0x100000]
    mov dx, 0x3F8
    out dx, al
    a32 mov al, [0x100000]
    out dx, al
    mov al, 'X'
    a32 mov [0x100000], al
    a32 mov al, [0x100000]
    out dx, al
    mov al, 'K'
    out dx, al
    hlt
