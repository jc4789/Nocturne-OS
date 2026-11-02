/* サービスの仕組みの libc の側：身元、能力、接続、サービスの起動の手順、利用者のフォルダー。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne_services.h"

n_handle n_store_private_open(n_handle cap, const char *region) {
    return (n_handle)__syscall(SYS_STORE_PRIVATE_OPEN, cap, (long)region, strlen(region), 0, 0);
}
n_result n_identity_self(struct n_identity *out) { return __syscall(SYS_IDENTITY_SELF, (long)out, 0, 0, 0, 0); }
n_result n_channel_peer(n_handle channel, struct n_identity *out) {
    return __syscall(SYS_CHANNEL_PEER, channel, (long)out, 0, 0, 0);
}
n_handle n_cap_derive(n_handle cap, enum n_cap what) { return (n_handle)__syscall(SYS_CAP_DERIVE, cap, what, 0, 0, 0); }

n_handle n_service_connect(const char *name) {
    if (!name || !*name) return -N_ERR_INVALID;
    size_t n = strlen(name);
    if (n >= N_SERVICE_TEXT_MAX) return -N_ERR_NAME_TOO_LONG;
    return (n_handle)__syscall(SYS_SERVICE_CONNECT, (long)name, n, 0, 0, 0);
}

n_handle n_connector_connect(n_handle connector, const char *argument) {
    struct n_service_control message = {.kind = N_SERVICE_CTL_CONNECTION};
    if (argument && strlen(argument) >= sizeof message.argument) return -N_ERR_NAME_TOO_LONG;
    if (argument) strcpy(message.argument, argument);
    n_handle ends[2];
    n_result r = n_channel_pair(ends);
    if (r < 0) return (n_handle)r;
    r = n_channel_send(connector, &message, sizeof message, ends[1]);
    n_handle_close(ends[1]);
    if (r < 0) {
        n_handle_close(ends[0]);
        return (n_handle)r;
    }
    return ends[0];
}

n_handle n_connector_accept(n_handle connector, char *argument, size_t cap) {
    for (;;) {
        struct n_service_control message;
        n_handle attached = -1;
        n_result r = n_channel_receive(connector, &message, sizeof message, 0, &attached);
        if (r < 0) return (n_handle)r;
        if (r == sizeof message && message.kind == N_SERVICE_CTL_STOP) {
            if (attached >= 0) n_handle_close(attached);
            return -N_ERR_CANCELLED;
        }
        if (r != sizeof message || message.kind != N_SERVICE_CTL_CONNECTION || attached < 0) {
            if (attached >= 0) n_handle_close(attached);
            continue;
        }
        message.argument[sizeof message.argument - 1] = 0;
        if (argument && cap) strlcpy(argument, message.argument, cap);
        return attached;
    }
}

/* 能力は起動の最初に一度だけ書き、以後は読むだけ。 */
static n_handle service_caps[N_CAP_COUNT];
static bool service_begun;

n_result n_service_begin(void) {
    for (int i = 0; i < N_CAP_COUNT; i++) service_caps[i] = -N_ERR_DENIED;
    service_begun = true;
    for (;;) {
        struct n_service_control message;
        n_handle attached = -1;
        n_result r = n_channel_receive(N_SERVICE_HANDLE_CONTROL, &message, sizeof message, N_CHANNEL_WAIT, &attached);
        if (r < 0) return r;
        if (r != sizeof message) {
            if (attached >= 0) n_handle_close(attached);
            return -N_ERR_PROTOCOL;
        }
        if (message.kind == N_SERVICE_CTL_CAPS_DONE) return 0;
        if (message.kind != N_SERVICE_CTL_CAP || message.value >= N_CAP_COUNT || attached < 0) {
            if (attached >= 0) n_handle_close(attached);
            return -N_ERR_PROTOCOL;
        }
        service_caps[message.value] = attached;
    }
}

n_handle n_service_cap(enum n_cap what) {
    if (!service_begun || (unsigned)what >= N_CAP_COUNT) return -N_ERR_DENIED;
    return service_caps[what];
}

n_result n_service_ready(void) {
    struct n_service_control message = {.kind = N_SERVICE_CTL_READY};
    n_result r = n_channel_send(N_SERVICE_HANDLE_CONTROL, &message, sizeof message, -1);
    return r < 0 ? r : 0;
}

static n_result call_on(n_handle channel, const void *request, size_t request_bytes, void *reply, size_t reply_bytes) {
    n_result r = n_channel_send(channel, request, request_bytes, -1);
    if (r < 0) return r;
    r = n_channel_receive(channel, reply, reply_bytes, N_CHANNEL_WAIT, NULL);
    if (r < 0) return r;
    return (size_t)r == reply_bytes ? 0 : -N_ERR_PROTOCOL;
}

n_result n_accounts_call(const struct n_accounts_request *request, struct n_accounts_reply *reply) {
    n_handle channel = n_service_connect("accounts");
    if (channel < 0) return channel;
    n_result r = call_on(channel, request, sizeof *request, reply, sizeof *reply);
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
    n_result r = call_on(channel, &request, sizeof request, &reply, sizeof reply);
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
    n_result r = call_on(channel, &request, sizeof request, &reply, sizeof reply);
    if (r >= 0) r = reply.result;
    if (r >= 0) r = receive_records(channel, reply.count, sizeof **out, (void **)out);
    n_handle_close(channel);
    return r;
}

n_result n_services_control(uint32_t op, const char *name, const char *argument, uint32_t session) {
    struct n_services_request request = {.op = op, .session = session};
    if (name) strlcpy(request.name, name, sizeof request.name);
    if (argument) strlcpy(request.argument, argument, sizeof request.argument);
    n_handle channel = n_service_connect(N_SERVICE_MANAGER);
    if (channel < 0) return channel;
    struct n_services_reply reply;
    n_result r = call_on(channel, &request, sizeof request, &reply, sizeof reply);
    n_handle_close(channel);
    return r < 0 ? r : reply.result;
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
