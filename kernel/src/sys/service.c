/* サービスの管理者への道。カーネルは名前を解かず、覚えない。SYS_SERVICE_CONNECT は channel の組を作り、
   一方の端を依頼者へ、もう一方を添付した要求（struct n_service_request）を管理者の channel へ渡す。
   管理者が要求を断れば端を閉じ、依頼者には N_ERR_CLOSED が見える。

   どの CPU からでも。manager_lock は manager の指だけを守り、その下で確保も待ちもしない。 */
#include "kernel.h"
#include "arch/spin.h"
#include "sys/authority.h"
#include "sys/channel.h"
#include "sys/clock.h"
#include "sys/object.h"

static struct spinlock manager_lock;
static struct handle *manager; /* 管理者の channel へ送る端。管理者が閉じたら次の登録が替わる。 */

int64_t sys_service_register(int cap) {
    int r = cap_check(cap, N_CAP_SERVICE_ROOT);
    if (r < 0) return r;
    struct handle *ours, *theirs;
    r = message_channel_pair(&ours, &theirs);
    if (r < 0) return r;
    message_channel_bind(theirs, identity_current());
    uint64_t f = spin_lock(&manager_lock);
    struct handle *old = manager;
    bool taken = old && message_channel_peer_open(old);
    if (!taken) manager = ours;
    spin_unlock(&manager_lock, f);
    if (taken) {
        handle_close(ours);
        handle_close(theirs);
        return -N_ERR_EXISTS;
    }
    if (old) handle_close(old);
    return handle_install(theirs);
}

int64_t sys_service_connect(const char *uname, size_t length) {
    if (!length) return -N_ERR_INVALID;
    if (length > UINT32_MAX) return -N_ERR_TOO_LARGE;
    uint64_t f = spin_lock(&manager_lock);
    struct handle *to = manager ? handle_ref(manager) : NULL;
    spin_unlock(&manager_lock, f);
    if (!to) return -N_ERR_NOT_FOUND;
    struct handle *client, *server;
    int64_t r = message_channel_pair(&client, &server);
    if (r < 0) {
        handle_close(to);
        return r;
    }
    message_channel_bind(client, identity_current());
    struct n_service_request request = {.from = *identity_current(), .name_length = (uint32_t)length};
    r = message_channel_request(to, &request, sizeof request, uname, length, server, CLOCK_NEVER);
    handle_close(server);
    handle_close(to);
    if (r < 0) {
        handle_close(client);
        return r == -N_ERR_CLOSED ? -N_ERR_NOT_FOUND : r;
    }
    return handle_install(client);
}
