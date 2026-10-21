/* nvrm_servetest: /bin/nvrm's request server (ports/nvidia/nvrm_serve.c, linked unchanged) against
   a fake RM OS layer, with clients on libnvrm's public API as NVK uses it. The server runs on a
   thread of this app; its clients connect from here. Checked:
   - HELLO (adapter state, the authenticated principal, never admin) and requests carrying
     arguments through the client's arena, refused outside it; the attached map object;
   - concurrent clients, and one connection shared by threads, getting only their own results;
   - a request waiting inside RM holds up only its own client;
   - a client's disconnect closes its RM file exactly once, with what it still held;
   - past the kernel's per-app thread limit the server refuses new clients without failing,
     keeps serving the others and takes a new client once one has gone.
   No GPU and no PCI claim are needed. Prints "nvrm_servetest: N failed". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include "nocturne.h"
#include "nvrm.h"
#include "nvrm_protocol.h"
#include "../ports/nvidia/nvrm_serve.h"
#include "nvtypes.h"
#include "nvstatus.h"
#include "nvos.h"
#include "nv_escape.h"
#include "nv-unix-nvos-params-wrappers.h"

/* A PCI function no GPU of the test machine is: the server's channel is this test's own. */
#define TARGET N_PCI_TARGET(0xfe, 0x1f, 7)
#define WAIT_MS 5000u
#define FAKE_CMD_ECHO 0x7e570001u
#define FAKE_CMD_BLOCK 0x7e570002u
#define MEM_UNEXPORTABLE 0xbadu
#define MAP_OFFSET 256u
#define MAP_BYTES (2 * N_PAGE_SIZE)
#define WORKERS 8
#define ROUNDS 200
#define SHARERS 4
/* sched.c: an app has at most 128 threads besides its first; this test asks for more clients. */
#define THREAD_LIMIT 128
#define LIMIT_TRIES (THREAD_LIMIT + 32)

static int failed;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %d: ", __LINE__);                                                                             \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            __atomic_add_fetch(&failed, 1, __ATOMIC_RELAXED);                                                          \
        }                                                                                                              \
    } while (0)

/* ---- the fake RM OS layer (nvrm_serve.h) ---- */

struct fake_file {
    struct fake_file *next;
    uint32_t id, principal, owner, closes, map_memory, unmaps;
    bool admin, map_pending;
    uint64_t map_linear, unmapped_linear;
    n_handle mapping; /* the view RM keeps until unmap or close */
};

/* FAKE_CMD_ECHO's parameters, which the server reaches only through the client's arena. */
struct echo {
    uint32_t tag, file;
    uint64_t word[8];
};

static struct {
    mtx_t lock;
    cnd_t changed;
    struct fake_file *files; /* every file ever opened, kept for the checks */
    uint32_t next_id, next_object;
    unsigned open_calls, ioctls, mixups;
    bool fail_open, not_ready, blocked, release;
} fake;

static uint64_t echo_word(uint64_t word, uint32_t tag) {
    return (word ^ ((uint64_t)tag << 32 | tag)) + 0x9e3779b97f4a7c15u;
}
static uint64_t map_linear(uint32_t memory) { return 0x5000000000u + (uint64_t)memory * N_PAGE_SIZE; }
static uint8_t map_byte(uint32_t memory, unsigned i) { return (uint8_t)(memory * 7 + i); }

void *nvrm_rm_file_open(bool admin, uint32_t principal) {
    mtx_lock(&fake.lock);
    fake.open_calls++;
    struct fake_file *f = NULL;
    if (fake.fail_open) fake.fail_open = false;
    else if ((f = calloc(1, sizeof *f))) {
        f->id = ++fake.next_id;
        f->principal = principal;
        f->admin = admin;
        f->mapping = -1;
        f->next = fake.files;
        fake.files = f;
    }
    mtx_unlock(&fake.lock);
    return f;
}

bool nvrm_rm_adapter_ready(void) {
    mtx_lock(&fake.lock);
    bool ready = !fake.not_ready;
    mtx_unlock(&fake.lock);
    return ready;
}

