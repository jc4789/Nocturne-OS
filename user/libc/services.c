/* システムのサービスの libc の側：準備の知らせ、利用者、管理者の口、機械、このプロセスの利用者のフォルダー。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne_services.h"
#include "nocturne_language.h"

n_handle n_store_private_open(n_handle cap, uint32_t region, uint32_t user, int access) {
    return (n_handle)__syscall(SYS_STORE_PRIVATE_OPEN, cap, region, user, access, 0);
}

n_result n_clock_wall_step(n_handle cap, int64_t unix_ns) { return __syscall(SYS_CLOCK_WALL_STEP, cap, unix_ns, 0, 0, 0); }
n_result n_clock_wall_slew(n_handle cap, int64_t offset_ns, uint32_t ppm) { return __syscall(SYS_CLOCK_WALL_SLEW, cap, offset_ns, ppm, 0, 0); }
n_result n_rtc_write(n_handle cap, int64_t unix_seconds) { return __syscall(SYS_RTC_WRITE, cap, unix_seconds, 0, 0, 0); }

n_result n_service_ready(void) {
    struct n_service_ready_message message = {.kind = N_SERVICE_READY};
    n_result r = n_channel_send(N_SERVICE_HANDLE_CONTROL, &message, sizeof message, -1);
    return r < 0 ? r : 0;
}

static n_result call_on(n_handle channel, const void *request, size_t request_bytes, void *reply, size_t reply_bytes, n_handle *attached) {
    n_result r = n_channel_send(channel, request, request_bytes, -1);
    if (r < 0) return r;
    r = n_channel_receive(channel, reply, reply_bytes, N_CHANNEL_WAIT, attached);
    if (r < 0) return r;
    return (size_t)r == reply_bytes ? 0 : -N_ERR_PROTOCOL;
}

n_result n_accounts_call(const struct n_accounts_request *request, struct n_accounts_reply *reply) {
    n_handle channel = n_service_connect("accounts");
    if (channel < 0) return channel;
    n_result r = call_on(channel, request, sizeof *request, reply, sizeof *reply, NULL);
    n_handle_close(channel);
    return r < 0 ? r : reply->result;
}

/* 応答の count に続く count 通の記録を、新しい配列へ受ける。 */
static n_result receive_records(n_handle channel, uint32_t count, size_t size, void **out) {
    char *all = count ? calloc(count, size) : malloc(1);
    if (!all) return -N_ERR_NO_MEMORY;
    for (uint32_t i = 0; i < count; i++) {
        n_result r = n_channel_receive(channel, all + i * size, size, N_CHANNEL_WAIT, NULL);
        if (r < 0 || (size_t)r != size) {
            free(all);
            return r < 0 ? r : -N_ERR_PROTOCOL;
        }
    }
    *out = all;
    return count;
}

n_result n_accounts_list(struct n_account **out) {
    n_handle channel = n_service_connect("accounts");
    if (channel < 0) return channel;
    struct n_accounts_request request = {.op = N_ACCOUNTS_LIST};
    struct n_accounts_reply reply;
    n_result r = call_on(channel, &request, sizeof request, &reply, sizeof reply, NULL);
    if (r >= 0) r = reply.result;
    if (r >= 0) r = receive_records(channel, reply.count, sizeof **out, (void **)out);
    n_handle_close(channel);
    return r;
}

n_result n_services_list(struct n_service_status **out) {
    n_handle channel = n_service_connect(N_SERVICE_MANAGER);
    if (channel < 0) return channel;
    struct n_services_request request = {.op = N_SERVICES_LIST};
    struct n_services_reply reply;
    n_result r = call_on(channel, &request, sizeof request, &reply, sizeof reply, NULL);
    if (r >= 0) r = reply.result;
    if (r >= 0) r = receive_records(channel, reply.count, sizeof **out, (void **)out);
    n_handle_close(channel);
    return r;
}

n_result n_services_control(uint32_t op, const char *name, const char *argument) {
    struct n_services_request request = {.op = op};
    if (name) strlcpy(request.name, name, sizeof request.name);
    if (argument) strlcpy(request.argument, argument, sizeof request.argument);
    n_handle channel = n_service_connect(N_SERVICE_MANAGER);
    if (channel < 0) return channel;
    struct n_services_reply reply;
    n_result r = call_on(channel, &request, sizeof request, &reply, sizeof reply, NULL);
    n_handle_close(channel);
    return r < 0 ? r : reply.result;
}

