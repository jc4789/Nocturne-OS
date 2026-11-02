/* セッションのサービス：ログインの窓を出し、選ばれた利用者の身元でデスクトップのプロセスを起こす。
   一度に一つのセッション。デスクトップが結果 0 で終わるとログアウトで、窓へ戻る。
   落ちたら同じセッションのまま起こし直す。
   画面の無い起動（nogui）では、窓の代わりにコンソールで聞いて、シェルを起こす。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include "nocturne.h"
#include "nocturne_services.h"
#include "ui.h"

#define DESKTOP N_FOLDER_SYSTEM "\\desktop"
#define SHELL N_FOLDER_SYSTEM "\\sh"
#define WINDOW 1u
#define REQUEST_MAX (sizeof(struct n_session_request) + 2 * N_SESSION_PATH_MAX)

static n_handle grant_cap;
static mtx_t current_lock;
/* 今のセッション。session が 0 なら無い。 */
static struct {
    uint32_t session;
    n_handle desktop;    /* app */
    n_handle to_desktop; /* デスクトップへの起動の依頼の channel */
} current = {.desktop = -1, .to_desktop = -1};

/* ---- セッションの口 ---- */
static void reply_and_close(n_handle channel, n_result result) {
    struct n_session_reply reply = {.result = result};
    n_channel_send(channel, &reply, sizeof reply, -1);
    n_handle_close(channel);
}

static void client_serve(void *arg) {
    n_handle channel = (n_handle)(intptr_t)arg;
    struct n_identity peer;
    char *message = malloc(REQUEST_MAX);
    if (!message || n_channel_peer(channel, &peer) < 0) {
        free(message);
        n_handle_close(channel);
        return;
    }
    n_result got = n_channel_receive(channel, message, REQUEST_MAX, N_CHANNEL_WAIT, NULL);
    const struct n_session_request *q = (const void *)message;
    if (got < (n_result)sizeof *q || q->path_bytes >= N_SESSION_PATH_MAX || q->argument_bytes >= N_SESSION_PATH_MAX ||
        (size_t)got != sizeof *q + q->path_bytes + q->argument_bytes) {
        free(message);
        reply_and_close(channel, -N_ERR_PROTOCOL);
        return;
    }
    mtx_lock(&current_lock);
    bool own = current.session && peer.session == current.session;
    n_result result = own ? 0 : -N_ERR_DENIED;
    n_handle forwarded = -1;
    if (own && q->op == N_SESSION_LAUNCH) {
        /* 相手の channel ごとデスクトップへ渡す。答えるのはデスクトップ。 */
        n_result sent = n_channel_send(current.to_desktop, message, (size_t)got, channel);
        if (sent >= 0) forwarded = channel;
        else result = sent;
    } else if (own && q->op == N_SESSION_END) {
        n_app_stop(current.desktop, N_APP_STOP_TREE);
    } else if (own) {
        result = -N_ERR_INVALID;
    }
    mtx_unlock(&current_lock);
    free(message);
    if (forwarded >= 0) n_handle_close(forwarded);
    else reply_and_close(channel, result);
}

static void connections_serve(void *arg) {
    (void)arg;
    n_handle queue = n_events_create();
    if (queue < 0) return;
    n_events_watch(queue, N_SERVICE_HANDLE_CONTROL, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, 1);
    for (;;) {
        struct n_event report;
        if (n_events_wait(queue, &report, 1, N_CLOCK_NEVER) < 0) exit(1);
        for (;;) {
            n_handle channel = n_connector_accept(N_SERVICE_HANDLE_CONTROL, NULL, 0);
            if (channel == -N_ERR_NOT_READY) break;
            if (channel < 0) {
                mtx_lock(&current_lock);
                if (current.desktop >= 0) n_app_stop(current.desktop, N_APP_STOP_TREE);
                mtx_unlock(&current_lock);
                exit(0);
            }
            n_result thread = thread_create(client_serve, (void *)(intptr_t)channel);
            if (thread < 0) n_handle_close(channel);
            else thread_detach((int)thread);
        }
    }
}

/* ---- セッションの始めと終わり ---- */
static uint32_t next_session;

