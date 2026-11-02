/* サービス display：画面ごとのモードの選択、配置、主画面、倍率の持ち主。カーネルは画面とモードの一覧、EDID の身元、
   実効の報告、適用の機構だけを持ち、選ばない。

   事象は三つ：カーネルの画面の見張り（モードの申し出、ドライバーの断り、大きさの変化）、設定のサービスの適用の依頼、
   管理者の止める知らせ。一つのスレッドが事象の行列で眠り、何も変わらなければ回らない。画面の数を n として、見直しは
   変化ごとに一度で n 回読む。プロセスや接続の数には関わらない。

   値は設定のサービスが保存する。モニターは EDID の身元の番号（n_screen_device）で引く。モードの試しの期限も設定の
   サービスが数え、ここは適用の依頼に従うだけ。ドライバーが試しのモードを出せなければ、ここが試しを取り消す。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "display.h"
#include "nocturne.h"
#include "nocturne_services.h"
#include "nocturne_settings.h"

#define MODE_DECL(k, tp, sc, how, nm, lb, ab, lo, hi, def) \
    {.domain = N_DOMAIN_DISPLAY, .key = k, .type = tp, .scope = sc, .apply = how, .flags = N_SETTING_DECL_CUSTOM, \
     .min = lo, .max = hi, .fallback = def, .name = nm, .label = lb, .about = ab}
static const struct n_setting_decl declarations[] = {
    MODE_DECL(N_DISPLAY_MODE, N_SETTING_MONITOR_MODE, N_SCOPE_DEVICE, N_APPLY_TRIAL, "display.mode", "解像度と拍",
              "モニターごとの解像度とリフレッシュレートです。", 0, 0, 0),
    MODE_DECL(N_DISPLAY_PLACE, N_SETTING_MONITOR_PLACE, N_SCOPE_DEVICE, N_APPLY_NOW, "display.place", "配置",
              "モニターごとのデスクトップの中の位置です。", 0, 0, 0),
    /* 試しで変える：どの値でも、見えなくなれば期限で戻る。下限：100 未満はモニターの画素より細かく描くことになる。 */
    MODE_DECL(N_DISPLAY_SCALE, N_SETTING_INT, N_SCOPE_DEVICE, N_APPLY_TRIAL, "display.scale", "倍率",
              "モニターごとの表示の大きさ（百分率）です。", 100, INT32_MAX, 100),
    MODE_DECL(N_DISPLAY_PRIMARY, N_SETTING_DEVICE, N_SCOPE_MACHINE, N_APPLY_NOW, "display.primary", "主画面",
              "新しい窓が開く画面です。", 0, 0, 0),
};

/* 一つの画面。now は最後に読んだ報告。 */
struct screen {
    struct screen_modes *now;
    uint64_t device;      /* 0：まだ */
    uint64_t offer_seen;
    uint64_t refused_seen;
    struct n_screen_mode *refused;
    size_t nrefused, refused_room;
    struct n_setting_monitor_place place;
    int32_t scale;
    int32_t x, y;         /* 配置の計算の結果 */
    bool done;
};

static n_handle cap = -1;
static struct screen *screens;
static size_t nscreens;
static uint64_t primary; /* 主画面のモニター。0：画面 0 */

static void complain(const char *what, n_result r) {
    printf("display: %s (%s)\n", what, n_result_text(r));
    fflush(stdout);
}

static struct n_setting_id id_of(uint32_t key, uint64_t device) {
    struct n_setting_id id = n_setting_at(N_DOMAIN_DISPLAY, key);
    id.device = device;
    return id;
}

/* ---- モード ---- */
static bool same(const struct n_screen_mode *a, const struct n_screen_mode *b) {
    return a->width == b->width && a->height == b->height && a->refresh_mhz == b->refresh_mhz &&
           (a->format == b->format || !a->format || !b->format);
}

static bool is_refused(const struct screen *s, const struct n_screen_mode *m) {
    for (size_t i = 0; i < s->nrefused; i++)
        if (same(&s->refused[i], m)) return true;
    return false;
}