static n_result machine_power(uint32_t op, uint32_t flags) {
    n_handle channel = n_service_connect("machine");
    if (channel < 0) return channel;
    struct n_machine_request request = {.op = op, .flags = flags};
    struct n_machine_reply reply;
    n_result r = call_on(channel, &request, sizeof request, &reply, sizeof reply, NULL);
    n_handle_close(channel);
    return r < 0 ? r : reply.result;
}

n_result n_machine_power(uint32_t op) { return machine_power(op, 0); }
n_result n_machine_choose(uint32_t op) { return machine_power(op, N_MACHINE_CHOSEN); }

n_result n_session_end(void) {
    n_handle channel = n_service_connect("session");
    if (channel < 0) return channel;
    struct n_session_request request = {.op = N_SESSION_END};
    struct n_session_reply reply;
    n_result r = call_on(channel, &request, sizeof request, &reply, sizeof reply, NULL);
    n_handle_close(channel);
    return r < 0 ? r : reply.result;
}

n_result n_session_stop(n_handle cap, uint32_t session) {
    return (n_result)__syscall(SYS_SESSION_STOP, cap, session, 0, 0, 0);
}

/* ---- プログラムの台帳 ---- */
/* 一通の要求を送り、応答を一通受ける。*body は free する。 */
static n_result programs_call(const struct n_programs_request *head, const char *type, const char *name, const char *path,
                              const char *argument, struct n_programs_reply *reply, char **body, size_t *body_bytes) {
    struct n_programs_request q = *head;
    q.type_bytes = (uint32_t)strlen(type) + 1;
    q.name_bytes = (uint32_t)strlen(name) + 1;
    q.path_bytes = (uint32_t)strlen(path) + 1;
    q.argument_bytes = (uint32_t)strlen(argument) + 1;
    size_t bytes = sizeof q + q.type_bytes + q.name_bytes + q.path_bytes + q.argument_bytes;
    char *message = malloc(bytes);
    if (!message) return -N_ERR_NO_MEMORY;
    char *at = message;
    memcpy(at, &q, sizeof q);
    at += sizeof q;
    memcpy(at, type, q.type_bytes);
    at += q.type_bytes;
    memcpy(at, name, q.name_bytes);
    at += q.name_bytes;
    memcpy(at, path, q.path_bytes);
    at += q.path_bytes;
    memcpy(at, argument, q.argument_bytes);
    n_handle channel = n_service_connect("programs");
    n_result r = channel;
    if (channel >= 0) r = n_channel_send(channel, message, bytes, -1);
    free(message);
    if (r >= 0) r = n_channel_receive(channel, NULL, 0, N_CHANNEL_WAIT | N_CHANNEL_PEEK, NULL);
    char *whole = NULL;
    if (r >= (n_result)sizeof *reply) {
        size_t size = (size_t)r;
        whole = malloc(size);
        r = whole ? n_channel_receive(channel, whole, size, N_CHANNEL_WAIT, NULL) : -N_ERR_NO_MEMORY;
    } else if (r >= 0) {
        r = -N_ERR_PROTOCOL;
    }
    if (channel >= 0) n_handle_close(channel);
    if (r < 0) {
        free(whole);
        return r;
    }
    memcpy(reply, whole, sizeof *reply);
    if (reply->result < 0) {
        free(whole);
        return reply->result;
    }
    *body_bytes = (size_t)r - sizeof *reply;
    memmove(whole, whole + sizeof *reply, *body_bytes);
    *body = whole;
    return 0;
}

/* count 個の n_program の配列の後ろに記録を置き、文字列はそこを指す。 */
static n_result programs_unpack(const char *body, size_t bytes, uint32_t count, struct n_program **out) {
    if (count > bytes / sizeof(struct n_program_head)) return -N_ERR_PROTOCOL;
    struct n_program *list = malloc(count * sizeof *list + bytes + 1);
    if (!list) return -N_ERR_NO_MEMORY;
    char *text = (char *)(list + count);
    memcpy(text, body, bytes);
    size_t at = 0;
    for (uint32_t i = 0; i < count; i++) {
        struct n_program_head h;
        if (bytes - at < sizeof h) goto bad;
        memcpy(&h, text + at, sizeof h);
        at += sizeof h;
        size_t size = (size_t)h.name_bytes + h.path_bytes + h.argument_bytes;
        if (bytes - at < size || !h.name_bytes || !h.path_bytes || !h.argument_bytes) goto bad;
        list[i] = (struct n_program){h.id, h.icon, h.flags, text + at, text + at + h.name_bytes, text + at + h.name_bytes + h.path_bytes};
        at += size;
    }
    *out = list;
    return count;
bad:
    free(list);
    return -N_ERR_PROTOCOL;
}

