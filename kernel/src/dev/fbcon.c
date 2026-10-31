/* The boot screen and the panic screen (dev/fbcon.h).

   The boot screen is a reader of the log like the others: a cursor of its own (with the console's
   text), whose text is drawn as a terminal draws it (UTF-8, wide characters, the few ANSI escapes
   a console app uses) on a canvas in RAM the size of the firmware's framebuffer, and the rows that
   changed are presented. Before log_start the writer draws (fbcon_boot_draw: one CPU, interrupts
   off, so nothing else touches the canvas); after it a thread of its own does, as the log's
   readers' interrupt wakes it, until the window manager takes the screen. A full screen scrolls
   by half of its rows at once, so that the whole text area is presented once per half a screen
   of lines rather than once per line.

   The canvas stays for the panic screen, which is drawn into it and presented without a lock or
   an allocation: every other CPU is stopped, and whatever they were doing to the canvas is drawn
   over. */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/fb.h"
#include "dev/fbcon.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "sys/clock.h"
#include "sys/log.h"
#include "sys/panic.h"
#include "sys/thread.h"
#include "sys/wait.h"

#define BANNER_H 72
#define CELL_W 8  /* FONT_SMALL's narrow cell */
#define CELL_H 16

static canvas_t cv;
static bool ready;                 /* the canvas and the cursor are there */
static bool started;               /* log_start: the thread draws */
static bool shown = true;          /* until the window manager takes the screen */
static bool drawing;               /* the boot's writer is drawing: a record written meanwhile waits */
static struct mutex draw_lock;     /* after log_start: the canvas, the cursor, shown */
static struct log_cursor *cursor;
static int cols, rows, cx, cy, top;
/* 0 empty, 1 narrow, 2 wide lead, 3 wide continuation. */
static uint8_t *cell_spans;
static uint32_t fg, bg;
static bool bold;
static int dirty_y0 = 1 << 30, dirty_y1 = -1;
static int esc_state, esc_args[8], esc_nargs;
static uint32_t utf8_cp, utf8_min;
static int utf8_need;

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
    if (dirty_y1 < 0) return;
    fb_present(cv.px, cv.pitch, NULL, 0, 0, dirty_y0, cv.w, dirty_y1 - dirty_y0);
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
    gfx_text(&cv, 76 + gfx_text_width(OS_NAME, FONT_LARGE) + 12, 28, "v" OS_VERSION, 0xFF8E9AC8, TRANSPARENT,
             FONT_SMALL);
    gfx_hline(&cv, 0, BANNER_H - 1, cv.w, 0xFF3A3470);
}

/* ---- the text ---- */

static void clear_text(void) {
    gfx_fill(&cv, 0, top, cv.w, cv.h - top, DEF_BG);
    cx = cy = 0;
    memset(cell_spans, 0, (size_t)cols * rows);
    mark(top, cv.h);
}

/* The cursor went past the last row: the newer half of the rows moves to the top. */
static void scroll(void) {
    int keep = rows / 2, drop = rows - keep;
    size_t row_px = (size_t)CELL_H * cv.pitch;
    memmove(&cv.px[top * cv.pitch], &cv.px[top * cv.pitch + drop * row_px], keep * row_px * 4);
    gfx_fill(&cv, 0, top + keep * CELL_H, cv.w, drop * CELL_H, DEF_BG);
    memmove(cell_spans, cell_spans + (size_t)drop * cols, (size_t)keep * cols);
    memset(cell_spans + (size_t)keep * cols, 0, (size_t)drop * cols);
    mark(top, top + rows * CELL_H);
    cy = keep;
}

static void newline(void) {
    cx = 0;
    if (++cy >= rows) scroll();
}

static void draw_cursor(bool on) {
    if (cx >= cols) return;
    int px = cx * CELL_W, py = top + cy * CELL_H;
    gfx_fill(&cv, px, py + CELL_H - 2, CELL_W, 2, on ? 0xFFF4E9C1 : bg);
    mark(py, py + CELL_H);
}

static void sgr(int n) {
    if (n == 0) {
        fg = DEF_FG;
        bg = DEF_BG;
        bold = false;
    } else if (n == 1) bold = true;
    else if (n == 22) bold = false;
    else if (n >= 30 && n <= 37) fg = palette[n - 30 + (bold ? 8 : 0)];
    else if (n == 39) fg = DEF_FG;
    else if (n >= 40 && n <= 47) bg = palette[n - 40];
    else if (n == 49) bg = DEF_BG;
    else if (n >= 90 && n <= 97) fg = palette[n - 90 + 8];
    else if (n >= 100 && n <= 107) bg = palette[n - 100 + 8];
}

