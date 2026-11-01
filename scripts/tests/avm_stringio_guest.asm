; Real-mode guest for the AVM string I/O test, loaded at 0x1000 with paging
; off.  The VMM writes "ABC" at 0x2000 and leaves 0x8000 zeroed.  Port 0x3F8
; and 0x3F9 are output ports the VMM reads.  Port 0x600 is an input the VMM
; supplies with "xyzq".  Expected output on 0x3F8: "ABCxyzqK".  Any mismatch
; in the register checks prints 'N' instead.

bits 16
org 0x1000

start:
    mov si, 0x2000          ; REP OUTS: "ABC" to 0x3F8
    mov cx, 3
    mov dx, 0x3F8
    rep outsb

    mov di, 0x3000          ; REP INS: "xyz" from 0x600 into 0x3000
    mov cx, 3
    mov dx, 0x600
    rep insb

    mov si, 0x3000          ; REP OUTS: "xyz" from 0x3000
    mov cx, 3
    mov dx, 0x3F8
    rep outsb

    mov di, 0x3010          ; single INS: "q" from 0x600 into 0x3010
    mov dx, 0x600
    insb
    cmp di, 0x3011          ; DI must move on by one byte
    je di_ok
    mov dx, 0x3F8
    mov al, 'N'
    out dx, al
di_ok:
    mov dx, 0x3F8
    mov al, [0x3010]
    out dx, al

    mov si, 0x8000          ; bulk REP OUTS: 5000 zero bytes to 0x3F9
    mov cx, 5000
    mov dx, 0x3F9
    rep outsb
    cmp si, 0x9388          ; SI moves past all 5000 bytes, CX ends at zero
    je si_ok
    mov dx, 0x3F8
    mov al, 'N'
    out dx, al
si_ok:
    cmp cx, 0
    je cx_ok
    mov dx, 0x3F8
    mov al, 'N'
    out dx, al
cx_ok:
    mov dx, 0x3F8
    mov al, 'K'
    out dx, al
    hlt