/* デスクトップが結果 0 で終わるまで。 */
static void desktop_session(const struct n_account *a, uint32_t session) {
    struct n_identity identity = {.user = a->user, .session = session, .cls = a->cls};
    char folder[sizeof "FOLDER=" + N_ACCOUNT_FOLDER_MAX];
    snprintf(folder, sizeof folder, "FOLDER=%s", a->folder);
    const char *arguments[1] = {"desktop"}, *settings[1] = {folder};
    for (;;) {
        n_handle ends[2];
        if (n_channel_pair(ends) < 0) return;
        n_handle log = n_system_log_open();
        int32_t handles[4] = {-1, log, log, ends[1]};
        struct n_launch launch = {.arguments = arguments, .settings = settings, .handles = handles, .argument_count = 1,
                                  .setting_count = 1, .handle_count = 4, .identity = &identity, .identity_cap = grant_cap};
        n_handle app = n_app_launch(DESKTOP, &launch);
        if (log >= 0) n_handle_close(log);
        n_handle_close(ends[1]);
        if (app < 0) {
            printf("session: cannot start " DESKTOP " (%s)\n", n_result_text(app));
            fflush(stdout);
            n_handle_close(ends[0]);
            return;
        }
        mtx_lock(&current_lock);
        current.session = session;
        current.desktop = app;
        current.to_desktop = ends[0];
        mtx_unlock(&current_lock);
        int status = 0;
        n_result r = n_app_result(app, N_CLOCK_NEVER, &status);
        mtx_lock(&current_lock);
        current.session = 0;
        current.desktop = current.to_desktop = -1;
        mtx_unlock(&current_lock);
        n_handle_close(ends[0]);
        n_handle_close(app);
        if (r >= 0 && status == 0) return;
        printf("session: the desktop ended (%d); starting it again\n", status);
        fflush(stdout);
    }
}

static void shell_session(const struct n_account *a, uint32_t session) {
    struct n_identity identity = {.user = a->user, .session = session, .cls = a->cls};
    char folder[sizeof "FOLDER=" + N_ACCOUNT_FOLDER_MAX];
    snprintf(folder, sizeof folder, "FOLDER=%s", a->folder);
    const char *arguments[1] = {"sh"}, *settings[2] = {folder, "TERMINAL=0 1 2"};
    int32_t handles[3] = {0, 1, 2};
    struct n_launch launch = {.arguments = arguments, .settings = settings, .handles = handles, .argument_count = 1,
                              .setting_count = n_terminal(1) ? 2 : 1, .handle_count = 3, .identity = &identity, .identity_cap = grant_cap};
    n_handle app = n_app_launch(SHELL, &launch);
    if (app < 0) {
        printf("session: cannot start " SHELL " (%s)\n", n_result_text(app));
        fflush(stdout);
        return;
    }
    int status = 0;
    n_app_result(app, N_CLOCK_NEVER, &status);
    n_handle_close(app);
    printf("\nsession: the shell ended (%d)\n", status);
    fflush(stdout);
}

static void session_run(const struct n_account *a, bool windows) {
    struct n_accounts_request prepare = {.op = N_ACCOUNTS_PREPARE, .user = a->user};
    struct n_accounts_reply reply;
    n_result r = n_accounts_call(&prepare, &reply);
    if (r < 0) {
        printf("session: %s's folder is not ready (%s)\n", a->name, n_result_text(r));
        fflush(stdout);
    }
    uint32_t session = ++next_session;
    if (windows) desktop_session(a, session);
    else shell_session(a, session);
    n_services_control(N_SERVICES_STOP_SESSION, NULL, NULL, session);
}

/* 起動のときに自動で入る利用者。 */
static bool auto_account(struct n_account *out) {
    struct n_account *list;
    n_result count = n_accounts_list(&list);
    bool found = false;
    for (n_result i = 0; i < count && !found; i++)
        if (list[i].auto_login) {
            *out = list[i];
            found = true;
        }
    if (count >= 0) free(list);
    return found;
}

