/* nvrm's request server: clients connect on NVRM_CHANNEL (include/nvrm_protocol.h), each with an
   argument arena both sides map. Every client is served by a thread of its own, so a request that
   waits on the GPU (a GSP control call, an idle wait, a fence) holds up only its own client; RM
   orders concurrent requests with its own locks (the API lock and the GPU locks), as it does for
   the threads of different processes in an ioctl. Requests go through the RM OS layer
   (nvrm_serve.h), and pointers inside them reach only the requesting client's arena. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include "nocturne.h"
#include "nvrm_protocol.h"
#include "nvrm_serve.h"
#include "display.h"
#include "uvm_nvrm.h"
#include "nvtypes.h"
#include "nvstatus.h"
#include "nvos.h"
#include "nv_escape.h"
#include "nv-unix-nvos-params-wrappers.h"

#define LISTENER_TOKEN 0
#define LEGACY_LISTENER_TOKEN 1
/* A client thread's own event queue */
#define CHANNEL_TOKEN 0
#define STOP_TOKEN 1

struct client {
    n_handle channel, arena_handle;
    n_handle events;          /* the client thread's: its channel and the server's end */
    uint8_t *arena;           /* nvrm's mapping */
    uint64_t arena_address;   /* the client's */
    size_t arena_bytes;
    uint32_t principal;      /* authenticated HELLO sender; every message must match */
    void *file;               /* RM's state for this client */
    struct nvrm_uvm_client *uvm;  /* its UVM state (NULL until its first NVRM_UVM) */
};

/* The request a thread is serving: os_memcpy_from_user / os_memcpy_to_user run on it. */
static thread_local struct client *current;

static mtx_t threads_lock;
static cnd_t thread_ended;
static size_t client_threads;    /* client threads still running (threads_lock) */
static uint32_t serving_ended;   /* set once, woken by address: every client thread ends */

void *nvrm_user_translate(const void *user, size_t n) {
    uint64_t at = (uint64_t)(uintptr_t)user;
    if (!current || !current->arena || at < current->arena_address) return NULL;
    uint64_t off = at - current->arena_address;
    if (n > current->arena_bytes || off > current->arena_bytes - n) return NULL;
    return current->arena + off;
}

n_handle nvrm_user_view(const void *user, size_t n, size_t *offset) {
    if (!nvrm_user_translate(user, n ? n : 1)) return -N_ERR_BAD_ADDRESS;
    uint64_t off = (uint64_t)(uintptr_t)user - current->arena_address;
    /* The arena is whole pages, so the page-rounded run stays inside it. */
    uint64_t first = off & ~(uint64_t)(N_PAGE_SIZE - 1);
    uint64_t end = (off + n + N_PAGE_SIZE - 1) & ~(uint64_t)(N_PAGE_SIZE - 1);
    *offset = (size_t)(off - first);
    return n_shared_view_access(current->arena_handle, (size_t)first, (size_t)(end - first), N_CACHE_DEFAULT,
                                N_MEMORY_READ_WRITE);
}

bool nvrm_user_arena(uint64_t *address, uint64_t *bytes) {
    if (!current || !current->arena) return false;
    *address = current->arena_address;
    *bytes = current->arena_bytes;
    return true;
}

/* Frees everything the client still holds in RM, then the connection. */
static void client_end(struct client *c) {
    if (c->uvm) {
        /* UVM's state refers to the client's RM state and arena: it goes first. */
        current = c;
        nvrm_uvm_client_end(c->uvm);
        current = NULL;
    }
    if (c->file) {
        /* Its direct-scanout images go with it, off the screen first (display.c); what it
           duplicated of them goes with its RM state. */
        nvrm_display_connection_end(c->file);
        nvrm_rm_file_close(c->file);
    }
    if (c->arena) n_memory_release(c->arena, c->arena_bytes);
    if (c->arena_handle >= 0) n_handle_close(c->arena_handle);
    if (c->events >= 0) n_handle_close(c->events);
    n_handle_close(c->channel);
    free(c);
}

static bool reply(struct client *c, struct nvrm_message *packet, n_handle attach) {
    if (packet->rm_status) packet->status = -N_ERR_IO;
    /* A dispatcher may update handles/status before its final error. Preserve
       all valid arguments on RM failure; only native/transport rejection has
       no argument result. Old Protocol1 clients still fail closed on status. */
    if (packet->status < 0 && !packet->rm_status) packet->size = 0;
    return n_channel_send(c->channel, packet, sizeof *packet + packet->size, attach) >= 0;
}

