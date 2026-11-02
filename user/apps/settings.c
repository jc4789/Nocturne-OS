/* 設定の窓。頁はサービス settings の宣言の表から組む。窓は値も文書も持たない：変更は settings へ渡し、
   settings が確かめ、持ち主へ適用し、保存する。誰が変えても見張りで知り、読み直して見せる。試しの期限も
   settings が数える。窓が死んでも値は戻る。
   「サービス」の頁は管理者の口、「利用者」の頁は accounts の口で動く。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <display.h>
#include <nocturne_services.h>
#include <nocturne_settings.h>
#include "ui.h"
#include "display_abi.h"

#define WINDOW 1
#define VIEWS 4              /* 窓が答えていない view の数の見込みの輪 */
#define TRIAL_MS SCREEN_MODE_TRIAL_MS
#define TOKEN_SETTINGS (UINT64_C(1) << 40)
#define TOKEN_SCREEN (TOKEN_SETTINGS + 1)
#define TOKEN_SOUND (TOKEN_SETTINGS + 2)
#define TOKEN_SERVICES (TOKEN_SETTINGS + 3)
#define TOKEN_NETWORK (TOKEN_SETTINGS + 4)

enum page { P_AUDIO, P_DISPLAY, P_WINDOW, P_INPUT, P_POWER, P_NETWORK, P_TIME, P_BOOT, P_SERVICES, P_USERS, P_PROGRAMS, P_COUNT };
static const struct {
    const char *label, *icon;
    uint32_t domain;
} pages[P_COUNT] = {
    [P_AUDIO] = {"音", "sound", N_DOMAIN_AUDIO},
    [P_DISPLAY] = {"画面", "display", N_DOMAIN_DISPLAY},
    [P_WINDOW] = {"窓", "settings", N_DOMAIN_WINDOW},
    [P_INPUT] = {"入力", "mouse", N_DOMAIN_INPUT},
    [P_POWER] = {"電源", "settings", N_DOMAIN_POWER},
    [P_NETWORK] = {"網", "settings", N_DOMAIN_NETWORK},
    [P_TIME] = {"時刻", "clock", N_DOMAIN_TIME},
    [P_BOOT] = {"起動", "settings", N_DOMAIN_BOOT},
    [P_SERVICES] = {"サービス", "settings", N_SETTING_ANY},
    [P_USERS] = {"利用者", "settings", N_SETTING_ANY},
    [P_PROGRAMS] = {"アプリ", "settings", N_SETTING_ANY},
};

enum act {
    A_PAGE = 1, A_BOOL, A_NUMBER, A_ENUM, A_TEXT, A_SCREEN, A_SIZE, A_RATE, A_FORMAT, A_KEEP, A_BACK, A_SCALE, A_PLACE, A_MAIN,
    A_SERVICE_RESTART, A_SERVICE_STOP, A_USER_CLASS, A_NEW_NAME, A_NEW_PASSWORD, A_NEW_ADMIN, A_NEW_CREATE,
    A_PROGRAM_CHOOSE, A_PROGRAM_REMOVE, A_PROGRAM_NAME, A_PROGRAM_PATH, A_PROGRAM_TYPES, A_PROGRAM_ADD,
    A_LINK_IP, A_LINK_MASK, A_LINK_GATEWAY, A_LINK_FIXED, A_LINK_DHCP
};
struct action {
    uint32_t node, arg;
    enum act act;
};
static struct sent_view {
    struct action *list;
    size_t count, capacity;
} sent[VIEWS];
static uint32_t view_number;

/* ---- 宣言と値 ---- */
struct shown {
    struct n_setting_decl d;
    int32_t number;
    char *draft;     /* 文字列と数の打ちかけ */
    size_t draft_cap;
};
static struct shown *shown;
static size_t shown_count;
static enum page page;
static char status[256];
static bool status_bad;

static const char *reason_text(uint32_t reason) {
    switch (reason) {
    case N_SETTING_OK: return "";
    case N_SETTING_UNKNOWN_KEY: return "この設定はありません。";
    case N_SETTING_WRONG_TYPE: return "値の形が合いません。";
    case N_SETTING_OUT_OF_RANGE: return "値が範囲の外です。";
    case N_SETTING_NOT_ALLOWED: return "この設定を変える権限がありません。";
    case N_SETTING_APPLY_REFUSED: return "持ち主が値を受け付けませんでした。";
    case N_SETTING_APPLY_GONE: return "持ち主が答えません。";
    case N_SETTING_SAVE_FAILED: return "保存できませんでした。値は元へ戻しました。";
    case N_SETTING_NO_TRIAL: return "試しはもう終わっています。";
    case N_SETTING_NEEDS_TRIAL: return "この設定は試してから確定します。";
    case N_SETTING_BUSY: return "前の変更の答えを待っています。";
    case N_SETTING_BAD_DECLARATION: return "宣言が正しくありません。";
    default: return "値が通りませんでした。";
    }
}
static void say(const char *text, bool bad) {
    strlcpy(status, text, sizeof status);
    status_bad = bad;
}
static void said(n_result r, uint32_t reason) {
    if (r >= 0) say("", false);
    else if (reason != N_SETTING_OK) say(reason_text(reason), true);
    else snprintf(status, sizeof status, "%s", n_result_text(r)), status_bad = true;
}

static void endpoint_text(const struct n_sockaddr *a, char *out, size_t cap) {
    snprintf(out, cap, "%u.%u.%u.%u", a->ip >> 24, a->ip >> 16 & 255, a->ip >> 8 & 255, a->ip & 255);
}
static bool endpoint_read(const char *s, struct n_sockaddr *out) {
    unsigned a, b, c, d;
    char extra;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
    *out = (struct n_sockaddr){.ip = a << 24 | b << 16 | c << 8 | d, .port = 53};
    return true;
}

/* 一つの値を読み直して見せるものにする。 */
static void shown_read(struct shown *s) {
    struct n_setting_id id = n_setting_at(s->d.domain, s->d.key);
    switch (s->d.type) {
    case N_SETTING_BOOL: case N_SETTING_INT: case N_SETTING_ENUM: {
        int32_t v;
        s->number = n_setting_get_int(id, &v) >= 0 ? v : (int32_t)s->d.fallback;
        if (s->d.type == N_SETTING_INT) snprintf(s->draft, s->draft_cap, "%d", s->number);
        break;
    }
    case N_SETTING_STRING: {
        n_result n = n_setting_get(id, s->draft, s->draft_cap - 1);
        s->draft[n >= 0 ? (size_t)n : 0] = 0;
        break;
    }
    case N_SETTING_ENDPOINT: {
        struct n_sockaddr a = {0};
        if (n_setting_get(id, &a, sizeof a) >= 0) endpoint_text(&a, s->draft, s->draft_cap);
        break;
    }
    default: break;
    }
}

static void shown_load(void) {
    struct n_setting_decl *list = NULL;
    n_result n = n_setting_list(&list);
    if (n < 0) {
        snprintf(status, sizeof status, "設定のサービスに届きません（%s）。", n_result_text(n));
        status_bad = true;
        return;
    }
    for (size_t i = 0; i < shown_count; i++) free(shown[i].draft);
    free(shown);
    shown = calloc((size_t)n ? (size_t)n : 1, sizeof *shown);
    shown_count = shown ? (size_t)n : 0;
    for (size_t i = 0; i < shown_count; i++) {
        shown[i].d = list[i];
        shown[i].draft_cap = list[i].type == N_SETTING_STRING ? (size_t)list[i].max + 1 : 64;
        shown[i].draft = calloc(1, shown[i].draft_cap);
        if (shown[i].draft) shown_read(&shown[i]);
    }
    free(list);
}
static void shown_changed(const struct n_settings_changed *c) {
    for (size_t i = 0; i < shown_count; i++)
        if (shown[i].d.domain == c->id.domain && shown[i].d.key == c->id.key && shown[i].draft) shown_read(&shown[i]);
}