/* ---- ログインの窓 ---- */
struct login {
    struct ui_session *ui;
    struct n_account *accounts;
    n_result count;
    int chosen;
    char name[N_ACCOUNT_NAME_MAX], password[N_ACCOUNT_PASSWORD_MAX], confirm[N_ACCOUNT_PASSWORD_MAX];
    char error[128];
    uint32_t name_node, password_node, confirm_node, go_node, first_item;
};

static void login_show(struct login *l) {
    bool first_user = l->count == 0;
    struct ui_msg m;
    ui_view(&m, WINDOW);
    ui_node(&m, "column"); ui_num(&m, "padding", 24); ui_num(&m, "gap", 12);
    ui_node(&m, "text"); ui_str(&m, "text", first_user ? "Welcome to Nocturne" : "Log in"); ui_str(&m, "style", "heading"); ui_end(&m);
    if (first_user) {
        ui_node(&m, "text"); ui_str(&m, "text", "Make the first user. This user administers the machine."); ui_num(&m, "wrap", 1); ui_end(&m);
        l->name_node = ui_node(&m, "field"); ui_str(&m, "value", l->name); ui_str(&m, "placeholder", "Name"); ui_end(&m);
    } else {
        l->first_item = m.nodes;
        for (n_result i = 0; i < l->count; i++) {
            ui_node(&m, "item"); ui_str(&m, "text", l->accounts[i].name); ui_str(&m, "icon", "home");
            ui_num(&m, "selected", i == l->chosen);
            ui_end(&m);
        }
    }
    l->password_node = ui_node(&m, "field"); ui_str(&m, "value", l->password); ui_str(&m, "placeholder", "Password");
    ui_num(&m, "secret", 1); ui_end(&m);
    if (first_user) {
        l->confirm_node = ui_node(&m, "field"); ui_str(&m, "value", l->confirm); ui_str(&m, "placeholder", "Password again");
        ui_num(&m, "secret", 1); ui_end(&m);
    }
    if (l->error[0]) {
        ui_node(&m, "text"); ui_str(&m, "text", l->error); ui_num(&m, "colour", ui_theme.danger); ui_end(&m);
    }
    l->go_node = ui_node(&m, "button"); ui_str(&m, "text", first_user ? "Create" : "Log in"); ui_str(&m, "variant", "primary"); ui_end(&m);
    ui_end(&m);
    ui_send(l->ui, &m);
}

static void login_error(struct login *l, const char *what, n_result r) {
    snprintf(l->error, sizeof l->error, "%s: %s", what, n_result_text(r));
    memset(l->password, 0, sizeof l->password);
    memset(l->confirm, 0, sizeof l->confirm);
}

/* 合っていれば out を書いて true。 */
static bool login_go(struct login *l, struct n_account *out) {
    struct n_accounts_request q = {0};
    struct n_accounts_reply reply;
    strlcpy(q.password, l->password, sizeof q.password);
    n_result r;
    if (!l->count) {
        if (strcmp(l->password, l->confirm)) {
            login_error(l, "The passwords differ", -N_ERR_INVALID);
            return false;
        }
        q.op = N_ACCOUNTS_CREATE;
        q.cls = N_CLASS_ADMIN;
        strlcpy(q.name, l->name, sizeof q.name);
        r = n_accounts_call(&q, &reply);
        if (r < 0) login_error(l, "This user cannot be made", r);
    } else {
        q.op = N_ACCOUNTS_VERIFY;
        strlcpy(q.name, l->accounts[l->chosen].name, sizeof q.name);
        r = n_accounts_call(&q, &reply);
        if (r < 0) login_error(l, "That is not the password", r);
    }
    memset(&q, 0, sizeof q);
    if (r < 0) return false;
    *out = reply.account;
    return true;
}

