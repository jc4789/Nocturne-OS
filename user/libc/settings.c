/* 設定の口（nocturne_settings.h）。サービス settings への一本の接続で、一度に一つの問いを送り、答えを待つ。 */
#include <nocturne_settings.h>
#include <nocturne_services.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

static mtx_t lock;
static once_flag lock_once = ONCE_FLAG_INIT;
static n_handle service = -1;
static uint32_t serial;
static uint8_t *answer; /* N_SETTINGS_MESSAGE_MAX の受け取り */

static void lock_init(void) { mtx_init(&lock, mtx_plain); }

static n_result connect_locked(void) {
    if (!answer && !(answer = malloc(N_SETTINGS_MESSAGE_MAX))) return -N_ERR_NO_MEMORY;
    if (service >= 0) return 0;
    n_handle h = n_service_connect(N_SETTINGS_SERVICE);
    if (h < 0) return h;
    service = h;
    return 0;
}

/* 問いを送り答えを受ける。答えの値は answer の中で、lock を持つ間だけ有効。 */
static n_result ask_locked(struct n_settings_request *m, const void *value, const void *extra,
                           const struct n_settings_reply **out) {
    n_result r = connect_locked();
    if (r < 0) return r;
    m->serial = ++serial;
    size_t size = sizeof *m + (size_t)m->length + m->extra;
    if (size > N_SETTINGS_MESSAGE_MAX) return -N_ERR_TOO_LARGE;
    uint8_t *send = malloc(size);
    if (!send) return -N_ERR_NO_MEMORY;
    memcpy(send, m, sizeof *m);
    if (m->length) memcpy(send + sizeof *m, value, m->length);
    if (m->extra) memcpy(send + sizeof *m + m->length, extra, m->extra);
    for (int attempt = 0; attempt < 2; attempt++) {
        r = n_channel_send(service, send, size, -1);
        if (r >= 0) r = n_channel_receive(service, answer, N_SETTINGS_MESSAGE_MAX, N_CHANNEL_WAIT, NULL);
        if (r != -N_ERR_CLOSED) break;
        /* サービスが起こし直された：一度だけつなぎ直す。 */
        n_handle_close(service);
        service = -1;
        n_result again = connect_locked();
        if (again < 0) {
            r = again;
            break;
        }
    }
    free(send);
    if (r < 0) return r;
    if ((size_t)r < sizeof **out) return -N_ERR_PROTOCOL;
    *out = (const struct n_settings_reply *)answer;
    return 0;
}

uint64_t n_setting_device(const char *text) {
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (; *text; text++) hash = (hash ^ (uint8_t)*text) * UINT64_C(0x100000001b3);
    return hash ? hash : 1;
}

n_result n_setting_get(struct n_setting_id id, void *out, size_t cap) {
    struct n_settings_request m = {.op = N_SETTINGS_GET, .id = id};
    const struct n_settings_reply *a;
    call_once(&lock_once, lock_init);
    mtx_lock(&lock);
    n_result r = ask_locked(&m, NULL, NULL, &a);
    if (r >= 0) r = a->result;
    if (r >= 0) {
        if (a->length > cap) r = -N_ERR_TOO_LARGE;
        else {
            if (a->length) memcpy(out, a + 1, a->length);
            r = (n_result)a->length;
        }
    }
    mtx_unlock(&lock);
    return r;
}

n_result n_setting_get_int(struct n_setting_id id, int32_t *out) {
    n_result r = n_setting_get(id, out, sizeof *out);
    return r < 0 ? r : r == (n_result)sizeof *out ? 0 : -N_ERR_PROTOCOL;
}

static n_result change(struct n_settings_request *m, const void *value, const void *extra, uint32_t *reason) {
    const struct n_settings_reply *a;
    call_once(&lock_once, lock_init);
    mtx_lock(&lock);
    n_result r = ask_locked(m, value, extra, &a);
    uint32_t why = N_SETTING_APPLY_GONE;
    if (r >= 0) {
        r = a->result;
        why = a->reason;
    }
    mtx_unlock(&lock);
    if (reason) *reason = why;
    return r;
}