/* ---- 画面 ---- */
static uint32_t screen_index;
static struct screen_modes screen;
static bool screen_known;
static uint64_t screen_device_id;
static struct n_setting_id trial_id;  /* 試しの中の鍵 */
static uint64_t trial_ends_ns; /* 0：試しの中でない */
static char scale_draft[16];
static struct n_audio_output sound;

static void screen_read(void) {
    screen_known = n_screen_modes_get(screen_index, &screen) >= 0;
    if (!screen_known && screen_index) {
        screen_index = 0;
        screen_known = n_screen_modes_get(0, &screen) >= 0;
    }
    if (screen_known && n_screen_device(screen_index, &screen_device_id) < 0) screen_known = false;
    if (screen_known) snprintf(scale_draft, sizeof scale_draft, "%u", screen.scale_percent);
}
static bool same_size(const struct n_screen_mode *a, const struct n_screen_mode *b) {
    return a->width == b->width && a->height == b->height;
}
static const struct n_screen_mode *mode_marked(void) {
    return screen.source == SCREEN_SOURCE_DRIVER && screen.wanted.width ? &screen.wanted : &screen.offered.shown;
}
static struct n_setting_id screen_id(uint32_t key, uint64_t device) {
    struct n_setting_id id = n_setting_at(N_DOMAIN_DISPLAY, key);
    id.device = device;
    return id;
}
static void trial_start(struct n_setting_id id, const void *value, size_t length, const void *before, size_t before_length) {
    uint32_t reason = N_SETTING_OK;
    n_result r = n_setting_try(id, value, length, before, before_length, TRIAL_MS, &reason);
    said(r, reason);
    if (r >= 0) {
        trial_id = id;
        trial_ends_ns = n_clock_monotonic() + TRIAL_MS * (uint64_t)N_NS_PER_MS;
    }
}
static void mode_try(const struct n_screen_mode *m) {
    struct n_setting_monitor_mode wanted = {m->width, m->height, m->refresh_mhz, m->format};
    const struct n_screen_mode *o = &screen.offered.shown;
    struct n_setting_monitor_mode before = {o->width, o->height, o->refresh_mhz, o->format};
    trial_start(screen_id(N_DISPLAY_MODE, screen_device_id), &wanted, sizeof wanted, &before, sizeof before);
}
/* 解像度を選んだ：その大きさのモニターの好みか、今の拍に一番近いもの。 */
static void size_try(const struct n_screen_mode *size) {
    uint32_t now = mode_marked()->refresh_mhz;
    const struct n_screen_mode *best = NULL;
    for (uint32_t i = 0; i < screen.offered.count; i++) {
        const struct n_screen_mode *m = &screen.offered.modes[i];
        if (!same_size(m, size)) continue;
        if (m->flags & N_SCREEN_MODE_PREFERRED) {
            best = m;
            break;
        }
        int64_t dm = (int64_t)m->refresh_mhz - now, db = best ? (int64_t)best->refresh_mhz - now : 0;
        if (dm < 0) dm = -dm;
        if (db < 0) db = -db;
        if (!best || dm < db || (dm == db && m->refresh_mhz > best->refresh_mhz)) best = m;
    }
    if (best) mode_try(best);
}
static void scale_try(void) {
    char *end;
    long v = strtol(scale_draft, &end, 10);
    if (end == scale_draft || *end || v < 0 || v > INT32_MAX) {
        say("数を入れてください。", true);
        return;
    }
    int32_t scale = (int32_t)v, before = (int32_t)screen.scale_percent;
    trial_start(screen_id(N_DISPLAY_SCALE, screen_device_id), &scale, sizeof scale, &before, sizeof before);
}

/* 配置：全部の画面の今の位置を書き、選んだ画面だけを主画面の縁へ動かす。自動の順と食い違わないように。 */
enum { PLACE_LEFT, PLACE_RIGHT, PLACE_ABOVE, PLACE_BELOW };
static void place_set(uint64_t device, const struct n_setting_monitor_place *place) {
    uint32_t reason = N_SETTING_OK;
    n_result r = n_setting_set(screen_id(N_DISPLAY_PLACE, device), place, sizeof *place, &reason);
    if (r < 0) said(r, reason);
}
static void screen_place(uint32_t where) {
    struct screen_modes *other = malloc(sizeof *other);
    if (!other || !screen_known) {
        free(other);
        return;
    }
    say("", false);
    int32_t main_x = 0, main_y = 0, main_width = 0, main_height = 0;
    for (uint32_t i = 0; i < screen.screens; i++)
        if (n_screen_modes_get(i, other) >= 0 && other->primary) {
            main_x = other->x, main_y = other->y;
            main_width = (int32_t)other->width, main_height = (int32_t)other->height;
        }
    for (uint32_t i = 0; i < screen.screens; i++) {
        uint64_t device;
        if (i == screen_index || n_screen_modes_get(i, other) < 0 || !other->on || n_screen_device(i, &device) < 0) continue;
        struct n_setting_monitor_place place = {other->x, other->y, 1};
        place_set(device, &place);
    }
    free(other);
    struct n_setting_monitor_place place = {main_x, main_y, 1};
    switch (where) {
    case PLACE_LEFT: place.x = main_x - (int32_t)screen.width; break;
    case PLACE_RIGHT: place.x = main_x + main_width; break;
    case PLACE_ABOVE: place.y = main_y - (int32_t)screen.height; break;
    default: place.y = main_y + main_height; break;
    }
    place_set(screen_device_id, &place);
}
static void screen_make_main(void) {
    uint32_t reason = N_SETTING_OK;
    n_result r = n_setting_set(n_setting_at(N_DOMAIN_DISPLAY, N_DISPLAY_PRIMARY), &screen_device_id, sizeof screen_device_id, &reason);
    said(r, reason);
}

/* ---- サービス ---- */
static struct n_service_status *services;
static uint32_t service_count;
static n_handle services_watch = -1;

static const char *service_state_text(uint32_t state) {
    switch (state) {
    case N_SERVICE_STOPPED: return "止まっています";
    case N_SERVICE_WAITING: return "依存を待っています";
    case N_SERVICE_STARTING: return "起動しています";
    case N_SERVICE_RUNNING: return "動いています";
    case N_SERVICE_BACKOFF: return "落ちました。起こし直しを待っています";
    case N_SERVICE_FAILED: return "起こし直しを諦めました";
    case N_SERVICE_ABSENT: return "この媒体にありません";
    default: return "";
    }
}

static void services_load(void) {
    struct n_service_status *list;
    n_result r = n_services_list(&list);
    if (r < 0) {
        snprintf(status, sizeof status, "サービスの管理者に届きません（%s）。", n_result_text(r));
        status_bad = true;
        return;
    }
    free(services);
    services = list;
    service_count = (uint32_t)r;
}
static void service_do(uint32_t index, uint32_t op) {
    if (index >= service_count) return;
    const struct n_service_status *st = &services[index];
    n_result r = n_services_control(op, st->name, st->argument);
    if (r < 0) snprintf(status, sizeof status, "%s：%s", st->name, n_result_text(r)), status_bad = true;
    else say("", false);
    services_load();
}
static void services_watch_open(n_handle queue) {
    n_handle h = n_service_connect(N_SERVICE_MANAGER);
    if (h < 0) return;
    struct n_services_request q = {.op = N_SERVICES_WATCH};
    struct n_services_reply a;
    n_result r = n_channel_send(h, &q, sizeof q, -1);
    if (r >= 0) r = n_channel_receive(h, &a, sizeof a, N_CHANNEL_WAIT, NULL);
    if (r < 0 || n_events_watch(queue, h, N_EVENT_RECEIVABLE, TOKEN_SERVICES) < 0) {
        n_handle_close(h);
        return;
    }
    services_watch = h;
}

