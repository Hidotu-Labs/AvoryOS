#ifndef FB_FB_H
#define FB_FB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../lock/spinlock.h"

/* ── Linux fbdev Visuals ─────────────────────────────────────────────────── */
#define FB_VISUAL_MONO01             0   /* Monochr. 1=Black 0=White */
#define FB_VISUAL_MONO10             1   /* Monochr. 1=White 0=Black */
#define FB_VISUAL_TRUECOLOR          2   /* True color */
#define FB_VISUAL_PSEUDOCOLOR        3   /* Pseudo color (like atari) */
#define FB_VISUAL_DIRECTCOLOR        4   /* Direct color */
#define FB_VISUAL_STATIC_PSEUDOCOLOR 5   /* Pseudo color readonly */
#define FB_VISUAL_FOURCC             6   /* Visual identified by a V4L2 FOURCC */

/* ── Linux fbdev Types ───────────────────────────────────────────────────── */
#define FB_TYPE_PACKED_PIXELS        0   /* Packed Pixels */
#define FB_TYPE_PLANES               1   /* Non interleaved planes */
#define FB_TYPE_INTERLEAVED_PLANES   2   /* Interleaved planes */
#define FB_TYPE_TEXT                 3   /* Text/Attributes */
#define FB_TYPE_VGA_PLANES           4   /* EGA/VGA planes */
#define FB_TYPE_FOURCC               5   /* Type identified by a V4L2 FOURCC */

/* ── Linux fbdev Activation ──────────────────────────────────────────────── */
#define FB_ACTIVATE_NOW              0   /* Set values immediately (or vbl)*/
#define FB_ACTIVATE_NXTOPEN          1   /* Activate on next open */
#define FB_ACTIVATE_TEST             2   /* Don't set, round up impossible */
#define FB_ACTIVATE_MASK             15
#define FB_ACTIVATE_VBL              16  /* Activate values on next vbl */
#define FB_CHANGE_CMAP_VBL           32  /* Change colormap on vbl */
#define FB_ACTIVATE_ALL              64  /* Change all VCs on this fb */
#define FB_ACTIVATE_FORCE            128 /* Force apply even if no change */
#define FB_ACTIVATE_INV_MODE         256 /* Invert video mode flags */

/* ── Linux fbdev Blanking Modes ──────────────────────────────────────────── */
#define FB_BLANK_UNBLANK             0   /* Screen on */
#define FB_BLANK_NORMAL              1   /* Blank screen */
#define FB_BLANK_VSYNC_SUSPEND       2   /* VSYNC suspended */
#define FB_BLANK_HSYNC_SUSPEND       3   /* HSYNC suspended */
#define FB_BLANK_POWERDOWN           4   /* Power off */

/* ── Linux fbdev Acceleration ────────────────────────────────────────────── */
#define FB_ACCEL_NONE                0   /* No hardware acceleration */

/* ── Linux fbdev IOCTLs ──────────────────────────────────────────────────── */
#define FBIOGET_VSCREENINFO          0x4600
#define FBIOPUT_VSCREENINFO          0x4601
#define FBIOGET_FSCREENINFO          0x4602
#define FBIOGETCMAP                  0x4604
#define FBIOPUTCMAP                  0x4605
#define FBIOPAN_DISPLAY              0x4606
#define FBIO_CURSOR                  0x4608
#define FBIOGET_CON2FBMAP            0x460F
#define FBIOPUT_CON2FBMAP            0x4610
#define FBIOBLANK                    0x4611
#define FBIO_WAITFORVSYNC            0x40044620
#define FBIODIRTYRECT                0x4620

/* ── Linux fbdev Data Structures ─────────────────────────────────────────── */

struct fb_bitfield {
    uint32_t offset;    /* beginning of bitfield */
    uint32_t length;    /* length of bitfield */
    uint32_t msb_right; /* != 0: Most significant bit is right */
};

struct fb_var_screeninfo {
    uint32_t xres;                /* visible resolution */
    uint32_t yres;
    uint32_t xres_virtual;        /* virtual resolution */
    uint32_t yres_virtual;
    uint32_t xoffset;             /* offset from virtual to visible */
    uint32_t yoffset;             /* resolution */

    uint32_t bits_per_pixel;      /* bits per pixel */
    uint32_t grayscale;           /* 0 = color, 1 = grayscale, >1 = FOURCC */
    struct fb_bitfield red;       /* bitfield in fb mem if true colour */
    struct fb_bitfield green;
    struct fb_bitfield blue;
    struct fb_bitfield transp;    /* transparency */

    uint32_t nonstd;              /* != 0 Non standard pixel format */

    uint32_t activate;            /* see FB_ACTIVATE_* */

    uint32_t height;              /* height of picture in mm */
    uint32_t width;               /* width of picture in mm */

    uint32_t accel_flags;         /* obsolete */

