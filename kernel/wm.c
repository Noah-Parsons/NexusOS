/* wm.c -- window manager + compositor. See wm.h. */

#include "wm.h"
#include "gfx.h"
#include "fb.h"
#include "fbcon.h"
#include "mouse.h"
#include "sched.h"
#include "kmalloc.h"
#include "console.h"
#include "string.h"
#include "pit.h"

#define MAX_WINDOWS  8
#define TITLE_H      24
#define BORDER       2
#define LINE_H       18
#define TXT_SCALE    2
#define PAD          6
#define BTN_W        20          /* title-bar button width */
#define TASKBAR_H    30
#define TB_BTN_W     150
#define TB_GAP       6

typedef struct {
    int  x, y, w, h;            /* h is body height (title bar is extra) */
    const char *title;
    const char *const *lines;
    int  nlines;
    uint32_t body;
    bool is_terminal;
    bool closable;
    bool minimized;
    bool used;
} window_t;

static surface_t screen;
static surface_t back;
static bool      active;

static window_t  wins[MAX_WINDOWS];
static int       zorder[MAX_WINDOWS];   /* indices, front-most last */
static int       nwins;

static volatile bool layout_dirty;
static uint64_t      last_console_ver;
static uint64_t      last_sec;

static uint32_t COL_DESKTOP, COL_BORDER, COL_TITLE_A, COL_TITLE_I,
                COL_BODY, COL_BODY_TXT, COL_TITLE_TXT,
                COL_CLOSE, COL_MIN, COL_TASKBAR, COL_TB_BTN, COL_TB_ACTIVE;

/* ---- arrow cursor bitmap ------------------------------------------------ */
#define CUR_H 19
static const char *const arrow[CUR_H] = {
    "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X",
    "X.......X", "X........X", "X.........X", "X......XXXXX", "X...X..X",
    "X..XX..X", "X.X  X..X", "XX   X..X", "X     X..X", "      X..X", "       XX",
};

static void draw_cursor(int mx, int my) {
    for (int r = 0; r < CUR_H; r++) {
        const char *row = arrow[r];
        for (int c = 0; row[c]; c++) {
            if (row[c] == 'X')      gfx_pixel(&screen, mx + c, my + r, 0x000000);
            else if (row[c] == '.') gfx_pixel(&screen, mx + c, my + r, 0xFFFFFF);
        }
    }
}

/* ---- text helpers ------------------------------------------------------- */

/* Draw text but stop before any glyph would cross `right`, so window text can
 * never spill past the window border onto the desktop. */
static void draw_text_clip(int x, int y, const char *s, uint32_t fg, int right) {
    int cx = x;
    for (; *s; s++) {
        if (cx + 8 * TXT_SCALE > right) break;
        gfx_glyph(&back, cx, y, *s, fg, 0, TXT_SCALE, false);
        cx += 8 * TXT_SCALE;
    }
}

static int content_width(const char *title, const char *const *lines, int n) {
    int maxc = (int)strlen(title) + 1;
    for (int i = 0; i < n; i++) {
        int l = (int)strlen(lines[i]);
        if (l > maxc) maxc = l;
    }
    return maxc * (8 * TXT_SCALE) + 16;
}

/* ---- window model ------------------------------------------------------- */

static int win_total_h(const window_t *w) { return TITLE_H + w->h; }

static int new_window(int x, int y, int w, int h, const char *title,
                      const char *const *lines, int nlines,
                      uint32_t body, bool terminal, bool closable) {
    if (nwins >= MAX_WINDOWS) return -1;
    int id = nwins;
    wins[id] = (window_t){ .x = x, .y = y, .w = w, .h = h, .title = title,
                           .lines = lines, .nlines = nlines, .body = body,
                           .is_terminal = terminal, .closable = closable,
                           .minimized = false, .used = true };
    zorder[nwins] = id;
    nwins++;
    layout_dirty = true;
    return id;
}

int wm_add_window(int x, int y, int w, int body_h, const char *title,
                  const char *const *lines, int nlines) {
    return new_window(x, y, w, body_h, title, lines, nlines, COL_BODY, false, true);
}

/* topmost non-minimized window id, or -1 */
static int focused_id(void) {
    for (int i = nwins - 1; i >= 0; i--)
        if (!wins[zorder[i]].minimized) return zorder[i];
    return -1;
}

