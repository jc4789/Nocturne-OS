/* サービスの管理者：カーネルが起こす最初のプロセス。根の能力（N_INITIAL_ROOT_HANDLE）を受ける。

   何を動かすかは下の宣言の表 declared[] が決める。表は C の型のあるデータで、プログラムに
   組み込まれている。起動の順は依存から計算し、依存の準備は相手の知らせ（n_service_ready）で知る。

   名前の持ち主はここだけ。要求は n_service_register の channel で受け、名前を三つに分ける：
     - N_SERVICE_MANAGER：この管理者の口。
     - N_SERVICE_BIND：名前の申し込み（n_service_listen）。
     - 宣言の名前：身元（要求の from）を宣言に照らし、相手を起こして、要求を受け口へ送る。
     - 申し込まれた名前：申し込んだ者の利用者とセッションの中でだけ引ける。宣言のプログラムが
       申し込んだ名前は機械全体で引ける。
   どれでもなければ添付の端を閉じる。

   錠は一つ（state_lock）。主のループと、口の thread が取る。
   主のループは相手の遅さで止まらない：受け口へは送れるときだけ一通ずつ送り、管理者の口と
   名前の申し込みの相手には thread ごとに答える。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <threads.h>
#include "nocturne.h"
#include "nocturne_services.h"
#include "nvrm.h"

/* ---- 宣言 ---- */
enum service_scope {
    SCOPE_MACHINE,  /* 機械に一つ。 */
    SCOPE_FUNCTION, /* 宣言の function_matches に合う PCI の機能ごとに一つ。引数は N_PCI_TARGET の 16 進数。 */
};
/* 失敗したときの扱い。結果 0 で終わるのは失敗ではない。 */
struct failure_rule {
    uint32_t restarts;    /* 起こし直す回数。超えたら諦めて知らせる。 */
    uint32_t interval_ms; /* 起こし直すまでの間隔 */
    uint32_t healthy_ms;  /* これだけ動けば、回数を数え直す */
};
#define CLIENTS_NONE (-1) /* class での接続を許さない */
struct declared_service {
    const char *name;
    const char *program; /* N_FOLDER_SYSTEM の中の名前 */
    enum n_class cls;
    enum service_scope scope;
    bool at_boot;  /* 要求を待たずに起こす */
    bool optional; /* プログラムが媒体に無くてもよい。依存の者は満たされたとみなす。 */
    bool console;  /* カーネルから受けたコンソールを 0、1、2 に渡す。無ければシステムのログ。 */
    const enum n_cap *caps;
    uint32_t cap_count;
    const char *const *needs; /* 先に準備が済んでいなければならないサービス。NULL 終わり。 */
    struct failure_rule on_failure;
    int clients_from;         /* この級以上は接続してよい。CLIENTS_NONE は誰も。 */
    bool (*function_matches)(const struct n_pciinfo *f); /* SCOPE_FUNCTION だけ */
};

#define CAPS(...) .caps = (const enum n_cap[]){__VA_ARGS__}, \
                  .cap_count = sizeof((const enum n_cap[]){__VA_ARGS__}) / sizeof(enum n_cap)
#define NAMES(...) (const char *const[]){__VA_ARGS__, NULL}

static bool nvidia_display(const struct n_pciinfo *f) { return nvrm_pci_is_gpu(f->vendor, f->cls, f->subcls, f->progif); }