/* ---- 網 ---- */
/* adapter ごとの打ちかけ。a.b.c.d の字数で足りる。 */
struct link_view {
    struct n_net_adapter adapter;
    char name[64];
    struct n_setting_id fixed_id;
    struct n_setting_ipv4 fixed;
    char ip[16], mask[16], gateway[16];
};
static struct link_view *link_views;
static size_t link_view_count;
static struct n_network_link *link_records;
static n_result link_record_count;

static void address_text(uint32_t ip, char *out) {
    if (ip) ip_format(ip, out);
    else out[0] = 0;
}

static void link_view_read(struct link_view *v) {
    static const char digits[] = "0123456789abcdef";
    char mac[18];
    for (int i = 0; i < 6; i++) {
        mac[i * 3] = digits[v->adapter.mac[i] >> 4];
        mac[i * 3 + 1] = digits[v->adapter.mac[i] & 15];
        mac[i * 3 + 2] = i < 5 ? ':' : 0;
    }
    v->fixed_id = n_setting_at(N_DOMAIN_NETWORK, N_NETWORK_FIXED_ADDRESS);
    v->fixed_id.device = n_setting_device(mac);
    memset(&v->fixed, 0, sizeof v->fixed);
    if (n_setting_get(v->fixed_id, &v->fixed, sizeof v->fixed) != sizeof v->fixed) memset(&v->fixed, 0, sizeof v->fixed);
    address_text(v->fixed.ip, v->ip);
    address_text(v->fixed.mask, v->mask);
    address_text(v->fixed.gateway, v->gateway);
}

/* 一覧を読み直す。打ちかけは残す。 */
static void network_load(void) {
    n_result count = n_net_adapters(NULL, 0);
    struct link_view *views = count > 0 ? calloc((size_t)count, sizeof *views) : NULL;
    struct n_net_adapter *list = count > 0 ? malloc((size_t)count * sizeof *list) : NULL;
    size_t kept = 0;
    if (views && list) {
        count = n_net_adapters(list, (size_t)count);
        for (n_result i = 0; i < count; i++, kept++) {
            struct link_view *v = &views[i];
            v->adapter = list[i];
            n_result named = n_net_adapter_name(list[i].id, v->name, sizeof v->name - 1);
            if (named > 0) v->name[named < (n_result)sizeof v->name ? named : (n_result)sizeof v->name - 1] = 0;
            const struct link_view *before = NULL;
            for (size_t b = 0; b < link_view_count && !before; b++)
                if (link_views[b].adapter.id == list[i].id) before = &link_views[b];
            if (before) {
                v->fixed_id = before->fixed_id;
                v->fixed = before->fixed;
                memcpy(v->ip, before->ip, sizeof v->ip);
                memcpy(v->mask, before->mask, sizeof v->mask);
                memcpy(v->gateway, before->gateway, sizeof v->gateway);
            } else {
                link_view_read(v);
            }
        }
    }
    free(list);
    free(link_views);
    link_views = views;
    link_view_count = kept;
    free(link_records);
    link_records = NULL;
    link_record_count = n_network_links(&link_records);
    if (link_record_count < 0) link_records = NULL;
}

static void link_fixed_apply(struct link_view *v) {
    struct n_setting_ipv4 value = {0};
    if (!ip_parse(v->ip, &value.ip) || !ip_parse(v->mask, &value.mask) || (v->gateway[0] && !ip_parse(v->gateway, &value.gateway))) {
        say("住所とマスクは a.b.c.d の形で入れてください。", true);
        return;
    }
    if (!value.ip || ~value.mask & (~value.mask + 1)) {
        say("マスクは上の位から続く 1 の形で入れてください。", true);
        return;
    }
    uint32_t reason = N_SETTING_OK;
    n_result r = n_setting_set(v->fixed_id, &value, sizeof value, &reason);
    said(r, reason);
    if (r >= 0) v->fixed = value;
}

static void link_dhcp_apply(struct link_view *v) {
    struct n_setting_ipv4 value = {0};
    uint32_t reason = N_SETTING_OK;
    n_result r = n_setting_set(v->fixed_id, &value, sizeof value, &reason);
    said(r, reason);
    if (r >= 0) link_view_read(v);
}

/* ---- 利用者 ---- */
static struct n_account *accounts;
static uint32_t account_count;
static char new_name[N_ACCOUNT_NAME_MAX], new_password[N_ACCOUNT_PASSWORD_MAX];
static bool new_admin;

static void accounts_load(void) {
    struct n_account *list;
    n_result r = n_accounts_list(&list);
    if (r < 0) {
        snprintf(status, sizeof status, "利用者のサービスに届きません（%s）。", n_result_text(r));
        status_bad = true;
        return;
    }
    free(accounts);
    accounts = list;
    account_count = (uint32_t)r;
}
static void account_class(uint32_t index, uint32_t cls) {
    if (index >= account_count) return;
    struct n_accounts_request q = {.op = N_ACCOUNTS_SET_CLASS, .user = accounts[index].user, .cls = cls};
    struct n_accounts_reply a;
    n_result r = n_accounts_call(&q, &a);
    if (r < 0) snprintf(status, sizeof status, "級を変えられません（%s）。", n_result_text(r)), status_bad = true;
    else say("", false);
    accounts_load();
}
static void account_create(void) {
    if (!*new_name) {
        say("名前を入れてください。", true);
        return;
    }
    struct n_accounts_request q = {.op = N_ACCOUNTS_CREATE, .cls = new_admin ? N_CLASS_ADMIN : N_CLASS_USER};
    strlcpy(q.name, new_name, sizeof q.name);
    strlcpy(q.password, new_password, sizeof q.password);
    struct n_accounts_reply a;
    n_result r = n_accounts_call(&q, &a);
    memset(&q, 0, sizeof q);
    if (r < 0) {
        snprintf(status, sizeof status, "作れません（%s）。", n_result_text(r));
        status_bad = true;
    } else {
        snprintf(status, sizeof status, "%s を作りました。", new_name);
        status_bad = false;
        new_name[0] = new_password[0] = 0;
    }
    accounts_load();
}

/* ---- view ---- */
static struct sent_view *building;
static void act_at(uint32_t node, enum act act, uint32_t arg) {
    if (building->count == building->capacity) {
        size_t grown = building->capacity ? building->capacity * 2 : 64;
        struct action *list = realloc(building->list, grown * sizeof *list);
        if (!list) return;
        building->list = list;
        building->capacity = grown;
    }
    building->list[building->count++] = (struct action){.node = node, .arg = arg, .act = act};
}
static void text_node(struct ui_msg *m, const char *s, const char *style, uint32_t colour) {
    ui_node(m, "text");
    ui_str(m, "text", s);
    if (style) ui_str(m, "style", style);
    if (colour) ui_num(m, "colour", colour);
    ui_end(m);
}
/* 鍵の頭：名前と説明。 */
static void setting_words(struct ui_msg *m, const struct n_setting_decl *d, const char *extra) {
    ui_node(m, "column"); ui_num(m, "gap", 4); ui_num(m, "grow", 1);
        text_node(m, d->label, "heading", 0);
        if (d->about[0]) text_node(m, d->about, "caption", 0);
        if (extra) text_node(m, extra, "caption", 0);
    ui_end(m);
}
static uint32_t options_of(const struct n_setting_decl *d, const char **out, uint32_t cap) {
    uint32_t n = 0;
    for (size_t at = 0; at < sizeof d->options && d->options[at] && n < cap;) {
        out[n++] = &d->options[at];
        at += strlen(&d->options[at]) + 1;
    }
    return n;
}

