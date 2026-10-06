/* Hyper-V integration services: heartbeat (so the host sees the guest is alive), shutdown (the
   Shut Down / Restart buttons in Hyper-V Manager and Stop-VM) and time synchronisation. Each is a
   VMBus channel speaking the "IC" message format: the host sends a request, the guest fills in
   the answer and sends the same message back. The first request on every channel negotiates the
   framework and message versions. */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/timer.h"
#include "hv/vmbus.h"
#include "sys/sched.h"

void power_off(void);
void power_reboot(void);

enum { IC_NEGOTIATE = 0, IC_HEARTBEAT = 1, IC_SHUTDOWN = 3, IC_TIMESYNC = 4 };
#define IC_FLAG_TRANSACTION 1
#define IC_FLAG_RESPONSE    4
#define IC_E_FAIL 0x80004005u

struct ic_version {
    uint16_t major, minor;
} PACKED;

struct ic_header {
    uint32_t pipe_flags, pipe_size; /* the "vmbus pipe" header */
    struct ic_version frame_ver;
    uint16_t type;
    struct ic_version msg_ver;
    uint16_t size;
    uint32_t status;
    uint8_t tid, flags, reserved[2];
} PACKED;

struct ic_negotiate {
    uint16_t frame_count, msg_count;
    uint32_t reserved;
    struct ic_version v[]; /* framework versions, then message versions */
} PACKED;

struct service {
    struct guid type;
    const char *name;
    uint32_t versions[3]; /* message versions we accept, best first, major << 16 | minor */
    void (*handle)(struct service *s, struct ic_header *h, uint8_t *body, uint32_t len);
    struct ic_version msg_ver; /* negotiated */
    bool ready;
};

static const uint32_t frame_versions[] = {0x30000, 0x10000};

/* ---- deferred work: power actions must not run in interrupt context ---- */

enum { ACT_NONE, ACT_SHUTDOWN, ACT_REBOOT };
static volatile int pending_action;
static struct wait_queue util_wq;

static void util_thread(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t f = irq_save();
        if (pending_action == ACT_NONE) wq_wait_timeout(&util_wq, 1000);
        irq_restore(f);
        int a = pending_action;
        if (a == ACT_NONE) continue;
        kprintf("hv-util: the host asked us to %s\n", a == ACT_REBOOT ? "restart" : "shut down");
        sleep_ms(100); /* let our reply reach the host */
        if (a == ACT_REBOOT) power_reboot();
        else power_off();
    }
}

/* ---- the services ---- */

static void heartbeat(struct service *s, struct ic_header *h, uint8_t *body, uint32_t len) {
    (void)s;
    (void)h;
    if (len >= 8) ++*(uint64_t *)body; /* answer with the next sequence number */
}

static void shutdown(struct service *s, struct ic_header *h, uint8_t *body, uint32_t len) {
    (void)s;
    if (len < 12) return;
    uint32_t flags = ((uint32_t *)body)[2];
    if (flags <= 1) pending_action = ACT_SHUTDOWN;
    else if (flags <= 3) pending_action = ACT_REBOOT;
    else h->status = IC_E_FAIL; /* hibernate: not supported */
}

/* The RTC on Hyper-V holds the host's local time and Nocturne has no time zones, so the clock
   runs in local time. The host sends UTC: we measure the host's offset from UTC once (rounded to
   15 minutes) and use the host's time through it, which corrects drift but keeps the zone. */
static bool tz_known;
static int64_t tz_offset;

static void timesync(struct service *s, struct ic_header *h, uint8_t *body, uint32_t len) {
    (void)h;
    uint64_t parent;
    uint8_t flags;
    if (s->msg_ver.major >= 4) {
        if (len < 17) return;
        parent = *(uint64_t *)body;
        flags = body[16];
    } else {
        if (len < 25) return;
        parent = *(uint64_t *)body;
        flags = body[24];
    }
    /* 100 ns units since 1601 */
    int64_t utc = (int64_t)(parent / 10000000) - 11644473600LL;
    if (!tz_known) {
        int64_t d = time_now() - utc;
        tz_offset = (d >= 0 ? d + 450 : d - 450) / 900 * 900;
        tz_known = true;
    }
    if (flags & 3) { /* SYNC (after boot or resume) or SAMPLE */
        int64_t want = utc + tz_offset;
        int64_t diff = want - time_now();
        if (diff > 1 || diff < -1) {
            time_set(want);
            if (flags & 1) kprintf("hv-util: clock adjusted by %ld s\n", diff);
        }
    }
}