n_result n_programs_list(struct n_program **out) {
    struct n_programs_request q = {.op = N_PROGRAMS_LIST, .language = n_language_now()};
    struct n_programs_reply reply;
    char *body;
    size_t bytes;
    n_result r = programs_call(&q, "", "", "", "", &reply, &body, &bytes);
    if (r < 0) return r;
    r = programs_unpack(body, bytes, reply.count, out);
    free(body);
    return r;
}

n_result n_programs_claims(struct n_program_claim **out) {
    struct n_programs_request q = {.op = N_PROGRAMS_CLAIMS};
    struct n_programs_reply reply;
    char *body;
    size_t bytes;
    n_result r = programs_call(&q, "", "", "", "", &reply, &body, &bytes);
    if (r < 0) return r;
    struct n_program_claim *list = reply.count <= bytes / sizeof(struct n_program_claim_head)
                                       ? malloc(reply.count * sizeof *list + bytes + 1) : NULL;
    if (!list) {
        free(body);
        return -N_ERR_NO_MEMORY;
    }
    char *text = (char *)(list + reply.count);
    memcpy(text, body, bytes);
    free(body);
    size_t at = 0;
    for (uint32_t i = 0; i < reply.count; i++) {
        struct n_program_claim_head h;
        if (bytes - at < sizeof h) goto bad;
        memcpy(&h, text + at, sizeof h);
        at += sizeof h;
        if (bytes - at < h.type_bytes || !h.type_bytes) goto bad;
        list[i] = (struct n_program_claim){h.id, h.current, text + at};
        at += h.type_bytes;
    }
    *out = list;
    return reply.count;
bad:
    free(list);
    return -N_ERR_PROTOCOL;
}

static n_result document_window(uint32_t kind, const char *start, char *out, size_t n) {
    struct n_document_window_request q = {.kind = kind};
    if (start) strlcpy(q.start, start, sizeof q.start);
    n_handle ends[2];
    n_result r = n_channel_pair(ends);
    if (r < 0) return r;
    const char *arguments[] = {N_DOCUMENT_WINDOW_PROGRAM};
    const int32_t handles[N_DOCUMENT_WINDOW_HANDLE + 1] = {-1, -1, -1, ends[1]};
    struct n_launch launch = {.arguments = arguments, .argument_count = 1, .handles = handles, .handle_count = N_DOCUMENT_WINDOW_HANDLE + 1};
    n_handle app = n_app_launch(N_DOCUMENT_WINDOW_PROGRAM, &launch);
    n_handle_close(ends[1]);
    if (app < 0) {
        n_handle_close(ends[0]);
        return app;
    }
    n_handle_close(app);
    r = n_channel_send(ends[0], &q, sizeof q, -1);
    if (r >= 0) {
        n_handle attached = -1;
        r = n_channel_receive(ends[0], out, n ? n - 1 : 0, N_CHANNEL_WAIT, &attached);
        if (attached >= 0) n_handle_close(attached);
        if (r == -N_ERR_CLOSED) r = -N_ERR_CANCELLED;
        else if (r >= 0) {
            out[r] = 0;
            r = 0;
        }
    }
    n_handle_close(ends[0]);
    return r;
}
n_result n_document_ask_open(const char *start, char *out, size_t n) { return document_window(N_DOCUMENT_WINDOW_OPEN, start, out, n); }
n_result n_document_ask_save(const char *start, char *out, size_t n) { return document_window(N_DOCUMENT_WINDOW_SAVE, start, out, n); }

n_result n_program_run(const struct n_program *p, const char *document) {
    const char *arguments[2] = {p->path, document ? document : p->argument};
    struct n_launch launch = {.arguments = arguments, .argument_count = arguments[1][0] ? 2 : 1};
    n_handle app = n_app_launch(p->path, &launch);
    if (app < 0) return app;
    n_result id = n_app_id(app);
    n_handle_close(app);
    return id;
}

n_result n_program_open_document(const char *path) {
    const char *end = strrchr(path, '\\'), *dot = strrchr(end ? end : path, '.');
    char *type = dot && dot[1] ? strdup(dot + 1) : strdup(N_PROGRAM_ANY_TYPE);
    if (!type) return -N_ERR_NO_MEMORY;
    for (char *c = type; *c; c++)
        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
    struct n_programs_request q = {.op = N_PROGRAMS_FOR_TYPE};
    struct n_programs_reply reply;
    char *body;
    size_t bytes;
    n_result r = programs_call(&q, type, "", "", "", &reply, &body, &bytes);
    free(type);
    if (r < 0) return r;
    struct n_program *one;
    r = programs_unpack(body, bytes, reply.count, &one);
    free(body);
    if (r < 0) return r;
    r = reply.count ? n_program_run(one, path) : -N_ERR_NOT_FOUND;
    free(one);
    return r;
}

