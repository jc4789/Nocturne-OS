/* 機械のサービス：電源と画面の読取りの能力の持ち主で、電源の方針の持ち主。能力をアプリへ配らず、依頼をここで受ける。
   依頼の相手はセッションの中のプロセスか、N_CLASS_ADMIN 以上。

   カーネルは事象を知らせ（電源ボタン、蓋、起床、ホストの依頼）、頼まれたことを実行するだけ。何を意味するかはここが
   設定（N_DOMAIN_POWER の鍵）で決める：ボタンと蓋と放置に何で答えるか、眠りと休止を許すか、窓が閉じるのをどれだけ
   待つか。

   一つのスレッドが事象の行列で眠る。回るのは事象か期限のときだけ：放置の期限は最後の入力から一つの時計で、設定が 0 なら
   持たない。窓の見張りは窓の閉じるのを待つ間だけ開く。接続はスレッドを持たず、数百あっても待つ間は行列の項目で、依頼
   一通の仕事はその接続だけに触れる。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "nocturne_services.h"
#include "nocturne_settings.h"

/* 人の目のための間。ファームウェアや装置の値ではなく、シェルが一度描いて読まれるだけの時間。 */
#define TELL_NS (200 * N_NS_PER_MS)            /* 止まると言ってからアプリを止めるまで */
#define CLOSING_TOLD_NS (1000 * N_NS_PER_MS)   /* これより長く待つなら残りの窓を知らせる */
#define SECONDS_NS (1000 * N_NS_PER_MS)

static n_handle power_cap, screen_cap, queue, owner;

/* ---- 設定 ---- */
#define ANSWERS "何もしない\0眠る\0休止する\0電源を切る\0"
#define ENUM_DECL(k, nm, lb, ab, def) \
    {.domain = N_DOMAIN_POWER, .key = k, .type = N_SETTING_ENUM, .scope = N_SCOPE_MACHINE, .apply = N_APPLY_NOW, \
     .min = 0, .max = 3, .fallback = def, .name = nm, .label = lb, .about = ab, .options = ANSWERS}
#define BOOL_DECL(k, nm, lb, ab, def) \
    {.domain = N_DOMAIN_POWER, .key = k, .type = N_SETTING_BOOL, .scope = N_SCOPE_MACHINE, .apply = N_APPLY_NOW, \
     .min = 0, .max = 1, .fallback = def, .name = nm, .label = lb, .about = ab}
#define TIME_DECL(k, nm, lb, ab, def) \
    {.domain = N_DOMAIN_POWER, .key = k, .type = N_SETTING_INT, .scope = N_SCOPE_MACHINE, .apply = N_APPLY_NOW, \
     .min = 0, .max = INT32_MAX, .fallback = def, .name = nm, .label = lb, .about = ab}
static const struct n_setting_decl declarations[] = {
    ENUM_DECL(N_POWER_BUTTON, "power.button", "電源ボタン", "電源ボタンを押したときにすることです。", N_POWER_ANSWER_OFF),
    ENUM_DECL(N_POWER_LID, "power.lid", "蓋を閉じたとき", "蓋のある機械で、蓋を閉じたときにすることです。", N_POWER_ANSWER_SLEEP),
    TIME_DECL(N_POWER_IDLE_MINUTES, "power.idle-minutes", "放置の時間（分）", "キーとポインターの入力がこの分数ないと下の動作をします。0 はしません。", 0),
    ENUM_DECL(N_POWER_IDLE_ANSWER, "power.idle-answer", "放置のとき", "放置の時間を超えたときにすることです。", N_POWER_ANSWER_SLEEP),
    BOOL_DECL(N_POWER_ALLOW_SLEEP, "power.allow-sleep", "眠りを許す", "切ると、この機械は眠りません。メニューにも出ません。", 1),
    BOOL_DECL(N_POWER_ALLOW_HIBERNATE, "power.allow-hibernate", "休止を許す", "切ると、この機械は休止しません。メニューにも出ません。", 1),
    TIME_DECL(N_POWER_CLOSE_SECONDS, "power.close-seconds", "窓が閉じるのを待つ秒数", "電源を切るとき、窓が一つも閉じないままこの秒数が過ぎると取りやめます。", 15),
    TIME_DECL(N_POWER_AGAIN_SECONDS, "power.again-seconds", "もう一度で閉じない窓を閉じる秒数", "取りやめからこの秒数の間にもう一度選ぶと、窓を待ち切って電源を切ります。", 60),
};
#define POLICY_KEYS (sizeof declarations / sizeof *declarations)

