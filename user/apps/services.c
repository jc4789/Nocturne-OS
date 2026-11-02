/* サービスの管理者：カーネルが起こす最初のプロセス。根の能力（N_CAP_ROOT）を受ける。

   何を動かすかは下の宣言の表 declared[] が決める。表は C の型のあるデータで、プログラムに
   組み込まれている。起動の順は依存から計算し、依存の準備は相手の READY で知る。
   要求は SYS_SERVICE_REGISTER の channel で受け、宣言に照らし、相手を起こして channel を渡す。

   錠は一つ（state_lock）。主のループと、一覧の口の thread が取る。
   主のループは相手の遅さで止まらない：サービスへの接続は送れるときだけ一通ずつ送り、
   一覧の口の相手には thread ごとに答える。 */
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
    SCOPE_MACHINE,  /* 機械に一つ。身元はシステム。 */
    SCOPE_SESSION,  /* セッションごとに一つ。身元は要求した者の利用者とセッション。 */
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
    const char *const *needs;           /* 先に READY でなければならないサービス。NULL 終わり。 */
    struct failure_rule on_failure;
    int clients_from;                   /* この級以上は接続してよい。CLIENTS_NONE は誰も。 */
    const char *const *client_services; /* 級によらず接続してよいサービス。NULL 終わり。 */
    bool (*function_matches)(const struct n_pciinfo *f); /* SCOPE_FUNCTION だけ */
};

#define CAPS(...) .caps = (const enum n_cap[]){__VA_ARGS__}, \
                  .cap_count = sizeof((const enum n_cap[]){__VA_ARGS__}) / sizeof(enum n_cap)
#define NAMES(...) (const char *const[]){__VA_ARGS__, NULL}

static bool nvidia_display(const struct n_pciinfo *f) { return nvrm_pci_is_gpu(f->vendor, f->cls, f->subcls, f->progif); }

static const struct declared_service declared[] = {
    {.name = "systemlog", .program = "systemlog", .cls = N_CLASS_SYSTEM, .at_boot = true,
     .on_failure = {5, 1000, 60000}, .clients_from = CLIENTS_NONE},
    {.name = "gpu", .program = "nvrm", .cls = N_CLASS_SYSTEM, .scope = SCOPE_FUNCTION, .at_boot = true, .optional = true,
     CAPS(N_CAP_PCI_CLAIM), .on_failure = {2, 2000, 60000}, .clients_from = N_CLASS_USER,
     .function_matches = nvidia_display},
    {.name = "accounts", .program = "accounts", .cls = N_CLASS_SYSTEM, .at_boot = true,
     CAPS(N_CAP_STORE_PRIVATE), .on_failure = {5, 500, 60000}, .clients_from = N_CLASS_USER},
    {.name = "settings", .program = "settingsd", .cls = N_CLASS_SYSTEM, .at_boot = true,
     CAPS(N_CAP_SETTINGS_APPLY, N_CAP_STORE_PRIVATE), .on_failure = {5, 500, 60000}, .clients_from = N_CLASS_USER},
    /* 試験の媒体にだけあるプログラム。C:\Nocturne\System の中にあるので、アプリは置けない。 */
    {.name = "tests", .program = "testrun", .cls = N_CLASS_SYSTEM, .at_boot = true, .optional = true,
     CAPS(N_CAP_POWER, N_CAP_PCI_CLAIM, N_CAP_IDENTITY_GRANT), .needs = NAMES("accounts"),
     .on_failure = {0, 0, 0}, .clients_from = N_CLASS_USER},
    {.name = "session", .program = "session", .cls = N_CLASS_SYSTEM, .at_boot = true, .console = true,
     CAPS(N_CAP_IDENTITY_GRANT), .needs = NAMES("accounts", "tests"),
     .on_failure = {5, 1000, 60000}, .clients_from = N_CLASS_USER},
    {.name = "sharedworkers", .program = "sharedworkers", .cls = N_CLASS_USER, .scope = SCOPE_SESSION,
     .on_failure = {3, 500, 60000}, .clients_from = N_CLASS_USER},
};
#define DECLARED (sizeof declared / sizeof *declared)
#define STOP_GRACE_MS 5000u /* 止める指示の後、これだけ待って強制的に止める */

