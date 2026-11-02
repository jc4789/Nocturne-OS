/* System monitor: the readouts and the process list are widgets of the interface engine
   (libc/ui.c); the two history graphs are surfaces drawn here. It samples once a second. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "ui.h"
#include "authority_abi.h"

#define GW 295 /* a graph, inside a card of padding 14 */
#define GH 84
#define W (2 * (GW + 28) + 10 + 24)
#define H 700
#define HIST 120
#define WINDOW 1
#define S_CPU 1
#define S_MEM 2

/* 窓の一行：アプリ一つか、カーネルの thread 一つ。アプリの thread の分は足して見せる。 */
enum group { G_SESSION, G_OTHER_SESSION, G_SERVICE, G_KERNEL, G_COUNT };
struct row {
    const struct n_procinfo *p;
    enum group group;
    float cpu;
    uint32_t node; /* the place in the view that events name */
};
static struct n_procinfo *procs, *prev; /* pid の順 */
static int nproc, nprev, sel_pid = -1;
static struct row *rows;
static int nrows;
static uint32_t own_session;
static float cpu_hist[HIST], mem_hist[HIST];
static uint64_t last_t;
static char status[96];
static uint32_t kill_node;

static int pid_order(const void *a, const void *b) {
    int x = ((const struct n_procinfo *)a)->pid, y = ((const struct n_procinfo *)b)->pid;
    return (x > y) - (x < y);
}

static const struct n_procinfo *find_pid(const struct n_procinfo *list, int count, int pid) {
    struct n_procinfo key = {.pid = pid};
    return bsearch(&key, list, (size_t)count, sizeof *list, pid_order);
}

static uint64_t prev_cpu(int pid) {
    const struct n_procinfo *p = prev ? find_pid(prev, nprev, pid) : NULL;
    return p ? p->cpu_ns : 0;
}

static enum group group_of(const struct n_procinfo *p) {
    if (!p->is_user) return G_KERNEL;
    if (!p->session) return G_SERVICE;
    return p->session == own_session ? G_SESSION : G_OTHER_SESSION;
}

static int row_order(const void *a, const void *b) {
    const struct row *x = a, *y = b;
    if (x->group != y->group) return x->group < y->group ? -1 : 1;
    int names = strcasecmp(x->p->name, y->p->name);
    return names ? names : (x->p->pid > y->p->pid) - (x->p->pid < y->p->pid);
}

static struct row *row_of(int pid, int count) {
    int low = 0, high = count;
    while (low < high) {
        int middle = low + (high - low) / 2;
        if (rows[middle].p->pid < pid) low = middle + 1;
        else high = middle;
    }
    return low < count && rows[low].p->pid == pid ? &rows[low] : NULL;
}