void nvrm_rm_file_close(void *file) {
    struct fake_file *f = file;
    mtx_lock(&fake.lock);
    if (f->mapping >= 0) n_handle_close(f->mapping);
    f->mapping = -1;
    f->closes++;
    cnd_broadcast(&fake.changed);
    mtx_unlock(&fake.lock);
}

static uint32_t fake_control(struct fake_file *f, void *args, uint32_t size) {
    if (size != sizeof(NVOS54_PARAMETERS)) return NV_ERR_INVALID_ARGUMENT;
    NVOS54_PARAMETERS *a = args;
    if (a->cmd != FAKE_CMD_ECHO && a->cmd != FAKE_CMD_BLOCK) return NV_ERR_NOT_SUPPORTED;
    if (a->paramsSize != sizeof(struct echo)) return NV_ERR_INVALID_ARGUMENT;
    struct echo *e = nvrm_user_translate(NvP64_VALUE(a->params), a->paramsSize);
    if (!e) return NV_ERR_INVALID_ADDRESS;
    mtx_lock(&fake.lock);
    /* every request on one file comes from the one connection (owner) that opened it */
    if (!f->owner) f->owner = a->hClient;
    else if (f->owner != a->hClient) fake.mixups++;
    if (a->cmd == FAKE_CMD_BLOCK) {
        fake.blocked = true;
        cnd_broadcast(&fake.changed);
        while (!fake.release) cnd_wait(&fake.changed, &fake.lock);
        fake.blocked = false;
    }
    mtx_unlock(&fake.lock);
    e->file = f->id;
    for (unsigned i = 0; i < 8; i++) e->word[i] = echo_word(e->word[i], e->tag);
    a->status = NV_OK;
    return NV_OK;
}

static uint32_t fake_alloc(void *args, uint32_t size) {
    if (size != sizeof(NVOS21_PARAMETERS)) return NV_ERR_INVALID_ARGUMENT;
    NVOS21_PARAMETERS *a = args;
    uint32_t *p = NULL;
    if (a->paramsSize && !(p = nvrm_user_translate(NvP64_VALUE(a->pAllocParms), a->paramsSize)))
        return NV_ERR_INVALID_ADDRESS;
    for (uint32_t i = 0; i < a->paramsSize / sizeof *p; i++) p[i] += a->hClass;
    if (!a->hObjectNew) {
        mtx_lock(&fake.lock);
        a->hObjectNew = 0xcaf00000u + ++fake.next_object;
        mtx_unlock(&fake.lock);
    }
    a->status = NV_OK;
    return NV_OK;
}

static uint32_t fake_map(struct fake_file *f, void *args, uint32_t size) {
    if (size != sizeof(nv_ioctl_nvos33_parameters_with_fd)) return NV_ERR_INVALID_ARGUMENT;
    nv_ioctl_nvos33_parameters_with_fd *m = args;
    mtx_lock(&fake.lock);
    f->map_pending = true;
    f->map_memory = m->params.hMemory;
    f->map_linear = map_linear(m->params.hMemory);
    mtx_unlock(&fake.lock);
    m->params.pLinearAddress = NV_PTR_TO_NvP64(map_linear(m->params.hMemory));
    m->params.status = NV_OK;
    return NV_OK;
}

static uint32_t fake_unmap(struct fake_file *f, void *args, uint32_t size) {
    if (size != sizeof(NVOS34_PARAMETERS)) return NV_ERR_INVALID_ARGUMENT;
    NVOS34_PARAMETERS *u = args;
    mtx_lock(&fake.lock);
    f->unmaps++;
    f->unmapped_linear = (uint64_t)(uintptr_t)NvP64_VALUE(u->pLinearAddress);
    if (f->mapping >= 0) n_handle_close(f->mapping);
    f->mapping = -1;
    mtx_unlock(&fake.lock);
    u->status = NV_OK;
    return NV_OK;
}

