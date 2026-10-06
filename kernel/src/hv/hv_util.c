/* Hyper-V integration services: heartbeat (so the host sees the guest is alive), shutdown (the
   Shut Down / Restart buttons in Hyper-V Manager and Stop-VM), time synchronisation and the
   key-value pair exchange (how the host learns the guest's name, OS and IP address). Each is a
   VMBus channel speaking the "IC" message format: the host sends a request, the guest fills in
   the answer and sends the same message back. The first request on every channel negotiates the
   framework and message versions. */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/timer.h"
#include "hv/vmbus.h"
#include "net/net.h"
#include "sys/sched.h"

void power_off(void);
void power_reboot(void);

enum { IC_NEGOTIATE = 0, IC_HEARTBEAT = 1, IC_KVP = 2, IC_SHUTDOWN = 3, IC_TIMESYNC = 4 };
#define IC_FLAG_TRANSACTION 1
#define IC_FLAG_RESPONSE    4
#define IC_E_FAIL 0x80004005u
#define IC_S_CONT 0x80070103u /* KVP: no more items in this pool */

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

/* ---- key-value pair exchange ----
   The host keeps pools of string pairs. It reads the "auto" pool by enumerating it item by item
   (what Hyper-V Manager shows on the Networking tab and Get-VM's details come from there), writes
   its own pairs into the others, and asks for each adapter's addresses with GET_IP_INFO. Keys and
   values travel as UTF-16. */
enum { KVP_GET, KVP_SET, KVP_DELETE, KVP_ENUMERATE, KVP_GET_IP_INFO, KVP_SET_IP_INFO };
enum { KVP_POOL_AUTO = 2 };
#define KVP_REG_SZ 1

struct kvp_value {
    uint32_t type, key_size, value_size; /* sizes in bytes, with the terminating 0 */
    uint16_t key[256];
    uint16_t value[1024];
} PACKED;

struct kvp_ip {
    uint8_t op, pool; /* no padding in this one */
    uint16_t adapter_id[128];
    uint8_t family, dhcp;
    uint16_t ip[1024], mask[1024], gateway[512], dns[1024];
} PACKED;

/* ASCII into a UTF-16 field of n units; returns the size in bytes, with the 0 */
static uint32_t to_utf16(uint16_t *out, size_t n, const char *s) {
    size_t i = 0;
    for (; s[i] && i + 1 < n; i++) out[i] = (uint8_t)s[i];
    out[i] = 0;
    return (uint32_t)(i + 1) * 2;
}

static void ipstr(char *buf, size_t n, uint32_t ip) {
    ksnprintf(buf, n, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 0xFF, ip >> 8 & 0xFF, ip & 0xFF);
}

/* the auto pool, in the order Linux guests report it */
static bool auto_item(uint32_t index, const char **key, char *value, size_t n) {
    static const char *const keys[] = {"FullyQualifiedDomainName", "IntegrationServicesVersion",
                                       "NetworkAddressIPv4", "NetworkAddressIPv6", "OSBuildNumber", "OSName",
                                       "OSMajorVersion", "OSMinorVersion", "OSVersion", "ProcessorArchitecture"};
    if (index >= ARRAY_SIZE(keys)) return false;
    *key = keys[index];
    uint8_t mac[6];
    uint32_t ip, mask, gw, dns;
    value[0] = 0;
    switch (index) {
    case 0: strlcpy(value, "nocturne", n); break;
    case 1: strlcpy(value, "3.1", n); break;
    case 2:
        if (net_config(mac, &ip, &mask, &gw, &dns)) ipstr(value, n, ip);
        break;
    case 3: break;
    case 4: strlcpy(value, OS_VERSION, n); break;
    case 5: strlcpy(value, OS_NAME, n); break;
    case 6: strlcpy(value, "0", n); break;
    case 7: strlcpy(value, "9", n); break;
    case 8: strlcpy(value, "0.9", n); break;
    case 9: strlcpy(value, "x86_64", n); break;
    }
    return true;
}

/* the host names an adapter by its MAC address; compare the hex digits only */
static bool same_mac(const uint16_t *id, const uint8_t mac[6]) {
    static const char hex[] = "0123456789abcdef";
    int k = 0;
    for (int i = 0; i < 128 && id[i] && k < 12; i++) {
        uint16_t c = id[i];
        if (c >= 'A' && c <= 'F') c += 'a' - 'A';
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) continue;
        if (c != (uint8_t)hex[k & 1 ? mac[k / 2] & 0xF : mac[k / 2] >> 4]) return false;
        k++;
    }
    return k == 12;
}