/* 宣言の順に今の値。 */
static int32_t policy[POLICY_KEYS];

static int32_t policy_of(uint32_t key) {
    for (size_t i = 0; i < POLICY_KEYS; i++)
        if (declarations[i].key == key) return policy[i];
    return 0;
}

/* ---- 呼び手 ---- */
struct client {
    struct client *next;
    n_handle channel;
    bool allowed, closed, waiting; /* waiting：電源の変更の結果を待っている（止まれば答えない） */
};
static struct client *clients, *dead;

static void client_reply(struct client *c, const struct n_machine_reply *reply, n_handle attached);

static void client_close(struct client *c) {
    if (c->closed) return;
    c->closed = true;
    n_events_unwatch(queue, (uint64_t)(uintptr_t)c);
    n_handle_close(c->channel);
    for (struct client **p = &clients; *p; p = &(*p)->next)
        if (*p == c) {
            *p = c->next;
            break;
        }
    c->next = dead;
    dead = c;
}

/* ---- 段取り ----
   電源を切る変更を一度に一つ進める。 */
enum { TOKEN_CONTROL = 1, TOKEN_EVENTS, TOKEN_OWNER, TOKEN_WINDOWS };

static struct {
    bool on;
    bool forced;     /* 待ち切れば窓が残っても止まる */
    bool told_forced;
    uint32_t action;
    uint64_t start, progress;
    n_result left;
    n_handle watch;
    char told[sizeof ((struct n_power_state *)0)->detail];
} closing;
static uint64_t force_until; /* 取りやめからこの時刻まで、もう一度選ぶと待ち切る */

static void say(uint32_t stage, uint32_t action, const char *title, bool forceable) {
    struct n_power_state s = {.stage = stage, .action = action, .forceable = forceable};
    if (title) strlcpy(s.detail, title, sizeof s.detail);
    n_power_say(power_cap, &s);
}

static void closing_end(void) {
    closing.on = false;
    n_events_unwatch(queue, TOKEN_WINDOWS);
    n_handle_close(closing.watch);
}

/* 変更の結果を待つ呼び手に、変更が起きなかったと答える。 */
static void waiters_answer(n_result result) {
    struct n_machine_reply reply = {.result = result};
    for (struct client *c = clients, *next; c; c = next) {
        next = c->next;
        if (c->waiting) {
            c->waiting = false;
            client_reply(c, &reply, -1);
        }
    }
}

static void power_stop(uint32_t action) {
    say(N_POWER_STOPPING, action, NULL, false);
    n_sleep_until(n_clock_after(TELL_NS));
    n_result r = n_power_request(power_cap, action); /* 戻らない */
    printf("machine: the power did not change (%s)\n", n_result_text(r));
    fflush(stdout);
    waiters_answer(r);
}

/* 窓の閉じ方を見る。 */
static void closing_look(void) {
    if (!closing.on) return;
    char title[sizeof closing.told] = "";
    n_result left = n_power_windows(power_cap, false, title, sizeof title);
    uint64_t now = n_clock_monotonic();
    if (left <= 0) {
        uint32_t action = closing.action;
        closing_end();
        power_stop(action);
        return;
    }
    if (left < closing.left) {
        closing.left = left;
        closing.progress = now;
    }
    uint64_t wait = (uint64_t)policy_of(N_POWER_CLOSE_SECONDS) * SECONDS_NS;
    if (closing.forced ? now - closing.start >= wait : now - closing.progress >= wait) {
        uint32_t action = closing.action;
        bool forced = closing.forced;
        closing_end();
        if (forced) {
            power_stop(action);
            return;
        }
        say(N_POWER_CANCELLED, action, title, false);
        printf("machine: cancelled, \"%s\" kept its window\n", title);
        fflush(stdout);
        force_until = now + (uint64_t)policy_of(N_POWER_AGAIN_SECONDS) * SECONDS_NS;
        waiters_answer(-N_ERR_BUSY);
        return;
    }
    if (now - closing.start >= CLOSING_TOLD_NS && (strcmp(title, closing.told) || closing.told_forced != closing.forced)) {
        closing.told_forced = closing.forced;
        say(N_POWER_CLOSING, closing.action, title, !closing.forced);
        strlcpy(closing.told, title, sizeof closing.told);
    }
}