/* An authenticated app identity is not an administrator capability. Ordinary
   channel clients remain unprivileged; only nvrm's own work is administrative. */
static bool client_is_admin(const struct client *c) {
    (void)c;
    return false;
}

/* The client's first message: its arena, mapped here too. */
static n_result hello(struct client *c, const struct nvrm_message *head, n_handle arena, uint32_t principal) {
    if (c->file) return -N_ERR_INVALID;
    if (arena < 0 || !head->arena_bytes || head->arena_bytes > SIZE_MAX || head->arena_bytes % N_PAGE_SIZE ||
        head->arena_address > UINT64_MAX - head->arena_bytes)
        return -N_ERR_INVALID;
    void *mapping;
    n_result mapped = n_shared_map(arena, (size_t)head->arena_bytes, N_MEMORY_READ_WRITE, &mapping);
    if (mapped < 0) return mapped;
    c->arena = mapping;
    c->file = nvrm_rm_file_open(client_is_admin(c), principal);
    if (!c->file) {
        /* Not connected: the caller closes the arena handle, and a later HELLO maps afresh
           instead of replacing (and losing) this mapping. */
        n_memory_release(c->arena, (size_t)head->arena_bytes);
        c->arena = NULL;
        return -N_ERR_NO_MEMORY;
    }
    c->arena_handle = arena;
    c->arena_address = head->arena_address;
    c->arena_bytes = (size_t)head->arena_bytes;
    c->principal = principal;
    return 0;
}

/* Serves the client's next message; false when the client is to be ended. */
static bool request(struct client *c) {
    n_result pending = n_channel_receive(c->channel, NULL, 0, N_CHANNEL_PEEK, NULL);
    if (pending == -N_ERR_NOT_READY) return true;
    /* The wire size field is uint32_t; no smaller driver-specific ceiling. The message is already
       in the system's channel buffer, so this copy is no larger than what the client sent. */
    if (pending < (n_result)sizeof(struct nvrm_message) ||
        (uint64_t)pending - sizeof(struct nvrm_message) > UINT32_MAX || (uint64_t)pending > SIZE_MAX)
        return false;
    struct nvrm_message *packet = malloc((size_t)pending);
    if (!packet) return false;
    struct n_channel_message received = {.attached = -1};
    n_result got = n_channel_receive_from(c->channel, packet, (size_t)pending, 0, &received);
    n_handle attached = received.attached;
    if (got < (n_result)sizeof *packet || packet->protocol != NVRM_PROTOCOL ||
        packet->size != (size_t)got - sizeof *packet) {
        /* the channel closed, or the client does not speak this protocol */
        if (attached >= 0) n_handle_close(attached);
        free(packet);
        return false;
    }
    packet->offset = packet->bytes = 0;
    /* Credentials describe the sender at send time, not the endpoint creator.
       A channel transferred to another app cannot impersonate the old peer. */
    if (!received.sender_app || received.sender_app > UINT32_MAX ||
        (c->file && received.sender_app != c->principal)) {
        packet->access = packet->rm_status = 0;
        packet->status = -N_ERR_DENIED;
        if (attached >= 0) n_handle_close(attached);
        goto send;
    }
    if (packet->escape == NVRM_HELLO) {
        packet->status = (int32_t)hello(c, packet, attached, (uint32_t)received.sender_app);
        /* The union slots are arena inputs until hello has consumed them. */
        packet->access = packet->rm_status = 0;
        if (!packet->status)
            packet->access = NVRM_HELLO_STATUS_PRESENT | (nvrm_rm_adapter_ready() ? NVRM_HELLO_ADAPTER_READY : 0);
        if (packet->status < 0 && attached >= 0 && attached != c->arena_handle) n_handle_close(attached);
        packet->size = 0;
        goto send;
    }
    packet->access = packet->rm_status = 0;
    if (attached >= 0) n_handle_close(attached);
    if (c->file && packet->escape == NVRM_UVM) {
        current = c;
        n_handle uvm_attach = -1;
        packet->status = (int32_t)nvrm_uvm_request(&c->uvm, c->principal, packet + 1, packet->size, &uvm_attach);
        current = NULL;
        /* The attached memory stays UVM's: it is sent, not closed. */
        bool sent = reply(c, packet, packet->status < 0 ? -1 : uvm_attach);
        free(packet);
        return sent;
    }
    if (c->file && packet->escape == NVRM_SCANOUT) {
        current = c;
        packet->status = (int32_t)nvrm_display_scanout(c->file, c->principal, packet + 1, packet->size);
        current = NULL;
        goto send;
    }
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
    return sent;
send:
    sent = reply(c, packet, -1);
    free(packet);
    return sent;
}

