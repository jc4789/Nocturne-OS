/* nvrm's request server: clients connect on NVRM_CHANNEL (include/nvrm_protocol.h), each with an
   argument arena both sides map; requests run one at a time through the RM OS layer
   (nvrm_serve.h), and pointers inside them reach only the requesting client's arena. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "nvrm_protocol.h"
#include "nvrm_serve.h"
#include "nvtypes.h"
#include "nvstatus.h"
#include "nvos.h"
#include "nv_escape.h"
#include "nv-unix-nvos-params-wrappers.h"

#define LISTENER_TOKEN 0

struct client {
    struct client *next;
    n_handle channel, arena_handle;
    uint8_t *arena;           /* nvrm's mapping */
    uint64_t arena_address;   /* the client's */
    size_t arena_bytes;
    void *file;               /* RM's state for this client */
};

static struct client *clients, *current;

void *nvrm_user_translate(const void *user, size_t n) {
    uint64_t at = (uint64_t)(uintptr_t)user;
    if (!current || !current->arena || at < current->arena_address) return NULL;
    uint64_t off = at - current->arena_address;
    if (n > current->arena_bytes || off > current->arena_bytes - n) return NULL;
    return current->arena + off;
}

static void client_end(n_handle events, struct client *c) {
    n_events_unwatch(events, (uint64_t)(uintptr_t)c);
    for (struct client **link = &clients; *link; link = &(*link)->next)
        if (*link == c) { *link = c->next; break; }
    if (c->file) nvrm_rm_file_close(c->file);
    if (c->arena) n_memory_release(c->arena, c->arena_bytes);
    if (c->arena_handle >= 0) n_handle_close(c->arena_handle);
    n_handle_close(c->channel);
    free(c);
}

static void reply(struct client *c, struct nvrm_message *head, const void *args, n_handle attach) {
    struct {
        struct nvrm_message head;
        uint8_t args[NVRM_ARGS_MAX];
    } out;
    out.head = *head;
    if (head->status < 0) out.head.size = 0;
    memcpy(out.args, args, out.head.size);
    n_channel_send(c->channel, &out, sizeof out.head + out.head.size, attach);
}

/* The client's first message: its arena, mapped here too. */
static n_result hello(struct client *c, const struct nvrm_message *head, n_handle arena) {
    if (c->file) return -N_ERR_INVALID;
    if (arena < 0 || !head->arena_bytes || head->arena_bytes > ((uint64_t)1 << 30) || head->arena_bytes % N_PAGE_SIZE)
        return -N_ERR_INVALID;
    c->arena = n_shared_map(arena, (size_t)head->arena_bytes, N_MEMORY_READ_WRITE);
    if (!c->arena) return -N_ERR_INVALID;
    c->arena_handle = arena;
    c->arena_address = head->arena_address;
    c->arena_bytes = (size_t)head->arena_bytes;
    c->file = nvrm_rm_file_open();
    return c->file ? 0 : -N_ERR_NO_MEMORY;
}

static void request(n_handle events, struct client *c) {
    struct {
        struct nvrm_message head;
        uint8_t args[NVRM_ARGS_MAX];
    } in;
    n_handle attached = -1;
    n_result got = n_channel_receive(c->channel, &in, sizeof in, 0, &attached);
    if (got == -N_ERR_NOT_READY) return;
    if (got < (n_result)sizeof in.head || in.head.protocol != NVRM_PROTOCOL ||
        in.head.size != (size_t)got - sizeof in.head) {
        /* the channel closed, or the client does not speak this protocol */
        if (attached >= 0) n_handle_close(attached);
        client_end(events, c);
        return;
    }
    struct nvrm_message head = in.head;
    head.offset = head.bytes = 0;
    if (head.escape == NVRM_HELLO) {
        head.status = (int32_t)hello(c, &in.head, attached);
        if (head.status < 0 && attached >= 0 && attached != c->arena_handle) n_handle_close(attached);
        head.size = 0;
        reply(c, &head, NULL, -1);
        return;
    }
    if (attached >= 0) n_handle_close(attached);
    if (!c->file || head.escape >= NVRM_HELLO) {
        head.status = -N_ERR_INVALID;
        reply(c, &head, NULL, -1);
        return;
    }
    current = c;
    nvrm_rm_ioctl(c->file, head.escape, in.args, head.size);
    n_handle mapped = -1;
    if (head.escape == NV_ESC_RM_MAP_MEMORY && head.size == sizeof(nv_ioctl_nvos33_parameters_with_fd)) {
        nv_ioctl_nvos33_parameters_with_fd *map = (void *)in.args;
        if (map->params.status == NV_OK) {
            mapped = nvrm_rm_mapping_object(&head.offset, &head.bytes);
            if (mapped < 0) map->params.status = NV_ERR_OPERATING_SYSTEM;
        }
    }
    current = NULL;
    head.status = 0;
    reply(c, &head, in.args, mapped);
    if (mapped >= 0) n_handle_close(mapped); /* the client holds its own */
}

n_result nvrm_serve(void) {
    n_handle listener = n_channel_listen(NVRM_CHANNEL);
    if (listener < 0) return listener;
    n_handle events = n_events_create();
    if (events < 0) return events;
    n_events_watch(events, listener, N_EVENT_RECEIVABLE, LISTENER_TOKEN);
    printf("nvrm: serving clients on \"%s\"\n", NVRM_CHANNEL);
    fflush(stdout);
    for (;;) {
        struct n_event ev[16];
        n_result n = n_events_wait(events, ev, 16, -1);
        if (n < 0) return n;
        for (n_result i = 0; i < n; i++) {
            if (ev[i].token == LISTENER_TOKEN) {
                n_handle ch;
                while ((ch = n_channel_accept(listener)) >= 0) {
                    struct client *c = calloc(1, sizeof *c);
                    if (!c) { n_handle_close(ch); continue; }
                    c->channel = ch;
                    c->arena_handle = -1;
                    c->next = clients;
                    clients = c;
                    n_events_watch(events, ch, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, (uint64_t)(uintptr_t)c);
                }
                continue;
            }
            struct client *c = (struct client *)(uintptr_t)ev[i].token;
            bool known = false;
            for (struct client *k = clients; k; k = k->next) known |= k == c;
            if (!known) continue; /* ended earlier in this batch */
            if (ev[i].events & N_EVENT_RECEIVABLE) request(events, c);
            else if (ev[i].events & N_EVENT_CLOSED) client_end(events, c);
        }
    }
}
