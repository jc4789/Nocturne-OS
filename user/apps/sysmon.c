/* System monitor */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

#define W 620
#define H 520
#define HIST 120
#define ROW 20
#define LIST_Y 200

static window_t *w;
static struct n_procinfo procs[128], prev[128];
static int nproc, nprev, sel_pid = -1, scroll;
static float cpu_hist[HIST], mem_hist[HIST], proc_cpu[128];
static uint64_t last_t;
static bool hot_kill;
static char status[96];

static uint64_t prev_cpu(int pid) {
    for (int i = 0; i < nprev; i++)
        if (prev[i].pid == pid) return prev[i].cpu_ms;
    return 0;
}

static void sample(void) {
    memcpy(prev, procs, sizeof procs);
    nprev = nproc;
    nproc = proclist(procs, 128);
    uint64_t now = uptime_ms(), dt = now - last_t;
    if (!last_t || !dt) dt = 1000;
    last_t = now;
    uint64_t busy = 0;
    for (int i = 0; i < nproc; i++) {
        uint64_t d = procs[i].cpu_ms - prev_cpu(procs[i].pid);
        if (procs[i].cpu_ms < prev_cpu(procs[i].pid)) d = 0;
        proc_cpu[i] = 100.0f * d / dt;
        if (procs[i].pid != 0) busy += d;
    }
    float cpu = 100.0f * busy / dt;
    if (cpu > 100) cpu = 100;
    struct n_sysinfo si;
    sysinfo(&si);
    float mem = 100.0f * (si.total_mem - si.free_mem) / (si.total_mem ? si.total_mem : 1);
    memmove(cpu_hist, cpu_hist + 1, sizeof(float) * (HIST - 1));
    memmove(mem_hist, mem_hist + 1, sizeof(float) * (HIST - 1));
    cpu_hist[HIST - 1] = cpu;
    mem_hist[HIST - 1] = mem;
}

static void graph(canvas_t *c, int x, int y, int gw, int gh, float *hist, uint32_t col, const char *label) {
    gfx_fill_round(c, x, y, gw, gh, 8, RGB(18, 16, 32));
    for (int i = 1; i < 4; i++) gfx_hline(c, x + 4, y + gh * i / 4, gw - 8, RGB(34, 32, 56));
    int prevx = -1, prevy = 0;
    for (int i = 0; i < HIST; i++) {
        int px = x + 4 + i * (gw - 8) / (HIST - 1);
        int py = y + gh - 4 - (int)(hist[i] * (gh - 8) / 100);
        for (int yy = py; yy < y + gh - 4; yy++) gfx_blend_pixel(c, px, yy, (col & 0xFFFFFF) | 0x30000000);
        if (prevx >= 0) {
            gfx_line(c, prevx, prevy, px, py, col);
            gfx_line(c, prevx, prevy - 1, px, py - 1, col);
        }
        prevx = px;
        prevy = py;
    }
    char b[48];
    snprintf(b, sizeof b, "%s %.0f%%", label, hist[HIST - 1]);
    gfx_text(c, x + 10, y + 6, b, UI_FG, TRANSPARENT, FONT_SMALL);
}

static const char *state_name(int s) {
    static const char *n[] = {"ready", "running", "sleeping", "zombie"};
    return s >= 0 && s < 4 ? n[s] : "?";
}

static int list_rows(void) { return (H - LIST_Y - 24 - 50) / ROW; }

