#ifndef FB_TERMINAL_H
#define FB_TERMINAL_H

#include <stdint.h>

#define NCCS 32

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

struct termios {
  tcflag_t c_iflag;
  tcflag_t c_oflag;
  tcflag_t c_cflag;
  tcflag_t c_lflag;
  cc_t c_line;
  cc_t c_cc[NCCS];
  speed_t __c_ispeed;
  speed_t __c_ospeed;
};

// Linux kernel ABI-compatible termios for TCGETS/TCSETS (ioctls 0x5401-0x5404).
// The Linux kernel uses NCCS=19 and no speed fields → 36 bytes total.
// glibc's __tcgetattr allocates exactly this size on the stack; writing more
// bytes would corrupt the stack and cause a crash.
#define KERNEL_NCCS 19

struct kernel_termios {
  tcflag_t c_iflag;
  tcflag_t c_oflag;
  tcflag_t c_cflag;
  tcflag_t c_lflag;
  cc_t c_line;
  cc_t c_cc[KERNEL_NCCS];
};

struct termios2 {
  tcflag_t c_iflag;
  tcflag_t c_oflag;
  tcflag_t c_cflag;
  tcflag_t c_lflag;
  cc_t c_line;
  cc_t c_cc[KERNEL_NCCS];
  speed_t c_ispeed;
  speed_t c_ospeed;
};

// termios flags (x86_64 Linux/Musl compatible)
#define ISIG 0000001
#define ICANON 0000002
#define ECHO 0000010
#define ECHOE 0000020
#define ECHOK 0000040
#define ECHONL 0000100
#define NOFLSH 0000200
#define TOSTOP 0000400

#define IGNBRK 0000001
#define BRKINT 0000002
#define IGNPAR 0000004
#define PARMRK 0000010
#define INPCK 0000020
#define ISTRIP 0000040
#define INLCR 0000100
#define IGNCR 0000200
#define ICRNL 0000400
#define IXON 0002000
#define IXOFF 0001000

#define OPOST 0000001
#define ONLCR 0000004

#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TCGETS2 0x802c542a
#define TCSETS2 0x402c542b
#define TCSETSW2 0x402c542c
#define TCSETSF2 0x402c542d
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGETD 0x5424
#define TIOCSETD 0x5423

#define VT_OPENQRY 0x5600
#define VT_GETMODE 0x5601
#define VT_SETMODE 0x5602
#define VT_GETSTATE 0x5603
#define VT_RELDISP 0x5605
#define VT_ACTIVATE 0x5606
#define VT_WAITACTIVE 0x5607
#define VT_DISALLOCATE 0x5608

#define KDSETMODE 0x4B3A
#define KDGETMODE 0x4B3B
#define KDGKBMODE 0x4B44
#define KDSKBMODE 0x4B45
#define KD_TEXT 0x00
#define KD_GRAPHICS 0x01

struct winsize {
  unsigned short ws_row;
  unsigned short ws_col;
  unsigned short ws_xpixel;
  unsigned short ws_ypixel;
};

extern struct termios console_termios;

#endif
