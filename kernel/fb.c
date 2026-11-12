/* fb.c -- linear-framebuffer graphics output. See fb.h. */

#include "fb.h"
#include "gfx.h"

/* Descriptor the bootloader leaves in low memory: boot.c after GOP setup on
 * the UEFI path, boot.asm after VBE setup on the BIOS path. Same layout from
 * both (VBE bases are 32-bit, so boot.asm zeroes the high dword):
 *   0x9700 : u64 physical LFB base   (64-bit: real UEFI may park it >4 GiB)
 *   0x9708 : u32 pitch, 0x970C width, 0x9710 height, 0x9714 bpp, 0x9718 valid */
#define FB_INFO_ADDR 0x9700

static framebuffer_t fb;

bool fb_init(void) {
    uint64_t           lfb  = *(volatile uint64_t *)(uintptr_t)FB_INFO_ADDR;
    volatile uint32_t *info = (volatile uint32_t *)(uintptr_t)(FB_INFO_ADDR + 8);

    uint32_t pitch  = info[0];
    uint32_t width  = info[1];
    uint32_t height = info[2];
    uint32_t bpp    = info[3];
    uint32_t valid  = info[4];

    if (!valid || lfb == 0 || width == 0 || height == 0) {
        fb.active = false;
        return false;
    }

    /* No page-table work here, which is what lets this run as the very first
     * thing in boot: before vmm_init() we are still on the boot page tables --
     * under UEFI the firmware's, which map the LFB because the firmware was
     * drawing on it; under BIOS kernel_entry_bios.asm's, which identity-map
     * the whole low 4 GiB, where VBE puts the LFB -- and after
     * vmm_init() the kernel's own tables cover it too (see map_framebuffer in
     * vmm.c). Mapping it from this side used to fail outright on any machine
     * with >= 4 GiB of RAM, because the identity map had already claimed the
     * MMIO hole with 2 MiB leaves that vmm_map refused to touch. */
    fb.addr   = (uint8_t *)(uintptr_t)lfb;
    fb.pitch  = pitch;
    fb.width  = width;
    fb.height = height;
    fb.bpp    = bpp;
    fb.active = true;
    return true;
}

bool fb_active(void) { return fb.active; }

const framebuffer_t *fb_get(void) { return &fb; }

void fb_putpixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb.active || x >= fb.width || y >= fb.height) return;
    uint8_t *p = fb.addr + (uint64_t)y * fb.pitch + (uint64_t)x * (fb.bpp / 8);
    if (fb.bpp == 32) {
        *(uint32_t *)p = color;
    } else {                       /* 24 bpp */
        p[0] = (uint8_t)(color & 0xFF);
        p[1] = (uint8_t)((color >> 8) & 0xFF);
        p[2] = (uint8_t)((color >> 16) & 0xFF);
    }
}

uint32_t fb_getpixel(uint32_t x, uint32_t y) {
    if (!fb.active || x >= fb.width || y >= fb.height) return 0;
    uint8_t *p = fb.addr + (uint64_t)y * fb.pitch + (uint64_t)x * (fb.bpp / 8);
    if (fb.bpp == 32) {
        return *(uint32_t *)p;
    }
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!fb.active) return;
    uint32_t x1 = x + w, y1 = y + h;
    if (x1 > fb.width)  x1 = fb.width;
    if (y1 > fb.height) y1 = fb.height;

    for (uint32_t yy = y; yy < y1; yy++) {
        uint8_t *row = fb.addr + (uint64_t)yy * fb.pitch + (uint64_t)x * (fb.bpp / 8);
        if (fb.bpp == 32) {
            uint32_t *px = (uint32_t *)row;
            for (uint32_t xx = x; xx < x1; xx++) *px++ = color;
        } else {
            uint8_t *px = row;
            for (uint32_t xx = x; xx < x1; xx++) {
                px[0] = (uint8_t)(color & 0xFF);
                px[1] = (uint8_t)((color >> 8) & 0xFF);
                px[2] = (uint8_t)((color >> 16) & 0xFF);
                px += 3;
            }
        }
    }
}

void fb_clear(uint32_t color) {
    fb_fill_rect(0, 0, fb.width, fb.height, color);
}

/* ---- early on-screen console ---------------------------------------------
 * fbcon only maintains a character grid; the window-manager compositor is what
 * renders it, and that does not start until the last line of kernel_main. On a
 * UEFI machine there is no VGA text buffer at 0xB8000 and usually no serial
 * port either, so until the WM comes up the kernel is booting completely blind
 * -- any failure before that point looks identical to a dead machine.
 *
 * This draws glyphs straight to the framebuffer so boot output is on screen
 * from the first line. console.c hands over to fbcon once the WM owns the
 * screen. */

#define EC_SCALE   2
#define EC_CELL_W  (8 * EC_SCALE)
#define EC_CELL_H  (8 * EC_SCALE)
#define EC_BG      0x10101C
#define EC_FG      0xDCDCDC

static surface_t ec_surface;
static uint32_t  ec_col, ec_row, ec_cols, ec_rows;
static bool      ec_on;

void fb_early_init(void) {
    if (!fb.active) return;
    ec_surface = (surface_t){ fb.addr, fb.pitch, fb.width, fb.height, fb.bpp };
    ec_cols = fb.width  / EC_CELL_W;
    ec_rows = fb.height / EC_CELL_H;
    ec_col  = 0;
    ec_row  = 0;
    ec_on   = true;
    fb_clear(EC_BG);
}

bool fb_early_active(void) { return ec_on; }

void fb_early_putc(char c) {
    if (!ec_on) return;

    if (c == '\r') { ec_col = 0; return; }
    if (c == '\b') { if (ec_col > 0) ec_col--; return; }

    if (c != '\n') {
        if ((unsigned char)c < 0x20) return;
        gfx_glyph(&ec_surface, (int)(ec_col * EC_CELL_W), (int)(ec_row * EC_CELL_H),
                  c, EC_FG, EC_BG, EC_SCALE, true);
        if (++ec_col < ec_cols) return;
    }

    ec_col = 0;
    if (++ec_row < ec_rows) return;

    /* Wrap to the top instead of scrolling: scrolling would mean reading the
     * whole framebuffer back out, and reads from the aperture are glacial. */
    ec_row = 0;
    fb_clear(EC_BG);
}