static const struct declared_service declared[] = {
    {.name = "systemlog", .program = "systemlog", .cls = N_CLASS_SYSTEM, .at_boot = true,
     .on_failure = {5, 1000, 60000}, .clients_from = CLIENTS_NONE},
    /* ドライバーは自分の機能だけを claim し、システムの級を持たない。相手は nvrm が申し込む名前で来る。 */
    {.name = "gpu", .program = "nvrm", .cls = N_CLASS_USER, .scope = SCOPE_FUNCTION, .at_boot = true, .optional = true,
     CAPS(N_CAP_PCI_CLAIM), .on_failure = {2, 2000, 60000}, .clients_from = CLIENTS_NONE,
     .function_matches = nvidia_display},
    {.name = "accounts", .program = "accounts", .cls = N_CLASS_SYSTEM, .at_boot = true,
     CAPS(N_CAP_STORE_PRIVATE), .on_failure = {5, 500, 60000}, .clients_from = N_CLASS_USER},
    {.name = "settings", .program = "settingsd", .cls = N_CLASS_SYSTEM, .at_boot = true,
     CAPS(N_CAP_SETTINGS_APPLY, N_CAP_STORE_PRIVATE), .on_failure = {5, 500, 60000}, .clients_from = N_CLASS_USER},
    /* 要るときに起こす。電源と画面の読取りの能力はここにだけ渡す。 */
    {.name = "machine", .program = "machine", .cls = N_CLASS_SYSTEM,
     CAPS(N_CAP_POWER, N_CAP_SCREEN_READ), .on_failure = {3, 500, 60000}, .clients_from = N_CLASS_USER},
    /* 試験の媒体にだけあるプログラム。C:\Nocturne\System の中にあるので、アプリは置けない。 */
    {.name = "tests", .program = "testrun", .cls = N_CLASS_SYSTEM, .at_boot = true, .optional = true,
     CAPS(N_CAP_POWER, N_CAP_PCI_CLAIM, N_CAP_SCREEN_READ, N_CAP_IDENTITY_GRANT), .needs = NAMES("accounts"),
     .clients_from = CLIENTS_NONE},
    {.name = "session", .program = "session", .cls = N_CLASS_SYSTEM, .at_boot = true, .console = true,
     CAPS(N_CAP_IDENTITY_GRANT, N_CAP_SHELL), .needs = NAMES("accounts", "tests"),
     .on_failure = {5, 1000, 60000}, .clients_from = N_CLASS_USER},
};
#define DECLARED (sizeof declared / sizeof *declared)
#define STOP_GRACE_MS 5000u /* 閉じた後、これだけ待って強制的に止める */

static const struct declared_service *declared_named(const char *name) {
    for (size_t i = 0; i < DECLARED; i++)
        if (!strcmp(declared[i].name, name)) return &declared[i];
    return NULL;
}

/* プログラムの番号は表の位置から。0 は宣言に無いプログラム。 */
static uint32_t program_number(const struct declared_service *d) { return (uint32_t)(d - declared) + 1; }

/* ---- 受け口へ送る列 ---- */
struct pending_request {
    struct pending_request *next;
    void *message;
    size_t bytes;
    n_handle attached;
};
struct mouth {
    n_handle send; /* 受け口へ送る端。無ければ -1：相手を起こしたら送る。 */
    bool sending_watched;
    struct pending_request *pending, **tail;
};

enum token_kind { TOKEN_REQUESTS, TOKEN_WAKE, TOKEN_APP, TOKEN_CONTROL, TOKEN_SENDABLE, TOKEN_BOUND_CLOSED };
#define TOKEN_MASK 7ull
#define TOKEN(p, kind) ((uint64_t)(uintptr_t)(p) | (uint64_t)(kind))

static mtx_t state_lock;
static n_handle root_cap, grant_cap, requests, events, wake_ends[2];

static void wake_main(void) {
    char byte = 0;
    n_channel_send(wake_ends[1], &byte, 1, -1);
}

static void mouth_init(struct mouth *m) {
    m->send = -1;
    m->tail = &m->pending;
}

/* 要求を積む。message は mouth のものになる。 */
static void mouth_put(struct mouth *m, void *message, size_t bytes, n_handle attached) {
    struct pending_request *p = malloc(sizeof *p);
    if (!p) {
        n_handle_close(attached);
        free(message);
        return;
    }
    *p = (struct pending_request){.message = message, .bytes = bytes, .attached = attached};
    *m->tail = p;
    m->tail = &p->next;
    if (m->send >= 0 && !m->sending_watched) {
        n_events_watch(events, m->send, N_EVENT_SENDABLE, TOKEN(m, TOKEN_SENDABLE));
        m->sending_watched = true;
    }
}

static void mouth_stop_watching(struct mouth *m) {
    if (m->sending_watched) n_events_unwatch(events, TOKEN(m, TOKEN_SENDABLE));
    m->sending_watched = false;
}

/* 待っている要求を一通送る。残れば次の N_EVENT_SENDABLE で続ける。 */
static void mouth_send_one(struct mouth *m) {
    struct pending_request *p = m->pending;
    if (p && m->send >= 0) {
        n_channel_send(m->send, p->message, p->bytes, p->attached);
        n_handle_close(p->attached);
        free(p->message);
        m->pending = p->next;
        if (!m->pending) m->tail = &m->pending;
        free(p);
    }
    if (!m->pending) mouth_stop_watching(m);
}

static void mouth_close_all(struct mouth *m) {
    mouth_stop_watching(m);
    while (m->pending) {
        struct pending_request *p = m->pending;
        m->pending = p->next;
        n_handle_close(p->attached);
        free(p->message);
        free(p);
    }
    m->tail = &m->pending;
}

