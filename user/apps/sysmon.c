/* System monitor: the readouts and the process list are widgets of the interface engine
   (libc/ui.c); the two history graphs are surfaces drawn here. It samples once a second. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "ui.h"

#define GW 295 /* a graph, inside a card of padding 14 */
#define GH 84
#define W (2 * (GW + 28) + 10 + 24)
#define H 700
#define HIST 120
#define MAXPROC 128
#define WINDOW 1
#define S_CPU 1
#define S_MEM 2

static struct n_procinfo procs[MAXPROC], prev[MAXPROC];
static int nproc, nprev, sel_pid = -1;
static float cpu_hist[HIST], mem_hist[HIST], proc_cpu[MAXPROC];
static uint64_t last_t;
static char status[96];
static uint32_t row_node[MAXPROC], kill_node; /* places in the view that events name */

static uint64_t prev_cpu(int pid) {
    for (int i = 0; i < nprev; i++)
        if (prev[i].pid == pid) return prev[i].cpu_ms;
    return 0;
}

static void sample(void) {
    memcpy(prev, procs, sizeof procs);
    nprev = nproc;
    nproc = proclist(procs, MAXPROC);
    uint64_t now = n_clock_uptime_ms(), dt = now - last_t;
    if (!last_t || !dt) dt = 1000;
    last_t = now;
    uint64_t busy = 0;
    for (int i = 0; i < nproc; i++) {
        uint64_t d = procs[i].cpu_ms - prev_cpu(procs[i].pid);
        if (procs[i].cpu_ms < prev_cpu(procs[i].pid)) d = 0;
        proc_cpu[i] = 100.0f * d / dt;
        if (procs[i].pid != 0) busy += d;
    }
    struct n_cpuinfo ci;
    unsigned task_cpus = cpu_info(&ci) == 0 && ci.scheduler_cpus ? ci.scheduler_cpus : 1;
    /* task cpu_msにはAP runnerの時間も含む。1CPU当たりのprocess %は維持し、
       全taskグラフだけを実scheduler容量で正規化する。純計算AP jobは含まない。 */
    float cpu = 100.0f * busy / dt / task_cpus;
    if (cpu > 100) cpu = 100;
    struct n_sysinfo si;
    sysinfo(&si);
    float mem = 100.0f * (si.total_mem - si.free_mem) / (si.total_mem ? si.total_mem : 1);
    memmove(cpu_hist, cpu_hist + 1, sizeof(float) * (HIST - 1));
    memmove(mem_hist, mem_hist + 1, sizeof(float) * (HIST - 1));
    cpu_hist[HIST - 1] = cpu;
    mem_hist[HIST - 1] = mem;
}

/* ---- the graphs ---- */

static void graph(canvas_t *c, const float *hist, uint32_t col) {
    const struct ui_theme *t = &ui_theme;
    const float pad = 6, step = (GW - 2 * pad) / (HIST - 1);
    gfx_fill(c, 0, 0, GW, GH, t->surface);
    ui_round_fill(c, 0, 0, GW, GH, 8, t->background);
    for (int i = 1; i < 4; i++) ui_line(c, pad, (float)(GH * i / 4), GW - pad, (float)(GH * i / 4), 1, t->divider);
    float y[HIST];
    for (int i = 0; i < HIST; i++) y[i] = GH - pad - hist[i] * (GH - 2 * pad) / 100;
    /* the area under the curve, a column of pixels at a time */
    for (int x = (int)pad; x <= GW - (int)pad; x++) {
        int i = MIN(HIST - 2, (int)(((float)x - pad) / step));
        float f = ((float)x - pad) / step - (float)i, top = y[i] + (y[i + 1] - y[i]) * f;
        if (top < GH - pad) gfx_fill_blend(c, x, (int)top, 1, GH - (int)pad - (int)top, (col & 0xFFFFFF) | 0x30000000);
    }
    for (int i = 1; i < HIST; i++) ui_line(c, pad + step * (float)(i - 1), y[i - 1], pad + step * (float)i, y[i], 2.2f, col);
}

static void draw_graphs(struct ui_session *s, canvas_t *cpu, canvas_t *mem) {
    graph(cpu, cpu_hist, ui_theme.accent);
    graph(mem, mem_hist, ui_theme.warning);
    ui_surface_changed(s, S_CPU);
    ui_surface_changed(s, S_MEM);
}

/* ---- the window ---- */

static const char *state_name(int s) {
    static const char *n[] = {"ready", "running", "sleeping", "zombie"};
    return s >= 0 && s < 4 ? n[s] : "?";
}