uint32_t nvrm_rm_ioctl(void *file, uint32_t escape, void *args, uint32_t size) {
    __atomic_add_fetch(&fake.ioctls, 1, __ATOMIC_RELAXED);
    switch (escape) {
    case NV_ESC_RM_CONTROL: return fake_control(file, args, size);
    case NV_ESC_RM_ALLOC: return fake_alloc(args, size);
    case NV_ESC_RM_MAP_MEMORY: return fake_map(file, args, size);
    case NV_ESC_RM_UNMAP_MEMORY: return fake_unmap(file, args, size);
    default: return NV_ERR_NOT_SUPPORTED;
    }
}

/* MEM_UNEXPORTABLE stands for a mapping RM made but cannot give out as a native object. */
n_handle nvrm_rm_mapping_object(void *file, uint64_t *offset, uint64_t *bytes, uint32_t *access) {
    struct fake_file *f = file;
    mtx_lock(&fake.lock);
    bool pending = f->map_pending;
    uint32_t memory = f->map_memory;
    f->map_pending = false;
    mtx_unlock(&fake.lock);
    if (!pending || memory == MEM_UNEXPORTABLE) return -1;
    n_handle h = n_shared_create(MAP_BYTES);
    if (h < 0) return -1;
    void *mapping;
    if (n_shared_map(h, MAP_BYTES, N_MEMORY_READ_WRITE, &mapping) < 0) {
        n_handle_close(h);
        return -1;
    }
    uint8_t *p = mapping;
    for (unsigned i = 0; i < 64; i++) p[MAP_OFFSET + i] = map_byte(memory, i);
    n_memory_release(p, MAP_BYTES);
    mtx_lock(&fake.lock);
    if (f->mapping >= 0) n_handle_close(f->mapping);
    f->mapping = h;
    mtx_unlock(&fake.lock);
    *offset = MAP_OFFSET;
    *bytes = MAP_BYTES;
    *access = N_MEMORY_READ_WRITE;
    return h;
}

/* A copy of file `id`'s record, taken under the lock. */
static struct fake_file file_record(uint32_t id) {
    struct fake_file copy = {.mapping = -1};
    mtx_lock(&fake.lock);
    for (struct fake_file *f = fake.files; f; f = f->next)
        if (f->id == id) copy = *f;
    mtx_unlock(&fake.lock);
    return copy;
}

static void fake_set(bool *flag, bool value) {
    mtx_lock(&fake.lock);
    *flag = value;
    cnd_broadcast(&fake.changed);
    mtx_unlock(&fake.lock);
}

/* Files still open, and files closed more than once. */
static void fake_count(unsigned *open, unsigned *over_closed) {
    unsigned o = 0, c = 0;
    mtx_lock(&fake.lock);
    for (struct fake_file *f = fake.files; f; f = f->next) {
        o += !f->closes;
        c += f->closes > 1;
    }
    mtx_unlock(&fake.lock);
    *open = o;
    if (over_closed) *over_closed = c;
}

/* The server ends a client on its own thread: wait until the fake has seen it. */
static bool wait_closed(uint32_t id) {
    uint64_t end = n_clock_uptime_ms() + WAIT_MS;
    while (!file_record(id).closes) {
        if (n_clock_uptime_ms() > end) return false;
        n_sleep_ms(2);
    }
    return true;
}

static bool wait_all_closed(void) {
    uint64_t end = n_clock_uptime_ms() + WAIT_MS;
    for (;;) {
        unsigned open;
        fake_count(&open, NULL);
        if (!open) return true;
        if (n_clock_uptime_ms() > end) return false;
        n_sleep_ms(2);
    }
}

/* ---- clients ---- */

/* An echo control on rm; the RM file that served it, 0 on any error or a wrong result. */
static uint32_t echo(struct nvrm *rm, uint32_t owner, uint32_t cmd, uint32_t tag) {
    struct echo e = {.tag = tag};
    for (unsigned i = 0; i < 8; i++) e.word[i] = (uint64_t)tag * 0x100000001u + i;
    if (nvrm_control(rm, owner, 0x1234, cmd, &e, sizeof e) != NV_OK) return 0;
    for (unsigned i = 0; i < 8; i++)
        if (e.word[i] != echo_word((uint64_t)tag * 0x100000001u + i, tag)) return 0;
    return e.file;
}