n_result n_program_register(const char *name, const char *path, const char *argument, uint32_t icon, uint32_t flags) {
    struct n_programs_request q = {.op = N_PROGRAMS_REGISTER, .icon = icon, .flags = flags};
    struct n_programs_reply reply;
    char *body;
    size_t bytes;
    n_result r = programs_call(&q, "", name, path, argument ? argument : "", &reply, &body, &bytes);
    if (r < 0) return r;
    struct n_program_head h;
    r = bytes >= sizeof h ? 0 : -N_ERR_PROTOCOL;
    if (r >= 0) {
        memcpy(&h, body, sizeof h);
        r = (n_result)h.id;
    }
    free(body);
    return r;
}

static n_result programs_simple(uint32_t op, uint32_t id, const char *type) {
    struct n_programs_request q = {.op = op, .id = id};
    struct n_programs_reply reply;
    char *body;
    size_t bytes;
    n_result r = programs_call(&q, type, "", "", "", &reply, &body, &bytes);
    if (r >= 0) free(body);
    return r;
}
n_result n_program_claim(uint32_t id, const char *type) { return programs_simple(N_PROGRAMS_CLAIM, id, type); }
n_result n_program_choose(uint32_t id, const char *type) { return programs_simple(N_PROGRAMS_CHOOSE, id, type); }
n_result n_program_remove(uint32_t id) { return programs_simple(N_PROGRAMS_REMOVE, id, ""); }

n_result n_machine_screen(uint32_t **pixels, int *width, int *height) {
    n_handle channel = n_service_connect("machine");
    if (channel < 0) return channel;
    struct n_machine_request request = {.op = N_MACHINE_SCREEN};
    struct n_machine_reply reply;
    n_handle shared = -1;
    n_result r = call_on(channel, &request, sizeof request, &reply, sizeof reply, &shared);
    n_handle_close(channel);
    if (r >= 0) r = reply.result;
    if (r >= 0 && shared < 0) r = -N_ERR_PROTOCOL;
    if (r < 0) {
        if (shared >= 0) n_handle_close(shared);
        return r;
    }
    size_t bytes = (size_t)reply.width * (size_t)reply.height * 4;
    void *view;
    r = n_shared_map(shared, bytes, N_MEMORY_READ, &view);
    n_handle_close(shared);
    if (r < 0) return r;
    uint32_t *copy = malloc(bytes);
    if (copy) memcpy(copy, view, bytes);
    n_memory_release(view, bytes);
    if (!copy) return -N_ERR_NO_MEMORY;
    *pixels = copy;
    *width = reply.width;
    *height = reply.height;
    return 0;
}

/* このプロセスの利用者のフォルダーは、身元から accounts が引く。一度引けば変わらない。
   システムの身元には利用者のフォルダーが無く、"C:\" を返す。 */
static char own_folder[N_ACCOUNT_FOLDER_MAX], own_temp[N_ACCOUNT_FOLDER_MAX + sizeof "\\AppData\\Temp"];
static volatile uint32_t own_folder_state; /* 0 まだ、1 引いている最中、2 済み */

static void own_folder_ready(void) {
    if (__atomic_load_n(&own_folder_state, __ATOMIC_ACQUIRE) == 2) return;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&own_folder_state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&own_folder_state, __ATOMIC_ACQUIRE) != 2) wait_on_address(&own_folder_state, 1, N_CLOCK_NEVER);
        return;
    }
    struct n_accounts_request request = {.op = N_ACCOUNTS_SELF};
    struct n_accounts_reply reply;
    if (n_accounts_call(&request, &reply) >= 0 && reply.account.folder[0]) {
        strlcpy(own_folder, reply.account.folder, sizeof own_folder);
        snprintf(own_temp, sizeof own_temp, "%s\\AppData\\Temp", own_folder);
    } else {
        strcpy(own_folder, "C:\\");
        strcpy(own_temp, "C:\\");
    }
    __atomic_store_n(&own_folder_state, 2, __ATOMIC_RELEASE);
    wake_address(&own_folder_state, 0x7fffffff);
}

const char *n_user_folder(void) {
    own_folder_ready();
    return own_folder;
}

const char *n_temp_folder(void) {
    own_folder_ready();
    return own_temp;
}