/* 宣言から部品を組む。 */
static void setting_card(struct ui_msg *m, size_t index) {
    const struct shown *s = &shown[index];
    const struct n_setting_decl *d = &s->d;
    ui_node(m, "card"); ui_num(m, "gap", 14); ui_str(m, "key", d->name);
    switch (d->type) {
    case N_SETTING_BOOL:
        ui_node(m, "row"); ui_num(m, "gap", 16);
            setting_words(m, d, NULL);
            act_at(ui_node(m, "toggle"), A_BOOL, (uint32_t)index);
            ui_num(m, "on", s->number != 0);
            ui_end(m);
        ui_end(m);
        break;
    case N_SETTING_ENUM: {
        const char *options[32];
        uint32_t n = options_of(d, options, 32);
        setting_words(m, d, NULL);
        act_at(ui_node(m, "segmented"), A_ENUM, (uint32_t)index);
        ui_strs(m, "options", options, n);
        ui_num(m, "selected", s->number);
        ui_end(m);
        break;
    }
    case N_SETTING_INT:
        setting_words(m, d, NULL);
        if (d->max - d->min <= 1000) {
            ui_node(m, "row"); ui_num(m, "gap", 14);
                act_at(ui_node(m, "slider"), A_NUMBER, (uint32_t)index);
                ui_num(m, "value", s->number); ui_num(m, "min", (double)d->min); ui_num(m, "max", (double)d->max);
                ui_num(m, "grow", 1); ui_num(m, "wheelStep", d->max - d->min > 20 ? 5 : 1);
                ui_end(m);
                char number[16];
                snprintf(number, sizeof number, "%d", s->number);
                ui_node(m, "text"); ui_str(m, "text", number); ui_num(m, "width", 56); ui_str(m, "textAlign", "end");
                ui_num(m, "weight", 1); ui_end(m);
            ui_end(m);
        } else {
            act_at(ui_node(m, "field"), A_TEXT, (uint32_t)index);
            ui_str(m, "value", s->draft); ui_str(m, "width", "fill");
            ui_end(m);
        }
        break;
    case N_SETTING_STRING:
    case N_SETTING_ENDPOINT:
        setting_words(m, d, NULL);
        act_at(ui_node(m, "field"), A_TEXT, (uint32_t)index);
        ui_str(m, "value", s->draft); ui_str(m, "width", "fill");
        if (d->flags & N_SETTING_DECL_SECRET) ui_num(m, "secret", 1);
        ui_end(m);
        break;
    default:
        setting_words(m, d, NULL);
    }
    ui_end(m);
}

static void mode_name(char *out, size_t cap, const struct n_screen_mode *mode) {
    if (mode->refresh_mhz) snprintf(out, cap, "%u × %u、%u.%02u Hz", mode->width, mode->height, mode->refresh_mhz / 1000, mode->refresh_mhz % 1000 / 10);
    else snprintf(out, cap, "%u × %u", mode->width, mode->height);
}

/* なぜモードの一覧が無いか。 */
static const char *screen_reason(void) {
    switch (screen.source) {
    case SCREEN_SOURCE_FIRMWARE: return "表示のドライバーが動いていないので、起動の時の解像度のままです。";
    case SCREEN_SOURCE_DRIVER: return "表示のドライバーがモニターを見ていません。";
    case SCREEN_SOURCE_HANDED_BACK: return "表示のドライバーが画面を返したので、起動の時の解像度です。";
    case SCREEN_SOURCE_DRIVER_STOPPED: return "表示のドライバーが止まったので、起動の時の解像度に戻っています。";
    case SCREEN_SOURCE_REMOTE: return "大きさはリモートデスクトップの窓に従います。";
    default: return "";
    }
}

static const char *format_name(uint32_t format) {
    switch (format) {
    case N_SCANOUT_XRGB2101010: return "10 ビット";
    case N_SCANOUT_XRGB8888: case N_SCANOUT_XBGR8888: return "8 ビット";
    default: return "";
    }
}

/* 画面が二つ以上のとき：どの画面を見るか、配置、主画面。 */
static void screens_card(struct ui_msg *m) {
    uint32_t n = screen.screens;
    const char **names = malloc(n * sizeof *names);
    char (*labels)[24] = malloc(n * sizeof *labels);
    if (!names || !labels) {
        free(names), free(labels);
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        snprintf(labels[i], sizeof labels[i], "画面 %u", i + 1);
        names[i] = labels[i];
    }
    ui_node(m, "card"); ui_num(m, "gap", 14); ui_str(m, "key", "screens");
        text_node(m, "画面の選択", "heading", 0);
        act_at(ui_node(m, "segmented"), A_SCREEN, 0);
        ui_strs(m, "options", names, n);
        ui_num(m, "selected", screen_index);
        ui_end(m);
        char line[96];
        snprintf(line, sizeof line, "デスクトップの中の (%d, %d) にあります。%s", screen.x, screen.y, screen.primary ? "主画面です。" : "");
        text_node(m, line, "caption", 0);
        if (!screen.primary) {
            text_node(m, "主画面に対する位置", NULL, 0);
            static const char *const where[] = {"左", "右", "上", "下"};
            ui_node(m, "row"); ui_num(m, "gap", 10);
            for (uint32_t i = 0; i < sizeof where / sizeof *where; i++) {
                act_at(ui_node(m, "button"), A_PLACE, i); ui_str(m, "text", where[i]); ui_end(m);
            }
            act_at(ui_node(m, "button"), A_MAIN, 0); ui_str(m, "text", "主画面にする"); ui_str(m, "variant", "primary"); ui_end(m);
            ui_end(m);
        }
    ui_end(m);
    free(names), free(labels);
}