/* One client's thread: its messages in order until it disconnects, sends what is not this
   protocol, or the server ends. */
static int client_thread(void *arg) {
    struct client *c = arg;
    /* Not ready: the server ended before this thread looked. */
    bool open = n_events_watch_address(c->events, &serving_ended, 0, STOP_TOKEN) >= 0;
    while (open) {
        struct n_event ev[2];
        n_result n = n_events_wait(c->events, ev, 2, -1);
        if (n < 0) {
            printf("nvrm: a client's event wait failed (%s); ending the client\n", n_result_text(n));
            break;
        }
        for (n_result i = 0; i < n && open; i++) {
            if (ev[i].token == STOP_TOKEN) open = false;
            else if (ev[i].events & N_EVENT_RECEIVABLE) open = request(c); /* queued data first */
            else if (ev[i].events & N_EVENT_CLOSED) open = false;
        }
    }
    client_end(c);
    mtx_lock(&threads_lock);
    client_threads--;
    cnd_signal(&thread_ended);
    mtx_unlock(&threads_lock);
    return 0;
}

static void client_start(n_handle channel) {
    struct client *c = calloc(1, sizeof *c);
    if (!c) {
        n_handle_close(channel);
        return;
    }
    c->channel = channel;
    c->arena_handle = -1;
    c->events = n_events_create();
    n_result r = c->events < 0 ? c->events : n_events_watch(c->events, channel, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, CHANNEL_TOKEN);
    const char *why = n_result_text(r);
    thrd_t thread;
    if (r >= 0) {
        mtx_lock(&threads_lock);
        client_threads++;
        mtx_unlock(&threads_lock);
        int made = thrd_create(&thread, client_thread, c);
        if (made == thrd_success) {
            thrd_detach(thread);
            return;
        }
        mtx_lock(&threads_lock);
        client_threads--;
        mtx_unlock(&threads_lock);
        /* C11 tells only a lack of memory apart; otherwise the process is at its thread limit
           (thread_create, nocturne.h) or ending */
        why = made == thrd_nomem ? "out of memory" : "no thread can be started for it";
    }
    printf("nvrm: cannot serve a new client (%s)\n", why);
    client_end(c);
}

n_result nvrm_serve(uint32_t pci_target) {
    if (pci_target > UINT16_MAX) return -N_ERR_INVALID;
    if (mtx_init(&threads_lock, mtx_plain) != thrd_success) return -N_ERR_NO_MEMORY;
    if (cnd_init(&thread_ended) != thrd_success) { mtx_destroy(&threads_lock); return -N_ERR_NO_MEMORY; }
    __atomic_store_n(&serving_ended, 0, __ATOMIC_RELEASE);
    char name[NVRM_CHANNEL_NAME_BYTES];
    snprintf(name, sizeof name, NVRM_CHANNEL_FORMAT, (unsigned)pci_target);
    n_handle listener = n_channel_listen(name);
    if (listener < 0) { cnd_destroy(&thread_ended); mtx_destroy(&threads_lock); return listener; }
    n_handle events = n_events_create();
    if (events < 0) { n_handle_close(listener); cnd_destroy(&thread_ended); mtx_destroy(&threads_lock); return events; }
    n_result watched = n_events_watch(events, listener, N_EVENT_RECEIVABLE, LISTENER_TOKEN);
    if (watched < 0) {
        n_handle_close(events); n_handle_close(listener);
        cnd_destroy(&thread_ended); mtx_destroy(&threads_lock);
        return watched;
    }
    /* Preserve the historical default rendezvous for programs built against
       it. Only one daemon owns this optional alias; every GPU always
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
            n_handle ready = ev[i].token == LISTENER_TOKEN ? listener : legacy;
            n_handle ch;
            while (ready >= 0 && (ch = n_channel_accept(ready)) >= 0) client_start(ch);
        }
    }
    /* RM is about to shut down: no new client, and every client frees what it still holds in RM
       first. A thread in the middle of a request finishes it (RM's own work) before it looks. */
    n_handle_close(events);
    if (legacy >= 0) n_handle_close(legacy);
    n_handle_close(listener);
    __atomic_store_n(&serving_ended, 1, __ATOMIC_RELEASE);
    wake_address(&serving_ended, 0);
    mtx_lock(&threads_lock);
    while (client_threads) cnd_wait(&thread_ended, &threads_lock);
    mtx_unlock(&threads_lock);
    cnd_destroy(&thread_ended);
    mtx_destroy(&threads_lock);
    return ended;
}
