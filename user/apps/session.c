/* セッションのサービス：ログインの窓を出し、選ばれた利用者の身元でデスクトップのシェルを起こす。
   シェルの名前の口はそのセッションの中でだけ引ける（§14）。一度に一つのセッション。
   シェルが落ちたら起こし直す。N_SESSION_END でシェルとその子を止め、窓へ戻る。
   画面の無い起動（nogui）では、窓の代わりにコンソールで聞いて、利用者の身元で sh を起こす。 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include "nocturne.h"
#include "nocturne_services.h"
#include "nocturne_settings.h"
#include "ui.h"

#define CONSOLE N_FOLDER_SYSTEM "\\sh"
#define DESKTOP N_FOLDER_SYSTEM "\\shell"
#define WINDOW 1u

static n_handle grant_cap, shell_cap;
static mtx_t current_lock;
/* 今のセッション。session が 0 なら無い。 */
static struct {
    uint32_t session;
    n_handle shell; /* セッションの最初のプログラム（シェルか sh）。無ければ -1。 */
} current = {.shell = -1};
static volatile uint32_t session_over;

static void end_current(void) {
    if (current.shell >= 0) n_app_stop(current.shell, N_APP_STOP_TREE);
    __atomic_store_n(&session_over, 1, __ATOMIC_RELEASE);
    wake_address(&session_over, INT_MAX);
}

/* ---- セッションの口 ---- */
static void client_serve(void *arg) {
    n_handle channel = (n_handle)(intptr_t)arg;
    struct n_identity peer;
    if (n_channel_peer(channel, &peer) < 0) {
        n_handle_close(channel);
        return;
    }
    for (;;) {
        struct n_session_request q;
        n_result got = n_channel_receive(channel, &q, sizeof q, N_CHANNEL_WAIT, NULL);
        if (got < 0 || got != sizeof q) break;
        struct n_session_reply reply = {.result = -N_ERR_INVALID};
        if (q.op == N_SESSION_END) {
            mtx_lock(&current_lock);
            reply.result = current.session && peer.session == current.session ? 0 : -N_ERR_DENIED;
            if (reply.result == 0) end_current();
            mtx_unlock(&current_lock);
        }
        if (n_channel_send(channel, &reply, sizeof reply, -1) < 0) break;
    }
    n_handle_close(channel);
}

static void connections_serve(void *arg) {
    (void)arg;
    for (;;) {
        n_handle channel = n_inlet_accept(N_SERVICE_HANDLE_CONTROL, N_CHANNEL_WAIT);
        if (channel < 0) {
            /* 管理者が閉じた：セッションを片付けて終わる */
            mtx_lock(&current_lock);
            end_current();
            mtx_unlock(&current_lock);
            exit(0);
        }
        n_result thread = thread_create(client_serve, (void *)(intptr_t)channel);
        if (thread < 0) n_handle_close(channel);
        else thread_detach((int)thread);
    }
}

/* ---- セッションの始めと終わり ---- */
static uint32_t next_session;
#define STOP_WAIT_NS (5 * N_NS_PER_S)
#define STOP_POLL_NS (20 * N_NS_PER_MS)

/* プログラムを current.shell にして終わりまで待つ。終わりの結果、起こせなければ負の n_result。 */
static n_result session_program(const char *path, const struct n_launch *launch) {
    n_handle app = n_app_launch(path, launch);
    if (app < 0) {
        printf("session: cannot start %s (%s)\n", path, n_result_text(app));
        fflush(stdout);
        return app;
    }
    mtx_lock(&current_lock);
    current.shell = app;
    /* N_SESSION_END が起こす前に来ていれば、止める相手はここで初めて決まる */
    if (__atomic_load_n(&session_over, __ATOMIC_ACQUIRE)) n_app_stop(app, N_APP_STOP_TREE);
    mtx_unlock(&current_lock);
    int status = 0;
    n_app_result(app, N_CLOCK_NEVER, &status);
    mtx_lock(&current_lock);
    current.shell = -1;
    mtx_unlock(&current_lock);
    n_handle_close(app);
    return status;
}

static void desktop_session(const struct n_identity *identity, const struct n_account *a) {
    const char *arguments[1] = {"shell"};
    int32_t handles[4] = {-1, -1, -1, shell_cap};
    struct n_launch launch = {.arguments = arguments, .handles = handles, .argument_count = 1, .handle_count = 4,
                              .folder = a->folder, .identity = identity, .identity_grant = grant_cap};
    while (!__atomic_load_n(&session_over, __ATOMIC_ACQUIRE)) {
        if (session_program(DESKTOP, &launch) < 0) return;
        if (__atomic_load_n(&session_over, __ATOMIC_ACQUIRE)) return;
        printf("session: the desktop's shell ended: it starts again\n");
        fflush(stdout);
        n_sleep_until(n_clock_after(N_NS_PER_S));
    }
}

static void shell_session(const struct n_identity *identity, const struct n_account *a) {
    static const int32_t terminals[] = {0, 1, 2};
    const char *arguments[1] = {"sh"};
    int32_t handles[3] = {0, 1, 2};
    struct n_launch launch = {.arguments = arguments, .handles = handles, .argument_count = 1, .handle_count = 3,
                              .folder = a->folder, .terminals = terminals, .terminal_count = n_terminal(1) ? 3 : 0,
                              .identity = identity, .identity_grant = grant_cap};
    n_result status = session_program(CONSOLE, &launch);
    if (status < 0) return;
    printf("\nsession: the shell ended (%d)\n", (int)status);
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
    if (!++next_session) ++next_session;
    struct n_identity identity = {.user = a->user, .session = next_session, .cls = a->cls};
    __atomic_store_n(&session_over, 0, __ATOMIC_RELEASE);
    mtx_lock(&current_lock);
    current.session = identity.session;
    mtx_unlock(&current_lock);
    /* その人の利用者の値を効かせる。設定が届かなくても入れる：既定の値で動く。 */
    r = n_setting_session(a->user);
    if (r < 0) {
        printf("session: %s's settings are not applied (%s)\n", a->name, n_result_text(r));
        fflush(stdout);
    }
    if (windows) desktop_session(&identity, a);
    else shell_session(&identity, a);
    /* セッションの身元のアプリを全部止める。起こし直された前のシェルの子もここで終わる。 */
    uint64_t give_up = n_clock_after(STOP_WAIT_NS);
    n_result left;
    while ((left = n_session_stop(grant_cap, identity.session)) > 0 && n_clock_monotonic() < give_up)
        n_sleep_until(n_clock_after(STOP_POLL_NS));
    if (left != 0) {
        printf("session: %s's apps did not end (%d left)\n", a->name, (int)left);
        fflush(stdout);
    }
    mtx_lock(&current_lock);
    current.session = 0;
    mtx_unlock(&current_lock);
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
    n_result r = mtx_init(&current_lock, mtx_plain) == thrd_success ? 0 : -N_ERR_NO_MEMORY;
    if (r >= 0) r = grant_cap = n_cap_find(N_CAP_IDENTITY_GRANT);
    if (r >= 0) r = shell_cap = n_cap_find(N_CAP_SHELL);
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
