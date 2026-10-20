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
#define LEGACY_LISTENER_TOKEN 1

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

static bool reply(struct client *c, struct nvrm_message *packet, n_handle attach) {
    if (packet->rm_status) packet->status = -N_ERR_IO;
    if (packet->status < 0) packet->size = 0;
    return n_channel_send(c->channel, packet, sizeof *packet + packet->size, attach) >= 0;
}

/* Whether RM treats the client's requests as an administrator's. A channel carries no identity
   of its peer, so no connection has a basis for it: every client is unprivileged, like an
   ordinary user of RM's control device, which is all graphics clients (NVK) need. nvrm's own
   threads (interrupts, work, timers) are the only administrators. A connection that can prove
   where it comes from would be granted here, per connection. */
static bool client_is_admin(const struct client *c) {
    (void)c;
    return false;
}

/* The client's first message: its arena, mapped here too. */
static n_result hello(struct client *c, const struct nvrm_message *head, n_handle arena) {
    if (c->file) return -N_ERR_INVALID;
    if (arena < 0 || !head->arena_bytes || head->arena_bytes > SIZE_MAX || head->arena_bytes % N_PAGE_SIZE ||
        head->arena_address > UINT64_MAX - head->arena_bytes)
        return -N_ERR_INVALID;
    c->arena = n_shared_map(arena, (size_t)head->arena_bytes, N_MEMORY_READ_WRITE);
    if (!c->arena) return -N_ERR_INVALID;
    c->arena_handle = arena;
    c->arena_address = head->arena_address;
    c->arena_bytes = (size_t)head->arena_bytes;
    c->file = nvrm_rm_file_open(client_is_admin(c));
    return c->file ? 0 : -N_ERR_NO_MEMORY;
}

static void request(n_handle events, struct client *c) {
    n_result pending = n_channel_receive(c->channel, NULL, 0, N_CHANNEL_PEEK, NULL);
    if (pending == -N_ERR_NOT_READY) return;
    /* The wire size field is uint32_t; no smaller driver-specific ceiling. */
    if (pending < (n_result)sizeof(struct nvrm_message) ||
        (uint64_t)pending - sizeof(struct nvrm_message) > UINT32_MAX || (uint64_t)pending > SIZE_MAX) {
        client_end(events, c);
        return;
    }
    struct nvrm_message *packet = malloc((size_t)pending);
    if (!packet) { client_end(events, c); return; }
    n_handle attached = -1;
    n_result got = n_channel_receive(c->channel, packet, (size_t)pending, 0, &attached);
    if (got < (n_result)sizeof *packet || packet->protocol != NVRM_PROTOCOL ||
        packet->size != (size_t)got - sizeof *packet) {
        /* the channel closed, or the client does not speak this protocol */
        if (attached >= 0) n_handle_close(attached);
        free(packet);
        client_end(events, c);
        return;
    }
    packet->offset = packet->bytes = 0;
    if (packet->escape == NVRM_HELLO) {
        packet->status = (int32_t)hello(c, packet, attached);
        /* The union slots are arena inputs until hello has consumed them. */
        packet->access = packet->rm_status = 0;
        if (packet->status < 0 && attached >= 0 && attached != c->arena_handle) n_handle_close(attached);
        packet->size = 0;
        goto send;
    }
    packet->access = packet->rm_status = 0;
    if (attached >= 0) n_handle_close(attached);
    if (!c->file || packet->escape >= NVRM_HELLO) {
        packet->status = -N_ERR_INVALID;
        goto send;
    }
    current = c;
    packet->rm_status = nvrm_rm_ioctl(c->file, packet->escape, packet + 1, packet->size);
    n_handle mapped = -1;
    if (packet->rm_status == NV_OK && packet->escape == NV_ESC_RM_MAP_MEMORY &&
        packet->size == sizeof(nv_ioctl_nvos33_parameters_with_fd)) {
        nv_ioctl_nvos33_parameters_with_fd *map = (void *)(packet + 1);
        if (map->params.status == NV_OK) {
            uint32_t access = 0;
            mapped = nvrm_rm_mapping_object(c->file, &packet->offset, &packet->bytes, &access);
            packet->access = access;
            if (mapped < 0) {
                /* RM succeeded but exporting the native view failed: release RM's
                   mapping as well, rather than leaving it live until disconnect. */
                NVOS34_PARAMETERS unmap = {.hClient = map->params.hClient, .hDevice = map->params.hDevice,
                    .hMemory = map->params.hMemory, .pLinearAddress = map->params.pLinearAddress,
                    .flags = map->params.flags};
                nvrm_rm_ioctl(c->file, NV_ESC_RM_UNMAP_MEMORY, &unmap, sizeof unmap);
                map->params.status = NV_ERR_OPERATING_SYSTEM;
                map->params.pLinearAddress = 0;
            }
        }
    }
    current = NULL;
    packet->status = 0;
    /* RM keeps its view handle until unmap/close, so it can revoke all derived maps. */
    bool sent = reply(c, packet, mapped);
    free(packet);
    if (!sent) client_end(events, c);
    return;
send:
    {
        bool sent = reply(c, packet, -1);
        free(packet);
        if (!sent) client_end(events, c);
    }
}

n_result nvrm_serve(uint32_t pci_target) {
    if (pci_target > UINT16_MAX) return -N_ERR_INVALID;
    char name[NVRM_CHANNEL_NAME_BYTES];
    snprintf(name, sizeof name, NVRM_CHANNEL_FORMAT, (unsigned)pci_target);
    n_handle listener = n_channel_listen(name);
    if (listener < 0) return listener;
    n_handle events = n_events_create();
    if (events < 0) { n_handle_close(listener); return events; }
    n_result watched = n_events_watch(events, listener, N_EVENT_RECEIVABLE, LISTENER_TOKEN);
    if (watched < 0) { n_handle_close(events); n_handle_close(listener); return watched; }
    /* Preserve the historical default rendezvous for existing /data/bin
       binaries. Only one daemon owns this optional alias; every GPU always
       retains its independent BDF endpoint. Alias failure never kills GPU #2. */
    n_handle legacy = n_channel_listen(NVRM_CHANNEL);
    if (legacy >= 0 && n_events_watch(events, legacy, N_EVENT_RECEIVABLE, LEGACY_LISTENER_TOKEN) < 0) {
        n_handle_close(legacy); legacy = -1;
    }
    printf("nvrm: serving clients on \"%s\"\n", name);
    fflush(stdout);
    n_result ended = 0;
    for (;;) {
        struct n_event ev[16];
        n_result n = n_events_wait(events, ev, 16, -1);
        if (n < 0) { ended = n; break; }
        for (n_result i = 0; i < n; i++) {
            if (ev[i].token == LISTENER_TOKEN || ev[i].token == LEGACY_LISTENER_TOKEN) {
                n_handle ready = ev[i].token == LISTENER_TOKEN ? listener : legacy;
                n_handle ch;
                while (ready >= 0 && (ch = n_channel_accept(ready)) >= 0) {
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
    /* RM is about to shut down: every client frees what it still holds in RM first. */
    while (clients) client_end(events, clients);
    n_handle_close(events);
    if (legacy >= 0) n_handle_close(legacy);
    n_handle_close(listener);
    return ended;
}