n_result n_setting_set(struct n_setting_id id, const void *value, size_t length, uint32_t *reason) {
    if (length > N_SETTING_VALUE_MAX) return -N_ERR_TOO_LARGE;
    struct n_settings_request m = {.op = N_SETTINGS_SET, .length = (uint32_t)length, .id = id};
    return change(&m, value, NULL, reason);
}
n_result n_setting_set_int(struct n_setting_id id, int32_t value, uint32_t *reason) {
    return n_setting_set(id, &value, sizeof value, reason);
}
n_result n_setting_try(struct n_setting_id id, const void *value, size_t length, const void *revert_to, size_t revert_length,
                       uint32_t trial_ms, uint32_t *reason) {
    if (length > N_SETTING_VALUE_MAX || revert_length > N_SETTING_VALUE_MAX) return -N_ERR_TOO_LARGE;
    struct n_settings_request m = {.op = N_SETTINGS_TRY, .length = (uint32_t)length, .argument = trial_ms,
                                   .extra = (uint32_t)(revert_to ? revert_length : 0), .id = id};
    return change(&m, value, revert_to, reason);
}
n_result n_setting_commit(struct n_setting_id id, uint32_t *reason) {
    struct n_settings_request m = {.op = N_SETTINGS_COMMIT, .id = id};
    return change(&m, NULL, NULL, reason);
}
n_result n_setting_revert(struct n_setting_id id, uint32_t *reason) {
    struct n_settings_request m = {.op = N_SETTINGS_REVERT, .id = id};
    return change(&m, NULL, NULL, reason);
}
n_result n_setting_session(uint32_t user) {
    struct n_settings_request m = {.op = N_SETTINGS_SESSION, .argument = user};
    return change(&m, NULL, NULL, NULL);
}

n_handle n_setting_watch(uint32_t domain, uint32_t key) {
    n_handle h = n_service_connect(N_SETTINGS_SERVICE);
    if (h < 0) return h;
    struct n_settings_request m = {.op = N_SETTINGS_WATCH, .serial = 1, .id = {.domain = domain, .key = key}};
    n_result r = n_channel_send(h, &m, sizeof m, -1);
    struct n_settings_reply a;
    if (r >= 0) r = n_channel_receive(h, &a, sizeof a, N_CHANNEL_WAIT, NULL);
    if (r >= 0) r = a.result;
    if (r < 0) {
        n_handle_close(h);
        return r;
    }
    return h;
}
n_result n_setting_watch_take(n_handle watch, struct n_settings_changed *out) {
    n_result r = n_channel_receive(watch, out, sizeof *out, 0, NULL);
    if (r < 0) return r;
    return (size_t)r == sizeof *out ? 0 : -N_ERR_PROTOCOL;
}

n_result n_setting_list(struct n_setting_decl **out) {
    struct n_setting_decl *all = NULL;
    size_t count = 0;
    uint32_t start = 0;
    call_once(&lock_once, lock_init);
    mtx_lock(&lock);
    n_result r;
    for (;;) {
        struct n_settings_request m = {.op = N_SETTINGS_LIST, .argument = start};
        const struct n_settings_reply *a;
        r = ask_locked(&m, NULL, NULL, &a);
        if (r >= 0) r = a->result;
        if (r < 0) break;
        size_t page = a->length / sizeof *all;
        struct n_setting_decl *grown = page ? realloc(all, (count + page) * sizeof *all) : all;
        if (page && !grown) {
            r = -N_ERR_NO_MEMORY;
            break;
        }
        all = grown;
        if (page) memcpy(all + count, a + 1, page * sizeof *all);
        count += page;
        if (!a->more) {
            r = (n_result)count;
            break;
        }
        start = a->more;
    }
    mtx_unlock(&lock);
    if (r < 0) {
        free(all);
        return r;
    }
    *out = all;
    return r;
}

n_result n_setting_declare(const struct n_setting_decl *declarations, size_t count, uint32_t *reason) {
    n_result r = 0;
    for (size_t i = 0; i < count && r >= 0; i++) {
        struct n_settings_request m = {.op = N_SETTINGS_DECLARE, .length = sizeof *declarations};
        r = change(&m, &declarations[i], NULL, reason);
    }
    return r;
}