/* One cell of the list: a fixed-width piece of text (width 0: takes what is left). */
static void cell(struct ui_msg *m, const char *text, int width, bool caption, bool has_colour, uint32_t colour) {
    ui_node(m, "text");
    ui_str(m, "text", text);
    if (width) ui_num(m, "width", width);
    else ui_num(m, "grow", 1);
    ui_num(m, "size", 13);
    if (caption) ui_str(m, "style", "caption");
    if (has_colour) ui_num(m, "colour", colour);
    ui_end(m);
}

static const int colw[] = {56, 200, 96, 64, 112, 0};

/* A card with a heading and the current value on top of a graph and a meter. */
static void meter(struct ui_msg *m, const char *name, float percent, uint32_t surface, const char *info) {
    char value[16];
    snprintf(value, sizeof value, "%.0f%%", percent);
    ui_node(m, "card"); ui_num(m, "padding", 14); ui_num(m, "gap", 8); ui_num(m, "width", GW + 28);
        ui_node(m, "row");
            ui_node(m, "text"); ui_str(m, "text", name); ui_str(m, "style", "heading"); ui_end(m);
            ui_node(m, "spacer"); ui_end(m);
            ui_node(m, "text"); ui_str(m, "text", value); ui_num(m, "size", 18); ui_num(m, "weight", 1); ui_end(m);
        ui_end(m);
        ui_node(m, "surface"); ui_num(m, "surface", surface); ui_num(m, "width", GW); ui_num(m, "height", GH); ui_end(m);
        ui_node(m, "progress"); ui_num(m, "value", percent); ui_num(m, "max", 100); ui_end(m);
        ui_node(m, "text"); ui_str(m, "text", info); ui_str(m, "style", "caption"); ui_num(m, "wrap", 1); ui_end(m);
    ui_end(m);
}

static void show(struct ui_session *s) {
    const struct ui_theme *t = &ui_theme;
    char b[128], cpu_info_text[96] = "", up_text[32];
    struct n_sysinfo si;
    sysinfo(&si);
    snprintf(b, sizeof b, "%lu / %lu MiB used   kernel heap %lu KiB", (unsigned long)((si.total_mem - si.free_mem) >> 20),
             (unsigned long)(si.total_mem >> 20), (unsigned long)(si.heap_used >> 10));
    struct n_cpuinfo ci;
    /* AP計算workerをtask実行CPUの使用率と混同しない。 */
    if (cpu_info(&ci) == 0)
        snprintf(cpu_info_text, sizeof cpu_info_text, "CPU %u / %u   task CPU %u   AP workers %u   jobs %lu", ci.online_cpus,
                 ci.detected_cpus, ci.scheduler_cpus, ci.worker_cpus, (unsigned long)ci.parallel_jobs);
    unsigned long up = si.uptime_ms / 1000;
    snprintf(up_text, sizeof up_text, "up %lu:%02lu:%02lu", up / 3600, up / 60 % 60, up % 60);
    char tasks[24];
    snprintf(tasks, sizeof tasks, "%d tasks", nproc);

    struct ui_msg m;
    ui_view(&m, WINDOW);
    ui_node(&m, "column"); ui_num(&m, "padding", 12); ui_num(&m, "gap", 10);
        ui_node(&m, "row"); ui_num(&m, "gap", 10); ui_str(&m, "align", "stretch");
            meter(&m, "Task CPUs", cpu_hist[HIST - 1], S_CPU, cpu_info_text);
            meter(&m, "Memory", mem_hist[HIST - 1], S_MEM, b);
        ui_end(&m);
        ui_node(&m, "card"); ui_num(&m, "grow", 1); ui_num(&m, "padding", 14); ui_num(&m, "gap", 8);
            ui_node(&m, "row"); ui_num(&m, "gap", 12);
                ui_node(&m, "text"); ui_str(&m, "text", "Processes"); ui_str(&m, "style", "heading"); ui_end(&m);
                ui_node(&m, "text"); ui_str(&m, "text", tasks); ui_str(&m, "style", "caption"); ui_end(&m);
                ui_node(&m, "spacer"); ui_end(&m);
                ui_node(&m, "text"); ui_str(&m, "text", up_text); ui_str(&m, "style", "caption"); ui_end(&m);
            ui_end(&m);
            static const char *const hdr[] = {"PID", "Name", "State", "CPU", "Memory", "Kind"};
            ui_node(&m, "row"); ui_nums(&m, "padding", (const double[]){0, 10}, 2);
                for (int i = 0; i < 6; i++) cell(&m, hdr[i], colw[i], true, false, 0);
            ui_end(&m);
            ui_node(&m, "divider"); ui_end(&m);
            ui_node(&m, "scroll"); ui_num(&m, "grow", 1); ui_num(&m, "gap", 2); ui_num(&m, "key", 1);
                for (int i = 0; i < nproc; i++) {
                    const struct n_procinfo *p = &procs[i];
                    bool picked = p->pid == sel_pid;
                    uint32_t fg = p->is_user ? t->text : t->textDim;
                    row_node[i] = ui_node(&m, "row");
                    ui_num(&m, "key", p->pid); ui_num(&m, "clickable", 1); ui_num(&m, "radius", 6); ui_num(&m, "gap", 0);
                    ui_nums(&m, "padding", (const double[]){5, 10}, 2);
                    if (picked) { ui_num(&m, "background", t->surfaceActive); ui_num(&m, "border", t->accent); }
                    snprintf(b, sizeof b, "%d", p->pid);
                    cell(&m, b, colw[0], false, true, fg);
                    cell(&m, p->name, colw[1], false, true, fg);
                    cell(&m, state_name(p->state), colw[2], false, true, t->textDim);
                    snprintf(b, sizeof b, "%.0f%%", proc_cpu[i]);
                    cell(&m, b, colw[3], false, true, proc_cpu[i] > 20 ? t->warning : fg);
                    if (p->is_user) snprintf(b, sizeof b, "%lu KiB", (unsigned long)p->mem_kb);
                    else b[0] = 0;
                    cell(&m, b, colw[4], false, true, fg);
                    cell(&m, p->is_user ? "user" : "kernel", colw[5], false, true, t->textDim);
                    ui_end(&m);
                }
            ui_end(&m);
        ui_end(&m);
        ui_node(&m, "row"); ui_num(&m, "gap", 12);
            ui_node(&m, "text"); ui_str(&m, "text", status); ui_str(&m, "style", "caption"); ui_num(&m, "grow", 1); ui_end(&m);
            kill_node = ui_node(&m, "button");
            ui_str(&m, "text", "End process"); ui_str(&m, "icon", "close"); ui_num(&m, "disabled", sel_pid <= 0); ui_end(&m);
        ui_end(&m);
    ui_end(&m);
    ui_send(s, &m);
}