static uint64_t closing_deadline(void) {
    if (!closing.on) return N_CLOCK_NEVER;
    uint64_t wait = (uint64_t)policy_of(N_POWER_CLOSE_SECONDS) * SECONDS_NS;
    uint64_t end = (closing.forced ? closing.start : closing.progress) + wait;
    uint64_t tell = closing.start + CLOSING_TOLD_NS;
    return n_clock_monotonic() < tell && tell < end ? tell : end;
}

/* 電源の変更を頼む。chosen は利用者の選択：窓を残して取りやめられ、もう一度で待ち切る。 */
static n_result power_ask(uint32_t action, bool chosen) {
    if (action == POWER_SLEEP || action == POWER_HIBERNATE) {
        if (!policy_of(action == POWER_SLEEP ? N_POWER_ALLOW_SLEEP : N_POWER_ALLOW_HIBERNATE)) return -N_ERR_DENIED;
        return closing.on ? -N_ERR_BUSY : n_power_request(power_cap, action);
    }
    if (closing.on) {
        closing.forced = true;
        closing.action = action;
        closing_look();
        return 0;
    }
    n_result left = n_power_windows(power_cap, true, NULL, 0);
    if (left <= 0) {
        power_stop(action);
        return 0;
    }
    n_handle watch = n_power_windows_watch(power_cap);
    if (watch < 0) return watch;
    printf("machine: asking %d windows to close\n", (int)left);
    fflush(stdout);
    uint64_t now = n_clock_monotonic();
    closing.on = true;
    closing.forced = !chosen || now < force_until;
    closing.told_forced = false;
    closing.action = action;
    closing.start = closing.progress = now;
    closing.left = left;
    closing.watch = watch;
    closing.told[0] = 0;
    n_events_watch(queue, watch, N_EVENT_RECEIVABLE, TOKEN_WINDOWS);
    return 0;
}

static void answer(int32_t how, const char *why) {
    uint32_t action;
    switch (how) {
    case N_POWER_ANSWER_SLEEP: action = POWER_SLEEP; break;
    case N_POWER_ANSWER_HIBERNATE: action = POWER_HIBERNATE; break;
    case N_POWER_ANSWER_OFF: action = POWER_OFF; break;
    default: return;
    }
    printf("machine: %s: %s\n", why, action == POWER_OFF ? "power off" : action == POWER_SLEEP ? "sleep" : "hibernation");
    fflush(stdout);
    /* ボタンと蓋は人の操作で、メニューの選択と同じ */
    power_ask(action, true);
}

/* ---- 事象 ---- */
static struct n_power_events seen;
static uint64_t idle_tried; /* 放置の答えを最後に試した時刻 */

/* 放置の期限。 */
static uint64_t idle_deadline(void) {
    int32_t minutes = policy_of(N_POWER_IDLE_MINUTES);
    if (!minutes || policy_of(N_POWER_IDLE_ANSWER) == N_POWER_ANSWER_NOTHING) return N_CLOCK_NEVER;
    struct n_power_events e;
    if (n_power_events(power_cap, &e) < 0) return N_CLOCK_NEVER;
    uint64_t from = e.last_input_ns;
    if (e.last_wake_ns > from) from = e.last_wake_ns;
    if (idle_tried > from) from = idle_tried;
    if (!from) from = idle_tried = n_clock_monotonic();
    return from + (uint64_t)minutes * 60 * SECONDS_NS;
}