static n_result serve_result;
static int serve_returned;

static int server_main(void *arg) {
    (void)arg;
    serve_result = nvrm_serve(TARGET);
    __atomic_store_n(&serve_returned, 1, __ATOMIC_RELEASE);
    return 0;
}

/* The server thread starts listening on its own time. */
static struct nvrm *first_connect(void) {
    uint64_t end = n_clock_uptime_ms() + WAIT_MS;
    for (;;) {
        struct nvrm *rm = NULL;
        int64_t r = nvrm_connect_device_status(TARGET, &rm);
        if (r >= 0) return rm;
        if (n_clock_uptime_ms() > end || __atomic_load_n(&serve_returned, __ATOMIC_ACQUIRE)) {
            CHECK(false, "no server to connect to: %s (nvrm_serve: %s)", n_result_text(r),
                  n_result_text(serve_result));
            return NULL;
        }
        n_sleep_ms(5);
    }
}

static void test_single(struct nvrm *rm) {
    CHECK(nvrm_device_ready(rm), "HELLO reports a ready adapter");
    uint32_t id = echo(rm, 1, FAKE_CMD_ECHO, 0x11);
    CHECK(id, "echo control through the arena");
    struct fake_file f = file_record(id);
    CHECK(f.principal == (uint32_t)n_app_self(), "principal %u is this app (%u)", f.principal,
          (unsigned)n_app_self());
    CHECK(!f.admin, "an app client is not an administrator");

    uint32_t object = 0, params[16];
    for (unsigned i = 0; i < 16; i++) params[i] = i;
    uint32_t r = nvrm_alloc(rm, 1, 1, &object, 0x55, params, sizeof params);
    CHECK(r == NV_OK, "alloc: %#x", r);
    CHECK((object & 0xfff00000u) == 0xcaf00000u, "alloc returns RM's new handle (%#x)", object);
    bool copied = true;
    for (unsigned i = 0; i < 16; i++) copied &= params[i] == i + 0x55;
    CHECK(copied, "alloc parameters reach RM and come back");

    /* pointers outside the arena never reach RM's memory */
    struct echo outside = {0};
    NVOS54_PARAMETERS a = {.hClient = 1, .hObject = 0x1234, .cmd = FAKE_CMD_ECHO,
                           .params = NV_PTR_TO_NvP64(&outside), .paramsSize = sizeof outside};
    r = nvrm_escape(rm, NV_ESC_RM_CONTROL, &a, sizeof a);
    CHECK(r == NV_ERR_INVALID_ADDRESS, "a pointer to the client's stack is refused: %#x", r);
    nvrm_lock(rm);
    uint8_t *base = nvrm_scratch(rm, sizeof outside);
    a.params = NV_PTR_TO_NvP64(base - 8);
    r = nvrm_escape(rm, NV_ESC_RM_CONTROL, &a, sizeof a);
    CHECK(r == NV_ERR_INVALID_ADDRESS, "a block starting before the arena is refused: %#x", r);
    a.params = NV_PTR_TO_NvP64(base);
    memset(base, 0, sizeof outside);
    ((struct echo *)base)->tag = 0x22;
    r = nvrm_escape(rm, NV_ESC_RM_CONTROL, &a, sizeof a);
    CHECK(r == NV_OK && a.status == NV_OK && ((struct echo *)base)->file == id &&
              ((struct echo *)base)->word[3] == echo_word(0, 0x22),
          "an arena block is RM's view of the same memory: %#x", r);
    nvrm_unlock(rm);

    r = nvrm_escape(rm, NV_ESC_RM_MAP_MEMORY_DMA, &a, sizeof a);
    CHECK(r == NV_ERR_NOT_SUPPORTED, "RM's status comes back unchanged: %#x", r);
    unsigned before = __atomic_load_n(&fake.ioctls, __ATOMIC_RELAXED);
    r = nvrm_escape(rm, NVRM_HELLO + 1, &a, sizeof a);
    CHECK(r == NV_ERR_OPERATING_SYSTEM, "a non-RM escape is refused by the server: %#x", r);
    r = nvrm_escape(rm, NVRM_HELLO, &a, sizeof a);
    CHECK(r == NV_ERR_OPERATING_SYSTEM, "a second HELLO is refused: %#x", r);
    CHECK(__atomic_load_n(&fake.ioctls, __ATOMIC_RELAXED) == before, "refused escapes never reach RM");
    CHECK(echo(rm, 1, FAKE_CMD_ECHO, 0x33) == id, "the connection still serves the same RM file");

    void *address = NULL;
    r = nvrm_map(rm, 1, 2, 7, 0, 64, 0, &address);
    CHECK(r == NV_OK && address, "map: %#x", r);
    if (r == NV_OK && address) {
        bool same = true;
        for (unsigned i = 0; i < 64; i++) same &= ((uint8_t *)address)[i] == map_byte(7, i);
        CHECK(same, "the mapped object arrives with the mapping's offset");
        r = nvrm_unmap(rm, 1, 2, 7, address, 0);
        CHECK(r == NV_OK, "unmap: %#x", r);
        f = file_record(id);
        CHECK(f.unmaps == 1 && f.unmapped_linear == map_linear(7), "unmap names RM's address of the mapping");
        CHECK(f.mapping < 0, "RM released its view");
    }
    r = nvrm_map(rm, 1, 2, MEM_UNEXPORTABLE, 0, 64, 0, &address);
    CHECK(r == NV_ERR_OPERATING_SYSTEM, "an unexportable mapping fails: %#x", r);
    f = file_record(id);
    CHECK(f.unmaps == 2 && f.unmapped_linear == map_linear(MEM_UNEXPORTABLE),
          "the server gave RM's unexportable mapping back (unmaps %u)", f.unmaps);

    /* a client that goes with a mapping still held */
    r = nvrm_map(rm, 1, 2, 9, 0, 64, 0, &address);
    CHECK(r == NV_OK, "map before disconnect: %#x", r);
    nvrm_disconnect(rm);
    CHECK(wait_closed(id), "disconnect closes the client's RM file");
    f = file_record(id);
    CHECK(f.closes == 1 && f.mapping < 0, "closed once (%u), with its mapping", f.closes);

    fake_set(&fake.not_ready, true);
    rm = NULL;
    int64_t s = nvrm_connect_device_status(TARGET, &rm);
    CHECK(s >= 0 && !nvrm_device_ready(rm), "HELLO reports an adapter that is not ready: %s",
          n_result_text(s));
    uint32_t unready = rm ? echo(rm, 1, FAKE_CMD_ECHO, 0x44) : 0;
    if (rm) nvrm_disconnect(rm);
    CHECK(unready && wait_closed(unready), "a not-ready adapter's client still gets its file closed");
    fake_set(&fake.not_ready, false);

    mtx_lock(&fake.lock);
    fake.fail_open = true;
    unsigned opened = fake.open_calls;
    mtx_unlock(&fake.lock);
    rm = NULL;
    s = nvrm_connect_device_status(TARGET, &rm);
    CHECK(s == -N_ERR_NO_MEMORY && !rm, "RM's failed open is the client's: %s", n_result_text(s));
    mtx_lock(&fake.lock);
    unsigned tried = fake.open_calls - opened;
    mtx_unlock(&fake.lock);
    CHECK(tried == 1, "one open was tried (%u)", tried);
    s = nvrm_connect_device_status(TARGET, &rm);
    CHECK(s >= 0, "the server serves the next client: %s", n_result_text(s));
    if (rm) nvrm_disconnect(rm);
}