static void draw_window(const window_t *w, bool focused) {
    int th = win_total_h(w);
    gfx_fill(&back, w->x - BORDER, w->y - BORDER,
             w->w + 2 * BORDER, th + 2 * BORDER, COL_BORDER);
    gfx_fill(&back, w->x, w->y, w->w, TITLE_H, focused ? COL_TITLE_A : COL_TITLE_I);

    /* Title-bar buttons, right to left: [close][min]. */
    int rx = w->x + w->w;
    if (w->closable) {
        int b = rx - BTN_W;
        gfx_fill(&back, b, w->y, BTN_W, TITLE_H, COL_CLOSE);
        gfx_glyph(&back, b + 2, w->y + 4, 'X', 0xFFFFFF, 0, TXT_SCALE, false);
        rx = b;
    }
    {
        int b = rx - BTN_W;
        gfx_fill(&back, b, w->y, BTN_W, TITLE_H, COL_MIN);
        gfx_glyph(&back, b + 2, w->y + 4, '_', 0xFFFFFF, 0, TXT_SCALE, false);
        rx = b;
    }
    draw_text_clip(w->x + 6, w->y + 4, w->title, COL_TITLE_TXT, rx - 2);

    gfx_fill(&back, w->x, w->y + TITLE_H, w->w, w->h, w->body);
    if (w->is_terminal) {
        fbcon_render(&back, w->x + PAD, w->y + TITLE_H + PAD);
    } else {
        for (int i = 0; i < w->nlines; i++)
            draw_text_clip(w->x + 8, w->y + TITLE_H + 8 + i * LINE_H,
                           w->lines[i], COL_BODY_TXT, w->x + w->w - 4);
    }
}

/* 0 = none, 1 = close, 2 = minimize */
static int titlebar_button(const window_t *w, int mx, int my) {
    if (my < w->y || my >= w->y + TITLE_H) return 0;
    int rx = w->x + w->w;
    if (w->closable) {
        if (mx >= rx - BTN_W && mx < rx) return 1;
        rx -= BTN_W;
    }
    if (mx >= rx - BTN_W && mx < rx) return 2;
    return 0;
}

/* topmost visible window whose bounds contain (px,py); returns z-order slot */
static int window_at(int px, int py) {
    for (int i = nwins - 1; i >= 0; i--) {
        const window_t *w = &wins[zorder[i]];
        if (w->minimized) continue;
        if (px >= w->x - BORDER && px < w->x + w->w + BORDER &&
            py >= w->y - BORDER && py < w->y + win_total_h(w) + BORDER)
            return i;
    }
    return -1;
}

static void raise_slot(int slot) {
    if (slot < 0 || slot == nwins - 1) return;
    int id = zorder[slot];
    for (int i = slot; i < nwins - 1; i++) zorder[i] = zorder[i + 1];
    zorder[nwins - 1] = id;
    layout_dirty = true;
}

static void focus_id(int id) {
    wins[id].minimized = false;
    for (int i = 0; i < nwins; i++)
        if (zorder[i] == id) { raise_slot(i); return; }
}

static void close_window(int id) {
    for (int i = 0; i < nwins; i++)
        if (zorder[i] == id) {
            for (int j = i; j < nwins - 1; j++) zorder[j] = zorder[j + 1];
            nwins--;
            break;
        }
    wins[id].used = false;
    layout_dirty = true;
}

/* ---- taskbar ------------------------------------------------------------ */

/* Uptime as H:MM:SS. The PIT runs at 100 Hz (pit_init(100) in kernel.c).
 * Hours are unbounded, so the clock no longer wraps; the previous MM:SS
 * format wrapped every 100 minutes. out must hold at least 24 bytes. */
static void format_uptime(char *out) {
    uint64_t s  = pit_ticks() / 100;
    uint64_t hh = s / 3600, mm = (s / 60) % 60, ss = s % 60;

    char digits[20];
    int n = 0;
    do { digits[n++] = '0' + (char)(hh % 10); hh /= 10; } while (hh);

    int i = 0;
    while (n) out[i++] = digits[--n];
    out[i++] = ':';
    out[i++] = '0' + (char)(mm / 10); out[i++] = '0' + (char)(mm % 10);
    out[i++] = ':';
    out[i++] = '0' + (char)(ss / 10); out[i++] = '0' + (char)(ss % 10);
    out[i]   = '\0';
}

static void draw_taskbar(int fid) {
    int W = (int)back.width, H = (int)back.height;
    int ty = H - TASKBAR_H;
    gfx_fill(&back, 0, ty, W, TASKBAR_H, COL_TASKBAR);

    int bx = 6;
    for (int id = 0; id < MAX_WINDOWS; id++) {
        if (!wins[id].used) continue;
        bool act = (id == fid);
        gfx_fill(&back, bx, ty + 4, TB_BTN_W, TASKBAR_H - 8,
                 act ? COL_TB_ACTIVE : COL_TB_BTN);
        draw_text_clip(bx + 6, ty + (TASKBAR_H - 16) / 2, wins[id].title,
                       COL_TITLE_TXT, bx + TB_BTN_W - 4);
        bx += TB_BTN_W + TB_GAP;
    }

    char clk[24];
    format_uptime(clk);
    /* Right-aligned: the string grows as uptime passes 9 and 99 hours. */
    int cw = (int)strlen(clk) * 8 * TXT_SCALE;
    draw_text_clip(W - 6 - cw, ty + (TASKBAR_H - 16) / 2, clk, COL_TITLE_TXT, W - 4);
}

static int taskbar_at(int mx, int my) {
    int H = (int)back.height;
    int ty = H - TASKBAR_H;
    if (my < ty + 4 || my >= ty + TASKBAR_H - 4) return -1;
    int bx = 6;
    for (int id = 0; id < MAX_WINDOWS; id++) {
        if (!wins[id].used) continue;
        if (mx >= bx && mx < bx + TB_BTN_W) return id;
        bx += TB_BTN_W + TB_GAP;
    }
    return -1;
}

