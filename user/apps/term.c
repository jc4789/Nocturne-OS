/* Nocturne Terminal: a VT100-ish terminal emulator window running the shell. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "nocturne.h"

#define PAD    6
#define CW     8
#define CH     16
#define MAXC   240
#define MAXR   120
#define SBLINES 1000

typedef struct {
    uint32_t ch;
    uint8_t fg, bg, attr;
} cell_t;

#define A_BOLD 1
#define A_REV  2
#define DEF_FG 7
#define DEF_BG 16

static const uint32_t palette[17] = {
    RGB(28, 26, 44),   RGB(236, 92, 108),  RGB(120, 220, 140), RGB(240, 200, 100), RGB(110, 150, 250),
    RGB(190, 130, 250), RGB(100, 210, 230), RGB(210, 210, 225), RGB(96, 94, 128),   RGB(255, 125, 140),
    RGB(150, 240, 170), RGB(255, 225, 130), RGB(145, 182, 255), RGB(215, 165, 255), RGB(140, 235, 250),
    RGB(250, 250, 255), RGB(18, 16, 30),
};

static window_t *win;
static int cols, rows;
static cell_t *screen[MAXR];
static cell_t *sb[SBLINES];
static int sb_head, sb_count;
static int view; /* lines scrolled back */
static int cx, cy, saved_cx, saved_cy;
static bool wrap_pending, cursor_visible = true, blink_on = true, focused = true;
static uint8_t cur_fg = DEF_FG, cur_bg = DEF_BG, cur_attr;
static bool dirty[MAXR];
static int shell_pid;
static int to_shell = -1;

static void mark_all(void) {
    for (int i = 0; i < rows; i++) dirty[i] = true;
}

static void clear_line(cell_t *l, int from, int to) {
    for (int i = from; i < to; i++) {
        l[i].ch = ' ';
        l[i].fg = cur_fg;
        l[i].bg = cur_bg;
        l[i].attr = 0;
    }
}

static void push_scrollback(cell_t *line) {
    if (!sb[sb_head]) sb[sb_head] = malloc(sizeof(cell_t) * MAXC);
    memcpy(sb[sb_head], line, sizeof(cell_t) * MAXC);
    sb_head = (sb_head + 1) % SBLINES;
    if (sb_count < SBLINES) sb_count++;
}

static void scroll_up(void) {
    push_scrollback(screen[0]);
    cell_t *first = screen[0];
    memmove(screen, screen + 1, sizeof(cell_t *) * (rows - 1));
    screen[rows - 1] = first;
    uint8_t ofg = cur_fg, obg = cur_bg;
    cur_fg = DEF_FG;
    cur_bg = DEF_BG;
    clear_line(first, 0, MAXC);
    cur_fg = ofg;
    cur_bg = obg;
    mark_all();
}

static void newline(void) {
    if (cy == rows - 1) scroll_up();
    else cy++;
}

static void put_char(uint32_t cp) {
    if (wrap_pending) {
        cx = 0;
        newline();
        wrap_pending = false;
    }
    cell_t *c = &screen[cy][cx];
    c->ch = cp;
    c->fg = cur_fg;
    c->bg = cur_bg;
    c->attr = cur_attr;
    dirty[cy] = true;
    if (cx == cols - 1) wrap_pending = true;
    else cx++;
}

/* ---- escape sequence parser ---- */
enum { S_NORMAL, S_ESC, S_CSI, S_OSC };
static int state;
static int params[16], nparams;
static bool csi_private;
static uint32_t utf_cp;
static int utf_need;

static void sgr(void) {
    if (nparams == 0) nparams = 1, params[0] = 0;
    for (int i = 0; i < nparams; i++) {
        int p = params[i];
        if (p == 0) {
            cur_fg = DEF_FG;
            cur_bg = DEF_BG;
            cur_attr = 0;
        } else if (p == 1) cur_attr |= A_BOLD;
        else if (p == 22) cur_attr &= ~A_BOLD;
        else if (p == 7) cur_attr |= A_REV;
        else if (p == 27) cur_attr &= ~A_REV;
        else if (p >= 30 && p <= 37) cur_fg = p - 30;
        else if (p == 39) cur_fg = DEF_FG;
        else if (p >= 40 && p <= 47) cur_bg = p - 40;
        else if (p == 49) cur_bg = DEF_BG;
        else if (p >= 90 && p <= 97) cur_fg = p - 90 + 8;
        else if (p >= 100 && p <= 107) cur_bg = p - 100 + 8;
    }
}

static int param(int i, int def) { return (i < nparams && params[i] > 0) ? params[i] : def; }

