/* Settings: display, mouse and sound, all by mouse. Every change applies at
 * once and is kept in /data/settings/desktop.conf; at startup init runs
 * "settings --apply", which puts the saved settings back before the desktop
 * is used. Nothing here needs a terminal or the boot command line. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "display.h"

#define W 760
#define H 500
#define SIDE 176
#define PANE_X (SIDE + 28)
#define SETTINGS_FOLDER "/data/settings"
#define SETTINGS_PATH "/data/settings/desktop.conf"
#define ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))

enum { SEC_DISPLAY, SEC_MOUSE, SEC_SOUND, NSEC };
static const char *const section_names[NSEC] = {"Display", "Mouse", "Sound"};

/* control ids: a group's base plus the index of its choice */
enum { ID_SECTION = 0, ID_DEEP = 100, ID_REFRESH = 200, ID_SPEED = 300, ID_POINTER_Y = 400,
       ID_VOLUME = 500, ID_VOLUME_DOWN = 501, ID_VOLUME_UP = 502 };

static const uint32_t refresh_choices[] = {30, 60, 75, 120, 144, 240, 0};
static const char *const refresh_names[] = {"30 Hz", "60 Hz", "75 Hz", "120 Hz", "144 Hz", "240 Hz", "Unlimited"};
static const char *const speed_names[] = {"Slowest", "Slow", "Normal", "Fast", "Fastest"};
static const char *const pointer_y_names[] = {"Automatic", "Normal", "Inverted"};
static const char *const pointer_y_keys[] = {"auto", "normal", "invert"};
static const char *const off_on[] = {"Off", "On"};

static window_t *win;
static struct desktop_settings cur;
static int volume = -1; /* -1: no sound device */
static int section;
static char status[192];
static bool status_error;

/* clickable areas of the last drawing */
struct hit { int x, y, w, h, id; };
static struct hit hits[64];
static int nhits, hot = -1;
static bool dragging_volume;
static int volume_x, volume_w; /* the slider track */

/* ---- the saved file ---- */

static bool saved_volume_known;
static int saved_volume;

/* Overlay the saved file on s (and saved_volume). False when there is none. */
static bool load_saved(struct desktop_settings *s) {
    void *data;
    size_t size;
    if (n_document_load(SETTINGS_PATH, &data, &size) < 0) return false;
    char *text = malloc(size + 1);
    if (!text) { n_memory_release(data, size); return false; }
    memcpy(text, data, size);
    text[size] = 0;
    n_memory_release(data, size);
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        size_t n = strlen(val);
        while (n && (val[n - 1] == '\r' || val[n - 1] == ' ')) val[--n] = 0;
        long v = strtol(val, NULL, 10);
        if (!strcmp(key, "refresh")) {
            if (v >= 0 && v <= 1000) s->refresh_hz = (uint32_t)v;
        } else if (!strcmp(key, "pointer_speed")) {
            if (v >= -2 && v <= 2) s->pointer_speed = (int8_t)v;
        } else if (!strcmp(key, "pointer_y")) {
            for (int i = 0; i < 3; i++)
                if (!strcmp(val, pointer_y_keys[i])) s->pointer_y = (uint8_t)i;
        } else if (!strcmp(key, "deep_colour")) {
            s->deep_simulation = !strcmp(val, "on") && s->deep_simulation_available;
        } else if (!strcmp(key, "volume")) {
            if (v >= 0 && v <= 100) { saved_volume = (int)v; saved_volume_known = true; }
        }
    }
    free(text);
    return true;
}

static void save(void) {
    char text[384];
    int n = snprintf(text, sizeof text,
                     "# Nocturne desktop settings, written by Settings\n"
                     "refresh=%u\npointer_speed=%d\npointer_y=%s\ndeep_colour=%s\n",
                     cur.refresh_hz, cur.pointer_speed, pointer_y_keys[cur.pointer_y % 3],
                     cur.deep_simulation ? "on" : "off");
    if (volume >= 0) n += snprintf(text + n, sizeof text - n, "volume=%d\n", volume);
    n_folder_create(SETTINGS_FOLDER); /* N_ERR_EXISTS after the first time */
    n_result r = n_document_save(SETTINGS_PATH, text, (size_t)n);
    status_error = r < 0;
    if (r < 0) snprintf(status, sizeof status, "Applied, but not saved: %s", n_result_text(r));
    else snprintf(status, sizeof status, "Applied and saved. It stays this way after a restart.");
}