/* ---- 実例 ---- */
struct instance {
    struct mouth mouth; /* 先頭：token の指は同じ番地 */
    struct instance *next;
    const struct declared_service *declared;
    char argument[N_SERVICE_ARGUMENT_MAX];
    uint32_t state;         /* enum n_service_state */
    uint64_t state_since;
    uint64_t changed;       /* 状態を変えたときの generation */
    bool wanted;            /* 起こす */
    bool broken;            /* 依存の表が解けない */
    bool stopping;          /* 受け口を閉じた */
    bool restart_after_stop;
    n_handle app;
    int64_t app_id;
    int64_t last_status;
    uint32_t restarts;
    uint64_t started_at;
    uint64_t retry_at;      /* N_SERVICE_BACKOFF の期限 */
    uint64_t stop_deadline; /* stopping の期限 */
};
static struct instance *instances; /* 依存の順に並ぶ */
static volatile uint32_t generation;

static void set_state(struct instance *in, uint32_t state) {
    if (in->state == state) return;
    in->state = state;
    in->state_since = n_clock_monotonic();
    in->changed = __atomic_add_fetch(&generation, 1, __ATOMIC_ACQ_REL);
    wake_address(&generation, INT_MAX);
}

static struct instance *instance_new(const struct declared_service *d, const char *argument, bool broken) {
    struct instance *in = calloc(1, sizeof *in);
    if (!in) return NULL;
    in->declared = d;
    if (argument) strlcpy(in->argument, argument, sizeof in->argument);
    in->app = -1;
    in->broken = broken;
    in->wanted = d->at_boot;
    in->state_since = n_clock_monotonic();
    mouth_init(&in->mouth);
    struct instance **at = &instances;
    while (*at) at = &(*at)->next;
    *at = in;
    return in;
}

static struct instance *instance_find(const struct declared_service *d, const char *argument) {
    for (struct instance *in = instances; in; in = in->next)
        if (in->declared == d && !strcmp(in->argument, argument ? argument : "")) return in;
    return NULL;
}

/* ---- 表から実例を作る。解けない項目は壊れた実例になり、理由をログへ書く。 ---- */
enum { VISIT_NEW, VISIT_OPEN, VISIT_DONE };

static void pci_functions(struct n_pciinfo **out, int *count) {
    *out = NULL;
    *count = 0;
    struct n_pciinfo *list = NULL;
    size_t capacity = 16;
    for (;;) {
        if (capacity > INT_MAX / 2) break;
        struct n_pciinfo *grown = realloc(list, capacity * sizeof *list);
        if (!grown) break;
        list = grown;
        int n = pcilist(list, (int)capacity);
        if (n < 0) break;
        if ((size_t)n < capacity) {
            *out = list;
            *count = n;
            return;
        }
        capacity *= 2;
    }
    free(list);
}

static void instances_of(const struct declared_service *d, bool broken, const struct n_pciinfo *functions, int function_count) {
    if (d->scope == SCOPE_MACHINE) {
        instance_new(d, NULL, broken);
        return;
    }
    for (int i = 0; i < function_count; i++) {
        if (!d->function_matches(&functions[i])) continue;
        char argument[8];
        snprintf(argument, sizeof argument, "%04x", (unsigned)N_PCI_TARGET(functions[i].bus, functions[i].dev, functions[i].func));
        instance_new(d, argument, broken);
    }
}

static bool visit(const struct declared_service *d, unsigned char *marks, const struct n_pciinfo *functions, int function_count) {
    size_t i = (size_t)(d - declared);
    if (marks[i] == VISIT_DONE) return true;
    if (marks[i] == VISIT_OPEN) {
        printf("services: %s is in a cycle of needs\n", d->name);
        return false;
    }
    marks[i] = VISIT_OPEN;
    bool solved = true;
    for (const char *const *need = d->needs; need && *need; need++) {
        const struct declared_service *other = declared_named(*need);
        if (!other) {
            printf("services: %s needs %s, which is not declared\n", d->name, *need);
            solved = false;
        } else if (!visit(other, marks, functions, function_count)) {
            solved = false;
        }
    }
    marks[i] = VISIT_DONE;
    instances_of(d, !solved, functions, function_count);
    return solved;
}

static void instances_from_table(void) {
    unsigned char marks[DECLARED] = {VISIT_NEW};
    struct n_pciinfo *functions;
    int function_count;
    pci_functions(&functions, &function_count);
    for (size_t i = 0; i < DECLARED; i++) visit(&declared[i], marks, functions, function_count);
    free(functions);
}