static void csi(char cmd) {
    switch (cmd) {
    case 'm': sgr(); break;
    case 'A': cy = MAX(0, cy - param(0, 1)); break;
    case 'B': cy = MIN(rows - 1, cy + param(0, 1)); break;
    case 'C': cx = MIN(cols - 1, cx + param(0, 1)); break;
    case 'D': cx = MAX(0, cx - param(0, 1)); break;
    case 'G': cx = MIN(cols - 1, param(0, 1) - 1); break;
    case 'H':
    case 'f':
        cy = MIN(rows - 1, param(0, 1) - 1);
        cx = MIN(cols - 1, param(1, 1) - 1);
        break;
    case 'J': {
        int m = nparams ? params[0] : 0;
        if (m == 2 || m == 3) {
            for (int i = 0; i < rows; i++) clear_line(screen[i], 0, MAXC);
            if (m == 3) sb_count = 0;
        } else if (m == 0) {
            clear_line(screen[cy], cx, MAXC);
            for (int i = cy + 1; i < rows; i++) clear_line(screen[i], 0, MAXC);
        } else if (m == 1) {
            clear_line(screen[cy], 0, cx + 1);
            for (int i = 0; i < cy; i++) clear_line(screen[i], 0, MAXC);
        }
        mark_all();
        break;
    }
    case 'K': {
        int m = nparams ? params[0] : 0;
        if (m == 0) clear_line(screen[cy], cx, MAXC);
        else if (m == 1) clear_line(screen[cy], 0, cx + 1);
        else clear_line(screen[cy], 0, MAXC);
        dirty[cy] = true;
        break;
    }
    case 's':
        saved_cx = cx;
        saved_cy = cy;
        break;
    case 'u':
        cx = MIN(saved_cx, cols - 1);
        cy = MIN(saved_cy, rows - 1);
        break;
    case 'h':
    case 'l':
        if (csi_private && param(0, 0) == 25) cursor_visible = cmd == 'h';
        break;
    default: break;
    }
    wrap_pending = false;
}

static void feed(const char *buf, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char b = (unsigned char)buf[i];
        int oy = cy;
        switch (state) {
        case S_NORMAL:
            if (utf_need) {
                if ((b & 0xC0) == 0x80) {
                    utf_cp = (utf_cp << 6) | (b & 0x3F);
                    if (--utf_need == 0) put_char(utf_cp);
                    break;
                }
                utf_need = 0;
            }
            if (b == 0x1b) state = S_ESC;
            else if (b == '\n') {
                cx = 0; /* behave like a tty with ONLCR */
                newline();
                wrap_pending = false;
            } else if (b == '\r') {
                cx = 0;
                wrap_pending = false;
            } else if (b == '\b') {
                if (cx > 0) cx--;
                wrap_pending = false;
            } else if (b == '\t') {
                int nx = (cx / 8 + 1) * 8;
                while (cx < nx && cx < cols - 1) put_char(' ');
            } else if (b == 7) {
            } else if (b >= 0xF0) {
                utf_cp = b & 0x07;
                utf_need = 3;
            } else if (b >= 0xE0) {
                utf_cp = b & 0x0F;
                utf_need = 2;
            } else if (b >= 0xC0) {
                utf_cp = b & 0x1F;
                utf_need = 1;
            } else if (b >= 32) {
                put_char(b);
            }
            break;
        case S_ESC:
            if (b == '[') {
                state = S_CSI;
                nparams = 0;
                params[0] = 0;
                csi_private = false;
            } else if (b == ']') {
                state = S_OSC;
            } else if (b == 'c') {
                cur_fg = DEF_FG;
                cur_bg = DEF_BG;
                cur_attr = 0;
                for (int r = 0; r < rows; r++) clear_line(screen[r], 0, MAXC);
                cx = cy = 0;
                mark_all();
                state = S_NORMAL;
            } else {
                state = S_NORMAL;
            }
            break;
        case S_CSI:
            if (b == '?') csi_private = true;
            else if (b >= '0' && b <= '9') {
                if (nparams == 0) nparams = 1;
                params[nparams - 1] = params[nparams - 1] * 10 + (b - '0');
            } else if (b == ';') {
                if (nparams == 0) nparams = 1;
                if (nparams < 16) params[nparams++] = 0;
            } else if (b >= 0x40 && b <= 0x7E) {
                csi((char)b);
                state = S_NORMAL;
            }
            break;
        case S_OSC:
            if (b == 7 || b == 0x1b) state = S_NORMAL;
            break;
        }
        dirty[oy] = true;
        dirty[cy] = true;
    }
}

/* ---- rendering ---- */
static cell_t *line_at(int r) {
    /* row r of the visible view, accounting for scrollback */
    int idx = r - view;
    if (idx >= 0) return screen[idx];
    int back = -idx; /* 1 = most recent scrollback line */
    if (back > sb_count) return NULL;
    return sb[(sb_head - back + SBLINES) % SBLINES];
}