static void screen_card(struct ui_msg *m) {
    char line[96];
    const struct ui_theme *t = &ui_theme;
    if (screen_known && screen.screens > 1) screens_card(m);
    ui_node(m, "card"); ui_num(m, "gap", 14); ui_str(m, "key", "screen");
    if (!screen_known) {
        text_node(m, "画面の情報が読めません。", "caption", 0);
        ui_end(m);
        return;
    }
    mode_name(line, sizeof line, &screen.offered.shown);
    text_node(m, screen.offered.name[0] ? screen.offered.name : "画面", "heading", 0);
    text_node(m, line, NULL, 0);
    bool listed = screen.source != SCREEN_SOURCE_REMOTE && screen.offered.count > 0;
    if (!listed || screen.source == SCREEN_SOURCE_HANDED_BACK) text_node(m, screen_reason(), "caption", 0);
    if (screen.refused_serial && screen.refused_serial == screen.wish_serial - 1) text_node(m, "ドライバーが選んだモードを出せなかったので、次のものにしました。", "caption", t->warning);
    if (trial_ends_ns) {
        uint64_t now = n_clock_monotonic();
        uint32_t left = now < trial_ends_ns ? (uint32_t)((trial_ends_ns - now) / (1000 * N_NS_PER_MS)) + 1 : 0;
        char words[96];
        snprintf(words, sizeof words, "このままにしますか。あと %u 秒で前へ戻ります。", left);
        ui_node(m, "card"); ui_num(m, "gap", 10); ui_num(m, "background", t->sidebar); ui_str(m, "key", "trial");
            text_node(m, words, "heading", 0);
            ui_node(m, "row"); ui_num(m, "gap", 10);
                act_at(ui_node(m, "button"), A_KEEP, 0); ui_str(m, "text", "このままにする"); ui_str(m, "variant", "primary"); ui_end(m);
                act_at(ui_node(m, "button"), A_BACK, 0); ui_str(m, "text", "前へ戻す"); ui_end(m);
            ui_end(m);
        ui_end(m);
    }
    if (listed) {
        const struct n_screen_mode *marked = mode_marked();
        ui_node(m, "row"); ui_num(m, "gap", 16); ui_str(m, "align", "start");
            ui_node(m, "column"); ui_num(m, "grow", 1); ui_num(m, "gap", 6);
                text_node(m, "解像度", "caption", 0);
                ui_node(m, "scroll"); ui_num(m, "maxHeight", 236); ui_num(m, "gap", 2);
                for (uint32_t i = 0; i < screen.offered.count; i++) {
                    const struct n_screen_mode *mode = &screen.offered.modes[i];
                    bool first = true;
                    for (uint32_t j = 0; j < i && first; j++) first = !same_size(&screen.offered.modes[j], mode);
                    if (!first) continue;
                    snprintf(line, sizeof line, "%u × %u", mode->width, mode->height);
                    act_at(ui_node(m, "item"), A_SIZE, i);
                    ui_str(m, "text", line); ui_num(m, "selected", same_size(mode, marked)); ui_str(m, "key", line);
                    ui_end(m);
                }
                ui_end(m);
            ui_end(m);
            ui_node(m, "column"); ui_num(m, "grow", 1); ui_num(m, "gap", 6);
                text_node(m, "リフレッシュレート", "caption", 0);
                ui_node(m, "scroll"); ui_num(m, "maxHeight", 236); ui_num(m, "gap", 2);
                for (uint32_t i = 0; i < screen.offered.count; i++) {
                    const struct n_screen_mode *mode = &screen.offered.modes[i];
                    if (!same_size(mode, marked)) continue;
                    bool first = true;
                    for (uint32_t j = 0; j < i && first; j++)
                        first = !same_size(&screen.offered.modes[j], mode) || screen.offered.modes[j].refresh_mhz != mode->refresh_mhz;
                    if (!first) continue;
                    snprintf(line, sizeof line, "%u.%02u Hz%s", mode->refresh_mhz / 1000, mode->refresh_mhz % 1000 / 10,
                             mode->flags & N_SCREEN_MODE_VRR ? "（可変）" : "");
                    act_at(ui_node(m, "item"), A_RATE, i);
                    ui_str(m, "text", line); ui_num(m, "selected", mode->refresh_mhz == marked->refresh_mhz); ui_str(m, "key", line);
                    ui_end(m);
                }
                ui_end(m);
            ui_end(m);
            /* 色の形式：同じ大きさと拍にいくつもあるときだけ */
            bool formats = false;
            for (uint32_t i = 0; i < screen.offered.count; i++) {
                const struct n_screen_mode *mode = &screen.offered.modes[i];
                formats = formats || (same_size(mode, marked) && mode->refresh_mhz == marked->refresh_mhz && mode->format != marked->format);
            }
            if (formats) {
                ui_node(m, "column"); ui_num(m, "grow", 1); ui_num(m, "gap", 6);
                    text_node(m, "色の深さ", "caption", 0);
                    for (uint32_t i = 0; i < screen.offered.count; i++) {
                        const struct n_screen_mode *mode = &screen.offered.modes[i];
                        if (!same_size(mode, marked) || mode->refresh_mhz != marked->refresh_mhz) continue;
                        act_at(ui_node(m, "item"), A_FORMAT, i);
                        ui_str(m, "text", format_name(mode->format)); ui_num(m, "selected", mode->format == marked->format);
                        ui_str(m, "key", format_name(mode->format));
                        ui_end(m);
                    }
                ui_end(m);
            }
        ui_end(m);
    }
    ui_end(m);
    ui_node(m, "card"); ui_num(m, "gap", 14); ui_str(m, "key", "scale");
        text_node(m, "倍率（百分率）", "heading", 0);
        text_node(m, "プログラムへ知らせる値です。描く大きさはまだ変わりません。100 が等倍です。", "caption", 0);
        act_at(ui_node(m, "field"), A_SCALE, 0);
        ui_str(m, "value", scale_draft); ui_str(m, "width", "fill");
        ui_end(m);
    ui_end(m);
}

static const char *link_state_text(uint32_t state) {
    switch (state) {
    case N_LINK_ABSENT: return "装置がありません";
    case N_LINK_SEARCHING: return "DHCP の答えを待っています";
    case N_LINK_LEASED: return "DHCP の住所を使っています";
    case N_LINK_FIXED: return "固定の住所を使っています";
    default: return "";
    }
}

static void link_field(struct ui_msg *m, const char *label, const char *value, enum act act, uint32_t index) {
    ui_node(m, "column"); ui_num(m, "gap", 4); ui_num(m, "grow", 1);
        text_node(m, label, "caption", 0);
        act_at(ui_node(m, "field"), act, index);
        ui_str(m, "value", value); ui_str(m, "width", "fill");
        ui_end(m);
    ui_end(m);
}

