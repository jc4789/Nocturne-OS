/* libFuzzer harness: Ethernet/ARP/IPv4/ICMP/UDP/DHCP/TCP receive paths of
   kernel/src/net/net.c, including TCP listen/accept, option parsing, the
   out-of-order list, and the user-facing send/recv/accept calls.

   Input = a sequence of records: [kind:u8][len:u16 LE][body]. Checksums and the
   IP header are rebuilt for kinds 1 and 2 so the fuzzer reaches the parsers
   behind the checksum gates; kind 0 injects an unmodified Ethernet frame. */
#include "../../../kernel/src/net/net.c"
#include "stubs.h"

static uint8_t my_mac[6] = {2, 0, 0, 0, 0, 1};
static int tx_frames;
static int fz_send(const void *f, size_t n) {
    tx_frames++;
    if (n < 14) abort();
    /* touch every byte so ASan sees an out-of-bounds frame */
    volatile uint8_t sink = 0;
    for (size_t i = 0; i < n; i++) sink ^= ((const uint8_t *)f)[i];
    return 0;
}
static struct netif fz_nif = {.driver = "fuzz", .mac = {2, 0, 0, 0, 0, 1}, .send = fz_send};

static uint16_t cs(const uint8_t *p, size_t n, uint32_t sum) {
    for (size_t i = 0; i + 1 < n; i += 2) sum += (uint32_t)p[i] << 8 | p[i + 1];
    if (n & 1) sum += (uint32_t)p[n - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static void inject_ip(uint8_t proto, uint32_t src, uint32_t dst, const uint8_t *l4, size_t n, bool fix) {
    static uint8_t buf[2048];
    if (n > 1500) n = 1500;
    uint8_t *e = buf, *ip = buf + 14, *u = ip + 20;
    memcpy(e, my_mac, 6);
    memcpy(e + 6, "\x02\x00\x00\x00\x00\x09", 6);
    e[12] = 8; e[13] = 0;
    memset(ip, 0, 20);
    ip[0] = 0x45; ip[2] = (uint8_t)((20 + n) >> 8); ip[3] = (uint8_t)(20 + n); ip[8] = 64; ip[9] = proto;
    ip[12] = src >> 24; ip[13] = src >> 16; ip[14] = src >> 8; ip[15] = src;
    ip[16] = dst >> 24; ip[17] = dst >> 16; ip[18] = dst >> 8; ip[19] = dst;
    uint16_t c = cs(ip, 20, 0); ip[10] = c >> 8; ip[11] = (uint8_t)c;
    memcpy(u, l4, n);
    if (fix) {
        size_t off = proto == 6 ? 16 : proto == 17 ? 6 : proto == 1 ? 2 : n;
        if (n >= 2 && off <= n - 2) {
            u[off] = u[off + 1] = 0;
            uint32_t sum = 0;
            if (proto != 1) {
                sum = (src >> 16) + (src & 0xffff) + (dst >> 16) + (dst & 0xffff) + proto + (uint32_t)n;
            }
            uint16_t k = cs(u, n, sum);
            if (proto == 17 && k == 0) k = 0xffff;
            u[off] = k >> 8; u[off + 1] = (uint8_t)k;
        }
    }
    eth_input(buf, 14 + 20 + n);
}

static void setup(void) {
    static int done;
    if (done) {
        /* reset connection table between runs so state does not accumulate */
        for (size_t i = 0; i < tcp_capacity; i++) if (tcbs[i] && tcbs[i]->state != T_FREE) tcp_free(tcbs[i]);
    }
    done = 1;
    nif = &fz_nif;
    my_ip = 0x0a000002; my_mask = 0xffffff00; my_gw = 0x0a000001; my_dns = my_gw;
    configured = true;
    receive_batch = false;
    fault_held_len = 0;
    tcp_rx_target = tcp_tx_target = 1u << 16;
    fz_task.pid = 7;
    timer_ticks = 1000;
    memset(&fault, 0, sizeof fault);
    dhcp_state = DHCP_REQUEST; dhcp_xid = 0x01020304;
    /* a listener and a few handles */
    fz_task.pid = 7;
    int l = (int)sys_tcp_listen(0, 8080, 2);
    (void)l;
    for (int k = 0; k < 2; k++) {
        /* synthetic established connection, fields as sys_tcp_connect leaves them */
        int h = tcp_alloc_slot();
        struct tcb *t = tcbs[h];
        memset(t, 0, sizeof *t);
        t->rx_capacity = t->tx_capacity = 1u << 14;
        t->rx = kmalloc(t->rx_capacity); t->tx = kmalloc(t->tx_capacity);
        t->pid = 7; tcp_generation++; t->generation = tcp_generation; t->user_open = true;
        while ((t->rx_capacity - 1) >> t->rx_shift > UINT16_MAX) t->rx_shift++;
        t->rip = 0x0a000009; t->rport = 4000 + k; t->lport = 5000 + k;
        memcpy(t->mac, "\x02\x00\x00\x00\x00\x09", 6);
        t->iss = 1000; t->snd_una = 1000; t->snd_nxt = t->snd_max = 1000;
        t->mss = 536; t->rto = 1000;
        t->state = k ? T_FIN_WAIT1 : T_ESTABLISHED;
        t->rcv_nxt = 5000; t->snd_wnd = 65535; t->snd_wl1 = 5000; t->snd_wl2 = 1000;
        t->cwnd = 4 * 536; t->ssthresh = 65535;
        if (k) { t->fin_pending = false; t->fin_sent = true; t->fin_seq = 1000; t->snd_nxt = t->snd_max = 1001; }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    setup();
    size_t at = 0;
    int steps = 0;
    while (at + 3 <= size && steps++ < 64) {
        uint8_t kind = data[at];
        size_t len = data[at + 1] | (size_t)data[at + 2] << 8;
        at += 3;
        if (len > size - at) len = size - at;
        const uint8_t *b = data + at;
        at += len;
        switch (kind % 7) {
        case 0: {
            static uint8_t f[1600];
            if (len > 1536) len = 1536;
            memcpy(f, b, len);
            if (len >= 6 && (b[0] & 0x80)) memcpy(f, my_mac, 6);
            eth_input(f, len);
            break;
        }
        case 1: { /* generic IPv4 */
            if (len < 6) break;
            uint32_t src = (uint32_t)b[1] << 24 | b[2] << 16 | b[3] << 8 | b[4];
            if (b[5] & 1) src = 0x0a000009;
            inject_ip(b[0] % 4 == 0 ? 1 : b[0] % 4 == 1 ? 17 : b[0] % 4 == 2 ? 6 : b[0], src, my_ip, b + 6, len - 6,
                      (b[5] & 2) != 0);
            break;
        }
        case 2: { /* TCP aimed at an existing tuple */
            if (len < 22) break;
            uint8_t seg[1500];
            size_t n = MIN(len - 2, sizeof seg);
            memcpy(seg, b + 2, n);
            struct tcb *t = tcbs[b[0] % tcp_capacity];
            uint32_t src = 0x0a000009;
            uint16_t sp = 4000, dp = 8080;
            if (b[1] & 1) { sp = 6000 + (b[1] >> 4); dp = 8080; }
            else if (t && t->state != T_FREE && t->state != T_LISTEN) { src = t->rip; sp = t->rport; dp = t->lport; }
            seg[0] = sp >> 8; seg[1] = (uint8_t)sp; seg[2] = dp >> 8; seg[3] = (uint8_t)dp;
            inject_ip(6, src, my_ip, seg, n, true);
            break;
        }
        case 3: { /* user-facing operations */
            if (len < 3) break;
            int h = b[1] % 8;
            switch (b[0] % 8) {
            case 0: { uint8_t buf[4096]; tcp_recv_data(h, buf, MIN((size_t)b[2] * 37, sizeof buf), 0, false); break; }
            case 1: { uint8_t buf[4096]; memset(buf, 0x5a, sizeof buf); tcp_send_data(h, buf, MIN((size_t)b[2] * 53, sizeof buf), uptime_ms() + 1, false, true); break; }
            case 2: if (tcbs[h] && tcbs[h]->state != T_FREE) tcp_close(tcbs[h]); break;
            case 3: { struct n_sockaddr r; sys_tcp_accept(h, 0, &r); break; }
            case 4: tcp_flush_pending(false); break;
            case 5: timer_ticks += (uint64_t)b[2] * 100; tcp_tick(); break;
            case 6: sys_tcp_listen(0, 8080 + b[2] % 3, b[2] % 5); break;
            case 7: tcp_flush_pending(true); break;
            }
            break;
        }
        case 4: { /* DHCP reply through the real UDP path */
            if (len < 240) break;
            uint8_t u[1500];
            size_t n = MIN(len + 8, sizeof u);
            memset(u, 0, 8);
            memcpy(u + 8, b, n - 8);
            u[0] = 0; u[1] = 67; u[2] = 0; u[3] = 68; u[4] = n >> 8; u[5] = (uint8_t)n;
            u[8] = 2; u[8 + 4] = 1; u[8 + 5] = 2; u[8 + 6] = 3; u[8 + 7] = 4;
            u[8 + 236] = 99; u[8 + 237] = 130; u[8 + 238] = 83; u[8 + 239] = 99;
            dhcp_state = (b[0] & 1) ? DHCP_REQUEST : DHCP_DISCOVER;
            inject_ip(17, 0x0a000001, my_ip, u, n, true);
            break;
        }
        case 5: { /* ARP */
            uint8_t f[64];
            memset(f, 0, sizeof f);
            memcpy(f, "\xff\xff\xff\xff\xff\xff", 6);
            f[12] = 8; f[13] = 6;
            memcpy(f + 14, b, MIN(len, sizeof f - 14));
            if (len > 8) { f[14] = 0; f[15] = 1; f[16] = 8; f[17] = 0; f[18] = 6; f[19] = 4; }
            eth_input(f, MIN(len, sizeof f - 14) + 14);
            break;
        }
        case 6: /* UDP to a user socket, exercising the queue */
            if (len < 2) break;
            { uint8_t u[600]; size_t n = MIN(len + 6, sizeof u); memset(u, 0, n); memcpy(u + 8, b, n - 8 > len ? len : n - 8);
              u[0] = 0; u[1] = 53; u[2] = 0x1f; u[3] = 0x90; u[4] = n >> 8; u[5] = (uint8_t)n;
              inject_ip(17, 0x0a000009, my_ip, u, n, true); }
            break;
        }
    }
    return 0;
}