static bool settings_get(struct desktop_settings *s) {
    return __syscall(SYS_SCREEN_INFO, (long)s, sizeof *s, SCREEN_SETTINGS_GET, 0, 0) == 0;
}

static bool settings_set(struct desktop_settings *s) {
    return __syscall(SYS_SCREEN_INFO, (long)s, sizeof *s, SCREEN_SETTINGS_SET, 0, 0) == 0;
}

/* At startup: the saved settings over the system's own. */
static int apply_saved(void) {
    struct desktop_settings s;
    if (!settings_get(&s)) return 1;
    if (!load_saved(&s)) return 0;
    if (!settings_set(&s)) return 1;
    if (saved_volume_known) n_audio_set_volume(saved_volume);
    return 0;
}

/* Apply cur, read back what the system took, save. */
static void change(void) {
    if (!settings_set(&cur)) {
        snprintf(status, sizeof status, "The system did not accept that setting.");
        status_error = true;
    } else {
        save();
    }
    settings_get(&cur);
}

static void set_volume(int v) {
    v = MAX(0, MIN(100, v));
    n_result r = n_audio_set_volume(v);
    if (r >= 0) volume = (int)r;
}

/* ---- drawing ---- */

static void add_hit(int x, int y, int w, int h, int id) {
    if (nhits < ARRAY_SIZE(hits)) hits[nhits++] = (struct hit){x, y, w, h, id};
}

static int hit_at(int x, int y) {
    for (int i = 0; i < nhits; i++)
        if (ui_hit(x, y, hits[i].x, hits[i].y, hits[i].w, hits[i].h)) return hits[i].id;
    return -1;
}

/* A row of choices; selected may be -1. Returns the height used. */
static int choices(canvas_t *c, int x, int y, const char *const *names, int n, int selected, int base, bool enabled) {
    const int h = 30;
    int total = 0, widths[8];
    for (int i = 0; i < n; i++) total += widths[i] = gfx_text_width(names[i], FONT_SMALL) + 26;
    gfx_fill_round(c, x - 2, y - 2, total + 4, h + 4, 8, RGB(24, 22, 44));
    for (int i = 0; i < n; i++) {
        int w = widths[i];
        uint32_t bg = i == selected ? UI_ACCENT : hot == base + i && enabled ? UI_BTN_HI : UI_BTN;
        if (!enabled && i != selected) bg = UI_PANEL;
        gfx_fill_round(c, x + 1, y, w - 2, h, 6, bg);
        uint32_t fg = enabled || i == selected ? UI_FG : UI_DIM;
        ui_text_center(c, x, y + (h - gfx_font_h(FONT_SMALL)) / 2, w, names[i], fg, FONT_SMALL);
        if (enabled) add_hit(x, y, w, h, base + i);
        x += w;
    }
    return h + 4;
}

static int label(canvas_t *c, int y, const char *title) {
    gfx_text(c, PANE_X, y, title, UI_FG, TRANSPARENT, FONT_SMALL);
    return gfx_font_h(FONT_SMALL) + 8;
}

static int hint(canvas_t *c, int y, const char *text) {
    gfx_text(c, PANE_X, y, text, UI_DIM, TRANSPARENT, FONT_SMALL);
    return gfx_font_h(FONT_SMALL) + 4;
}

static void draw_display(canvas_t *c, int y) {
    struct screen_format f;
    struct n_sysinfo si;
    sysinfo(&si);
    int bpc = screen_format(&f) == 0 && f.bits_per_channel ? f.bits_per_channel : 8;
    char line[160];
    y += label(c, y, "Colour depth");
    snprintf(line, sizeof line, "Screen %ux%u at %d bpc. Desktop: %d bpc (%d-bit colour).",
             si.fb_w, si.fb_h, cur.native_bits_per_channel, bpc, bpc * 3);
    y += hint(c, y, line) + 6;
    y += label(c, y, "10-bit colour");
    bool can = cur.deep_simulation_available;
    y += choices(c, PANE_X, y, off_on, 2, cur.deep_simulation, ID_DEEP, can) + 6;
    if (can) {
        y += hint(c, y, "Draws in 10 bits per channel, dithered for this 8 bpc screen:");
        y += hint(c, y, "smoother gradients and 10-bit video.");
    } else if (cur.native_bits_per_channel >= 10) {
        y += hint(c, y, "This screen already shows 10 bits per channel.");
    } else {
        y += hint(c, y, "Not available for this screen's pixel format.");
    }
    y += 18;
    y += label(c, y, "Refresh rate");
    int sel = -1;
    for (int i = 0; i < ARRAY_SIZE(refresh_choices); i++)
        if (refresh_choices[i] == cur.refresh_hz) sel = i;
    y += choices(c, PANE_X, y, refresh_names, ARRAY_SIZE(refresh_choices), sel, ID_REFRESH, true) + 6;
    if (sel < 0) {
        snprintf(line, sizeof line, "Now %u Hz.", cur.refresh_hz);
        y += hint(c, y, line);
    }
    y += hint(c, y, "How often the desktop is redrawn. Pick your monitor's rate.");
    y += 18;
    y += label(c, y, "Resolution");
    snprintf(line, sizeof line, "%ux%u, chosen by the firmware when the computer starts.", si.fb_w, si.fb_h);
    hint(c, y, line);
}

