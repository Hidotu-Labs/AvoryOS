; Real-mode guest for the AVM string MMIO test, loaded at 0x1000 with paging
; off.  0xB8000 is not in any memslot, so each access is an MMIO exit.  REP
; STOSW writes four words of 0x0741 there.  REP LODSW reads one word, which
; the VMM answers with 0x4B4F.  The guest prints AL then AH to COM1: "OK".

bits 16
org 0x1000

start:
    mov ax, 0xB800
    mov es, ax
    xor di, di
    mov ax, 0x0741
    mov cx, 4
    rep stosw               ; four MMIO writes at 0xB8000..0xB8006

    mov ax, 0xB800
    mov ds, ax
    xor si, si
    mov cx, 1
    rep lodsw               ; one MMIO read at 0xB8000

    mov dx, 0x3F8
    out dx, al              ; 'O'
    mov al, ah
    out dx, al              ; 'K'
    hlt