    /* Timing */
    uint32_t pixclock;            /* pixel clock in ps */
    uint32_t left_margin;         /* time from sync to picture */
    uint32_t right_margin;        /* time from picture to sync */
    uint32_t upper_margin;        /* time from sync to picture */
    uint32_t lower_margin;
    uint32_t hsync_len;           /* length of horizontal sync */
    uint32_t vsync_len;           /* length of vertical sync */
    uint32_t sync;
    uint32_t vmode;
    uint32_t rotate;              /* angle we rotate counter clockwise */
    uint32_t colorspace;          /* colorspace for FOURCC-based modes */
    uint32_t reserved[4];         /* Reserved for future compatibility */
};

struct fb_fix_screeninfo {
    char id[16];                  /* identification string e.g. "AvoryFB" */
    unsigned long smem_start;     /* Start of frame buffer mem (physical address) */
    uint32_t smem_len;            /* Length of frame buffer mem */
    uint32_t type;                /* see FB_TYPE_* */
    uint32_t type_aux;            /* Interleave for interleaved Planes */
    uint32_t visual;              /* see FB_VISUAL_* */
    uint16_t xpanstep;            /* zero if no hardware panning */
    uint16_t ypanstep;            /* zero if no hardware panning */
    uint16_t ywrapstep;           /* zero if no hardware ywrap */
    uint32_t line_length;         /* length of a line in bytes */
    unsigned long mmio_start;     /* Start of Memory Mapped I/O */
    uint32_t mmio_len;            /* Length of Memory Mapped I/O */
    uint32_t accel;               /* Indicate to driver which specific chip */
    uint16_t capabilities;
    uint16_t reserved[2];         /* Reserved for future compatibility */
};

struct fb_cmap {
    uint32_t start;               /* First entry */
    uint32_t len;                 /* Number of entries */
    uint16_t *red;                /* Red values */
    uint16_t *green;
    uint16_t *blue;
    uint16_t *transp;             /* transparency, can be NULL */
};

struct fb_copyarea {
    uint32_t dx;
    uint32_t dy;
    uint32_t width;
    uint32_t height;
    uint32_t sx;
    uint32_t sy;
};

struct fb_fillrect {
    uint32_t dx;
    uint32_t dy;
    uint32_t width;
    uint32_t height;
    uint32_t color;
    uint32_t rop;
};

struct fb_image {
    uint32_t dx;
    uint32_t dy;
    uint32_t width;
    uint32_t height;
    uint32_t fg_color;
    uint32_t bg_color;
    uint8_t  depth;
    const char *data;
    struct fb_cmap cmap;
};

/* ── Kernel Driver Representation ────────────────────────────────────────── */

struct fb_info;
struct vfs_node;

struct fb_ops {
    int (*fb_open)(struct fb_info *info, int user);
    int (*fb_release)(struct fb_info *info, int user);
    uint32_t (*fb_read)(struct fb_info *info, uint32_t offset, uint32_t count, uint8_t *buf);
    uint32_t (*fb_write)(struct fb_info *info, uint32_t offset, uint32_t count, const uint8_t *buf);
    int (*fb_check_var)(struct fb_var_screeninfo *var, struct fb_info *info);
    int (*fb_set_par)(struct fb_info *info);
    int (*fb_blank)(int blank, struct fb_info *info);
    int (*fb_pan_display)(struct fb_var_screeninfo *var, struct fb_info *info);
    void (*fb_fillrect)(struct fb_info *info, const struct fb_fillrect *rect);
    void (*fb_copyarea)(struct fb_info *info, const struct fb_copyarea *region);
    void (*fb_imageblit)(struct fb_info *info, const struct fb_image *image);
    uint64_t (*fb_mmap)(struct fb_info *info, uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t offset);
    int (*fb_ioctl)(struct fb_info *info, uint32_t cmd, uint64_t arg);
};

struct fb_info {
    int node;                     /* fb index (0 for fb0) */
    struct fb_var_screeninfo var; /* Current var screeninfo */
    struct fb_fix_screeninfo fix; /* Current fix screeninfo */
    struct fb_ops *fbops;
    void *screen_base;            /* Frontbuffer virtual address (HHDM VRAM) */
    uint64_t screen_size;         /* Total frontbuffer size in bytes */
    uint64_t phys_base;           /* Physical start address */
    
    /* Double buffering / Dirty tracking */
    void *backbuffer;             /* System RAM cacheable backbuffer */
    bool backbuffer_enabled;      /* Rendering targets backbuffer when true */
    bool is_dirty;                /* Pending blit to frontbuffer */
    uint32_t dirty_min_x;
    uint32_t dirty_min_y;
    uint32_t dirty_max_x;
    uint32_t dirty_max_y;

    int kd_mode;                  /* KD_TEXT (0) or KD_GRAPHICS (1) */
    bool blanked;
    spinlock_t lock;
    struct vfs_node *vfs_node;
    void *par;
};

#endif /* FB_FB_H */