/* ---- 起こす ---- */
static bool needs_met(const struct declared_service *d) {
    for (const char *const *need = d->needs; need && *need; need++) {
        const struct declared_service *other = declared_named(*need);
        /* 実例が一つも無い宣言（合う装置が無い）は満たされている */
        for (struct instance *in = instances; in; in = in->next)
            if (in->declared == other && in->state != N_SERVICE_RUNNING && in->state != N_SERVICE_ABSENT &&
                !(other->optional && in->state == N_SERVICE_FAILED))
                return false;
    }
    return true;
}

static void give_up(struct instance *in, const char *why, n_result result) {
    in->last_status = result;
    in->wanted = false;
    mouth_close_all(&in->mouth);
    printf("services: %s%s%s gave up: %s (%s)\n", in->declared->name, in->argument[0] ? ":" : "", in->argument, why,
           n_result_text(result));
    fflush(stdout);
    set_state(in, N_SERVICE_FAILED);
}

static void start(struct instance *in) {
    const struct declared_service *d = in->declared;
    char path[N_SERVICE_PROGRAM_MAX + sizeof N_FOLDER_SYSTEM + 1];
    snprintf(path, sizeof path, "%s\\%s", N_FOLDER_SYSTEM, d->program);
    struct n_entry e;
    if (n_entry_info(path, &e) < 0 && d->optional) {
        in->wanted = false;
        set_state(in, N_SERVICE_ABSENT);
        return;
    }
    n_handle ends[2];
    n_result r = n_channel_pair(ends);
    if (r < 0) {
        give_up(in, "no channel", r);
        return;
    }
    n_handle log = d->console ? -1 : n_system_log_open();
    if (log < 0) log = -1;
    int32_t handles[4 + N_CAP_COUNT] = {d->console ? 0 : -1, d->console ? 1 : log, d->console ? 2 : log, ends[1]};
    uint32_t handle_count = 4;
    for (uint32_t i = 0; i < d->cap_count; i++) {
        n_handle cap = n_cap_derive(root_cap, d->caps[i]);
        if (cap >= 0) handles[handle_count++] = cap;
        else printf("services: %s: a capability cannot be made (%s)\n", d->name, n_result_text(cap));
    }
    const char *arguments[2] = {d->program, in->argument};
    const char *settings[2] = {"FOLDER=C:\\", "TERMINAL=0 1 2"};
    struct n_identity identity = {.user = 0, .session = 0, .cls = (uint32_t)d->cls, .program = program_number(d)};
    struct n_launch launch = {.arguments = arguments, .settings = settings, .handles = handles,
                              .argument_count = in->argument[0] ? 2 : 1, .setting_count = d->console && n_terminal(1) ? 2 : 1,
                              .handle_count = handle_count, .identity = &identity, .identity_grant = grant_cap};
    n_handle app = n_app_launch(path, &launch);
    if (log >= 0) n_handle_close(log);
    n_handle_close(ends[1]);
    for (uint32_t i = 4; i < handle_count; i++) n_handle_close(handles[i]);
    if (app < 0) {
        n_handle_close(ends[0]);
        give_up(in, "cannot start", app);
        return;
    }
    in->app = app;
    in->mouth.send = ends[0];
    in->app_id = n_app_id(app);
    in->started_at = n_clock_monotonic();
    in->stopping = false;
    n_events_watch(events, app, N_EVENT_APP_ENDED, TOKEN(in, TOKEN_APP));
    n_events_watch(events, ends[0], N_EVENT_RECEIVABLE, TOKEN(in, TOKEN_CONTROL));
    set_state(in, N_SERVICE_STARTING);
    if (in->mouth.pending) {
        n_events_watch(events, ends[0], N_EVENT_SENDABLE, TOKEN(&in->mouth, TOKEN_SENDABLE));
        in->mouth.sending_watched = true;
    }
}

/* 受け口を閉じる：サービスの n_inlet_accept が N_ERR_CLOSED になる。期限の後は強制的に止める。 */
static void close_mouth(struct instance *in) {
    mouth_stop_watching(&in->mouth);
    n_events_unwatch(events, TOKEN(in, TOKEN_CONTROL));
    if (in->mouth.send >= 0) n_handle_close(in->mouth.send);
    in->mouth.send = -1;
}

static void stop(struct instance *in) {
    in->wanted = false;
    if (in->app < 0) {
        if (in->state != N_SERVICE_ABSENT) set_state(in, N_SERVICE_STOPPED);
        return;
    }
    in->stopping = true;
    in->stop_deadline = n_clock_after(STOP_GRACE_MS * N_NS_PER_MS);
    close_mouth(in);
}