static void idle_look(void) {
    if (n_clock_monotonic() < idle_deadline()) return;
    idle_tried = n_clock_monotonic();
    answer(policy_of(N_POWER_IDLE_ANSWER), "the machine was left alone");
}

static void events_look(void) {
    struct n_power_events e;
    if (n_power_events(power_cap, &e) < 0) return;
    struct n_power_events before = seen;
    seen = e;
    if (e.button_presses != before.button_presses) answer(policy_of(N_POWER_BUTTON), "the power button");
    if (e.lid_changes != before.lid_changes && e.lid_closed) answer(policy_of(N_POWER_LID), "the lid closed");
    if (e.host_asks != before.host_asks) {
        printf("machine: the host asks for power change %u\n", e.host_action);
        fflush(stdout);
        power_ask(e.host_action, false);
    }
}

static void owner_take(void) {
    for (;;) {
        struct n_setting_id id;
        int32_t value = 0;
        uint32_t length, serial;
        n_result r = n_setting_owner_take(owner, &id, &value, sizeof value, &length, &serial);
        if (r == -N_ERR_NOT_READY) return;
        if (r < 0) exit(1); /* 設定のサービスが落ちた：起こし直されて宣言し直す */
        n_result done = -N_ERR_NOT_FOUND;
        for (size_t i = 0; i < POLICY_KEYS; i++)
            if (id.domain == N_DOMAIN_POWER && declarations[i].key == id.key && length == sizeof value) {
                policy[i] = value;
                done = 0;
            }
        n_setting_owner_answer(owner, serial, done);
    }
}

/* ---- 依頼 ---- */
/* 画面の絵の共有ページ。reply に大きさを書く。 */
static n_handle screen_pixels(struct n_machine_reply *reply) {
    int width, height;
    n_result r = screen_grab(screen_cap, NULL, 0, &width, &height);
    if (r < 0) return (n_handle)r;
    size_t bytes = (size_t)width * (size_t)height * 4;
    n_handle shared = n_shared_create(bytes);
    if (shared < 0) return shared;
    void *view;
    r = n_shared_map(shared, bytes, N_MEMORY_READ_WRITE, &view);
    if (r >= 0) {
        r = screen_grab(screen_cap, view, bytes, &width, &height);
        n_memory_release(view, bytes);
    }
    if (r < 0) {
        n_handle_close(shared);
        return (n_handle)r;
    }
    reply->width = width;
    reply->height = height;
    return shared;
}

static uint32_t power_action(uint32_t op) {
    return op == N_MACHINE_POWER_OFF ? POWER_OFF : op == N_MACHINE_REBOOT ? POWER_REBOOT : op == N_MACHINE_SLEEP ? POWER_SLEEP : POWER_HIBERNATE;
}

/* 答えが受け手の列に入らない呼び手は読んでいない：閉じる。 */
static void client_reply(struct client *c, const struct n_machine_reply *reply, n_handle attached) {
    n_result sent = n_channel_send(c->channel, reply, sizeof *reply, attached);
    if (attached >= 0) n_handle_close(attached);
    if (sent < 0) client_close(c);
}

static void client_request(struct client *c, const struct n_machine_request *q) {
    struct n_machine_reply reply = {.result = -N_ERR_DENIED};
    n_handle attached = -1;
    if (c->allowed) {
        switch (q->op) {
        case N_MACHINE_POWER_OFF:
        case N_MACHINE_REBOOT:
        case N_MACHINE_SLEEP:
        case N_MACHINE_HIBERNATE: {
            bool chosen = q->flags & N_MACHINE_CHOSEN;
            uint32_t action = power_action(q->op);
            bool waits = !chosen && (action == POWER_OFF || action == POWER_REBOOT);
            if (waits) c->waiting = true;
            reply.result = q->flags & ~N_MACHINE_CHOSEN ? -N_ERR_INVALID : power_ask(action, chosen);
            if (reply.result >= 0 && waits) return; /* 止まれば答えない。起きなければ waiters_answer が答える */
            c->waiting = false;
            break;
        }
        case N_MACHINE_SCREEN:
            attached = screen_pixels(&reply);
            reply.result = attached < 0 ? attached : 0;
            if (attached < 0) attached = -1;
            break;
        default:
            reply.result = -N_ERR_INVALID;
            break;
        }
    }
    client_reply(c, &reply, attached);
}

