/* fb.h -- linear-framebuffer graphics output.
 *
 * The bootloader sets a linear-framebuffer mode -- GOP under UEFI
 * (bootloader/boot.c), VBE under BIOS (bootloader/boot.asm) -- and leaves a
 * descriptor in low memory. fb_init() reads it, maps the framebuffer
 * into the kernel address space, and exposes simple drawing primitives. Colors are
 * 0x00RRGGBB; the module converts to the framebuffer's pixel format.
 */
#ifndef NEXUS_FB_H
#define NEXUS_FB_H

#include "types.h"

typedef struct {
    uint8_t  *addr;     /* mapped framebuffer base (identity-mapped to phys) */
    uint32_t  pitch;    /* bytes per scanline                                */
    uint32_t  width;    /* pixels                                            */
    uint32_t  height;   /* pixels                                            */
    uint32_t  bpp;      /* bits per pixel (24 or 32)                         */
    bool      active;
} framebuffer_t;

bool                 fb_init(void);
bool                 fb_active(void);
const framebuffer_t *fb_get(void);

/* Early text console drawn straight to the framebuffer. Live from fb_init()
 * until the window manager's terminal window takes over; see fb.c. */
void fb_early_init(void);
bool fb_early_active(void);
void fb_early_putc(char c);

void     fb_clear(uint32_t color);
void     fb_putpixel(uint32_t x, uint32_t y, uint32_t color);
uint32_t fb_getpixel(uint32_t x, uint32_t y);
void     fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

static inline uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

#endif