static void draw_row(int r) {
    canvas_t *c = &win->c;
    int y = PAD + r * CH;
    cell_t *l = line_at(r);
    gfx_fill(c, 0, y, win->w, CH, palette[DEF_BG]);
    if (!l) return;
    for (int x = 0; x < cols; x++) {
        cell_t *cl = &l[x];
        uint8_t fg = cl->fg, bg = cl->bg;
        if ((cl->attr & A_BOLD) && fg < 8) fg += 8;
        uint32_t fgc = palette[fg], bgc = palette[bg];
        if (cl->attr & A_REV) {
            uint32_t t = fgc;
            fgc = bgc;
            bgc = t;
        }
        bool cur = view == 0 && r == cy && x == cx && cursor_visible && blink_on;
        if (cur && focused) {
            fgc = palette[DEF_BG];
            bgc = RGB(200, 180, 255);
        }
        gfx_char(c, PAD + x * CW, y, gfx_glyph_for(cl->ch ? cl->ch : ' '), fgc, bgc, FONT_SMALL);
        if (cur && !focused) gfx_rect(c, PAD + x * CW, y, CW, CH, RGB(200, 180, 255));
    }
}

static void render(void) {
    int y0 = -1, y1 = -1;
    for (int r = 0; r < rows; r++) {
        if (!dirty[r]) continue;
        draw_row(r);
        dirty[r] = false;
        if (y0 < 0) y0 = r;
        y1 = r;
    }
    if (y0 >= 0) win_update_rect(win, 0, PAD + y0 * CH, win->w, (y1 - y0 + 1) * CH);
}

static void full_redraw(void) {
    gfx_fill(&win->c, 0, 0, win->w, win->h, palette[DEF_BG]);
    mark_all();
    render();
    win_update(win);
}

static void resize_grid(int w, int h) {
    int ncols = MIN(MAXC, MAX(10, (w - 2 * PAD) / CW));
    int nrows = MIN(MAXR, MAX(3, (h - 2 * PAD) / CH));
    /* shrinking: push lines off the top so that the cursor stays visible */
    while (cy >= nrows) {
        push_scrollback(screen[0]);
        cell_t *first = screen[0];
        memmove(screen, screen + 1, sizeof(cell_t *) * (rows - 1));
        screen[rows - 1] = first;
        clear_line(first, 0, MAXC);
        cy--;
    }
    for (int r = nrows; r < rows; r++) {
        free(screen[r]);
        screen[r] = NULL;
    }
    for (int r = rows; r < nrows; r++) {
        screen[r] = malloc(sizeof(cell_t) * MAXC);
        clear_line(screen[r], 0, MAXC);
    }
    cols = ncols;
    rows = nrows;
    if (cx >= cols) cx = cols - 1;
    view = 0;
}

/* ---- keyboard ---- */
static void send(const char *s, int n) {
    if (to_shell >= 0) write(to_shell, s, n);
}

static void paste(void) {
    char buf[4096];
    int n = clipboard_get(buf, sizeof buf);
    if (n <= 0) return;
    if (n > (int)sizeof buf) n = sizeof buf;
    for (int i = 0; i < n; i++)
        if (buf[i] == '\n') buf[i] = '\r';
    send(buf, n);
}

static void copy_screen(void) {
    char *buf = malloc(rows * (cols * 4 + 1) + 1);
    int n = 0;
    for (int r = 0; r < rows; r++) {
        int end = cols;
        while (end > 0 && screen[r][end - 1].ch == ' ') end--;
        for (int x = 0; x < end; x++) {
            uint32_t ch = screen[r][x].ch;
            buf[n++] = ch < 128 ? (char)ch : '?';
        }
        buf[n++] = '\n';
    }
    clipboard_set(buf, n);
    free(buf);
}