static void get_ip_info(struct ic_header *h, struct kvp_ip *m) {
    uint8_t mac[6];
    uint32_t ip, mask, gw, dns;
    if (!net_config(mac, &ip, &mask, &gw, &dns) || !same_mac(m->adapter_id, mac)) {
        h->status = IC_E_FAIL;
        return;
    }
    char s[16];
    m->family = 1; /* IPv4 */
    m->dhcp = 1;
    ipstr(s, sizeof s, ip);
    to_utf16(m->ip, ARRAY_SIZE(m->ip), s);
    ipstr(s, sizeof s, mask);
    to_utf16(m->mask, ARRAY_SIZE(m->mask), s);
    ipstr(s, sizeof s, gw);
    to_utf16(m->gateway, ARRAY_SIZE(m->gateway), s);
    ipstr(s, sizeof s, dns);
    to_utf16(m->dns, ARRAY_SIZE(m->dns), s);
}

static void kvp(struct service *s, struct ic_header *h, uint8_t *body, uint32_t len) {
    (void)s;
    if (h->type != IC_KVP || len < 4) return;
    uint8_t op = body[0], pool = body[1];
    if (op == KVP_GET_IP_INFO) {
        if (len < sizeof(struct kvp_ip)) h->status = IC_E_FAIL;
        else get_ip_info(h, (struct kvp_ip *)body);
        return;
    }
    if (op == KVP_ENUMERATE) {
        if (len < 4 + 4 + sizeof(struct kvp_value)) {
            h->status = IC_E_FAIL;
            return;
        }
        uint32_t index = *(uint32_t *)(body + 4);
        struct kvp_value *v = (struct kvp_value *)(body + 8);
        const char *key;
        char value[64];
        if (pool != KVP_POOL_AUTO || !auto_item(index, &key, value, sizeof value)) {
            h->status = IC_S_CONT; /* the end of the pool */
            return;
        }
        v->type = KVP_REG_SZ;
        v->key_size = to_utf16(v->key, ARRAY_SIZE(v->key), key);
        v->value_size = to_utf16(v->value, ARRAY_SIZE(v->value), value);
        return;
    }
    if (op == KVP_GET) h->status = IC_E_FAIL; /* we keep none of the host's pairs */
    /* SET, DELETE, SET_IP_INFO: accepted and forgotten */
}

static struct service services[] = {
    {.type = GUID_INIT(0x57164f39, 0x9115, 0x4e78, 0xab, 0x55, 0x38, 0x2f, 0x3b, 0xd5, 0x42, 0x2d),
     .name = "heartbeat", .versions = {0x30000, 0x10000}, .handle = heartbeat},
    {.type = GUID_INIT(0x0e0b6031, 0x5213, 0x4934, 0x81, 0x8b, 0x38, 0xd9, 0x0c, 0xed, 0x39, 0xdb),
     .name = "shutdown", .versions = {0x30001, 0x30000, 0x10000}, .handle = shutdown},
    {.type = GUID_INIT(0x9527e630, 0xd0ae, 0x497b, 0xad, 0xce, 0xe8, 0x0a, 0xb0, 0x17, 0x5c, 0xaf),
     .name = "time sync", .versions = {0x40000, 0x30000, 0x10000}, .handle = timesync},
    {.type = GUID_INIT(0xa9a0f4e7, 0x5a45, 0x4d96, 0xb8, 0x27, 0x8a, 0x84, 0x1e, 0x8c, 0x03, 0xe6),
     .name = "key-value exchange", .versions = {0x40000, 0x30000, 0x10000}, .handle = kvp},
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
    static uint8_t pkt[16384]; /* interrupt context: callbacks do not nest. KVP needs ~7.5 KiB */
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
    char names[96] = "";
    for (size_t i = 0; i < ARRAY_SIZE(services); i++) {
        struct vmbus_channel *ch = vmbus_find(&services[i].type, 0);
        if (!ch || vmbus_open(ch, 32 * 1024, 32 * 1024, util_callback, &services[i])) continue;
        if (names[0]) strlcat(names, ", ", sizeof names);
        strlcat(names, services[i].name, sizeof names);
    }
    if (!names[0]) return;
    kthread_create("hv-util", util_thread, NULL);
    kprintf("hv-util: %s\n", names);
}
