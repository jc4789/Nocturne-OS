/* 機械のサービス：電源と画面の読取りの能力の持ち主。能力をアプリへ配らず、依頼をここで受ける。
   相手はセッションの中のプロセスか、N_CLASS_ADMIN 以上。 */
#include <stdio.h>
#include <string.h>
#include "nocturne.h"
#include "nocturne_services.h"

static n_handle power_cap, screen_cap;

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

static void client_serve(void *arg) {
    n_handle channel = (n_handle)(intptr_t)arg;
    struct n_identity peer;
    if (n_channel_peer(channel, &peer) < 0) {
        n_handle_close(channel);
        return;
    }
    bool allowed = peer.session != 0 || peer.cls >= N_CLASS_ADMIN;
    for (;;) {
        struct n_machine_request q;
        n_result got = n_channel_receive(channel, &q, sizeof q, N_CHANNEL_WAIT, NULL);
        if (got < 0 || got != sizeof q) break;
        struct n_machine_reply reply = {.result = -N_ERR_DENIED};
        n_handle attached = -1;
        if (allowed) {
            switch (q.op) {
            case N_MACHINE_POWER_OFF:
            case N_MACHINE_REBOOT:
            case N_MACHINE_SLEEP:
            case N_MACHINE_HIBERNATE:
                reply.result = q.flags & ~N_MACHINE_CHOSEN ? -N_ERR_INVALID : n_power_request(power_cap, power_action(q.op), q.flags & N_MACHINE_CHOSEN ? N_POWER_CHOSEN : 0);
                break;
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
        n_result sent = n_channel_send(channel, &reply, sizeof reply, attached);
        if (attached >= 0) n_handle_close(attached);
        if (sent < 0) break;
    }
    n_handle_close(channel);
}

int main(void) {
    power_cap = n_cap_find(N_CAP_POWER);
    screen_cap = n_cap_find(N_CAP_SCREEN_READ);
    if (power_cap < 0 || screen_cap < 0) {
        printf("machine: the capabilities were not given (%s)\n", n_result_text(power_cap < 0 ? power_cap : screen_cap));
        fflush(stdout);
        return 1;
    }
    n_service_ready();
    for (;;) {
        n_handle channel = n_inlet_accept(N_SERVICE_HANDLE_CONTROL, N_CHANNEL_WAIT);
        if (channel < 0) return 0;
        n_result thread = thread_create(client_serve, (void *)(intptr_t)channel);
        if (thread < 0) n_handle_close(channel);
        else thread_detach((int)thread);
    }
}