/* 起こせるものを起こす。表の順は依存の順。 */
static void advance(void) {
    uint64_t now = n_clock_monotonic();
    for (struct instance *in = instances; in; in = in->next) {
        if (in->state == N_SERVICE_BACKOFF && now >= in->retry_at) {
            in->restarts++;
            set_state(in, N_SERVICE_WAITING);
        }
        if (in->stopping && in->app >= 0 && now >= in->stop_deadline) n_app_stop(in->app, N_APP_STOP_TREE);
        if (!in->wanted || in->app >= 0 || in->state == N_SERVICE_BACKOFF) continue;
        if (in->state == N_SERVICE_FAILED || in->state == N_SERVICE_ABSENT) continue;
        if (in->broken) {
            give_up(in, "its needs cannot be solved", -N_ERR_INVALID);
            continue;
        }
        if (!needs_met(in->declared)) {
            set_state(in, N_SERVICE_WAITING);
            continue;
        }
        start(in);
    }
}

static uint64_t next_deadline(void) {
    uint64_t soonest = N_CLOCK_NEVER;
    for (struct instance *in = instances; in; in = in->next) {
        if (in->state == N_SERVICE_BACKOFF && in->retry_at < soonest) soonest = in->retry_at;
        if (in->stopping && in->app >= 0 && in->stop_deadline < soonest) soonest = in->stop_deadline;
    }
    return soonest;
}

static void app_ended(struct instance *in) {
    const struct declared_service *d = in->declared;
    int status = 0;
    n_result r = n_app_result(in->app, 0, &status);
    if (r == -N_ERR_NOT_READY) return;
    n_events_unwatch(events, TOKEN(in, TOKEN_APP));
    close_mouth(in);
    n_handle_close(in->app);
    in->app = -1;
    in->app_id = 0;
    in->last_status = r < 0 ? r : status;
    bool asked = in->stopping;
    in->stopping = false;
    if (in->restart_after_stop) {
        in->restart_after_stop = false;
        in->wanted = true;
        in->restarts = 0;
        set_state(in, N_SERVICE_WAITING);
        return;
    }
    if (asked || (r >= 0 && status == 0)) {
        /* 頼まれた終わりか、仕事を終えた。待っている相手がいれば起こし直す。 */
        in->wanted = in->mouth.pending != NULL;
        set_state(in, in->wanted ? N_SERVICE_WAITING : N_SERVICE_STOPPED);
        return;
    }
    if (n_clock_monotonic() - in->started_at >= (uint64_t)d->on_failure.healthy_ms * N_NS_PER_MS) in->restarts = 0;
    printf("services: %s%s%s ended (%lld), restart %u of %u\n", d->name, in->argument[0] ? ":" : "", in->argument,
           (long long)in->last_status, in->restarts, d->on_failure.restarts);
    fflush(stdout);
    if (in->restarts >= d->on_failure.restarts) {
        give_up(in, "too many failures", r < 0 ? r : -N_ERR_STOPPED);
        return;
    }
    in->retry_at = n_clock_after((uint64_t)d->on_failure.interval_ms * N_NS_PER_MS);
    set_state(in, N_SERVICE_BACKOFF);
}

static void control_receive(struct instance *in) {
    for (;;) {
        struct n_service_ready_message message;
        n_handle attached = -1;
        n_result r = n_channel_receive(in->mouth.send, &message, sizeof message, 0, &attached);
        if (attached >= 0) n_handle_close(attached);
        if (r == -N_ERR_CLOSED) n_events_unwatch(events, TOKEN(in, TOKEN_CONTROL)); /* 相手が閉じた。終わりは TOKEN_APP で知る。 */
        if (r < 0) return;
        if (r == sizeof message && message.kind == N_SERVICE_READY && in->state == N_SERVICE_STARTING)
            set_state(in, N_SERVICE_RUNNING);
    }
}

/* ---- 申し込まれた名前 ---- */
struct bound_name {
    struct mouth mouth; /* 先頭：token の指は同じ番地 */
    struct bound_name *next;
    char *name;
    bool machine_wide;
    uint32_t user, session;
    bool dead;
};
static struct bound_name *bound_names;

static bool bound_reaches(const struct bound_name *b, const struct n_identity *from) {
    return b->machine_wide || (b->user == from->user && b->session == from->session);
}

/* 同じ名前は、機械全体か、同じ利用者とセッションの中で一つ。機械全体の名前が先：
   利用者は機械のサービスの名前を自分の範囲で隠せない。 */