static void link_card(struct ui_msg *m, uint32_t index) {
    const struct link_view *v = &link_views[index];
    const struct n_network_link *record = NULL;
    struct n_network_link *walk = link_records;
    for (n_result i = 0; i < link_record_count; i++, walk = n_network_link_next(walk))
        if (walk->adapter == v->adapter.id) record = walk;
    char line[160], text[16], text2[16];
    const uint8_t *mac = v->adapter.mac;
    snprintf(line, sizeof line, "%s  %02x:%02x:%02x:%02x:%02x:%02x", v->name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    char key[24];
    snprintf(key, sizeof key, "adapter %u", (unsigned)v->adapter.id);
    ui_node(m, "card"); ui_num(m, "gap", 10); ui_str(m, "key", key);
        text_node(m, line, "heading", 0);
        text_node(m, record ? link_state_text(record->state) : v->adapter.present ? "サービス network に届きません" : link_state_text(N_LINK_ABSENT), "caption", 0);
        if (v->adapter.ip) {
            snprintf(line, sizeof line, "住所 %s  マスク %s", ip_format(v->adapter.ip, text), ip_format(v->adapter.mask, text2));
            text_node(m, line, "caption", 0);
        }
        if (record) {
            if (record->gateway) {
                snprintf(line, sizeof line, "ゲートウェイ %s", ip_format(record->gateway, text));
                text_node(m, line, "caption", 0);
            }
            const uint32_t *addresses = n_network_link_addresses(record);
            for (uint32_t i = 0; i < record->dns_count; i++) {
                snprintf(line, sizeof line, "DNS %s", ip_format(addresses[i], text));
                text_node(m, line, "caption", 0);
            }
            for (uint32_t i = 0; i < record->ntp_count; i++) {
                snprintf(line, sizeof line, "NTP %s", ip_format(addresses[record->dns_count + i], text));
                text_node(m, line, "caption", 0);
            }
            if (record->state == N_LINK_LEASED && record->lease_seconds) {
                snprintf(line, sizeof line, "借りた住所の残り %u 秒", (unsigned)record->lease_seconds);
                text_node(m, line, "caption", 0);
            }
        }
        ui_node(m, "row"); ui_num(m, "gap", 10);
            link_field(m, "固定の住所", v->ip, A_LINK_IP, index);
            link_field(m, "マスク", v->mask, A_LINK_MASK, index);
            link_field(m, "ゲートウェイ", v->gateway, A_LINK_GATEWAY, index);
        ui_end(m);
        ui_node(m, "row"); ui_num(m, "gap", 10);
            act_at(ui_node(m, "button"), A_LINK_FIXED, index); ui_str(m, "text", "固定の住所を使う"); ui_end(m);
            act_at(ui_node(m, "button"), A_LINK_DHCP, index); ui_str(m, "text", "DHCP を使う"); ui_end(m);
        ui_end(m);
    ui_end(m);
}

static void services_page(struct ui_msg *m) {
    if (!service_count) text_node(m, "サービスの一覧がありません。", "caption", 0);
    for (uint32_t i = 0; i < service_count; i++) {
        const struct n_service_status *st = &services[i];
        char name[N_SERVICE_NAME_MAX + N_SERVICE_ARGUMENT_MAX + 1], line[160];
        snprintf(name, sizeof name, st->argument[0] ? "%s:%s" : "%s", st->name, st->argument);
        snprintf(line, sizeof line, "%s。起こし直し %u 回。", service_state_text(st->state), st->restarts);
        ui_node(m, "card"); ui_num(m, "gap", 8); ui_str(m, "key", name);
            ui_node(m, "row"); ui_num(m, "gap", 10);
                ui_node(m, "column"); ui_num(m, "gap", 4); ui_num(m, "grow", 1);
                    text_node(m, name, "heading", 0);
                    text_node(m, line, "caption", 0);
                ui_end(m);
                act_at(ui_node(m, "button"), A_SERVICE_RESTART, i); ui_str(m, "text", "起こし直す"); ui_end(m);
                act_at(ui_node(m, "button"), A_SERVICE_STOP, i); ui_str(m, "text", "止める"); ui_end(m);
            ui_end(m);
        ui_end(m);
    }
}

static void users_page(struct ui_msg *m) {
    static const char *const classes[] = {"利用者", "管理者"};
    for (uint32_t i = 0; i < account_count; i++) {
        const struct n_account *a = &accounts[i];
        ui_node(m, "card"); ui_num(m, "gap", 8); ui_str(m, "key", a->name);
            ui_node(m, "row"); ui_num(m, "gap", 10);
                ui_node(m, "column"); ui_num(m, "gap", 4); ui_num(m, "grow", 1);
                    text_node(m, a->name, "heading", 0);
                    text_node(m, a->folder, "caption", 0);
                ui_end(m);
                act_at(ui_node(m, "segmented"), A_USER_CLASS, i);
                ui_strs(m, "options", classes, 2);
                ui_num(m, "selected", a->cls == N_CLASS_ADMIN);
                ui_end(m);
            ui_end(m);
        ui_end(m);
    }
    ui_node(m, "card"); ui_num(m, "gap", 10); ui_str(m, "key", "new");
        text_node(m, "利用者を作る", "heading", 0);
        act_at(ui_node(m, "field"), A_NEW_NAME, 0);
        ui_str(m, "value", new_name); ui_str(m, "placeholder", "名前"); ui_str(m, "width", "fill");
        ui_end(m);
        act_at(ui_node(m, "field"), A_NEW_PASSWORD, 0);
        ui_str(m, "value", new_password); ui_str(m, "placeholder", "合言葉"); ui_str(m, "width", "fill"); ui_num(m, "secret", 1);
        ui_end(m);
        ui_node(m, "row"); ui_num(m, "gap", 10);
            act_at(ui_node(m, "checkbox"), A_NEW_ADMIN, 0);
            ui_str(m, "text", "管理者にする"); ui_num(m, "on", new_admin);
            ui_end(m);
            act_at(ui_node(m, "button"), A_NEW_CREATE, 0); ui_str(m, "text", "作る"); ui_str(m, "variant", "primary"); ui_end(m);
        ui_end(m);
    ui_end(m);
}

/* ---- アプリ：既定と登録（サービス programs） ---- */
static struct n_program *program_list;
static n_result program_total;
static struct n_program_claim *claim_list; /* 種類の順。"*" が先頭。 */
static n_result claim_total;
static char *program_name_text, *program_path_text, *program_types_text;

static void programs_load(void) {
    free(program_list);
    free(claim_list);
    program_list = NULL;
    claim_list = NULL;
    program_total = n_programs_list(&program_list);
    claim_total = n_programs_claims(&claim_list);
    if (program_total < 0 || claim_total < 0) {
        snprintf(status, sizeof status, "%s", n_result_text(program_total < 0 ? program_total : claim_total));
        status_bad = true;
    }
}

static const char *program_label(uint32_t id) {
    for (n_result i = 0; i < program_total; i++)
        if (program_list[i].id == id) return program_list[i].name;
    return "";
}

/* 種類の組 [group, end) を開けるプログラムの番号：名乗ったものと、何でも開くもの。 */
static size_t type_candidates(size_t group, size_t end, uint32_t *ids) {
    size_t n = 0;
    for (size_t i = group; i < end; i++) ids[n++] = claim_list[i].id;
    for (n_result i = 0; i < claim_total && !strcmp(claim_list[i].type, N_PROGRAM_ANY_TYPE); i++) {
        bool known = false;
        for (size_t k = 0; k < n; k++) known = known || ids[k] == claim_list[i].id;
        if (!known) ids[n++] = claim_list[i].id;
    }
    return n;
}

static size_t type_end(size_t group) {
    size_t end = group;
    while (end < (size_t)claim_total && !strcmp(claim_list[end].type, claim_list[group].type)) end++;
    return end;
}

static void program_type_chosen(uint32_t group, uint32_t option) {
    if (claim_total <= 0 || group >= (uint32_t)claim_total) return;
    uint32_t *ids = malloc((size_t)claim_total * sizeof *ids);
    if (!ids) return;
    size_t n = type_candidates(group, type_end(group), ids);
    n_result r = option < n ? n_program_choose(ids[option], claim_list[group].type) : -N_ERR_INVALID;
    free(ids);
    said(r, N_SETTING_OK);
    programs_load();
}

static void text_keep(char **field, const char *text) {
    char *copy = strdup(text ? text : "");
    if (!copy) return;
    free(*field);
    *field = copy;
}

static void program_add(void) {
    if (!program_name_text || !*program_name_text || !program_path_text || !*program_path_text) {
        say("名前とパスを入れてください。", true);
        return;
    }
    n_result id = n_program_register(program_name_text, program_path_text, NULL, N_ICON_GENERIC, N_PROGRAM_MENU);
    said(id, N_SETTING_OK);
    for (char *t = program_types_text ? strtok(program_types_text, " ,") : NULL; id >= 0 && t; t = strtok(NULL, " ,")) {
        if (*t == '.') t++;
        for (char *c = t; *c; c++)
            if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        n_result claimed = n_program_claim((uint32_t)id, t);
        if (claimed < 0) said(claimed, N_SETTING_OK);
    }
    if (id >= 0) {
        text_keep(&program_name_text, "");
        text_keep(&program_path_text, "");
        text_keep(&program_types_text, "");
    }
    programs_load();
}

static void programs_page(struct ui_msg *m) {
    size_t total = claim_total > 0 ? (size_t)claim_total : 0, group = 0;
    while (group < total && !strcmp(claim_list[group].type, N_PROGRAM_ANY_TYPE)) group++;
    uint32_t *ids = malloc((total + 1) * sizeof *ids);
    const char **names = malloc((total + 1) * sizeof *names);
    text_node(m, "文書の種類ごとの既定", "heading", 0);
    while (ids && names && group < total) {
        size_t end = type_end(group), n = type_candidates(group, end, ids);
        uint32_t selected = 0;
        for (size_t i = 0; i < n; i++) {
            names[i] = program_label(ids[i]);
            if (ids[i] == claim_list[group].current) selected = (uint32_t)i;
        }
        char heading[64];
        snprintf(heading, sizeof heading, ".%s", claim_list[group].type);
        ui_node(m, "card"); ui_num(m, "gap", 8); ui_str(m, "key", claim_list[group].type);
            ui_node(m, "row"); ui_num(m, "gap", 10);
                ui_node(m, "column"); ui_num(m, "gap", 4); ui_num(m, "grow", 1);
                    text_node(m, heading, "heading", 0);
                ui_end(m);
                act_at(ui_node(m, "segmented"), A_PROGRAM_CHOOSE, (uint32_t)group);
                ui_strs(m, "options", names, (int)n);
                ui_num(m, "selected", selected);
                ui_end(m);
            ui_end(m);
        ui_end(m);
        group = end;
    }
    free(ids);
    free(names);
    text_node(m, "登録したプログラム", "heading", 0);
    for (n_result i = 0; i < program_total; i++) {
        const struct n_program *p = &program_list[i];
        if (p->flags & N_PROGRAM_SYSTEM) continue;
        ui_node(m, "card"); ui_num(m, "gap", 8); ui_str(m, "key", p->path);
            ui_node(m, "row"); ui_num(m, "gap", 10);
                ui_node(m, "column"); ui_num(m, "gap", 4); ui_num(m, "grow", 1);
                    text_node(m, p->name, "heading", 0);
                    text_node(m, p->path, "caption", 0);
                ui_end(m);
                act_at(ui_node(m, "button"), A_PROGRAM_REMOVE, (uint32_t)i); ui_str(m, "text", "消す"); ui_end(m);
            ui_end(m);
        ui_end(m);
    }
    ui_node(m, "card"); ui_num(m, "gap", 10); ui_str(m, "key", "new program");
        text_node(m, "プログラムを足す", "heading", 0);
        act_at(ui_node(m, "field"), A_PROGRAM_NAME, 0);
        ui_str(m, "value", program_name_text ? program_name_text : ""); ui_str(m, "placeholder", "名前"); ui_str(m, "width", "fill");
        ui_end(m);
        act_at(ui_node(m, "field"), A_PROGRAM_PATH, 0);
        ui_str(m, "value", program_path_text ? program_path_text : ""); ui_str(m, "placeholder", "C:\\Programs の中のパス"); ui_str(m, "width", "fill");
        ui_end(m);
        act_at(ui_node(m, "field"), A_PROGRAM_TYPES, 0);
        ui_str(m, "value", program_types_text ? program_types_text : ""); ui_str(m, "placeholder", "開ける種類（空白で区切る）"); ui_str(m, "width", "fill");
        ui_end(m);
        act_at(ui_node(m, "button"), A_PROGRAM_ADD, 0); ui_str(m, "text", "足す"); ui_str(m, "variant", "primary"); ui_end(m);
    ui_end(m);
}

static void show(struct ui_session *ui) {
    const struct ui_theme *t = &ui_theme;
    view_number++;
    building = &sent[view_number % VIEWS];
    building->count = 0;
    struct ui_msg m;
    ui_view(&m, WINDOW);
    ui_node(&m, "row"); ui_num(&m, "gap", 0); ui_str(&m, "align", "stretch");
        ui_node(&m, "column"); ui_num(&m, "width", 216); ui_nums(&m, "padding", (double[]){22, 12}, 2); ui_num(&m, "gap", 4);
        ui_num(&m, "background", t->sidebar);
            ui_node(&m, "row"); ui_num(&m, "gap", 10); ui_nums(&m, "padding", (double[]){0, 10, 18, 10}, 4);
                ui_node(&m, "icon"); ui_str(&m, "icon", "settings"); ui_num(&m, "size", 22); ui_num(&m, "colour", t->accent); ui_end(&m);
                ui_node(&m, "text"); ui_str(&m, "text", "設定"); ui_str(&m, "style", "title"); ui_num(&m, "size", 19); ui_end(&m);
            ui_end(&m);
            for (uint32_t i = 0; i < P_COUNT; i++) {
                act_at(ui_node(&m, "item"), A_PAGE, i);
                ui_str(&m, "text", pages[i].label); ui_str(&m, "icon", pages[i].icon);
                ui_num(&m, "selected", i == page); ui_str(&m, "key", pages[i].label);
                ui_end(&m);
            }
        ui_end(&m);
        ui_node(&m, "column"); ui_num(&m, "grow", 1); ui_num(&m, "gap", 0);
            ui_node(&m, "scroll"); ui_num(&m, "grow", 1); ui_nums(&m, "padding", (double[]){26, 30}, 2); ui_num(&m, "gap", 14);
            ui_str(&m, "key", pages[page].label);
                text_node(&m, pages[page].label, "title", 0);
                if (page == P_SERVICES) services_page(&m);
                else if (page == P_USERS) users_page(&m);
                else if (page == P_PROGRAMS) programs_page(&m);
                else {
                    if (page == P_DISPLAY) screen_card(&m);
                    if (page == P_NETWORK) {
                        if (!link_view_count) text_node(&m, "網の装置がありません。", "caption", 0);
                        for (uint32_t i = 0; i < link_view_count; i++) link_card(&m, i);
                    }
                    if (page == P_AUDIO) {
                        char line[80];
                        snprintf(line, sizeof line, sound.device[0] ? "%s で鳴らします。" : "音の装置がありません。", sound.device);
                        text_node(&m, line, "caption", 0);
                    }
                    for (size_t i = 0; i < shown_count; i++)
                        if (shown[i].d.domain == pages[page].domain && !(shown[i].d.flags & N_SETTING_DECL_CUSTOM)) setting_card(&m, i);
                }
            ui_end(&m);
            ui_node(&m, "row"); ui_nums(&m, "padding", (double[]){12, 30}, 2); ui_num(&m, "background", t->sidebar);
                text_node(&m, status[0] ? status : "変更はすぐに効き、再起動の後も残ります。", "caption", status_bad ? t->warning : t->textDim);
            ui_end(&m);
        ui_end(&m);
    ui_end(&m);
    ui_send(ui, &m);
}

/* ---- 操作 ---- */
static void page_open(enum page p) {
    page = p;
    say("", false);
    if (p == P_SERVICES) services_load();
    if (p == P_USERS) accounts_load();
    if (p == P_PROGRAMS) programs_load();
    if (p == P_DISPLAY) screen_read();
    if (p == P_AUDIO) n_audio_output(&sound);
    if (p == P_NETWORK) network_load();
}

static void set_number(struct shown *s, int32_t v) {
    uint32_t reason = N_SETTING_OK;
    n_result r = n_setting_set_int(n_setting_at(s->d.domain, s->d.key), v, &reason);
    said(r, reason);
    if (r >= 0) s->number = v;
    shown_read(s);
}

static void submit_text(struct shown *s) {
    struct n_setting_id id = n_setting_at(s->d.domain, s->d.key);
    uint32_t reason = N_SETTING_OK;
    n_result r;
    switch (s->d.type) {
    case N_SETTING_STRING:
        r = n_setting_set(id, s->draft, strlen(s->draft), &reason);
        break;
    case N_SETTING_INT: {
        char *end;
        long v = strtol(s->draft, &end, 10);
        if (end == s->draft || *end) {
            say("数を入れてください。", true);
            return;
        }
        r = n_setting_set_int(id, (int32_t)v, &reason);
        break;
    }
    case N_SETTING_ENDPOINT: {
        struct n_sockaddr a;
        if (!endpoint_read(s->draft, &a)) {
            say("a.b.c.d の形で入れてください。", true);
            return;
        }
        r = n_setting_set(id, &a, sizeof a, &reason);
        break;
    }
    default:
        return;
    }
    said(r, reason);
    shown_read(s);
}

static const struct action *action_of(const struct ui_event *e) {
    const struct sent_view *v = &sent[e->view % VIEWS];
    for (size_t i = 0; i < v->count; i++)
        if (v->list[i].node == e->node) return &v->list[i];
    return NULL;
}

/* 一つの事象。窓を閉じるなら false。 */
static bool on_event(const struct ui_event *e) {
    if (e->type == UI_EVENT_CLOSE) return false;
    if (e->type == UI_EVENT_KEY) {
        if (e->flags & UI_UP) return true;
        if (e->text && !strcmp(e->text, "Escape")) return false;
        return true;
    }
    const struct action *a = action_of(e);
    if (!a) return true;
    struct shown *s = a->arg < shown_count ? &shown[a->arg] : NULL;
    switch (a->act) {
    case A_PAGE: page_open((enum page)(a->arg < P_COUNT ? a->arg : 0)); break;
    case A_BOOL: if (s && e->type == UI_EVENT_TOGGLE) set_number(s, e->a != 0); break;
    case A_ENUM: if (s && e->type == UI_EVENT_CHOOSE) set_number(s, (int32_t)e->a); break;
    case A_NUMBER:
        if (s && e->type == UI_EVENT_SLIDE) {
            s->number = (int32_t)(e->a + (e->a < 0 ? -0.5 : 0.5));
            set_number(s, s->number);
        }
        break;
    case A_TEXT:
        if (!s) break;
        if (e->type == UI_EVENT_TEXT) strlcpy(s->draft, e->text ? e->text : "", s->draft_cap);
        else if (e->type == UI_EVENT_SUBMIT) submit_text(s);
        break;
    case A_SCREEN:
        if (e->type == UI_EVENT_CHOOSE) {
            screen_index = (uint32_t)e->a;
            screen_read();
        }
        break;
    case A_SIZE: if (a->arg < screen.offered.count && e->type == UI_EVENT_CLICK) size_try(&screen.offered.modes[a->arg]); break;
    case A_RATE:
    case A_FORMAT: if (a->arg < screen.offered.count && e->type == UI_EVENT_CLICK) mode_try(&screen.offered.modes[a->arg]); break;
    case A_SCALE:
        if (e->type == UI_EVENT_TEXT) strlcpy(scale_draft, e->text ? e->text : "", sizeof scale_draft);
        else if (e->type == UI_EVENT_SUBMIT) scale_try();
        break;
    case A_PLACE: if (e->type == UI_EVENT_CLICK) screen_place(a->arg); break;
    case A_MAIN: if (e->type == UI_EVENT_CLICK) screen_make_main(); break;
    case A_KEEP: {
        uint32_t reason = N_SETTING_OK;
        n_result r = n_setting_commit(trial_id, &reason);
        said(r, reason);
        trial_ends_ns = 0;
        break;
    }
    case A_BACK: {
        uint32_t reason = N_SETTING_OK;
        n_result r = n_setting_revert(trial_id, &reason);
        said(r, reason);
        trial_ends_ns = 0;
        break;
    }
    case A_SERVICE_RESTART: service_do(a->arg, N_SERVICES_RESTART); break;
    case A_SERVICE_STOP: service_do(a->arg, N_SERVICES_STOP); break;
    case A_USER_CLASS: if (e->type == UI_EVENT_CHOOSE) account_class(a->arg, e->a != 0 ? N_CLASS_ADMIN : N_CLASS_USER); break;
    case A_NEW_NAME: if (e->type == UI_EVENT_TEXT) strlcpy(new_name, e->text ? e->text : "", sizeof new_name); break;
    case A_NEW_PASSWORD: if (e->type == UI_EVENT_TEXT) strlcpy(new_password, e->text ? e->text : "", sizeof new_password); break;
    case A_NEW_ADMIN: if (e->type == UI_EVENT_TOGGLE) new_admin = e->a != 0; break;
    case A_NEW_CREATE: account_create(); break;
    case A_PROGRAM_CHOOSE: if (e->type == UI_EVENT_CHOOSE) program_type_chosen(a->arg, (uint32_t)e->a); break;
    case A_PROGRAM_REMOVE:
        if (a->arg < (uint32_t)(program_total > 0 ? program_total : 0)) {
            said(n_program_remove(program_list[a->arg].id), N_SETTING_OK);
            programs_load();
        }
        break;
    case A_PROGRAM_NAME: if (e->type == UI_EVENT_TEXT) text_keep(&program_name_text, e->text); break;
    case A_PROGRAM_PATH: if (e->type == UI_EVENT_TEXT) text_keep(&program_path_text, e->text); break;
    case A_PROGRAM_TYPES: if (e->type == UI_EVENT_TEXT) text_keep(&program_types_text, e->text); break;
    case A_PROGRAM_ADD: program_add(); break;
    case A_LINK_IP:
    case A_LINK_MASK:
    case A_LINK_GATEWAY:
        if (a->arg < link_view_count) {
            struct link_view *v = &link_views[a->arg];
            char *draft = a->act == A_LINK_IP ? v->ip : a->act == A_LINK_MASK ? v->mask : v->gateway;
            if (e->type == UI_EVENT_TEXT) strlcpy(draft, e->text ? e->text : "", sizeof v->ip);
            else if (e->type == UI_EVENT_SUBMIT) link_fixed_apply(v);
        }
        break;
    case A_LINK_FIXED: if (a->arg < link_view_count && e->type == UI_EVENT_CLICK) link_fixed_apply(&link_views[a->arg]); break;
    case A_LINK_DHCP: if (a->arg < link_view_count && e->type == UI_EVENT_CLICK) link_dhcp_apply(&link_views[a->arg]); break;
    }
    return true;
}

int main(void) {
    struct ui_session *ui = ui_session_new();
    if (!ui || ui_window(ui, WINDOW, 880, 640, WIN_RESIZABLE | WIN_CENTER, "設定", NULL, NULL) < 0) return 1;
    n_handle queue = ui_session_queue(ui);
    n_handle settings_watch = n_setting_watch(N_SETTING_ANY, N_SETTING_ANY);
    if (settings_watch >= 0) n_events_watch(queue, settings_watch, N_EVENT_RECEIVABLE, TOKEN_SETTINGS);
    n_handle screen_watch = n_screen_watch();
    if (screen_watch >= 0) n_events_watch(queue, screen_watch, N_EVENT_RECEIVABLE, TOKEN_SCREEN);
    n_handle sound_watch = n_audio_output_watch();
    if (sound_watch >= 0) n_events_watch(queue, sound_watch, N_EVENT_RECEIVABLE, TOKEN_SOUND);
    services_watch_open(queue);
    n_handle network_watch = n_net_watch();
    if (network_watch >= 0) n_events_watch(queue, network_watch, N_EVENT_RECEIVABLE, TOKEN_NETWORK);
    shown_load();
    page_open(P_AUDIO);
    show(ui);
    for (bool running = true; running && ui_session_open(ui);) {
        int timeout = ui_session_timeout(ui);
        uint64_t deadline = timeout < 0 ? N_CLOCK_NEVER : n_clock_after((uint64_t)timeout * N_NS_PER_MS);
        if (trial_ends_ns) { /* 秒読みを見せる */
            uint64_t tick = n_clock_after(1000 * N_NS_PER_MS);
            if (tick < deadline) deadline = tick;
        }
        struct n_event events[16];
        n_result n = n_events_wait(queue, events, sizeof events / sizeof *events, deadline);
        if (n < 0) break;
        bool redraw = trial_ends_ns != 0;
        for (n_result i = 0; i < n; i++) {
            switch (events[i].token) {
            case TOKEN_SETTINGS: {
                struct n_settings_changed c;
                while (n_setting_watch_take(settings_watch, &c) >= 0) {
                    shown_changed(&c);
                    if (c.id.domain == N_DOMAIN_DISPLAY) {
                        if (c.id.key == N_DISPLAY_MODE || c.id.key == N_DISPLAY_SCALE) {
                            if (c.what == N_SETTING_ON_TRIAL) {
                                trial_id = c.id;
                                trial_ends_ns = n_clock_after((uint64_t)c.remaining_ms * N_NS_PER_MS);
                            } else {
                                if (c.what == N_SETTING_REVERTED && trial_ends_ns) say("前へ戻りました。", false);
                                trial_ends_ns = 0;
                            }
                        }
                        screen_read();
                    }
                }
                redraw = true;
                break;
            }
            case TOKEN_SCREEN:
                n_watch_take(screen_watch);
                screen_read();
                redraw = true;
                break;
            case TOKEN_NETWORK:
                n_watch_take(network_watch);
                if (page == P_NETWORK) network_load();
                redraw = true;
                break;
            case TOKEN_SOUND:
                n_watch_take(sound_watch);
                n_audio_output(&sound);
                redraw = true;
                break;
            case TOKEN_SERVICES: {
                struct n_service_status st;
                while (n_channel_receive(services_watch, &st, sizeof st, 0, NULL) >= 0) {}
                if (page == P_SERVICES) services_load();
                redraw = true;
                break;
            }
            default: break;
            }
        }
        ui_session_pump(ui);
        struct ui_event e;
        char text[N_SETTING_VALUE_MAX > 4096 ? 4096 : N_SETTING_VALUE_MAX];
        while (running && ui_next_event(ui, &e, text, sizeof text)) {
            running = on_event(&e);
            redraw = true;
        }
        if (running && redraw) show(ui);
    }
    ui_session_free(ui);
    return 0;
}