static void refuse(struct screen *s, const struct n_screen_mode *m) {
    if (is_refused(s, m)) return;
    if (s->nrefused == s->refused_room) {
        size_t room = s->refused_room ? s->refused_room * 2 : 4;
        struct n_screen_mode *grown = realloc(s->refused, room * sizeof *grown);
        if (!grown) return;
        s->refused = grown;
        s->refused_room = room;
    }
    s->refused[s->nrefused++] = *m;
}

static void unrefuse(struct screen *s, const struct n_screen_mode *m) {
    for (size_t i = 0; i < s->nrefused; i++)
        if (same(&s->refused[i], m)) {
            s->refused[i] = s->refused[--s->nrefused];
            return;
        }
}

static uint32_t distance(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

/* 申し出の中で断られていない width x height のうち、拍が mhz に一番近いもの（同じなら高い方）。width 0 はどの大きさでも
   一番大きいもの。 */
static const struct n_screen_mode *nearest(const struct screen *s, uint32_t width, uint32_t height, uint32_t mhz) {
    const struct n_screen_modes *o = &s->now->offered;
    const struct n_screen_mode *best = NULL;
    for (uint32_t i = 0; i < o->count; i++) {
        const struct n_screen_mode *m = &o->modes[i];
        if ((width && (m->width != width || m->height != height)) || is_refused(s, m)) continue;
        if (best) {
            uint64_t area = (uint64_t)m->width * m->height, best_area = (uint64_t)best->width * best->height;
            if (!width && area != best_area) {
                if (area < best_area) continue;
            } else {
                uint32_t d = distance(m->refresh_mhz, mhz), bd = distance(best->refresh_mhz, mhz);
                if (d > bd || (d == bd && m->refresh_mhz <= best->refresh_mhz)) continue;
            }
        }
        best = m;
    }
    return best;
}

/* 画面に出すモード：利用者の選択、モニターの好み、好みの大きさの別の拍、一番大きい大きさで好みに近い拍の順。
   全部断られていれば NULL。 */
static const struct n_screen_mode *mode_for(const struct screen *s, const struct n_setting_monitor_mode *chosen) {
    const struct n_screen_modes *o = &s->now->offered;
    const struct n_screen_mode *preferred = NULL;
    for (uint32_t i = 0; i < o->count; i++) {
        const struct n_screen_mode *m = &o->modes[i];
        struct n_screen_mode wanted = {chosen->width, chosen->height, chosen->refresh_mhz, 0, 0, chosen->format};
        if (chosen->width && same(m, &wanted) && !is_refused(s, m)) return m;
        if (!preferred && (m->flags & N_SCREEN_MODE_PREFERRED)) preferred = m;
    }
    uint32_t mhz = preferred ? preferred->refresh_mhz : UINT32_MAX; /* 好みが無ければ一番高い拍 */
    const struct n_screen_mode *m = preferred ? nearest(s, preferred->width, preferred->height, mhz) : NULL;
    return m ? m : nearest(s, 0, 0, mhz);
}

static void mode_ask(uint32_t index, const struct n_screen_mode *m) {
    n_result r = n_screen_mode_apply(cap, index, m);
    if (r < 0) complain("the mode was not taken", r);
}

/* 保存された選択（試しの中ならその値）で、申し出に答える。 */
static void mode_answer_offer(uint32_t index) {
    struct screen *s = &screens[index];
    struct n_setting_monitor_mode chosen = {0};
    n_setting_get(id_of(N_DISPLAY_MODE, s->device), &chosen, sizeof chosen);
    const struct n_screen_mode *m = mode_for(s, &chosen);
    if (m) mode_ask(index, m);
}

static void mode_look(uint32_t index) {
    struct screen *s = &screens[index];
    const struct screen_modes *now = s->now;
    if (!now->offered.count) {
        s->offer_seen = now->offer_serial;
        s->nrefused = 0;
        return;
    }
    if (!now->refused_serial) s->nrefused = 0; /* いま来たモニターで、断りはまだ無い */
    if (now->refused_serial == now->wish_serial && now->refused_serial != s->refused_seen) {
        s->refused_seen = now->refused_serial;
        refuse(s, &now->wanted);
        /* 試しのモードだったら前のモードへ戻る：戻す適用が届く。試しでなければ次のものへ。 */
        if (n_setting_revert(id_of(N_DISPLAY_MODE, s->device), NULL) >= 0) return;
        struct n_setting_monitor_mode chosen = {0};
        n_setting_get(id_of(N_DISPLAY_MODE, s->device), &chosen, sizeof chosen);
        const struct n_screen_mode *m = mode_for(s, &chosen);
        if (!m && !is_refused(s, &now->offered.shown)) m = &now->offered.shown;
        if (m) mode_ask(index, m); /* どれも出せなければ、これ以上望まない */
        return;
    }
    if (now->offer_serial == s->offer_seen) return;
    s->offer_seen = now->offer_serial;
    if (!now->offered.done) mode_answer_offer(index); /* 今のドライバーは次の望みで動く */
}

/* ---- 配置 ---- */
static bool screen_on(const struct screen *s) { return s->now && s->now->on && s->now->width && s->now->height; }

static bool overlaps(const struct screen *a, int32_t x, int32_t y, const struct screen *s) {
    return x < a->x + (int32_t)a->now->width && a->x < x + (int32_t)s->now->width && y < a->y + (int32_t)a->now->height &&
           a->y < y + (int32_t)s->now->height;
}

/* 縁を分け合う：ポインターが渡れる。 */
static bool touches(const struct screen *a, int32_t x, int32_t y, const struct screen *s) {
    int32_t ax2 = a->x + (int32_t)a->now->width, ay2 = a->y + (int32_t)a->now->height;
    int32_t x2 = x + (int32_t)s->now->width, y2 = y + (int32_t)s->now->height;
    bool across = (x == ax2 || x2 == a->x) && y < ay2 && a->y < y2;
    bool along = (y == ay2 || y2 == a->y) && x < ax2 && a->x < x2;
    return across || along;
}

/* s を望みの位置に近く置く：置き済みの画面に重ねず、どれかと縁を分け合う。 */
static void place_one(struct screen *s, int32_t x, int32_t y) {
    bool any = false, touching = false;
    const struct screen *closest = NULL;
    int64_t closest_distance = INT64_MAX;
    for (size_t i = 0; i < nscreens; i++) {
        const struct screen *a = &screens[i];
        if (!a->done) continue;
        any = true;
        touching = touching || touches(a, x, y, s);
        int64_t dx = (int64_t)x + (int64_t)s->now->width / 2 - a->x - (int64_t)a->now->width / 2;
        int64_t dy = (int64_t)y + (int64_t)s->now->height / 2 - a->y - (int64_t)a->now->height / 2;
        int64_t d = dx * dx + dy * dy;
        if (d < closest_distance) {
            closest_distance = d;
            closest = a;
        }
    }
    if (any && !touching) {
        const struct screen *a = closest;
        int32_t ax2 = a->x + (int32_t)a->now->width, ay2 = a->y + (int32_t)a->now->height;
        bool left = x + (int32_t)s->now->width < a->x, right = x > ax2;
        bool above = y + (int32_t)s->now->height < a->y, below = y > ay2;
        if (right) x = ax2;
        else if (left) x = a->x - (int32_t)s->now->width;
        if (below) y = ay2;
        else if (above) y = a->y - (int32_t)s->now->height;
        if ((left || right) && (above || below)) y = a->y; /* 角だけでは渡れない */
    }
    for (bool moved = true; moved;) {
        moved = false;
        for (size_t i = 0; i < nscreens; i++) {
            const struct screen *a = &screens[i];
            if (a->done && overlaps(a, x, y, s)) {
                x = a->x + (int32_t)a->now->width;
                moved = true;
            }
        }
    }
    s->x = x;
    s->y = y;
    s->done = true;
}

static size_t main_screen(void) {
    for (size_t i = 0; primary && i < nscreens; i++)
        if (screens[i].device == primary && screen_on(&screens[i])) return i;
    return 0;
}

/* 位置を決め、原点から始まるように全体を動かし、実効の報告と違う画面だけカーネルへ渡す。 */
static void arrange(void) {
    size_t main = main_screen();
    int32_t right = 0, least_x = INT32_MAX, least_y = INT32_MAX;
    for (size_t i = 0; i < nscreens; i++) screens[i].done = false;
    /* 主画面、置かれた画面、自動の画面の順に */
    for (int pass = 0; pass < 3; pass++)
        for (size_t i = 0; i < nscreens; i++) {
            struct screen *s = &screens[i];
            if (!screen_on(s) || s->done) continue;
            if (pass == 0 && i != main) continue;
            if (pass == 1 && !s->place.placed) continue;
            place_one(s, s->place.placed ? s->place.x : right, s->place.placed ? s->place.y : 0);
            if (s->x + (int32_t)s->now->width > right) right = s->x + (int32_t)s->now->width;
        }
    for (size_t i = 0; i < nscreens; i++)
        if (screens[i].done) {
            if (screens[i].x < least_x) least_x = screens[i].x;
            if (screens[i].y < least_y) least_y = screens[i].y;
        }
    for (size_t i = 0; i < nscreens; i++) {
        struct screen *s = &screens[i];
        if (!s->done) continue;
        struct n_screen_arrange a = {.x = s->x - least_x, .y = s->y - least_y, .flags = N_ARRANGE_PLACED,
                                     .scale_percent = (uint32_t)s->scale};
        if (i == main) a.flags |= N_ARRANGE_PRIMARY;
        const struct screen_modes *now = s->now;
        if (now->placed && now->x == a.x && now->y == a.y && now->scale_percent == a.scale_percent &&
            (i != main || now->primary))
            continue;
        n_result r = n_screen_arrange(cap, (uint32_t)i, &a);
        if (r < 0) complain("the arrangement was not taken", r);
    }
}

/* ---- 見直し ---- */
static bool screen_read(uint32_t index) {
    if (index >= nscreens) {
        struct screen *grown = realloc(screens, (index + 1) * sizeof *grown);
        if (!grown) return false;
        screens = grown;
        memset(&screens[nscreens], 0, (index + 1 - nscreens) * sizeof *grown);
        nscreens = index + 1;
    }
    struct screen *s = &screens[index];
    if (!s->now && !(s->now = malloc(sizeof *s->now))) return false;
    return n_screen_modes_get(index, s->now) >= 0;
}

/* モニターが変わった：そのモニターの値を読み直す。 */
static void monitor_look(uint32_t index) {
    struct screen *s = &screens[index];
    uint64_t device;
    if (n_screen_device(index, &device) < 0 || device == s->device) return;
    s->device = device;
    s->nrefused = 0;
    s->refused_seen = 0;
    struct n_setting_monitor_place place = {0};
    n_setting_get(id_of(N_DISPLAY_PLACE, device), &place, sizeof place);
    s->place = place;
    int32_t scale = (int32_t)declarations[2].fallback;
    n_setting_get_int(id_of(N_DISPLAY_SCALE, device), &scale);
    s->scale = scale;
    /* 同じ申し出にも答え直す：前のモニターの望みが残っている */
    s->offer_seen = 0;
}

static void look(void) {
    uint32_t count = 1;
    for (uint32_t i = 0; i < count; i++) {
        if (!screen_read(i)) {
            complain("a screen could not be read", -N_ERR_NO_MEMORY);
            return;
        }
        count = screens[i].now->screens;
        monitor_look(i);
        mode_look(i);
    }
    for (size_t i = count; i < nscreens; i++) { /* もう無い画面 */
        if (screens[i].now) screens[i].now->on = 0;
        screens[i].device = 0;
    }
    /* 上のモードの依頼で報告が変わっていても、次の見張りでもう一度来る */
    arrange();
}

/* ---- 適用の依頼 ---- */
static n_result on_apply(const struct n_setting_id *id, const void *value, uint32_t length) {
    union {
        struct n_setting_monitor_mode mode;
        struct n_setting_monitor_place place;
        int32_t scale;
        uint64_t device;
    } v;
    if (length > sizeof v) return -N_ERR_INVALID;
    memcpy(&v, value, length);
    if (id->key == N_DISPLAY_PRIMARY) {
        if (length != sizeof v.device) return -N_ERR_INVALID;
        primary = v.device;
        arrange();
        return 0;
    }
    struct screen *s = NULL;
    uint32_t index = 0;
    for (size_t i = 0; i < nscreens && !s; i++)
        if (screens[i].device == id->device) {
            s = &screens[i];
            index = (uint32_t)i;
        }
    if (!s) return 0; /* いないモニターの値：つながったときに読む */
    switch (id->key) {
    case N_DISPLAY_MODE: {
        if (length != sizeof v.mode) return -N_ERR_INVALID;
        if (!s->now->offered.count) return v.mode.width ? -N_ERR_NOT_FOUND : 0;
        struct n_screen_mode wanted = {v.mode.width, v.mode.height, v.mode.refresh_mhz, 0, 0, v.mode.format};
        if (v.mode.width) unrefuse(s, &wanted);
        const struct n_screen_mode *m = mode_for(s, &v.mode);
        if (v.mode.width && (!m || !same(m, &wanted))) {
            /* 申し出に無いモード：選択として残し、出せるようになるまで今のまま */
            return 0;
        }
        if (m) mode_ask(index, m);
        return 0;
    }
    case N_DISPLAY_PLACE:
        if (length != sizeof v.place) return -N_ERR_INVALID;
        s->place = v.place;
        arrange();
        return 0;
    case N_DISPLAY_SCALE:
        if (length != sizeof v.scale) return -N_ERR_INVALID;
        s->scale = v.scale;
        arrange();
        return 0;
    default:
        return -N_ERR_NOT_FOUND;
    }
}

enum { TOKEN_CONTROL = 1, TOKEN_SCREEN, TOKEN_OWNER };

int main(void) {
    cap = n_cap_find(N_CAP_DISPLAY);
    n_handle queue = n_events_create();
    n_handle owner = n_setting_owner_open(declarations, sizeof declarations / sizeof *declarations, NULL);
    if (cap < 0 || queue < 0 || owner < 0) {
        complain("cannot start", cap < 0 ? cap : queue < 0 ? queue : owner);
        return 1;
    }
    n_handle watch = n_screen_watch(); /* デスクトップが無ければ -N_ERR_UNSUPPORTED：起動の時の画面のまま */
    n_events_watch(queue, N_SERVICE_HANDLE_CONTROL, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, TOKEN_CONTROL);
    n_events_watch(queue, owner, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, TOKEN_OWNER);
    if (watch >= 0) n_events_watch(queue, watch, N_EVENT_RECEIVABLE, TOKEN_SCREEN);
    uint64_t chosen_primary = 0;
    if (n_setting_get(n_setting_at(N_DOMAIN_DISPLAY, N_DISPLAY_PRIMARY), &chosen_primary, sizeof chosen_primary) >= 0)
        primary = chosen_primary;
    if (watch >= 0) look();
    n_service_ready();
    for (;;) {
        struct n_event events[8];
        n_result n = n_events_wait(queue, events, sizeof events / sizeof *events, N_CLOCK_NEVER);
        if (n < 0) return 1;
        bool again = false;
        for (n_result i = 0; i < n; i++) {
            switch (events[i].token) {
            case TOKEN_CONTROL: {
                n_handle end = n_inlet_accept(N_SERVICE_HANDLE_CONTROL, 0);
                if (end == -N_ERR_CLOSED) return 0;
                if (end >= 0) n_handle_close(end); /* 誰の接続も受けない */
                break;
            }
            case TOKEN_SCREEN:
                n_watch_take(watch);
                again = true;
                break;
            case TOKEN_OWNER:
                for (;;) {
                    struct n_setting_id id;
                    union {
                        struct n_setting_monitor_mode mode;
                        struct n_setting_monitor_place place;
                        uint64_t device;
                    } value;
                    uint32_t length, serial;
                    n_result r = n_setting_owner_take(owner, &id, &value, sizeof value, &length, &serial);
                    if (r == -N_ERR_NOT_READY) break;
                    if (r < 0) return 1; /* 設定のサービスが落ちた：起こし直されて宣言し直す */
                    n_setting_owner_answer(owner, serial, id.domain == N_DOMAIN_DISPLAY ? on_apply(&id, &value, length) : -N_ERR_NOT_FOUND);
                }
                break;
            default: break;
            }
        }
        if (again) look();
    }
}