static void draw_mouse(canvas_t *c, int y) {
    y += label(c, y, "Pointer speed");
    y += choices(c, PANE_X, y, speed_names, 5, cur.pointer_speed + 2, ID_SPEED, true) + 24;
    y += label(c, y, "Up and down");
    y += choices(c, PANE_X, y, pointer_y_names, 3, cur.pointer_y, ID_POINTER_Y, true) + 6;
    y += hint(c, y, cur.pointer_y_detected ? "Automatic: inverted, because this host reports it upside down."
                                           : "Automatic: as the mouse reports it.");
    hint(c, y, "If the pointer moves the wrong way, choose Inverted.");
}

static void draw_sound(canvas_t *c, int y) {
    y += label(c, y, "Volume");
    if (volume < 0) {
        hint(c, y, "No sound device was found.");
        return;
    }
    int bx = PANE_X, h = 30;
    ui_button(c, bx, y, 36, h, "-", hot == ID_VOLUME_DOWN, false);
    add_hit(bx, y, 36, h, ID_VOLUME_DOWN);
    volume_x = bx + 52;
    volume_w = 300;
    int ty = y + h / 2 - 3;
    gfx_fill_round(c, volume_x, ty, volume_w, 6, 3, UI_PANEL);
    int fill = volume * volume_w / 100;
    if (fill > 0) gfx_fill_round(c, volume_x, ty, fill, 6, 3, UI_ACCENT);
    int kx = volume_x + fill;
    uint32_t knob = hot == ID_VOLUME || dragging_volume ? RGB(255, 255, 255) : UI_FG;
    gfx_fill_round(c, kx - 8, y + h / 2 - 9, 18, 18, 9, knob);
    add_hit(volume_x - 10, y, volume_w + 20, h, ID_VOLUME);
    ui_button(c, volume_x + volume_w + 16, y, 36, h, "+", hot == ID_VOLUME_UP, false);
    add_hit(volume_x + volume_w + 16, y, 36, h, ID_VOLUME_UP);
    char pct[16];
    snprintf(pct, sizeof pct, "%d%%", volume);
    gfx_text(c, volume_x + volume_w + 68, y + (h - gfx_font_h(FONT_SMALL)) / 2, pct, UI_FG, TRANSPARENT, FONT_SMALL);
    y += h + 10;
    hint(c, y, "Drag or scroll the slider; the taskbar speaker does the same.");
}

static void draw(void) {
    canvas_t *c = &win->c;
    nhits = 0;
    gfx_fill(c, 0, 0, W, H, UI_BG);
    gfx_fill(c, 0, 0, SIDE, H, RGB(22, 20, 40));
    gfx_text(c, 22, 22, "Settings", RGB(255, 255, 255), TRANSPARENT, FONT_LARGE);
    for (int i = 0; i < NSEC; i++) {
        int y = 78 + i * 46;
        bool sel = i == section;
        if (sel || hot == ID_SECTION + i) gfx_fill_round(c, 10, y, SIDE - 20, 38, 8, sel ? UI_PANEL : RGB(32, 30, 58));
        if (sel) gfx_fill_round(c, 10, y + 9, 4, 20, 2, UI_ACCENT);
        gfx_text(c, 28, y + (38 - gfx_font_h(FONT_SMALL)) / 2, section_names[i], sel ? UI_FG : UI_DIM, TRANSPARENT,
                 FONT_SMALL);
        add_hit(10, y, SIDE - 20, 38, ID_SECTION + i);
    }
    gfx_text(c, PANE_X, 22, section_names[section], RGB(255, 255, 255), TRANSPARENT, FONT_LARGE);
    int y = 22 + gfx_font_h(FONT_LARGE) + 22;
    if (section == SEC_DISPLAY) draw_display(c, y);
    else if (section == SEC_MOUSE) draw_mouse(c, y);
    else draw_sound(c, y);
    gfx_fill(c, SIDE, H - 40, W - SIDE, 1, UI_PANEL);
    const char *s = status[0] ? status : "Changes apply at once and are kept after a restart.";
    gfx_text(c, PANE_X, H - 28, s, status_error ? UI_ACCENT2 : UI_DIM, TRANSPARENT, FONT_SMALL);
    win_update(win);
}