static struct service services[] = {
    {.type = GUID_INIT(0x57164f39, 0x9115, 0x4e78, 0xab, 0x55, 0x38, 0x2f, 0x3b, 0xd5, 0x42, 0x2d),
     .name = "heartbeat", .versions = {0x30000, 0x10000}, .handle = heartbeat},
    {.type = GUID_INIT(0x0e0b6031, 0x5213, 0x4934, 0x81, 0x8b, 0x38, 0xd9, 0x0c, 0xed, 0x39, 0xdb),
     .name = "shutdown", .versions = {0x30001, 0x30000, 0x10000}, .handle = shutdown},
    {.type = GUID_INIT(0x9527e630, 0xd0ae, 0x497b, 0xad, 0xce, 0xe8, 0x0a, 0xb0, 0x17, 0x5c, 0xaf),
     .name = "time sync", .versions = {0x40000, 0x30000, 0x10000}, .handle = timesync},
};

static bool pick(const struct ic_version *offered, int n, const uint32_t *ours, int nours, struct ic_version *out) {
    for (int i = 0; i < nours && ours[i]; i++)
        for (int j = 0; j < n; j++)
            if (offered[j].major == ours[i] >> 16 && offered[j].minor == (ours[i] & 0xFFFF)) {
                *out = offered[j];
                return true;
            }
    return false;
}

static void negotiate(struct service *s, struct ic_header *h, uint8_t *body, uint32_t len) {
    struct ic_negotiate *n = (void *)body;
    h->size = 0x10;
    if (len < sizeof *n || sizeof *n + (n->frame_count + n->msg_count) * 4u > len) return;
    struct ic_version fw, msg;
    bool ok = pick(n->v, n->frame_count, frame_versions, ARRAY_SIZE(frame_versions), &fw) &&
              pick(n->v + n->frame_count, n->msg_count, s->versions, ARRAY_SIZE(s->versions), &msg);
    if (!ok) {
        n->frame_count = n->msg_count = 0;
        kprintf("hv-util: %s: no common version\n", s->name);
        return;
    }
    n->frame_count = n->msg_count = 1;
    n->v[0] = fw;
    n->v[1] = msg;
    s->msg_ver = msg;
    s->ready = true;
}

static void util_callback(struct vmbus_channel *ch) {
    static uint8_t pkt[4096]; /* interrupt context: callbacks do not nest */
    struct service *s = ch->priv;
    int n;
    while ((n = vmbus_recv(ch, pkt, sizeof pkt)) != 0) {
        if (n < 0) continue;
        struct vmpacket_desc *d = (void *)pkt;
        if (d->type != VM_PKT_DATA_INBAND) continue;
        uint32_t len = vmpacket_datalen(pkt);
        struct ic_header *h = vmpacket_data(pkt);
        if (len < sizeof *h) continue;
        uint8_t *body = (uint8_t *)(h + 1);
        uint32_t blen = len - sizeof *h;
        h->status = 0;
        if (h->type == IC_NEGOTIATE) negotiate(s, h, body, blen);
        else s->handle(s, h, body, blen);
        h->flags = IC_FLAG_TRANSACTION | IC_FLAG_RESPONSE;
        vmbus_send(ch, VM_PKT_DATA_INBAND, 0, h, len, d->trans_id);
        if (pending_action != ACT_NONE) wq_wake_all(&util_wq);
    }
}

void hv_util_init(void) {
    if (!vmbus_ready()) return;
    char names[64] = "";
    for (size_t i = 0; i < ARRAY_SIZE(services); i++) {
        struct vmbus_channel *ch = vmbus_find(&services[i].type, 0);
        if (!ch || vmbus_open(ch, 16 * 1024, 16 * 1024, util_callback, &services[i])) continue;
        if (names[0]) strlcat(names, ", ", sizeof names);
        strlcat(names, services[i].name, sizeof names);
    }
    if (!names[0]) return;
    kthread_create("hv-util", util_thread, NULL);
    kprintf("hv-util: %s\n", names);
}