static void erase_cell(int x) {
    if (x >= cols) return;
    uint8_t *line = cell_spans + (size_t)cy * cols;
    if (line[x] == 3 && x > 0) x--;
    int n = line[x] == 2 ? 2 : 1;
    gfx_fill(&cv, x * CELL_W, top + cy * CELL_H, n * CELL_W, CELL_H, bg);
    memset(line + x, 0, n);
}

static void putglyph(uint32_t g) {
    int cells = gfx_codepoint_width(g, FONT_SMALL) / CELL_W;
    if (cx + cells > cols) newline();
    erase_cell(cx);
    if (cells == 2) erase_cell(cx + 1);
    int px = cx * CELL_W, py = top + cy * CELL_H;
    gfx_char(&cv, px, py, g, fg, bg, FONT_SMALL);
    cell_spans[(size_t)cy * cols + cx] = cells == 2 ? 2 : 1;
    if (cells == 2) cell_spans[(size_t)cy * cols + cx + 1] = 3;
    mark(py, py + CELL_H);
    cx += cells;
}

static void escape(uint8_t c) {
    if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            esc_nargs = 0;
            esc_args[0] = 0;
            return;
        }
        esc_state = 0;
        return;
    }
    if (c >= '0' && c <= '9') {
        if (esc_nargs == 0) esc_nargs = 1;
        esc_args[esc_nargs - 1] = esc_args[esc_nargs - 1] * 10 + (c - '0');
        return;
    }
    if (c == ';') {
        if (esc_nargs == 0) esc_nargs = 1;
        if (esc_nargs < (int)ARRAY_SIZE(esc_args)) esc_args[esc_nargs++] = 0;
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
        if (a0 == 2) clear_text();
        break;
    case 'K':
        if (cx < cols) {
            erase_cell(cx);
            memset(cell_spans + (size_t)cy * cols + cx, 0, cols - cx);
        }
        gfx_fill(&cv, cx * CELL_W, top + cy * CELL_H, cv.w - cx * CELL_W, CELL_H, bg);
        mark(top + cy * CELL_H, top + cy * CELL_H + CELL_H);
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
}

static void put(char ch) {
    uint8_t c = (uint8_t)ch;
    if (esc_state) {
        escape(c);
        return;
    }
    if (utf8_need) {
        if ((c & 0xC0) == 0x80) {
            utf8_cp = (utf8_cp << 6) | (c & 0x3F);
            if (--utf8_need == 0) {
                bool valid = utf8_cp >= utf8_min && utf8_cp <= 0x10FFFF && !(utf8_cp >= 0xD800 && utf8_cp <= 0xDFFF);
                putglyph(valid ? utf8_cp : 0xFFFD);
            }
            return;
        }
        utf8_need = 0;
        putglyph(0xFFFD);
    }
    switch (c) {
    case 0x1B: esc_state = 1; return;
    case '\n': newline(); return;
    case '\r': cx = 0; return;
    case '\b':
        if (cx > 0) {
            cx--;
            if (cell_spans[(size_t)cy * cols + cx] == 3 && cx > 0) cx--;
        }
        return;
    case '\t':
        for (int spaces = 8 - cx % 8; spaces--;) putglyph(' ');
        return;
    case 7: return;
    }
    if (c >= 0xC2 && c <= 0xF4) {
        utf8_need = c >= 0xF0 ? 3 : (c >= 0xE0 ? 2 : 1);
        utf8_min = utf8_need == 3 ? 0x10000 : utf8_need == 2 ? 0x800 : 0x80;
        utf8_cp = c & (0x3F >> utf8_need);
        return;
    }
    if (c < 32) return;
    if (c >= 0x80) {
        putglyph(0xFFFD);
        return;
    }
    putglyph(c);
}

/* The boot's one CPU, or draw_lock held: the log's text since the last draw, until there is no
   more; then the rows that changed. */
static void draw_some(void) {
    char piece[512];
    draw_cursor(false);
    for (size_t n; (n = log_read(cursor, piece, sizeof piece));)
        for (size_t i = 0; i < n; i++) put(piece[i]);
    draw_cursor(true);
    flush();
}

/* ---- the boot screen ---- */