static void client_look(struct client *c) {
    while (!c->closed) {
        struct n_machine_request q;
        n_result got = n_channel_receive(c->channel, &q, sizeof q, 0, NULL);
        if (got == -N_ERR_NOT_READY) return;
        if (got != sizeof q) {
            client_close(c);
            return;
        }
        client_request(c, &q);
    }
}

static void client_add(n_handle channel) {
    struct n_identity peer;
    struct client *c = calloc(1, sizeof *c);
    if (!c || n_channel_peer(channel, &peer) < 0 ||
        n_events_watch(queue, channel, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, (uint64_t)(uintptr_t)c) < 0) {
        free(c);
        n_handle_close(channel);
        return;
    }
    c->channel = channel;
    c->allowed = peer.session != 0 || peer.cls >= N_CLASS_ADMIN;
    c->next = clients;
    clients = c;
}

/* 受け口に来た相手を全部受ける。false は管理者が名前を閉じた知らせ。 */
static bool accept_all(void) {
    for (;;) {
        n_handle end = n_inlet_accept(N_SERVICE_HANDLE_CONTROL, 0);
        if (end == -N_ERR_NOT_READY) return true;
        if (end == -N_ERR_CLOSED) return false;
        if (end >= 0) client_add(end);
    }
}

int main(void) {
    power_cap = n_cap_find(N_CAP_POWER);
    screen_cap = n_cap_find(N_CAP_SCREEN_READ);
    queue = n_events_create();
    owner = n_setting_owner_open(declarations, POLICY_KEYS, NULL);
    n_handle watch = n_power_events_watch(power_cap);
    n_result bad = power_cap < 0 ? power_cap : screen_cap < 0 ? screen_cap : queue < 0 ? queue : owner < 0 ? owner : watch < 0 ? watch : 0;
    if (bad < 0) {
        printf("machine: cannot start (%s)\n", n_result_text(bad));
        fflush(stdout);
        return 1;
    }
    for (size_t i = 0; i < POLICY_KEYS; i++) {
        policy[i] = (int32_t)declarations[i].fallback;
        n_setting_get_int(n_setting_at(N_DOMAIN_POWER, declarations[i].key), &policy[i]);
    }
    n_events_watch(queue, N_SERVICE_HANDLE_CONTROL, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, TOKEN_CONTROL);
    n_events_watch(queue, watch, N_EVENT_RECEIVABLE, TOKEN_EVENTS);
    n_events_watch(queue, owner, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, TOKEN_OWNER);
    /* 起動より前の事象：ホストの依頼は聞く。ボタンと蓋の過去は今のことではない。 */
    n_power_events(power_cap, &seen);
    seen.host_asks = 0;
    events_look();
    n_service_ready();
    for (;;) {
        uint64_t deadline = closing_deadline(), idle = idle_deadline();
        if (idle < deadline) deadline = idle;
        struct n_event events[16];
        n_result n = n_events_wait(queue, events, sizeof events / sizeof *events, deadline);
        if (n < 0) return 1;
        bool look = false;
        for (n_result i = 0; i < n; i++) {
            uint64_t token = events[i].token;
            if (token == TOKEN_CONTROL) {
                if (!accept_all()) return 0;
            } else if (token == TOKEN_EVENTS) {
                n_watch_take(watch);
                events_look();
            } else if (token == TOKEN_OWNER) {
                owner_take();
            } else if (token == TOKEN_WINDOWS) {
                if (closing.on) n_watch_take(closing.watch);
                look = true;
            } else {
                struct client *c = (struct client *)(uintptr_t)token;
                if (!c->closed) client_look(c);
            }
        }
        if (look || !n) closing_look();
        if (!n) idle_look();
        while (dead) {
            struct client *c = dead;
            dead = c->next;
            free(c);
        }
    }
}
