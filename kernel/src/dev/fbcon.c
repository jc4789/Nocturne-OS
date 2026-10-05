/* Framebuffer text console with a tiny ANSI escape parser. */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/fb.h"
#include "dev/fbcon.h"
#include "mm/pmm.h"

#define BANNER_H 72

static canvas_t cv;
static bool ready, enabled;
static int cols, rows, cx, cy, top;
static uint32_t fg, bg;
static bool bold;
static int dirty_y0 = 1 << 30, dirty_y1 = -1;
static int esc_state, esc_args[8], esc_nargs;

static const uint32_t palette[16] = {
    0xFF0B0E1A, 0xFFE0566B, 0xFF6BD68B, 0xFFE8C36A, 0xFF6A8CE8, 0xFFB57CE8, 0xFF5CC8D6, 0xFFC9CCE0,
    0xFF4A5070, 0xFFFF7A8E, 0xFF8EF0AA, 0xFFFFDC8A, 0xFF8EAEFF, 0xFFD3A0FF, 0xFF85E8F2, 0xFFFFFFFF,
};
#define DEF_FG palette[7]
#define DEF_BG 0xFF0B0E1A

static void mark(int y0, int y1) {
    if (y0 < dirty_y0) dirty_y0 = y0;
    if (y1 > dirty_y1) dirty_y1 = y1;
}

static void flush(void) {
    if (!enabled || dirty_y1 < 0) return;
    fb_present(cv.px, cv.pitch, 0, dirty_y0, cv.w, dirty_y1 - dirty_y0);
    dirty_y0 = 1 << 30;
    dirty_y1 = -1;
}

static void draw_banner(void) {
    gfx_gradient_v(&cv, 0, 0, cv.w, BANNER_H, 0xFF1A1440, 0xFF0B0E1A);
    /* a little moon */
    gfx_fill_circle(&cv, 40, 36, 20, 0xFFF4E9C1);
    gfx_fill_circle(&cv, 50, 30, 18, 0xFF181338);
    /* stars */
    static const int stars[][2] = {{90, 14}, {130, 50}, {170, 22}, {250, 58}, {310, 12}, {420, 40}};
    for (unsigned i = 0; i < ARRAY_SIZE(stars); i++) gfx_pixel(&cv, cv.w - stars[i][0], stars[i][1], 0xFFFFFFFF);
    gfx_text(&cv, 76, 12, OS_NAME, 0xFFF4E9C1, TRANSPARENT, FONT_LARGE);
    gfx_text(&cv, 76 + 8 * 16 + 12, 28, "v" OS_VERSION " - a hobby OS dreamt up overnight", 0xFF8E9AC8,
             TRANSPARENT, FONT_SMALL);
    gfx_hline(&cv, 0, BANNER_H - 1, cv.w, 0xFF3A3470);
}

void fbcon_init(void) {
    uint64_t bytes = (uint64_t)fb.width * fb.height * 4;
    uint64_t phys = pmm_alloc_contig(ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE);
    if (!phys) return;
    gfx_init(&cv, phys_to_virt(phys), fb.width, fb.height, fb.width);
    top = BANNER_H + 4;
    cols = fb.width / 8;
    rows = (fb.height - top) / 16;
    fg = DEF_FG;
    bg = DEF_BG;
    gfx_fill(&cv, 0, 0, cv.w, cv.h, DEF_BG);
    draw_banner();
    ready = enabled = true;
    mark(0, cv.h);
    /* replay what was logged before the console existed */
    static char early[8192];
    size_t n = klog_read(early, sizeof early);
    fbcon_write(early, n);
}

void fbcon_clear(void) {
    if (!ready) return;
    gfx_fill(&cv, 0, top, cv.w, cv.h - top, DEF_BG);
    cx = cy = 0;
    mark(top, cv.h);
    flush();
}

static void scroll(void) {
    int line = 16 * cv.pitch;
    memmove(&cv.px[top * cv.pitch], &cv.px[top * cv.pitch + line], (size_t)(rows - 1) * line * 4);
    gfx_fill(&cv, 0, top + (rows - 1) * 16, cv.w, 16, DEF_BG);
    mark(top, top + rows * 16);
    cy = rows - 1;
}

static void newline(void) {
    cx = 0;
    if (++cy >= rows) scroll();
}

static void draw_cursor(bool on) {
    if (cx >= cols) return;
    int px = cx * 8, py = top + cy * 16;
    gfx_fill(&cv, px, py + 14, 8, 2, on ? 0xFFF4E9C1 : bg);
    mark(py, py + 16);
}