static const struct declared_service *declared_named(const char *name) {
    for (size_t i = 0; i < DECLARED; i++)
        if (!strcmp(declared[i].name, name)) return &declared[i];
    return NULL;
}

/* プログラムの番号は表の位置から。0 は宣言に無いプログラム。 */
static uint32_t program_number(const struct declared_service *d) { return (uint32_t)(d - declared) + 1; }

/* ---- 実例 ---- */
struct pending_connection {
    struct pending_connection *next;
    n_handle channel;
    char argument[N_SERVICE_ARGUMENT_MAX];
};
struct instance {
    struct instance *next;
    const struct declared_service *declared;
    char argument[N_SERVICE_ARGUMENT_MAX];
    uint32_t user, session, cls;
    uint32_t state;              /* enum n_service_state */
    uint64_t state_since;
    uint64_t changed;            /* 状態を変えたときの generation */
    bool wanted;                 /* 起こす */
    bool broken;                 /* 依存の表が解けない */
    bool stopping;               /* 止める指示を出した */
    bool restart_after_stop;
    bool sending_watched;        /* N_EVENT_SENDABLE を見張っている */
    bool dead;                   /* 次の掃除で消す */
    n_handle app, control;
    int64_t app_id;
    int64_t last_status;
    uint32_t restarts;
    uint64_t started_at;
    uint64_t retry_at;           /* N_SERVICE_BACKOFF の期限 */
    uint64_t stop_deadline;      /* stopping の期限 */
    struct pending_connection *pending, **pending_tail;
};

enum token_kind { TOKEN_REQUESTS, TOKEN_WAKE, TOKEN_APP, TOKEN_CONTROL, TOKEN_SENDABLE };
#define TOKEN_MASK 7ull
#define TOKEN(inst, kind) ((uint64_t)(uintptr_t)(inst) | (uint64_t)(kind))

static mtx_t state_lock;
static struct instance *instances; /* 依存の順に並ぶ */
static volatile uint32_t generation;
static n_handle root_cap, grant_cap, requests, events, wake_ends[2];

static void wake_main(void) {
    char byte = 0;
    n_channel_send(wake_ends[1], &byte, 1, -1);
}

static void set_state(struct instance *in, uint32_t state) {
    if (in->state == state) return;
    in->state = state;
    in->state_since = n_clock_monotonic();
    in->changed = __atomic_add_fetch(&generation, 1, __ATOMIC_ACQ_REL);
    wake_address(&generation, INT_MAX);
}

static struct instance *instance_new(const struct declared_service *d, const char *argument, uint32_t user, uint32_t session,
                                     uint32_t cls) {
    struct instance *in = calloc(1, sizeof *in);
    if (!in) return NULL;
    in->declared = d;
    if (argument) strlcpy(in->argument, argument, sizeof in->argument);
    in->user = user;
    in->session = session;
    in->cls = cls;
    in->app = in->control = -1;
    in->pending_tail = &in->pending;
    in->state_since = n_clock_monotonic();
    struct instance **at = &instances;
    while (*at) at = &(*at)->next;
    *at = in;
    return in;
}

static struct instance *instance_find(const struct declared_service *d, const char *argument, uint32_t session) {
    for (struct instance *in = instances; in; in = in->next)
        if (!in->dead && in->declared == d && !strcmp(in->argument, argument ? argument : "") &&
            (d->scope != SCOPE_SESSION || in->session == session))
            return in;
    return NULL;
}