struct worker {
    thrd_t thread;
    bool started;
    struct nvrm *rm;
    uint32_t owner, first_tag, file;
    unsigned bad;
};

static int worker_main(void *arg) {
    struct worker *w = arg;
    for (uint32_t i = 0; i < ROUNDS; i++) {
        uint32_t file = echo(w->rm, w->owner, FAKE_CMD_ECHO, w->first_tag + i);
        if (!file || (w->file && file != w->file)) w->bad++;
        if (!w->file) w->file = file;
    }
    return 0;
}

/* All n at once, each on a thread of its own. */
static void workers_run(struct worker *w, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        w[i].started = w[i].rm && thrd_create(&w[i].thread, worker_main, &w[i]) == thrd_success;
        CHECK(w[i].started, "worker %u", i);
    }
    for (unsigned i = 0; i < n; i++)
        if (w[i].started) thrd_join(w[i].thread, NULL);
}

static void test_concurrent(void) {
    struct worker w[WORKERS] = {0};
    for (unsigned i = 0; i < WORKERS; i++) {
        int64_t s = nvrm_connect_device_status(TARGET, &w[i].rm);
        CHECK(s >= 0, "client %u: %s", i, n_result_text(s));
        w[i].owner = 100 + i;
        w[i].first_tag = (i + 1) << 16;
    }
    workers_run(w, WORKERS);
    for (unsigned i = 0; i < WORKERS; i++) {
        CHECK(!w[i].bad && w[i].file, "client %u: %u of %u results were wrong or another's", i, w[i].bad, ROUNDS);
        for (unsigned j = 0; j < i; j++)
            CHECK(w[i].file != w[j].file, "clients %u and %u were served by one RM file", i, j);
    }

    /* one connection, several threads: libnvrm orders them, the server keeps one file */
    struct worker s[SHARERS] = {0};
    struct nvrm *shared = NULL;
    int64_t r = nvrm_connect_device_status(TARGET, &shared);
    CHECK(r >= 0, "shared client: %s", n_result_text(r));
    if (shared) {
        for (unsigned i = 0; i < SHARERS; i++) {
            s[i].rm = shared;
            s[i].owner = 200;
            s[i].first_tag = (0x80 + i) << 16;
        }
        workers_run(s, SHARERS);
        for (unsigned i = 0; i < SHARERS; i++)
            CHECK(!s[i].bad && s[i].file == s[0].file, "sharer %u: %u wrong results", i, s[i].bad);
        nvrm_disconnect(shared);
    }
    for (unsigned i = 0; i < WORKERS; i++)
        if (w[i].rm) nvrm_disconnect(w[i].rm);
    mtx_lock(&fake.lock);
    unsigned mixups = fake.mixups;
    mtx_unlock(&fake.lock);
    CHECK(!mixups, "%u requests reached another connection's RM file", mixups);
    CHECK(wait_all_closed(), "every concurrent client's file closed");
}