static void sgr(int n) {
    if (n == 0) { fg = DEF_FG; bg = DEF_BG; bold = false; }
    else if (n == 1) bold = true;
    else if (n == 22) bold = false;
    else if (n >= 30 && n <= 37) fg = palette[n - 30 + (bold ? 8 : 0)];
    else if (n == 39) fg = DEF_FG;
    else if (n >= 40 && n <= 47) bg = palette[n - 40];
    else if (n == 49) bg = DEF_BG;
    else if (n >= 90 && n <= 97) fg = palette[n - 90 + 8];
    else if (n >= 100 && n <= 107) bg = palette[n - 100 + 8];
}

static void putglyph(uint8_t g) {
    if (cx >= cols) newline();
    int px = cx * 8, py = top + cy * 16;
    gfx_char(&cv, px, py, g, fg, bg, FONT_SMALL);
    mark(py, py + 16);
    cx++;
}

static uint32_t utf8_cp;
static int utf8_need;

static void putc_raw(char ch) {
    uint8_t c = (uint8_t)ch;
    if (esc_state == 1) {
        if (c == '[') { esc_state = 2; esc_nargs = 0; esc_args[0] = 0; return; }
        esc_state = 0;
        return;
    }
    if (esc_state == 2) {
        if (c >= '0' && c <= '9') {
            if (esc_nargs == 0) esc_nargs = 1;
            esc_args[esc_nargs - 1] = esc_args[esc_nargs - 1] * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            if (esc_nargs == 0) esc_nargs = 1;
            if (esc_nargs < 8) esc_args[esc_nargs++] = 0;
            return;
        }
        if (c == '?') return;
        esc_state = 0;
        int a0 = esc_nargs ? esc_args[0] : 0;
        switch (c) {
        case 'm':
            if (!esc_nargs) sgr(0);
            for (int i = 0; i < esc_nargs; i++) sgr(esc_args[i]);
            break;
        case 'J':
            if (a0 == 2) fbcon_clear();
            break;
        case 'K':
            gfx_fill(&cv, cx * 8, top + cy * 16, cv.w - cx * 8, 16, bg);
            mark(top + cy * 16, top + cy * 16 + 16);
            break;
        case 'H': {
            int r = esc_nargs >= 1 && esc_args[0] ? esc_args[0] - 1 : 0;
            int cc = esc_nargs >= 2 && esc_args[1] ? esc_args[1] - 1 : 0;
            cy = MIN(r, rows - 1);
            cx = MIN(cc, cols - 1);
            break;
        }
        case 'A': cy = MAX(0, cy - MAX(1, a0)); break;
        case 'B': cy = MIN(rows - 1, cy + MAX(1, a0)); break;
        case 'C': cx = MIN(cols - 1, cx + MAX(1, a0)); break;
        case 'D': cx = MAX(0, cx - MAX(1, a0)); break;
        }
        return;
    }
    if (utf8_need) {
        if ((c & 0xC0) == 0x80) {
            utf8_cp = (utf8_cp << 6) | (c & 0x3F);
            if (--utf8_need == 0) putglyph(gfx_glyph_for(utf8_cp));
            return;
        }
        utf8_need = 0;
    }
    switch (c) {
    case 0x1B: esc_state = 1; return;
    case '\n': newline(); return;
    case '\r': cx = 0; return;
    case '\b': if (cx > 0) cx--; return;
    case '\t': do putglyph(' '); while (cx % 8); return;
    case 7: return;
    }
    if (c >= 0xC0) {
        utf8_need = c >= 0xF0 ? 3 : (c >= 0xE0 ? 2 : 1);
        utf8_cp = c & (0x3F >> utf8_need);
        return;
    }
    if (c < 32) return;
    putglyph(c);
}

void fbcon_write(const char *s, size_t n) {
    if (!ready || !enabled) return;
    uint64_t f = irq_save();
    draw_cursor(false);
    for (size_t i = 0; i < n; i++) putc_raw(s[i]);
    draw_cursor(true);
    flush();
    irq_restore(f);
}

void fbcon_write_kernel(const char *s, size_t n) {
    fbcon_write(s, n);
}

void fbcon_set_enabled(bool on) {
    enabled = on;
    if (on && ready) {
        mark(0, cv.h);
        flush();
    }
}

bool fbcon_enabled(void) { return ready && enabled; }

/* ---- the panic screen ---- */
#include "arch/cpu.h"