static struct bound_name *bound_find(const char *name, const struct n_identity *from) {
    struct bound_name *own = NULL;
    for (struct bound_name *b = bound_names; b; b = b->next) {
        if (b->dead || strcmp(b->name, name) || !bound_reaches(b, from)) continue;
        if (b->machine_wide) return b;
        own = b;
    }
    return own;
}

static bool name_is_ours(const char *name) {
    return !strcmp(name, N_SERVICE_MANAGER) || !strcmp(name, N_SERVICE_BIND) || declared_named(name);
}

/* 名前の申し込みの一件：本文が名前、添付が受け口へ送る端。 */
struct bind_client { n_handle channel; struct n_identity from; };

static void bind_serve(void *arg) {
    struct bind_client *client = arg;
    n_handle send_end = -1;
    int32_t result = -N_ERR_PROTOCOL;
    n_result size = n_channel_receive(client->channel, NULL, 0, N_CHANNEL_PEEK | N_CHANNEL_WAIT, NULL);
    char *name = size > 0 ? malloc((size_t)size + 1) : NULL;
    if (size > 0 && !name) result = -N_ERR_NO_MEMORY;
    if (name && n_channel_receive(client->channel, name, (size_t)size, 0, &send_end) == size && send_end >= 0) {
        name[size] = 0;
        struct bound_name *b = calloc(1, sizeof *b);
        mtx_lock(&state_lock);
        if (!b) result = -N_ERR_NO_MEMORY;
        else if (strlen(name) != (size_t)size || name_is_ours(name)) result = -N_ERR_DENIED;
        else if (bound_find(name, &client->from)) result = -N_ERR_EXISTS;
        else {
            b->machine_wide = client->from.program != 0 || client->from.cls >= N_CLASS_SYSTEM;
            b->user = client->from.user;
            b->session = client->from.session;
            b->name = name;
            mouth_init(&b->mouth);
            b->mouth.send = send_end;
            b->next = bound_names;
            bound_names = b;
            n_events_watch(events, send_end, N_EVENT_CLOSED, TOKEN(b, TOKEN_BOUND_CLOSED));
            name = NULL;
            b = NULL;
            send_end = -1;
            result = 0;
        }
        mtx_unlock(&state_lock);
        free(b);
    }
    free(name);
    if (send_end >= 0) n_handle_close(send_end);
    n_channel_send(client->channel, &result, sizeof result, -1);
    n_handle_close(client->channel);
    free(client);
}

/* 受ける側が全部閉じた：名前を消す。記憶を返すのは報告の組の後（bound_sweep）。 */
static void bound_closed(struct bound_name *b) {
    n_events_unwatch(events, TOKEN(b, TOKEN_BOUND_CLOSED));
    mouth_close_all(&b->mouth);
    n_handle_close(b->mouth.send);
    b->mouth.send = -1;
    b->dead = true;
}

static void bound_sweep(void) {
    for (struct bound_name **at = &bound_names; *at;) {
        struct bound_name *b = *at;
        if (!b->dead) {
            at = &b->next;
            continue;
        }
        *at = b->next;
        free(b->name);
        free(b);
    }
}

/* ---- 管理者の口：一人の相手に一つの thread ---- */
static void status_of(const struct instance *in, struct n_service_status *out) {
    const struct declared_service *d = in->declared;
    *out = (struct n_service_status){.cls = (uint32_t)d->cls, .state = in->state, .restarts = in->restarts,
                                     .restarts_allowed = d->on_failure.restarts, .app = in->app_id,
                                     .last_status = in->last_status, .state_since = in->state_since};
    strlcpy(out->name, d->name, sizeof out->name);
    strlcpy(out->argument, in->argument, sizeof out->argument);
    strlcpy(out->program, d->program, sizeof out->program);
}

/* 錠の下で写す。送るのは錠の外。newer_than より後に変わった実例だけ。-1 は全部。 */
static n_result snapshot(const char *name, const char *argument, int64_t newer_than, struct n_service_status **out) {
    mtx_lock(&state_lock);
    size_t count = 0;
    for (struct instance *in = instances; in; in = in->next) count++;
    struct n_service_status *all = malloc((count ? count : 1) * sizeof *all);
    size_t n = 0;
    if (all)
        for (struct instance *in = instances; in; in = in->next) {
            if ((int64_t)in->changed <= newer_than) continue;
            if (name && (strcmp(in->declared->name, name) || strcmp(in->argument, argument))) continue;
            status_of(in, &all[n++]);
        }
    mtx_unlock(&state_lock);
    if (!all) return -N_ERR_NO_MEMORY;
    *out = all;
    return (n_result)n;
}