static void handle_key(struct gui_event *e) {
    if (!e->pressed) return;
    uint32_t k = e->key;
    bool ctrl = e->mods & NMOD_CTRL, shift = e->mods & NMOD_SHIFT;
    if (ctrl && shift && (k == 'V' || k == 'v')) {
        paste();
        return;
    }
    if (ctrl && shift && (k == 'C' || k == 'c')) {
        copy_screen();
        return;
    }
    if (shift && (k == NKEY_PGUP || k == NKEY_PGDN)) {
        int d = k == NKEY_PGUP ? rows / 2 : -rows / 2;
        view = MAX(0, MIN(sb_count, view + d));
        mark_all();
        return;
    }
    if (view) {
        view = 0;
        mark_all();
    }
    if (ctrl && (k == 'c' || k == 'C')) {
        /* interrupt: kill everything the shell started; otherwise tell the shell */
        if (killtree(shell_pid, 0) > 0) feed("^C", 2);
        else send("\x03", 1);
        return;
    }
    char buf[8];
    int n = 0;
    const char *seq = NULL;
    switch (k) {
    case NKEY_ENTER: buf[n++] = '\r'; break;
    case NKEY_BACKSPACE: buf[n++] = 127; break;
    case NKEY_TAB: buf[n++] = '\t'; break;
    case NKEY_ESC: buf[n++] = 0x1b; break;
    case NKEY_UP: seq = "\x1b[A"; break;
    case NKEY_DOWN: seq = "\x1b[B"; break;
    case NKEY_RIGHT: seq = "\x1b[C"; break;
    case NKEY_LEFT: seq = "\x1b[D"; break;
    case NKEY_HOME: seq = "\x1b[H"; break;
    case NKEY_END: seq = "\x1b[F"; break;
    case NKEY_DELETE: seq = "\x1b[3~"; break;
    case NKEY_PGUP: seq = "\x1b[5~"; break;
    case NKEY_PGDN: seq = "\x1b[6~"; break;
    default:
        if (k < 128) {
            char ch = (char)k;
            if (ctrl && ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) ch &= 0x1F;
            buf[n++] = ch;
        }
        break;
    }
    if (seq) send(seq, (int)strlen(seq));
    else if (n) send(buf, n);
}

int main(int argc, char **argv) {
    win = win_open(PAD * 2 + 88 * CW, PAD * 2 + 28 * CH, "Terminal", WIN_RESIZABLE);
    if (!win) {
        fprintf(stderr, "term: cannot open window\n");
        return 1;
    }
    resize_grid(win->w, win->h);

    int in_p[2], out_p[2];
    if (pipe(in_p) < 0 || pipe(out_p) < 0) return 1;
    fcntl(in_p[0], F_SETTTY, 1);
    fcntl(out_p[1], F_SETTTY, 1);
    int fdmap[3] = {in_p[0], out_p[1], out_p[1]};
    char *sargv[16] = {"sh", NULL};
    const char *prog = "/bin/sh";
    if (argc > 1) {
        prog = argv[1];
        for (int i = 1; i < argc && i < 15; i++) sargv[i - 1] = argv[i];
        sargv[MIN(argc - 1, 15)] = NULL;
    }
    char path[256];
    if (!find_program(prog, path, sizeof path)) strlcpy(path, prog, sizeof path);
    shell_pid = spawn(path, sargv, fdmap, 0);
    close(in_p[0]);
    close(out_p[1]);
    if (shell_pid < 0) {
        const char *msg = "term: could not start the shell\r\n";
        feed(msg, (int)strlen(msg));
    }
    to_shell = in_p[1];
    int from_shell = out_p[0];
    fcntl(from_shell, F_SETFL, O_NONBLOCK);
    full_redraw();

    uint64_t last_blink = uptime_ms();
    bool alive = shell_pid >= 0;
    for (;;) {
        struct n_pollfd pf[2] = {{win->fd, N_POLLIN, 0}, {from_shell, N_POLLIN, 0}};
        int np = alive ? 2 : 1;
        uint64_t now = uptime_ms();
        int timeout = (int)(500 - MIN(500, now - last_blink));
        if (win->qi < win->qn) timeout = 0;
        poll(pf, np, timeout);

        struct gui_event e;
        while (win_event(win, &e, 0) > 0) {
            switch (e.type) {
            case EV_CLOSE:
                if (shell_pid > 0) killtree(shell_pid, 1);
                return 0;
            case EV_KEY: handle_key(&e); break;
            case EV_RESIZE:
                resize_grid(e.x, e.y);
                full_redraw();
                break;
            case EV_WHEEL:
                view = MAX(0, MIN(sb_count, view - e.wheel * 3));
                mark_all();
                break;
            case EV_FOCUS:
            case EV_UNFOCUS:
                focused = e.type == EV_FOCUS;
                dirty[cy] = true;
                break;
            default: break;
            }
        }
        if (alive) {
            char buf[4096];
            for (int iter = 0; iter < 16; iter++) {
                ssize_t n = read(from_shell, buf, sizeof buf);
                if (n > 0) {
                    if (view) {
                        view = 0;
                        mark_all();
                    }
                    feed(buf, (int)n);
                    continue;
                }
                if (n == 0) alive = false;
                break;
            }
            if (!alive) {
                /* the shell is gone: close the terminal */
                render();
                return 0;
            }
        }
        now = uptime_ms();
        if (now - last_blink >= 500) {
            last_blink = now;
            blink_on = !blink_on;
            dirty[cy] = true;
        }
        render();
    }
}