struct blocked_call {
    struct nvrm *rm;
    uint32_t file;
};

static int blocked_main(void *arg) {
    struct blocked_call *b = arg;
    b->file = echo(b->rm, 300, FAKE_CMD_BLOCK, 0x55);
    return 0;
}

static void test_blocking(void) {
    struct nvrm *slow = NULL, *other = NULL;
    CHECK(nvrm_connect_device_status(TARGET, &slow) >= 0 && nvrm_connect_device_status(TARGET, &other) >= 0,
          "two clients");
    if (!slow || !other) {
        if (slow) nvrm_disconnect(slow);
        if (other) nvrm_disconnect(other);
        return;
    }
    struct blocked_call b = {.rm = slow};
    thrd_t thread;
    bool started = thrd_create(&thread, blocked_main, &b) == thrd_success;
    CHECK(started, "blocking thread");
    uint64_t end = n_clock_uptime_ms() + WAIT_MS;
    mtx_lock(&fake.lock);
    while (started && !fake.blocked && n_clock_uptime_ms() <= end) {
        mtx_unlock(&fake.lock);
        n_sleep_ms(2);
        mtx_lock(&fake.lock);
    }
    bool blocked = fake.blocked;
    mtx_unlock(&fake.lock);
    CHECK(blocked, "a request waits inside RM");
    uint32_t id = echo(other, 301, FAKE_CMD_ECHO, 0x66);
    mtx_lock(&fake.lock);
    bool still = fake.blocked;
    fake.release = true;
    cnd_broadcast(&fake.changed);
    mtx_unlock(&fake.lock);
    CHECK(id && still, "another client is served while the first waits in RM");
    if (started) thrd_join(thread, NULL);
    CHECK(b.file && b.file != id, "the waiting request completes once RM lets it");
    fake_set(&fake.release, false);
    nvrm_disconnect(slow);
    nvrm_disconnect(other);
    CHECK(wait_all_closed(), "both files closed");
}