static void sample(void) {
    struct n_procinfo *now_list;
    n_result listed = n_processes(&now_list);
    if (listed < 0) return;
    qsort(now_list, (size_t)listed, sizeof *now_list, pid_order);
    struct row *new_rows = malloc(((size_t)listed + 1) * sizeof *new_rows);
    if (!new_rows) {
        free(now_list);
        return;
    }
    free(prev);
    prev = procs;
    nprev = nproc;
    procs = now_list;
    nproc = (int)listed;
    free(rows);
    rows = new_rows;
    nrows = 0;
    for (int i = 0; i < nproc; i++)
        if (!procs[i].app || procs[i].pid == procs[i].app)
            rows[nrows++] = (struct row){.p = &procs[i], .group = group_of(&procs[i])};
    uint64_t now = n_clock_monotonic(), dt = now - last_t;
    if (!last_t || !dt) dt = N_NS_PER_S;
    last_t = now;
    uint64_t busy = 0;
    for (int i = 0; i < nproc; i++) {
        uint64_t before = prev_cpu(procs[i].pid);
        uint64_t d = procs[i].cpu_ns < before ? 0 : procs[i].cpu_ns - before;
        struct row *r = row_of(procs[i].app ? procs[i].app : procs[i].pid, nrows);
        if (r) r->cpu += 100.0f * (float)d / (float)dt;
        if (procs[i].pid != 0) busy += d;
    }
    qsort(rows, (size_t)nrows, sizeof *rows, row_order);
    struct n_cpuinfo ci;
    unsigned task_cpus = cpu_info(&ci) == 0 && ci.scheduler_cpus ? ci.scheduler_cpus : 1;
    /* task cpu_nsにはAP runnerの時間も含む。1CPU当たりのprocess %は維持し、
       全taskグラフだけを実scheduler容量で正規化する。純計算AP jobは含まない。 */
    float cpu = 100.0f * (float)busy / (float)dt / task_cpus;
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

static const char *group_name(enum group g) {
    static const char *const names[G_COUNT] = {"このセッション", "ほかのセッション", "サービス", "カーネル"};
    return names[g];
}
static const char *class_name(const struct n_procinfo *p) {
    if (!p->is_user) return "kernel";
    return p->cls >= N_CLASS_SYSTEM ? "system" : p->cls >= N_CLASS_ADMIN ? "admin" : "user";
}

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
    unsigned long up = si.uptime_ns / N_NS_PER_S;
    snprintf(up_text, sizeof up_text, "up %lu:%02lu:%02lu", up / 3600, up / 60 % 60, up % 60);
    char tasks[48];
    snprintf(tasks, sizeof tasks, "%d 個、thread %d", nrows, nproc);

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
                for (int i = 0; i < nrows; i++) {
                    if (!i || rows[i - 1].group != rows[i].group) {
                        int in_group = 0;
                        while (i + in_group < nrows && rows[i + in_group].group == rows[i].group) in_group++;
                        snprintf(b, sizeof b, "%s (%d)", group_name(rows[i].group), in_group);
                        ui_node(&m, "text"); ui_str(&m, "text", b); ui_str(&m, "style", "heading"); ui_num(&m, "key", -1 - (int)rows[i].group); ui_end(&m);
                    }
                    const struct n_procinfo *p = rows[i].p;
                    bool picked = p->pid == sel_pid;
                    uint32_t fg = p->is_user ? t->text : t->textDim;
                    rows[i].node = ui_node(&m, "row");
                    ui_num(&m, "key", p->pid); ui_num(&m, "clickable", 1); ui_num(&m, "radius", 6); ui_num(&m, "gap", 0);
                    ui_nums(&m, "padding", (const double[]){5, 10}, 2);
                    if (picked) { ui_num(&m, "background", t->surfaceActive); ui_num(&m, "border", t->accent); }
                    snprintf(b, sizeof b, "%d", p->pid);
                    cell(&m, b, colw[0], false, true, fg);
                    cell(&m, p->name, colw[1], false, true, fg);
                    cell(&m, state_name(p->state), colw[2], false, true, t->textDim);
                    snprintf(b, sizeof b, "%.0f%%", rows[i].cpu);
                    cell(&m, b, colw[3], false, true, rows[i].cpu > 20 ? t->warning : fg);
                    if (p->is_user) snprintf(b, sizeof b, "%lu KiB", (unsigned long)p->mem_kb);
                    else b[0] = 0;
                    cell(&m, b, colw[4], false, true, fg);
                    cell(&m, class_name(p), colw[5], false, true, t->textDim);
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
    const struct n_procinfo *p = find_pid(procs, nproc, pid);
    if (p && p->is_user) {
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
    struct n_identity self;
    if (n_identity_self(&self) >= 0) own_session = self.session;
    sample();
    snprintf(status, sizeof status, "Select a process to end it");
    uint64_t next = (n_clock_monotonic() / N_NS_PER_MS) + 1000;
    draw_graphs(s, cpu_c, mem_c);
    show(s);
    for (bool running = true; running;) {
        int64_t wait = (int64_t)(next - (n_clock_monotonic() / N_NS_PER_MS));
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
                for (int i = 0; i < nrows; i++)
                    if (e.node == rows[i].node) {
                        sel_pid = rows[i].p->pid;
                        changed = true;
                        break;
                    }
                break;
            }
        }
        if (!running || !ui_session_open(s)) break;
        if ((n_clock_monotonic() / N_NS_PER_MS) >= next) {
            sample();
            next = (n_clock_monotonic() / N_NS_PER_MS) + 1000;
            resample = changed = true;
        }
        if (resample) draw_graphs(s, cpu_c, mem_c);
        if (changed) show(s);
    }
    ui_session_free(s);
    return 0;
}