/* ---- input ---- */

static void volume_from_x(int x) { set_volume(((x - volume_x) * 100 + volume_w / 2) / volume_w); }

static void click(int id, int x) {
    if (id < 0) return;
    if (id < ID_SECTION + NSEC) {
        section = id - ID_SECTION;
        status[0] = 0;
    } else if (id >= ID_DEEP && id < ID_DEEP + 2) {
        cur.deep_simulation = (uint8_t)(id - ID_DEEP);
        change();
    } else if (id >= ID_REFRESH && id < ID_REFRESH + ARRAY_SIZE(refresh_choices)) {
        cur.refresh_hz = refresh_choices[id - ID_REFRESH];
        change();
    } else if (id >= ID_SPEED && id < ID_SPEED + 5) {
        cur.pointer_speed = (int8_t)(id - ID_SPEED - 2);
        change();
    } else if (id >= ID_POINTER_Y && id < ID_POINTER_Y + 3) {
        cur.pointer_y = (uint8_t)(id - ID_POINTER_Y);
        change();
    } else if (id == ID_VOLUME_DOWN || id == ID_VOLUME_UP) {
        set_volume(volume + (id == ID_VOLUME_UP ? 5 : -5));
        save();
    } else if (id == ID_VOLUME) {
        dragging_volume = true;
        volume_from_x(x);
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--apply")) return apply_saved();
    if (!settings_get(&cur)) {
        fprintf(stderr, "settings: the desktop is not running\n");
        return 1;
    }
    n_result v = n_audio_volume();
    volume = v < 0 ? -1 : (int)v;
    if (argc > 1 && !strcmp(argv[1], "mouse")) section = SEC_MOUSE;
    if (argc > 1 && !strcmp(argv[1], "sound")) section = SEC_SOUND;
    win = win_open(W, H, "Settings", WIN_CENTER);
    if (!win) return 1;
    draw();
    int shown_bpc = display_bits_per_channel();
    for (;;) {
        struct gui_event e;
        int r = win_event(win, &e, 500);
        if (r < 0 || (r > 0 && e.type == EV_CLOSE)) break;
        bool redraw = false;
        if (r == 0) {
            /* the taskbar speaker, or a depth switch the compositor just made */
            n_result nv = n_audio_volume();
            int now_bpc = display_bits_per_channel();
            if ((nv >= 0 && nv != volume && !dragging_volume) || now_bpc != shown_bpc) {
                if (nv >= 0 && !dragging_volume) volume = (int)nv;
                shown_bpc = now_bpc;
                redraw = true;
            }
        } else if (e.type == EV_MOUSE_MOVE) {
            if (dragging_volume) {
                volume_from_x(e.x);
                redraw = true;
            }
            int h = hit_at(e.x, e.y);
            if (h != hot) { hot = h; redraw = true; }
        } else if (e.type == EV_MOUSE_DOWN && (e.buttons & 1)) {
            click(hit_at(e.x, e.y), e.x);
            redraw = true;
        } else if (e.type == EV_MOUSE_UP) {
            if (dragging_volume) {
                dragging_volume = false;
                save();
                redraw = true;
            }
        } else if (e.type == EV_WHEEL && section == SEC_SOUND && volume >= 0 && hit_at(e.x, e.y) == ID_VOLUME) {
            set_volume(volume - e.wheel * 5);
            save();
            redraw = true;
        } else if (e.type == EV_UNFOCUS && dragging_volume) {
            dragging_volume = false;
            save();
            redraw = true;
        } else if (e.type == EV_KEY && e.pressed) {
            if (e.key == NKEY_ESC) break;
            if (e.key == NKEY_UP || e.key == NKEY_DOWN) {
                section = (section + (e.key == NKEY_DOWN ? 1 : NSEC - 1)) % NSEC;
                status[0] = 0;
                redraw = true;
            }
        }
        if (redraw) {
            if (dragging_volume) hot = ID_VOLUME;
            draw();
        }
    }
    win_close(win);
    return 0;
}