static void test_thread_limit(void) {
    static struct nvrm *conn[LIMIT_TRIES];
    static uint32_t file[LIMIT_TRIES];
    unsigned accepted = 0, refused = 0, open, over;
    bool late = false;
    fake_count(&open, NULL);
    CHECK(!open, "no client left from earlier checks (%u)", open);
    for (unsigned i = 0; i < LIMIT_TRIES; i++) {
        struct nvrm *rm = NULL;
        int64_t s = nvrm_connect_device_status(TARGET, &rm);
        if (s >= 0) {
            late |= refused != 0;
            conn[accepted++] = rm;
            continue;
        }
        refused++;
        CHECK(s == -N_ERR_CLOSED, "a refused client sees its connection closed: %s", n_result_text(s));
    }
    printf("nvrm_servetest: %u clients served, %u refused at the thread limit\n", accepted, refused);
    CHECK(refused && accepted && accepted < THREAD_LIMIT, "the limit refuses some clients (%u served)", accepted);
    CHECK(!late, "no client was served after the first refusal while none had gone");
    fake_count(&open, NULL);
    CHECK(open == accepted, "refused clients opened no RM file (%u open, %u served)", open, accepted);

    unsigned working = 0;
    for (unsigned i = 0; i < accepted; i++) {
        file[i] = echo(conn[i], 400 + i, FAKE_CMD_ECHO, 0x7700 + i);
        working += file[i] != 0;
    }
    CHECK(working == accepted, "every served client still works at the limit (%u of %u)", working, accepted);

    if (accepted) {
        /* the slot comes back once the client's thread has ended, just after its file closed */
        nvrm_disconnect(conn[0]);
        CHECK(wait_closed(file[0]), "the disconnected client's file closed");
        uint64_t end = n_clock_uptime_ms() + WAIT_MS;
        int64_t s;
        do {
            conn[0] = NULL;
            s = nvrm_connect_device_status(TARGET, &conn[0]);
            if (s < 0) n_sleep_ms(2);
        } while (s < 0 && n_clock_uptime_ms() <= end);
        CHECK(s >= 0, "a new client is served once one has gone: %s", n_result_text(s));
        if (s >= 0) {
            file[0] = echo(conn[0], 999, FAKE_CMD_ECHO, 0x8800);
            CHECK(file[0], "the new client works");
            struct nvrm *extra = NULL;
            s = nvrm_connect_device_status(TARGET, &extra);
            CHECK(s == -N_ERR_CLOSED, "at the limit again, the next client is refused: %s", n_result_text(s));
            if (extra) nvrm_disconnect(extra);
        } else {
            conn[0] = conn[--accepted];
        }
    }
    for (unsigned i = 0; i < accepted; i++) nvrm_disconnect(conn[i]);
    CHECK(wait_all_closed(), "every client's file closed after the limit");
    fake_count(&open, &over);
    CHECK(!open && !over, "%u files left open, %u closed twice", open, over);
}

int main(void) {
    if (mtx_init(&fake.lock, mtx_plain) != thrd_success || cnd_init(&fake.changed) != thrd_success) {
        printf("nvrm_servetest: 1 failed\n");
        return 1;
    }
    thrd_t server;
    if (thrd_create(&server, server_main, NULL) != thrd_success) {
        printf("nvrm_servetest: no server thread\nnvrm_servetest: 1 failed\n");
        return 1;
    }
    thrd_detach(server); /* it serves until this app ends */
    struct nvrm *rm = first_connect();
    if (rm) {
        test_single(rm);
        test_concurrent();
        test_blocking();
        test_thread_limit();
    }
    CHECK(!__atomic_load_n(&serve_returned, __ATOMIC_ACQUIRE), "the server is still serving (ended: %s)",
          n_result_text(serve_result));
    printf("nvrm_servetest: %d failed\n", failed);
    return failed != 0;
}