static n_result send_statuses(n_handle channel, const struct n_service_status *all, n_result count) {
    for (n_result i = 0; i < count; i++) {
        n_result r = n_channel_send(channel, &all[i], sizeof all[i], -1);
        if (r < 0) return r;
    }
    return 0;
}

static n_result change_instance(uint32_t op, const char *name, const char *argument) {
    const struct declared_service *d = declared_named(name);
    if (!d) return -N_ERR_NOT_FOUND;
    mtx_lock(&state_lock);
    struct instance *in = instance_find(d, argument);
    n_result r = in ? 0 : -N_ERR_NOT_FOUND;
    if (in && op == N_SERVICES_STOP) stop(in);
    if (in && op == N_SERVICES_RESTART) {
        if (in->app >= 0) {
            in->restart_after_stop = true;
            stop(in);
        } else {
            in->wanted = true;
            in->restarts = 0;
            set_state(in, N_SERVICE_WAITING);
        }
    }
    mtx_unlock(&state_lock);
    wake_main();
    return r;
}

static void watch_client(n_handle channel) {
    n_handle queue = n_events_create();
    if (queue < 0) return;
    enum { WATCH_CLIENT = 1, WATCH_GENERATION };
    n_events_watch(queue, channel, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, WATCH_CLIENT);
    int64_t seen = -1;
    for (bool open = true; open;) {
        uint32_t now = __atomic_load_n(&generation, __ATOMIC_ACQUIRE);
        struct n_service_status *all;
        n_result count = snapshot(NULL, NULL, seen, &all);
        if (count < 0) break;
        n_result sent = send_statuses(channel, all, count);
        free(all);
        if (sent < 0) break;
        seen = now;
        n_events_watch_address(queue, &generation, now, WATCH_GENERATION);
        struct n_event reports[2];
        n_result got = n_events_wait(queue, reports, 2, N_CLOCK_NEVER);
        if (got < 0) break;
        for (n_result i = 0; i < got; i++)
            if (reports[i].token == WATCH_CLIENT) open = false;
    }
    n_handle_close(queue);
}

static void manager_serve(void *arg) {
    struct bind_client *client = arg;
    n_handle channel = client->channel;
    uint32_t cls = client->from.cls;
    free(client);
    for (;;) {
        struct n_services_request request;
        n_result got = n_channel_receive(channel, &request, sizeof request, N_CHANNEL_WAIT, NULL);
        if (got < 0 || got != sizeof request) break;
        request.name[sizeof request.name - 1] = 0;
        request.argument[sizeof request.argument - 1] = 0;
        struct n_services_reply reply = {0};
        struct n_service_status *all = NULL;
        n_result count = 0;
        switch (request.op) {
        case N_SERVICES_LIST:
            count = snapshot(NULL, NULL, -1, &all);
            break;
        case N_SERVICES_STATUS:
            count = snapshot(request.name, request.argument, -1, &all);
            break;
        case N_SERVICES_RESTART:
        case N_SERVICES_STOP:
            reply.result = cls >= N_CLASS_ADMIN ? change_instance(request.op, request.name, request.argument) : -N_ERR_DENIED;
            break;
        case N_SERVICES_WATCH:
            if (n_channel_send(channel, &reply, sizeof reply, -1) >= 0) watch_client(channel);
            n_handle_close(channel);
            return;
        default:
            reply.result = -N_ERR_INVALID;
            break;
        }
        if (count < 0) reply.result = count;
        else reply.count = (uint32_t)count;
        n_result sent = n_channel_send(channel, &reply, sizeof reply, -1);
        if (sent >= 0 && count > 0) sent = send_statuses(channel, all, count);
        free(all);
        if (sent < 0) break;
    }
    n_handle_close(channel);
}

static void serve_on_thread(void (*fn)(void *), n_handle channel, const struct n_identity *from) {
    struct bind_client *client = malloc(sizeof *client);
    if (client) *client = (struct bind_client){.channel = channel, .from = *from};
    n_result thread = client ? thread_create(fn, client) : -N_ERR_NO_MEMORY;
    if (thread < 0) {
        free(client);
        n_handle_close(channel);
        return;
    }
    thread_detach((int)thread);
}

/* ---- 宣言に照らす ---- */
static bool client_allowed(const struct declared_service *d, const struct n_identity *from) {
    return d->clients_from != CLIENTS_NONE && from->cls >= (uint32_t)d->clients_from;
}