/* 表の依存を解いて、起こす順に実例を作る。解けない項目は壊れた実例になり、理由をログへ書く。 */
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
    if (d->scope == SCOPE_SESSION) return; /* 要求が来たときに作る */
    if (d->scope == SCOPE_MACHINE) {
        struct instance *in = instance_new(d, NULL, 0, 0, d->cls);
        if (in) {
            in->broken = broken;
            in->wanted = d->at_boot;
        }
        return;
    }
    for (int i = 0; i < function_count; i++) {
        if (!d->function_matches(&functions[i])) continue;
        char argument[8];
        snprintf(argument, sizeof argument, "%04x", (unsigned)N_PCI_TARGET(functions[i].bus, functions[i].dev, functions[i].func));
        struct instance *in = instance_new(d, argument, 0, 0, d->cls);
        if (in) {
            in->broken = broken;
            in->wanted = d->at_boot;
        }
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
static bool need_met(const char *name) {
    const struct declared_service *d = declared_named(name);
    for (struct instance *in = instances; in; in = in->next) {
        if (in->declared != d) continue;
        if (in->state != N_SERVICE_RUNNING && in->state != N_SERVICE_ABSENT) return false;
    }
    /* 実例が一つも無い宣言（合う装置が無い）は満たされている */
    return true;
}

static bool needs_met(const struct declared_service *d) {
    for (const char *const *need = d->needs; need && *need; need++)
        if (!need_met(*need)) return false;
    return true;
}

static void watch_app(struct instance *in) {
    n_events_watch(events, in->app, N_EVENT_APP_ENDED, TOKEN(in, TOKEN_APP));
    n_events_watch(events, in->control, N_EVENT_RECEIVABLE, TOKEN(in, TOKEN_CONTROL));
}

static void unwatch_app(struct instance *in) {
    n_events_unwatch(events, TOKEN(in, TOKEN_APP));
    n_events_unwatch(events, TOKEN(in, TOKEN_CONTROL));
    if (in->sending_watched) n_events_unwatch(events, TOKEN(in, TOKEN_SENDABLE));
    in->sending_watched = false;
}

static n_result give_caps(struct instance *in) {
    const struct declared_service *d = in->declared;
    for (uint32_t i = 0; i < d->cap_count; i++) {
        n_handle cap = n_cap_derive(root_cap, d->caps[i]);
        if (cap < 0) return cap;
        struct n_service_control message = {.kind = N_SERVICE_CTL_CAP, .value = d->caps[i]};
        n_result r = n_channel_send(in->control, &message, sizeof message, cap);
        n_handle_close(cap);
        if (r < 0) return r;
    }
    struct n_service_control done = {.kind = N_SERVICE_CTL_CAPS_DONE};
    n_result r = n_channel_send(in->control, &done, sizeof done, -1);
    return r < 0 ? r : 0;
}

static void give_up(struct instance *in, const char *why, n_result result) {
    in->last_status = result;
    in->wanted = false;
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
    n_result found = n_entry_info(path, &e);
    if (found < 0 && d->optional) {
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
    int32_t handles[4] = {d->console ? 0 : log, d->console ? 1 : log, d->console ? 2 : log, ends[1]};
    const char *arguments[2] = {d->program, in->argument};
    const char *settings[2] = {"FOLDER=C:\\", "TERMINAL=0 1 2"};
    struct n_identity identity = {.user = in->user, .session = in->session, .cls = in->cls, .program = program_number(d)};
    struct n_launch launch = {.arguments = arguments, .settings = settings, .handles = handles,
                              .argument_count = in->argument[0] ? 2 : 1, .setting_count = d->console && n_terminal(1) ? 2 : 1,
                              .handle_count = 4, .identity = &identity, .identity_cap = grant_cap};
    n_handle app = n_app_launch(path, &launch);
    if (log >= 0) n_handle_close(log);
    n_handle_close(ends[1]);
    if (app < 0) {
        n_handle_close(ends[0]);
        give_up(in, "cannot start", app);
        return;
    }
    in->app = app;
    in->control = ends[0];
    in->app_id = n_app_id(app);
    in->started_at = n_clock_monotonic();
    in->stopping = false;
    r = give_caps(in);
    if (r < 0) {
        printf("services: %s: cannot give its capabilities (%s)\n", d->name, n_result_text(r));
        n_app_stop(app, N_APP_STOP_TREE);
    }
    watch_app(in);
    set_state(in, N_SERVICE_STARTING);
    if (in->pending && !in->sending_watched) {
        n_events_watch(events, in->control, N_EVENT_SENDABLE, TOKEN(in, TOKEN_SENDABLE));
        in->sending_watched = true;
    }
}

static void stop(struct instance *in) {
    in->wanted = false;
    if (in->app < 0) {
        if (in->state != N_SERVICE_ABSENT) set_state(in, N_SERVICE_STOPPED);
        return;
    }
    struct n_service_control message = {.kind = N_SERVICE_CTL_STOP};
    in->stopping = true;
    in->stop_deadline = n_clock_after(STOP_GRACE_MS * N_NS_PER_MS);
    /* 詰まっている相手には送らない：期限で止める */
    if (!in->pending) n_channel_send(in->control, &message, sizeof message, -1);
}

/* 起こせるものを起こす。掃除もここ。表の順は依存の順。 */
static void advance(void) {
    uint64_t now = n_clock_monotonic();
    for (struct instance *in = instances; in; in = in->next) {
        if (in->dead) continue;
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
    for (struct instance **at = &instances; *at;) {
        struct instance *in = *at;
        if (!in->dead) {
            at = &in->next;
            continue;
        }
        *at = in->next;
        free(in);
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

static void pending_close_all(struct instance *in) {
    while (in->pending) {
        struct pending_connection *p = in->pending;
        in->pending = p->next;
        n_handle_close(p->channel);
        free(p);
    }
    in->pending_tail = &in->pending;
}

/* app が終わった。 */
static void app_ended(struct instance *in) {
    const struct declared_service *d = in->declared;
    int status = 0;
    n_result r = n_app_result(in->app, 0, &status);
    if (r == -N_ERR_NOT_READY) return;
    unwatch_app(in);
    n_handle_close(in->app);
    n_handle_close(in->control);
    in->app = in->control = -1;
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
        in->wanted = in->pending != NULL;
        set_state(in, in->wanted ? N_SERVICE_WAITING : N_SERVICE_STOPPED);
        if (!in->wanted && d->scope == SCOPE_SESSION) in->dead = true;
        return;
    }
    if (n_clock_monotonic() - in->started_at >= (uint64_t)d->on_failure.healthy_ms * N_NS_PER_MS) in->restarts = 0;
    printf("services: %s%s%s ended (%lld), restart %u of %u\n", d->name, in->argument[0] ? ":" : "", in->argument,
           (long long)in->last_status, in->restarts, d->on_failure.restarts);
    fflush(stdout);
    if (in->restarts >= d->on_failure.restarts) {
        pending_close_all(in);
        give_up(in, "too many failures", r < 0 ? r : -N_ERR_STOPPED);
        return;
    }
    in->retry_at = n_clock_after((uint64_t)d->on_failure.interval_ms * N_NS_PER_MS);
    set_state(in, N_SERVICE_BACKOFF);
}

static void control_receive(struct instance *in) {
    for (;;) {
        struct n_service_control message;
        n_handle attached = -1;
        n_result r = n_channel_receive(in->control, &message, sizeof message, 0, &attached);
        if (attached >= 0) n_handle_close(attached);
        if (r < 0) return;
        if (r == sizeof message && message.kind == N_SERVICE_CTL_READY && in->state == N_SERVICE_STARTING)
            set_state(in, N_SERVICE_RUNNING);
    }
}

/* 待っている接続を一通送る。残れば次の N_EVENT_SENDABLE で続ける。 */
static void send_one_pending(struct instance *in) {
    struct pending_connection *p = in->pending;
    if (p) {
        struct n_service_control message = {.kind = N_SERVICE_CTL_CONNECTION};
        strlcpy(message.argument, p->argument, sizeof message.argument);
        n_channel_send(in->control, &message, sizeof message, p->channel);
        n_handle_close(p->channel);
        in->pending = p->next;
        if (!in->pending) in->pending_tail = &in->pending;
        free(p);
    }
    if (!in->pending && in->sending_watched) {
        n_events_unwatch(events, TOKEN(in, TOKEN_SENDABLE));
        in->sending_watched = false;
    }
}

static void deliver(struct instance *in, n_handle channel, const char *argument) {
    if (in->state == N_SERVICE_FAILED || in->state == N_SERVICE_ABSENT) {
        n_handle_close(channel);
        return;
    }
    struct pending_connection *p = malloc(sizeof *p);
    if (!p) {
        n_handle_close(channel);
        return;
    }
    p->next = NULL;
    p->channel = channel;
    strlcpy(p->argument, argument ? argument : "", sizeof p->argument);
    *in->pending_tail = p;
    in->pending_tail = &p->next;
    in->wanted = true;
    if (in->control >= 0 && !in->sending_watched) {
        n_events_watch(events, in->control, N_EVENT_SENDABLE, TOKEN(in, TOKEN_SENDABLE));
        in->sending_watched = true;
    }
}

/* ---- 宣言に照らす ---- */
static bool client_allowed(const struct declared_service *d, const struct n_identity *peer) {
    if (d->clients_from != CLIENTS_NONE && peer->cls >= (uint32_t)d->clients_from) return true;
    for (const char *const *name = d->client_services; name && *name; name++) {
        const struct declared_service *other = declared_named(*name);
        if (other && program_number(other) == peer->program) return true;
    }
    return false;
}

static void manager_client(n_handle channel);

static void request_one(const struct n_service_request *request, n_handle channel) {
    char name[N_SERVICE_NAME_MAX], argument[N_SERVICE_ARGUMENT_MAX] = "";
    size_t bytes = request->name_bytes;
    const char *colon = memchr(request->name, ':', bytes);
    size_t name_bytes = colon ? (size_t)(colon - request->name) : bytes;
    if (name_bytes >= sizeof name || (colon && bytes - name_bytes - 1 >= sizeof argument)) {
        n_handle_close(channel);
        return;
    }
    memcpy(name, request->name, name_bytes);
    name[name_bytes] = 0;
    if (colon) {
        memcpy(argument, colon + 1, bytes - name_bytes - 1);
        argument[bytes - name_bytes - 1] = 0;
    }
    if (!strcmp(name, N_SERVICE_MANAGER)) {
        manager_client(channel);
        return;
    }
    struct n_identity peer;
    const struct declared_service *d = declared_named(name);
    if (!d || n_channel_peer(channel, &peer) < 0 || !client_allowed(d, &peer)) {
        n_handle_close(channel);
        return;
    }
    if (d->scope == SCOPE_SESSION && !peer.session) {
        n_handle_close(channel);
        return;
    }
    struct instance *in = instance_find(d, d->scope == SCOPE_FUNCTION ? argument : NULL, peer.session);
    if (!in && d->scope == SCOPE_SESSION) {
        uint32_t cls = peer.cls < (uint32_t)d->cls ? peer.cls : (uint32_t)d->cls;
        in = instance_new(d, NULL, peer.user, peer.session, cls);
    }
    if (!in) {
        n_handle_close(channel);
        return;
    }
    deliver(in, channel, argument);
}

static void requests_receive(void) {
    for (;;) {
        struct n_service_request request;
        n_handle attached = -1;
        n_result r = n_channel_receive(requests, &request, sizeof request, 0, &attached);
        if (r < 0) return;
        if (attached < 0) continue;
        if (r != sizeof request || request.name_bytes >= sizeof request.name) {
            n_handle_close(attached);
            continue;
        }
        request_one(&request, attached);
    }
}

/* ---- 管理者の口：一人の相手に一つの thread ---- */
static void status_of(const struct instance *in, struct n_service_status *out) {
    const struct declared_service *d = in->declared;
    *out = (struct n_service_status){.cls = in->cls, .state = in->state, .user = in->user, .session = in->session,
                                     .restarts = in->restarts, .restarts_allowed = d->on_failure.restarts,
                                     .app = in->app_id, .last_status = in->last_status, .state_since = in->state_since};
    strlcpy(out->name, d->name, sizeof out->name);
    strlcpy(out->argument, in->argument, sizeof out->argument);
    strlcpy(out->program, d->program, sizeof out->program);
}

/* 錠の下で写す。送るのは錠の外。 */
/* newer_than より後に変わった実例だけ。-1 は全部。 */
static n_result snapshot(const char *name, const char *argument, uint32_t session, int64_t newer_than, struct n_service_status **out) {
    mtx_lock(&state_lock);
    size_t count = 0;
    for (struct instance *in = instances; in; in = in->next) count++;
    struct n_service_status *all = malloc((count ? count : 1) * sizeof *all);
    size_t n = 0;
    if (all) {
        for (struct instance *in = instances; in; in = in->next) {
            if (in->dead || (int64_t)in->changed <= newer_than) continue;
            if (name && (strcmp(in->declared->name, name) || strcmp(in->argument, argument) ||
                         (in->declared->scope == SCOPE_SESSION && in->session != session)))
                continue;
            status_of(in, &all[n++]);
        }
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

static n_result change_instance(uint32_t op, const char *name, const char *argument, uint32_t session) {
    const struct declared_service *d = declared_named(name);
    if (!d) return -N_ERR_NOT_FOUND;
    mtx_lock(&state_lock);
    struct instance *in = instance_find(d, d->scope == SCOPE_FUNCTION ? argument : NULL, session);
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

static n_result stop_session(uint32_t session) {
    mtx_lock(&state_lock);
    for (struct instance *in = instances; in; in = in->next)
        if (!in->dead && in->session == session) {
            stop(in);
            if (in->app < 0) in->dead = true;
        }
    mtx_unlock(&state_lock);
    wake_main();
    return 0;
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
        n_result count = snapshot(NULL, NULL, 0, seen, &all);
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

static void client_thread(void *arg) {
    n_handle channel = (n_handle)(intptr_t)arg;
    struct n_identity peer;
    if (n_channel_peer(channel, &peer) < 0) {
        n_handle_close(channel);
        return;
    }
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
            count = snapshot(NULL, NULL, 0, -1, &all);
            break;
        case N_SERVICES_STATUS:
            count = snapshot(request.name, request.argument, request.session, -1, &all);
            break;
        case N_SERVICES_RESTART:
        case N_SERVICES_STOP:
            reply.result = peer.cls >= N_CLASS_ADMIN ? change_instance(request.op, request.name, request.argument, request.session)
                                                     : -N_ERR_DENIED;
            break;
        case N_SERVICES_STOP_SESSION:
            reply.result = peer.cls >= N_CLASS_SYSTEM ? stop_session(request.session) : -N_ERR_DENIED;
            break;
        case N_SERVICES_WATCH:
            reply.result = 0;
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

static void manager_client(n_handle channel) {
    n_result thread = thread_create(client_thread, (void *)(intptr_t)channel);
    if (thread < 0) {
        n_handle_close(channel);
        return;
    }
    thread_detach((int)thread);
}

/* ---- 主のループ ---- */
static void report(const struct n_event *e) {
    uint64_t kind = e->token & TOKEN_MASK;
    struct instance *in = (struct instance *)(uintptr_t)(e->token & ~TOKEN_MASK);
    if (in && in->dead) return;
    switch (kind) {
    case TOKEN_REQUESTS:
        requests_receive();
        break;
    case TOKEN_WAKE: {
        char bytes[64];
        while (n_channel_receive(wake_ends[0], bytes, sizeof bytes, N_CHANNEL_DISCARD, NULL) >= 0) {}
        break;
    }
    case TOKEN_APP:
        if (in->app >= 0) app_ended(in);
        break;
    case TOKEN_CONTROL:
        if (in->control >= 0) control_receive(in);
        break;
    case TOKEN_SENDABLE:
        if (in->control >= 0) send_one_pending(in);
        break;
    }
}

int main(void) {
    root_cap = N_ROOT_CAP_HANDLE;
    printf("\n\x1b[1;35m*\x1b[0m Welcome to \x1b[1mNocturne \x1b[0m- a small operating system for quiet nights.\n");
    fflush(stdout);
    if (mtx_init(&state_lock, mtx_plain) != thrd_success || (events = n_events_create()) < 0) return 1;
    n_handle service_root = n_cap_derive(root_cap, N_CAP_SERVICE_ROOT);
    grant_cap = n_cap_derive(root_cap, N_CAP_IDENTITY_GRANT);
    requests = service_root >= 0 ? (n_handle)__syscall(SYS_SERVICE_REGISTER, service_root, 0, 0, 0, 0) : service_root;
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