static bool login_window(struct n_account *out) {
    struct login l = {.chosen = 0};
    l.count = n_accounts_list(&l.accounts);
    if (l.count < 0) {
        printf("session: no list of users (%s)\n", n_result_text(l.count));
        fflush(stdout);
        return false;
    }
    l.ui = ui_session_new();
    bool done = false;
    if (l.ui && ui_window(l.ui, WINDOW, 420, l.count ? 300 + 40 * (int)l.count : 380, WIN_CENTER, "Nocturne", NULL, NULL) >= 0) {
        login_show(&l);
        while (!done) {
            ui_session_wait(l.ui, -1);
            struct ui_event e;
            char text[N_ACCOUNT_PASSWORD_MAX];
            bool changed = false;
            while (!done && ui_next_event(l.ui, &e, text, sizeof text)) {
                if (e.type == UI_EVENT_TEXT) {
                    if (e.node == l.password_node) strlcpy(l.password, text, sizeof l.password);
                    else if (!l.count && e.node == l.confirm_node) strlcpy(l.confirm, text, sizeof l.confirm);
                    else if (!l.count && e.node == l.name_node) strlcpy(l.name, text, sizeof l.name);
                    changed = true;
                } else if (e.type == UI_EVENT_CLICK && l.count && e.node >= l.first_item && e.node < l.first_item + (uint32_t)l.count) {
                    l.chosen = (int)(e.node - l.first_item);
                    memset(l.password, 0, sizeof l.password);
                    l.error[0] = 0;
                    changed = true;
                } else if ((e.type == UI_EVENT_CLICK && e.node == l.go_node) || e.type == UI_EVENT_SUBMIT) {
                    l.error[0] = 0;
                    done = login_go(&l, out);
                    changed = true;
                } else if (e.type == UI_EVENT_CLOSE && (e.flags & UI_GONE)) {
                    /* 窓は閉じられない：作り直す */
                    ui_window(l.ui, WINDOW, 420, l.count ? 300 + 40 * (int)l.count : 380, WIN_CENTER, "Nocturne", NULL, NULL);
                    changed = true;
                }
                memset(text, 0, sizeof text);
            }
            if (changed && !done) login_show(&l);
        }
    }
    if (l.ui) ui_session_free(l.ui);
    if (l.count >= 0) free(l.accounts);
    memset(l.password, 0, sizeof l.password);
    memset(l.confirm, 0, sizeof l.confirm);
    return done;
}

/* ---- コンソールのログイン ---- */
static bool console_line(const char *prompt, char *out, size_t cap) {
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(out, (int)cap, stdin)) return false;
    out[strcspn(out, "\r\n")] = 0;
    return true;
}

static bool console_login(struct n_account *out) {
    struct n_accounts_request q = {0};
    struct n_accounts_reply reply;
    struct n_account *list;
    n_result count = n_accounts_list(&list);
    if (count < 0) return false;
    free(list);
    if (!count) printf("Make the first user. This user administers the machine.\n");
    if (!console_line("Name: ", q.name, sizeof q.name) || !console_line("Password: ", q.password, sizeof q.password)) return false;
    q.op = count ? N_ACCOUNTS_VERIFY : N_ACCOUNTS_CREATE;
    q.cls = N_CLASS_ADMIN;
    n_result r = n_accounts_call(&q, &reply);
    memset(&q, 0, sizeof q);
    if (r < 0) {
        printf("%s\n", n_result_text(r));
        fflush(stdout);
        return false;
    }
    *out = reply.account;
    return true;
}

int main(void) {
    n_result r = n_service_begin();
    if (r >= 0 && mtx_init(&current_lock, mtx_plain) != thrd_success) r = -N_ERR_NO_MEMORY;
    if (r >= 0) r = grant_cap = n_service_cap(N_CAP_IDENTITY_GRANT);
    n_result thread = r >= 0 ? thread_create(connections_serve, NULL) : r;
    if (thread < 0) {
        printf("session: cannot begin (%s)\n", n_result_text(thread));
        fflush(stdout);
        return 1;
    }
    thread_detach((int)thread);
    /* 前の session のサービスの実例と番号が重ならないように、時計から始める */
    next_session = (uint32_t)(n_clock_monotonic() / N_NS_PER_MS);
    n_service_ready();
    bool windows = gui_available();
    struct n_account account;
    bool automatic = auto_account(&account); /* 自動で入るのは起動の一度だけ */
    for (;;) {
        if (!automatic && !(windows ? login_window(&account) : console_login(&account))) continue;
        automatic = false;
        session_run(&account, windows);
    }
}