/* 一つの要求。message は呼ばれた側のもの。 */
static void request_one(void *message, size_t bytes, n_handle channel) {
    const struct n_service_request *request = message;
    char *name = strndup((const char *)(request + 1), request->name_length);
    if (name && !strcmp(name, N_SERVICE_MANAGER)) {
        serve_on_thread(manager_serve, channel, &request->from);
    } else if (name && !strcmp(name, N_SERVICE_BIND)) {
        serve_on_thread(bind_serve, channel, &request->from);
    } else if (name) {
        const struct declared_service *d = declared_named(name);
        if (d) {
            struct instance *in = d->scope == SCOPE_MACHINE && client_allowed(d, &request->from) ? instance_find(d, NULL) : NULL;
            if (in && in->state != N_SERVICE_FAILED && in->state != N_SERVICE_ABSENT) {
                in->wanted = true;
                mouth_put(&in->mouth, message, bytes, channel);
                free(name);
                return;
            }
        } else {
            struct bound_name *b = bound_find(name, &request->from);
            if (b) {
                mouth_put(&b->mouth, message, bytes, channel);
                free(name);
                return;
            }
        }
        n_handle_close(channel);
    } else {
        n_handle_close(channel);
    }
    free(name);
    free(message);
}

static void requests_receive(void) {
    for (;;) {
        n_result size = n_channel_receive(requests, NULL, 0, N_CHANNEL_PEEK, NULL);
        if (size < 0) return;
        void *message = malloc(size ? (size_t)size : 1);
        n_handle attached = -1;
        n_result got = message ? n_channel_receive(requests, message, (size_t)size, 0, &attached)
                               : n_channel_receive(requests, NULL, 0, N_CHANNEL_DISCARD, NULL);
        if (got < 0 || !message) {
            free(message);
            if (got < 0 && got != -N_ERR_NO_MEMORY) return;
            continue;
        }
        const struct n_service_request *request = message;
        if ((size_t)got < sizeof *request || attached < 0 || request->name_length != (size_t)got - sizeof *request) {
            if (attached >= 0) n_handle_close(attached);
            free(message);
            continue;
        }
        request_one(message, (size_t)got, attached);
    }
}

/* ---- 主のループ ---- */
static void report(const struct n_event *e) {
    uint64_t kind = e->token & TOKEN_MASK;
    void *p = (void *)(uintptr_t)(e->token & ~TOKEN_MASK);
    struct instance *in = p;
    switch (kind) {
    case TOKEN_REQUESTS:
        requests_receive();
        break;
    case TOKEN_WAKE:
        while (n_channel_receive(wake_ends[0], NULL, 0, N_CHANNEL_DISCARD, NULL) >= 0) {}
        break;
    case TOKEN_APP:
        if (in->app >= 0) app_ended(in);
        break;
    case TOKEN_CONTROL:
        if (in->mouth.send >= 0) control_receive(in);
        break;
    case TOKEN_SENDABLE:
        mouth_send_one(p);
        break;
    case TOKEN_BOUND_CLOSED:
        if (!((struct bound_name *)p)->dead) bound_closed(p);
        break;
    }
}

int main(void) {
    root_cap = N_INITIAL_ROOT_HANDLE;
    printf("\n\x1b[1;35m*\x1b[0m Welcome to \x1b[1mNocturne \x1b[0m- a small operating system for quiet nights.\n");
    fflush(stdout);
    if (mtx_init(&state_lock, mtx_plain) != thrd_success || (events = n_events_create()) < 0) return 1;
    n_handle service_root = n_cap_derive(root_cap, N_CAP_SERVICE_ROOT);
    grant_cap = n_cap_derive(root_cap, N_CAP_IDENTITY_GRANT);
    requests = service_root >= 0 ? n_service_register(service_root) : service_root;
    if (requests < 0 || grant_cap < 0 || n_channel_pair(wake_ends) < 0) {
        printf("services: the kernel gave no authority (%s)\n", n_result_text(requests < 0 ? requests : grant_cap));
        fflush(stdout);
        return 1;
    }
    n_events_watch(events, requests, N_EVENT_RECEIVABLE, TOKEN(NULL, TOKEN_REQUESTS));
    n_events_watch(events, wake_ends[0], N_EVENT_RECEIVABLE, TOKEN(NULL, TOKEN_WAKE));
    instances_from_table();
    for (;;) {
        mtx_lock(&state_lock);
        advance();
        bound_sweep();
        uint64_t deadline = next_deadline();
        mtx_unlock(&state_lock);
        struct n_event reports[32];
        n_result got = n_events_wait(events, reports, 32, deadline);
        if (got < 0) continue;
        mtx_lock(&state_lock);
        for (n_result i = 0; i < got; i++) report(&reports[i]);
        mtx_unlock(&state_lock);
    }
}