/* ---- input -------------------------------------------------------------- */

static void poll_input(void) {
    static bool prev_left;
    static bool dragging;
    static int  drag_id, drag_dx, drag_dy;

    int  mx   = mouse_x();
    int  my   = mouse_y();
    bool left = (mouse_buttons() & MOUSE_BTN_LEFT) != 0;

    if (left && !prev_left) {
        int tid = taskbar_at(mx, my);
        if (tid >= 0) {
            focus_id(tid);                  /* restore + raise */
        } else {
            int slot = window_at(mx, my);
            if (slot >= 0) {
                int id = zorder[slot];
                raise_slot(slot);
                int btn = titlebar_button(&wins[id], mx, my);
                if (btn == 1) {
                    close_window(id);
                } else if (btn == 2) {
                    wins[id].minimized = true; layout_dirty = true;
                } else if (my < wins[id].y + TITLE_H) {
                    dragging = true; drag_id = id;
                    drag_dx = mx - wins[id].x; drag_dy = my - wins[id].y;
                }
            }
        }
    }
    if (left && dragging && wins[drag_id].used) {
        wins[drag_id].x = mx - drag_dx;
        wins[drag_id].y = my - drag_dy;
        layout_dirty = true;
    }
    if (!left) dragging = false;
    prev_left = left;
}

/* ---- compose / present -------------------------------------------------- */

static void compose(void) {
    gfx_fill(&back, 0, 0, (int)back.width, (int)back.height, COL_DESKTOP);
    int fid = focused_id();
    for (int i = 0; i < nwins; i++) {
        int id = zorder[i];
        if (wins[id].minimized) continue;
        draw_window(&wins[id], id == fid);
    }
    draw_taskbar(fid);
}

static void present(void) {
    gfx_blit(&screen, &back);
    draw_cursor(mouse_x(), mouse_y());
}

static void compositor_task(void) {
    for (;;) {
        poll_input();
        uint64_t v = fbcon_version();
        uint64_t sec = pit_ticks() / 100;
        if (layout_dirty || v != last_console_ver || sec != last_sec) {
            compose();
            last_console_ver = v;
            last_sec = sec;
            layout_dirty = false;
        }
        present();
        sched_sleep_ms(16);
    }
}

/* ---- init --------------------------------------------------------------- */

static const char *const w_welcome[] = {
    "Welcome to NexusOS.",
    "",
    "A from-scratch x86-64 OS",
    "with a compositing window",
    "manager. Drag me by my",
    "title bar; click to raise.",
};
static const char *const w_system[] = {
    "x86-64 long mode",
    "Preemptive scheduler",
    "FAT12 storage",
    "VESA framebuffer 1024x768",
    "PS/2 keyboard + mouse",
};

void wm_init(void) {
    if (!fb_active()) return;
    const framebuffer_t *f = fb_get();

    screen = (surface_t){ f->addr, f->pitch, f->width, f->height, f->bpp };

    uint64_t sz = (uint64_t)f->pitch * f->height;
    uint8_t *mem = (uint8_t *)kmalloc(sz);
    if (!mem) { console_puts("[wm] back buffer alloc failed\n"); return; }
    back = (surface_t){ mem, f->pitch, f->width, f->height, f->bpp };

    COL_DESKTOP   = gfx_rgb(38, 44, 66);
    COL_BORDER    = gfx_rgb(70, 70, 95);
    COL_TITLE_A   = gfx_rgb(60, 95, 205);
    COL_TITLE_I   = gfx_rgb(75, 78, 95);
    COL_BODY      = gfx_rgb(238, 238, 244);
    COL_BODY_TXT  = gfx_rgb(24, 24, 34);
    COL_TITLE_TXT = gfx_rgb(245, 245, 250);
    COL_CLOSE     = gfx_rgb(200, 70, 70);
    COL_MIN       = gfx_rgb(90, 92, 110);
    COL_TASKBAR   = gfx_rgb(22, 26, 40);
    COL_TB_BTN    = gfx_rgb(55, 60, 82);
    COL_TB_ACTIVE = gfx_rgb(60, 95, 205);

    /* The terminal hosts the text console; size its grid, then make a window
     * exactly big enough to show it. */
    uint32_t cols = 46, rows = 19;
    fbcon_init(cols, rows);
    uint32_t cell = fbcon_cell();
    int tw = (int)(cols * cell) + 2 * PAD;
    int thh = (int)(rows * cell) + 2 * PAD;

    wm_add_window(70,  70, content_width("Welcome", w_welcome, 6),
                  6 * LINE_H + 16, "Welcome", w_welcome, 6);
    wm_add_window(540, 100, content_width("System", w_system, 5),
                  5 * LINE_H + 16, "System",  w_system, 5);
    new_window(70, 340, tw, thh, "Terminal", NULL, 0, fbcon_bg(), true, false);

    active = true;
    task_create("compositor", compositor_task);
    console_puts("NexusOS terminal. Type 'help' for commands.\n");
}