void fbcon_init(void) {
    uint64_t bytes = (uint64_t)fb.width * fb.height * 4;
    uint64_t phys = pmm_alloc_contig(ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE, 64);
    if (!phys) return;
    gfx_init(&cv, phys_to_virt(phys), fb.width, fb.height, fb.width);
    top = BANNER_H + 4;
    cols = fb.width / CELL_W;
    rows = (fb.height - top) / CELL_H;
    if (cols < 2 || rows < 1) return;
    if (!(cell_spans = kzalloc((size_t)cols * rows)) || !(cursor = log_cursor_new(LOG_CURSOR_CONSOLE))) return;
    fg = DEF_FG;
    bg = DEF_BG;
    gfx_fill(&cv, 0, 0, cv.w, cv.h, DEF_BG);
    draw_banner();
    mark(0, cv.h);
    ready = true;
    fbcon_boot_draw(); /* what the log says from its start */
}

void fbcon_boot_draw(void) {
    if (!ready || started || drawing) return;
    drawing = true;
    draw_some();
    drawing = false;
}

static void screen_thread(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t seen = log_told();
        mutex_lock(&draw_lock);
        bool on = shown;
        if (on) draw_some();
        else {
            log_cursor_free(cursor);
            cursor = NULL;
        }
        mutex_unlock(&draw_lock);
        if (!on) return;
        log_wait(seen, CLOCK_NEVER);
    }
}

void fbcon_start(void) {
    if (!ready) return;
    started = true;
    if (!kthread_create("boot screen", screen_thread, NULL)) {
        kprintf("fbcon: no thread for the boot screen: it stops here\n");
        shown = false;
    }
}

void fbcon_stop(void) {
    if (!ready) return;
    mutex_lock(&draw_lock);
    shown = false; /* the thread ends at the next records */
    mutex_unlock(&draw_lock);
}

void fbcon_repaint(void) {
    if (!ready) return;
    mutex_lock(&draw_lock);
    if (shown) {
        mark(0, cv.h);
        flush();
    }
    mutex_unlock(&draw_lock);
}

/* ---- the panic screen ---- */

static int pr_y;
static void pline(const char *s, uint32_t col) {
    gfx_text(&cv, 48, pr_y, s, col, TRANSPARENT, FONT_SMALL);
    pr_y += 18;
}

/* What the kernel said last, as many lines as fit below the rest: on a computer without a serial
   port a photo of this screen is the log of a start that never got to keep it. */
static void log_tail(void) {
    static char tail[16384];
    struct log_cursor *c = log_panic_cursor();
    log_cursor_rewind(c);
    size_t n = log_newest(c, tail, sizeof tail);
    pr_y += 18;
    int lines = (cv.h - 24 - pr_y) / 18;
    if (lines <= 1) return;
    pline("The last lines of the log:", 0xFFE8C36A);
    lines--;
    size_t end = n && tail[n - 1] == '\n' ? n - 1 : n, start = end;
    for (int shown_lines = 1; start > 0; start--)
        if (tail[start - 1] == '\n' && ++shown_lines > lines) break;
    char line[256];
    for (size_t at = start; at < end;) {
        size_t len = 0;
        for (; at < end && tail[at] != '\n'; at++)
            if (len < sizeof line - 1) line[len++] = tail[at] >= 0x20 || tail[at] < 0 ? tail[at] : ' ';
        at++;
        line[len] = 0;
        pline(line, 0xFF9AA0C0);
    }
}

void fbcon_panic(const struct panic_line *lines, unsigned count) {
    if (!ready || !fb.addr) return;
    static const uint32_t tones[] = {[PANIC_MESSAGE] = 0xFFFF8E9E, [PANIC_HEADING] = 0xFFE8C36A,
                                     [PANIC_DETAIL] = 0xFFC9CCE0};
    gfx_noclip(&cv);
    gfx_gradient_v(&cv, 0, 0, cv.w, cv.h, 0xFF2A0F3A, 0xFF0A0614);
    gfx_fill_circle(&cv, cv.w - 120, 110, 50, 0xFFF4E9C1);
    gfx_fill_circle(&cv, cv.w - 100, 95, 46, 0xFF26103A);
    gfx_text(&cv, 48, 48, OS_NAME " had a nightmare :(", 0xFFFFFFFF, TRANSPARENT, FONT_LARGE);
    pr_y = 110;
    pline("The kernel hit a problem it could not recover from and has stopped.", 0xFFC9CCE0);
    pr_y += 10;
    for (unsigned i = 0; i < count; i++) {
        if (lines[i].tone == PANIC_HEADING) pr_y += 8;
        pline(lines[i].text, lines[i].tone < ARRAY_SIZE(tones) ? tones[lines[i].tone] : 0xFFC9CCE0);
        if (i == 0) pr_y += 10;
    }
    pr_y += 10;
    pline("Restart the computer (hold the power button if it does not answer). Sweet dreams.", 0xFF8E9AC8);
    log_tail();
    fb_drop_holds();
    fb_present_last(cv.px, cv.pitch, cv.w, cv.h);
}
