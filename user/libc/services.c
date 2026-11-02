/* システムのサービスの libc の側：準備の知らせ、利用者、管理者の口、機械、このプロセスの利用者のフォルダー。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne_services.h"

n_handle n_store_private_open(n_handle cap, uint32_t region, uint32_t user, int access) {
    return (n_handle)__syscall(SYS_STORE_PRIVATE_OPEN, cap, region, user, access, 0);
}

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