/* End the selected process (and what it started), if it is a user process. */
static void end_selected(void) {
    int pid = sel_pid;
    n_result res = -N_ERR_NO_APP;
    for (int i = 0; i < nproc; i++)
        if (procs[i].pid == pid && procs[i].is_user) {
            n_handle app = n_app_open(pid);
            res = app < 0 ? app : n_app_stop(app, N_APP_STOP_TREE);
            if (app >= 0) n_handle_close(app);
        }
    if (res >= 0) snprintf(status, sizeof status, "ended process %d", pid);
    else snprintf(status, sizeof status, "cannot end process %d", pid);
    sel_pid = -1;
    sample();
}

int main(void) {
    struct ui_session *s = ui_session_new();
    if (!s || ui_window(s, WINDOW, W, H, 0, "System Monitor", NULL, NULL) < 0) return 1;
    canvas_t *cpu_c = ui_surface(s, S_CPU, GW, GH, 0), *mem_c = ui_surface(s, S_MEM, GW, GH, 0);
    if (!cpu_c || !mem_c) return 1;
    sample();
    snprintf(status, sizeof status, "Select a process to end it");
    uint64_t next = n_clock_uptime_ms() + 1000;
    draw_graphs(s, cpu_c, mem_c);
    show(s);
    for (bool running = true; running;) {
        int64_t wait = (int64_t)(next - n_clock_uptime_ms());
        ui_session_wait(s, wait > 0 ? (int)wait : 0);
        bool changed = false, resample = false;
        struct ui_event e;
        char text[16];
        while (ui_next_event(s, &e, text, sizeof text)) {
            switch (e.type) {
            case UI_EVENT_CLOSE:
                running = false;
                break;
            case UI_EVENT_KEY:
                if (!(e.flags & UI_UP) && !strcmp(e.text, "Escape")) running = false;
                break;
            case UI_EVENT_CLICK:
                if (e.node == kill_node) {
                    if (sel_pid > 0) end_selected();
                    resample = changed = true;
                    break;
                }
                for (int i = 0; i < nproc; i++)
                    if (e.node == row_node[i]) {
                        sel_pid = procs[i].pid;
                        changed = true;
                        break;
                    }
                break;
            }
        }
        if (!running || !ui_session_open(s)) break;
        if (n_clock_uptime_ms() >= next) {
            sample();
            next = n_clock_uptime_ms() + 1000;
            resample = changed = true;
        }
        if (resample) draw_graphs(s, cpu_c, mem_c);
        if (changed) show(s);
    }
    ui_session_free(s);
    return 0;
}