static void draw(void) {
    canvas_t *c = &w->c;
    gfx_fill(c, 0, 0, W, H, RGB(26, 24, 44));
    graph(c, 12, 12, (W - 36) / 2, 140, cpu_hist, RGB(130, 200, 255), "CPU");
    graph(c, 24 + (W - 36) / 2, 12, (W - 36) / 2, 140, mem_hist, RGB(250, 170, 110), "Memory");
    struct n_sysinfo si;
    sysinfo(&si);
    char b[128];
    unsigned long up = si.uptime_ms / 1000;
    snprintf(b, sizeof b, "%d tasks   %lu / %lu MiB used   kernel heap %lu KiB   up %lu:%02lu:%02lu", nproc,
             (unsigned long)((si.total_mem - si.free_mem) >> 20), (unsigned long)(si.total_mem >> 20),
             (unsigned long)(si.heap_used >> 10), up / 3600, up / 60 % 60, up % 60);
    gfx_text(c, 14, 164, b, UI_DIM, TRANSPARENT, FONT_SMALL);
    int y = LIST_Y;
    gfx_fill(c, 0, y, W, 22, RGB(36, 34, 60));
    const char *hdr[] = {"PID", "Name", "State", "CPU", "Memory", "Kind"};
    int colx[] = {14, 64, 240, 340, 410, 510};
    for (int i = 0; i < 6; i++) gfx_text(c, colx[i], y + 3, hdr[i], UI_DIM, TRANSPARENT, FONT_SMALL);
    y += 24;
    int rows = list_rows();
    for (int r = 0; r < rows && scroll + r < nproc; r++) {
        int i = scroll + r;
        struct n_procinfo *p = &procs[i];
        int ry = y + r * ROW;
        if (p->pid == sel_pid) gfx_fill_round(c, 4, ry, W - 8, ROW, 4, RGB(84, 70, 170));
        else if (r & 1) gfx_fill(c, 0, ry, W, ROW, RGB(30, 28, 50));
        uint32_t fgc = p->is_user ? UI_FG : RGB(160, 160, 200);
        snprintf(b, sizeof b, "%d", p->pid);
        gfx_text(c, colx[0], ry + 2, b, fgc, TRANSPARENT, FONT_SMALL);
        gfx_text(c, colx[1], ry + 2, p->name, fgc, TRANSPARENT, FONT_SMALL);
        gfx_text(c, colx[2], ry + 2, state_name(p->state), UI_DIM, TRANSPARENT, FONT_SMALL);
        snprintf(b, sizeof b, "%.0f%%", proc_cpu[i]);
        gfx_text(c, colx[3], ry + 2, b, proc_cpu[i] > 20 ? UI_ACCENT2 : fgc, TRANSPARENT, FONT_SMALL);
        if (p->is_user) {
            snprintf(b, sizeof b, "%lu KiB", (unsigned long)p->mem_kb);
            gfx_text(c, colx[4], ry + 2, b, fgc, TRANSPARENT, FONT_SMALL);
        }
        gfx_text(c, colx[5], ry + 2, p->is_user ? "user" : "kernel", UI_DIM, TRANSPARENT, FONT_SMALL);
    }
    gfx_fill(c, 0, H - 46, W, 46, RGB(34, 30, 60));
    ui_button(c, W - 130, H - 38, 116, 30, "End process", hot_kill, false);
    gfx_text(c, 14, H - 30, status, UI_DIM, TRANSPARENT, FONT_SMALL);
}

int main(void) {
    w = win_open(W, H, "System Monitor", 0);
    if (!w) return 1;
    sample();
    snprintf(status, sizeof status, "select a process to end it");
    uint64_t next = uptime_ms() + 1000;
    draw();
    win_update(w);
    for (;;) {
        int64_t wait = (int64_t)(next - uptime_ms());
        struct gui_event e;
        int r = win_event(w, &e, wait > 0 ? (int)wait : 0);
        if (r < 0) break;
        if (r == 0) {
            sample();
            next = uptime_ms() + 1000;
        } else if (e.type == EV_CLOSE) break;
        else if (e.type == EV_MOUSE_MOVE) {
            bool h = ui_hit(e.x, e.y, W - 130, H - 38, 116, 30);
            if (h == hot_kill) continue;
            hot_kill = h;
        } else if (e.type == EV_MOUSE_DOWN) {
            int idx = scroll + (e.y - LIST_Y - 24) / ROW;
            if (e.y >= LIST_Y + 24 && idx < nproc && e.y < H - 46) sel_pid = procs[idx].pid;
        } else if (e.type == EV_MOUSE_UP) {
            if (ui_hit(e.x, e.y, W - 130, H - 38, 116, 30) && sel_pid > 0) {
                int pid = sel_pid;
                int res = -1;
                for (int i = 0; i < nproc; i++)
                    if (procs[i].pid == pid && procs[i].is_user) res = killtree(pid, 1);
                if (res >= 0) snprintf(status, sizeof status, "ended process %d", pid);
                else snprintf(status, sizeof status, "cannot end process %d", pid);
                sel_pid = -1;
                sample();
            }
        } else if (e.type == EV_WHEEL) {
            scroll = MAX(0, MIN(nproc - list_rows(), scroll + e.wheel * 2));
            if (scroll < 0) scroll = 0;
        } else if (e.type == EV_KEY && e.pressed && e.key == NKEY_ESC) break;
        else continue;
        draw();
        win_update(w);
    }
    win_close(w);
    return 0;
}