static int pr_y;
static void pline(canvas_t *c, const char *s, uint32_t col) {
    gfx_text(c, 48, pr_y, s, col, TRANSPARENT, FONT_SMALL);
    pr_y += 18;
}

static void panic_draw(const char *msg, struct regs *r) {
    if (!fb.addr) return;
    canvas_t c;
    /* draw straight into a full-screen buffer if we have one, else a scratch row approach */
    if (ready) c = cv;
    else return;
    gfx_noclip(&c);
    gfx_gradient_v(&c, 0, 0, c.w, c.h, 0xFF2A0F3A, 0xFF0A0614);
    gfx_fill_circle(&c, c.w - 120, 110, 50, 0xFFF4E9C1);
    gfx_fill_circle(&c, c.w - 100, 95, 46, 0xFF26103A);
    gfx_text(&c, 48, 48, "Nocturne had a nightmare :(", 0xFFFFFFFF, TRANSPARENT, FONT_LARGE);
    pr_y = 110;
    pline(&c, "The kernel hit a problem it could not recover from and has stopped.", 0xFFC9CCE0);
    pr_y += 10;
    pline(&c, msg, 0xFFFF8E9E);
    pr_y += 10;
    if (r) {
        char buf[160];
        ksnprintf(buf, sizeof buf, "RIP %016lx  RSP %016lx  RFLAGS %08lx  CS %lx SS %lx", r->rip, r->rsp, r->rflags, r->cs, r->ss);
        pline(&c, buf, 0xFFE8C36A);
        ksnprintf(buf, sizeof buf, "RAX %016lx  RBX %016lx  RCX %016lx  RDX %016lx", r->rax, r->rbx, r->rcx, r->rdx);
        pline(&c, buf, 0xFFC9CCE0);
        ksnprintf(buf, sizeof buf, "RSI %016lx  RDI %016lx  RBP %016lx  R8  %016lx", r->rsi, r->rdi, r->rbp, r->r8);
        pline(&c, buf, 0xFFC9CCE0);
        ksnprintf(buf, sizeof buf, "R9  %016lx  R10 %016lx  R11 %016lx  R12 %016lx", r->r9, r->r10, r->r11, r->r12);
        pline(&c, buf, 0xFFC9CCE0);
        ksnprintf(buf, sizeof buf, "R13 %016lx  R14 %016lx  R15 %016lx  ERR %lx", r->r13, r->r14, r->r15, r->error);
        pline(&c, buf, 0xFFC9CCE0);
        ksnprintf(buf, sizeof buf, "CR2 %016lx  CR3 %016lx  vector %lu", read_cr2(), read_cr3(), r->vector);
        pline(&c, buf, 0xFFC9CCE0);
        /* backtrace via frame pointers */
        pr_y += 8;
        pline(&c, "Backtrace:", 0xFFE8C36A);
        uint64_t *fp = (uint64_t *)r->rbp;
        for (int i = 0; i < 10 && fp && (uint64_t)fp > 0xFFFF800000000000ULL; i++) {
            ksnprintf(buf, sizeof buf, "  #%d  %016lx", i, fp[1]);
            pline(&c, buf, 0xFFC9CCE0);
            fp = (uint64_t *)fp[0];
        }
    }
    pr_y += 10;
    pline(&c, "Please restart the virtual machine. Sweet dreams.", 0xFF8E9AC8);
    enabled = true;
    fb_present(c.px, c.pitch, 0, 0, c.w, c.h);
}

void panic_screen(const char *msg) { panic_draw(msg, NULL); }

void panic_regs(const char *what, struct regs *r) {
    cli();
    char buf[256];
    ksnprintf(buf, sizeof buf, "%s (error %lx) at %p", what, r->error, (void *)r->rip);
    kprintf("\n*** KERNEL PANIC: %s\n", buf);
    kprintf("RIP=%p RSP=%p CR2=%p RAX=%p RBX=%p RCX=%p RDX=%p\n", (void *)r->rip, (void *)r->rsp,
            (void *)read_cr2(), (void *)r->rax, (void *)r->rbx, (void *)r->rcx, (void *)r->rdx);
    uint64_t *fp = (uint64_t *)r->rbp;
    for (int i = 0; i < 10 && fp && (uint64_t)fp > 0xFFFF800000000000ULL; i++) {
        kprintf("  #%d %p\n", i, (void *)fp[1]);
        fp = (uint64_t *)fp[0];
    }
    panic_draw(buf, r);
    for (;;) hlt();
}
