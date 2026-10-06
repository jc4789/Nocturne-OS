/* A small RDP server, so that Hyper-V's enhanced session mode can show the Nocturne desktop.
   VMConnect reaches it through a Hyper-V socket (port 3389), the way it reaches xrdp in a Linux
   guest. Like xrdp configured for Hyper-V, it uses standard RDP security with no encryption:
   the transport never leaves the host, and only the host's administrators can open it.

   What is implemented (MS-RDPBCGR):
     - the connection sequence: X.224, MCS with GCC conference data, channel joins, client
       info, licensing (we answer "valid client"), capability exchange and finalisation;
     - output as fast-path updates: uncompressed bitmaps in 64x64 tiles (only tiles that
       changed are sent) and the pointer shape;
     - input as fast-path or slow-path events: scancodes, Unicode characters, mouse and wheel.
   Of the virtual channels the clipboard's is used (text both ways), and drdynvc for display
   control; sound, drives and printers are joined but ignored. The desktop takes the size of the
   client's window when it connects and whenever the window is resized, and goes back to its
   own when the client leaves. */
#include "kernel.h"
#include "dev/input.h"
#include "gui/wm.h"
#include "hv/hvsock.h"
#include "mm/heap.h"
#include "sys/sched.h"

#define RDP_PORT 3389
#define MAX_PDU 65536
#define TILE 64

#define PROTOCOL_RDP 0
#define IO_CHANNEL 1003
#define SERVER_CHANNEL 1002
#define SHARE_ID 0x000103EA

/* security header flags */
#define SEC_INFO_PKT    0x0040
#define SEC_LICENSE_PKT 0x0080

/* share control PDU types */
#define PDUTYPE_DEMANDACTIVE  0x1
#define PDUTYPE_CONFIRMACTIVE 0x3
#define PDUTYPE_DEACTIVATEALL 0x6
#define PDUTYPE_DATA          0x7

/* data PDU types */
#define PDUTYPE2_CONTROL          0x14
#define PDUTYPE2_INPUT            0x1C
#define PDUTYPE2_SYNCHRONIZE      0x1F
#define PDUTYPE2_REFRESH_RECT     0x21
#define PDUTYPE2_SUPPRESS_OUTPUT  0x23
#define PDUTYPE2_SHUTDOWN_REQUEST 0x24
#define PDUTYPE2_SHUTDOWN_DENIED  0x25
#define PDUTYPE2_FONTLIST         0x27
#define PDUTYPE2_FONTMAP          0x28

#define CTRLACTION_REQUEST_CONTROL 1
#define CTRLACTION_GRANTED_CONTROL 2
#define CTRLACTION_COOPERATE       4

/* fast-path output update codes */
#define FP_UPDATE_BITMAP  0x1
#define FP_UPDATE_POINTER 0xA

/* pointer event flags */
#define PTRFLAGS_HWHEEL         0x0400
#define PTRFLAGS_WHEEL          0x0200
#define PTRFLAGS_WHEEL_NEGATIVE 0x0100
#define PTRFLAGS_MOVE           0x0800
#define PTRFLAGS_DOWN           0x8000
#define PTRFLAGS_BUTTON1        0x1000
#define PTRFLAGS_BUTTON2        0x2000
#define PTRFLAGS_BUTTON3        0x4000

/* ---- byte buffers ---- */

struct wbuf {
    uint8_t *b;
    uint32_t n, cap;
};

static void w8(struct wbuf *w, uint8_t v) {
    if (w->n < w->cap) w->b[w->n] = v;
    w->n++;
}
static void w16(struct wbuf *w, uint16_t v) {
    w8(w, (uint8_t)v);
    w8(w, (uint8_t)(v >> 8));
}
static void w16be(struct wbuf *w, uint16_t v) {
    w8(w, (uint8_t)(v >> 8));
    w8(w, (uint8_t)v);
}
static void w32(struct wbuf *w, uint32_t v) {
    w16(w, (uint16_t)v);
    w16(w, (uint16_t)(v >> 16));
}
static void wbytes(struct wbuf *w, const void *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) w8(w, ((const uint8_t *)p)[i]);
}
static void wzero(struct wbuf *w, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) w8(w, 0);
}
static void put16(struct wbuf *w, uint32_t at, uint16_t v) {
    if (at + 2 <= w->cap) {
        w->b[at] = (uint8_t)v;
        w->b[at + 1] = (uint8_t)(v >> 8);
    }
}
static void put16be(struct wbuf *w, uint32_t at, uint16_t v) {
    if (at + 2 <= w->cap) {
        w->b[at] = (uint8_t)(v >> 8);
        w->b[at + 1] = (uint8_t)v;
    }
}

struct rbuf {
    const uint8_t *p;
    uint32_t n, pos;
    bool err;
};

static bool rhave(struct rbuf *r, uint32_t n) {
    if (r->pos + n > r->n) r->err = true;
    return !r->err;
}
static uint8_t r8(struct rbuf *r) { return rhave(r, 1) ? r->p[r->pos++] : 0; }
static uint16_t r16(struct rbuf *r) {
    if (!rhave(r, 2)) return 0;
    uint16_t v = (uint16_t)(r->p[r->pos] | r->p[r->pos + 1] << 8);
    r->pos += 2;
    return v;
}
static uint16_t r16be(struct rbuf *r) {
    if (!rhave(r, 2)) return 0;
    uint16_t v = (uint16_t)(r->p[r->pos] << 8 | r->p[r->pos + 1]);
    r->pos += 2;
    return v;
}
static uint32_t r32(struct rbuf *r) {
    uint32_t lo = r16(r);
    return lo | (uint32_t)r16(r) << 16;
}
static void rskip(struct rbuf *r, uint32_t n) {
    if (rhave(r, n)) r->pos += n;
}
static uint32_t rleft(struct rbuf *r) { return r->err ? 0 : r->n - r->pos; }

/* BER length (MCS connect PDUs) */
static uint32_t ber_len(struct rbuf *r) {
    uint8_t b = r8(r);
    if (!(b & 0x80)) return b;
    uint32_t v = 0;
    for (int i = 0; i < (b & 0x7F) && i < 4; i++) v = v << 8 | r8(r);
    return v;
}
/* PER length (GCC and MCS domain PDUs) */
static uint32_t per_len(struct rbuf *r) {
    uint8_t b = r8(r);
    return (b & 0x80) ? ((uint32_t)(b & 0x7F) << 8 | r8(r)) : b;
}

/* ---- session ---- */

struct rdp {
    struct hvsock *s;
    uint8_t *in, *out;
    uint32_t protocols;
    char client_name[16 * 3 + 1]; /* UTF-8 */
    int width, height, bpp;
    uint16_t user_id;
    uint16_t channel_ids[16];
    char channel_names[16][9];
    uint32_t channel_opts[16];
    int nchannels;
    /* the clipboard channel */
    int clip;              /* index into channel_ids, -1 without one */
    bool clip_ready;       /* the client has sent its first format list */
    uint32_t clip_seq;     /* the local clipboard as the client last heard of it */
    uint32_t clip_wanted;  /* the format we asked the client for */
    /* the dynamic channels' channel, and on it the display control channel */
    int dvc;               /* index into channel_ids, -1 without one */
    bool disp_open;
    int want_w, want_h;    /* the size the client's window has become (0: none pending) */
    uint64_t resize_at;    /* when to take it: a window being dragged sends many */
    struct {               /* a channel message being reassembled, per channel */
        uint8_t *buf;
        uint32_t len, total;
    } vc[16];
    bool fastpath_output, attached, started, active, suppress;
    uint32_t *shadow; /* the picture the client has, for spotting changed tiles */
    uint8_t *dirty;   /* tiles to look at */
    int tiles_x, tiles_y;
    int pointer_shape;
    int mouse_buttons;
};

static int send_raw(struct rdp *c, struct wbuf *w) {
    if (w->n > w->cap) {
        kprintf("rdp: PDU of %u bytes too big\n", w->n);
        return -EINVAL;
    }
    return hvsock_write(c->s, w->b, w->n);
}

/* Start an X.224 data PDU (TPKT + data TPDU); finish with x224_end(). */
static struct wbuf x224_begin(struct rdp *c) {
    struct wbuf w = {c->out, 0, MAX_PDU};
    w8(&w, 3);
    w8(&w, 0);
    w16be(&w, 0); /* length, patched */
    w8(&w, 2);
    w8(&w, 0xF0);
    w8(&w, 0x80);
    return w;
}
static int x224_end(struct rdp *c, struct wbuf *w) {
    put16be(w, 2, (uint16_t)w->n);
    return send_raw(c, w);
}

/* MCS Send Data Indication on a channel: header up to the PER length, patched by sdi_end */
static uint32_t sdi_begin(struct wbuf *w, uint16_t channel) {
    w8(w, 26 << 2);
    w16be(w, SERVER_CHANNEL - 1001);
    w16be(w, channel);
    w8(w, 0x70);
    w16be(w, 0); /* length */
    return w->n;
}
static void sdi_end(struct wbuf *w, uint32_t start) { put16be(w, start - 2, (uint16_t)(0x8000 | (w->n - start))); }

/* A share data PDU on the I/O channel: write the payload with fill() */
static int send_data_pdu(struct rdp *c, uint8_t type2, void (*fill)(struct rdp *c, struct wbuf *w, void *arg),
                         void *arg) {
    struct wbuf w = x224_begin(c);
    uint32_t start = sdi_begin(&w, IO_CHANNEL);
    uint32_t sc = w.n;
    w16(&w, 0); /* totalLength */
    w16(&w, PDUTYPE_DATA | 0x10);
    w16(&w, SERVER_CHANNEL);
    w32(&w, SHARE_ID);
    w8(&w, 0);
    w8(&w, 1); /* stream: low priority */
    w16(&w, 0); /* uncompressedLength */
    w8(&w, type2);
    w8(&w, 0);
    w16(&w, 0);
    fill(c, &w, arg);
    put16(&w, sc, (uint16_t)(w.n - sc));
    put16(&w, sc + 12, (uint16_t)(w.n - sc - 14));
    sdi_end(&w, start);
    return x224_end(c, &w);
}

/* ---- connection sequence ---- */

/* Read one PDU: a TPKT (X.224) one or a fast-path one. Returns its length or -errno. */
static int read_pdu(struct rdp *c, uint64_t timeout_ms) {
    int r = hvsock_read_full(c->s, c->in, 2, timeout_ms);
    if (r) return r;
    uint32_t len;
    if (c->in[0] == 3) {
        if ((r = hvsock_read_full(c->s, c->in + 2, 2, 5000))) return r;
        len = (uint32_t)c->in[2] << 8 | c->in[3];
        if (len < 4) return -EINVAL;
        if ((r = hvsock_read_full(c->s, c->in + 4, len - 4, 5000))) return r;
    } else {
        len = c->in[1];
        uint32_t hdr = 2;
        if (len & 0x80) {
            if ((r = hvsock_read_full(c->s, c->in + 2, 1, 5000))) return r;
            len = (len & 0x7F) << 8 | c->in[2];
            hdr = 3;
        }
        if (len < hdr) return -EINVAL;
        if ((r = hvsock_read_full(c->s, c->in + hdr, len - hdr, 5000))) return r;
    }
    return (int)len;
}

static int connection_request(struct rdp *c, int len) {
    struct rbuf r = {c->in, (uint32_t)len, 4, false};
    r8(&r); /* length indicator */
    if ((r8(&r) & 0xF0) != 0xE0) return -EINVAL;
    rskip(&r, 5);
    /* an optional cookie or routing token line, then an optional negotiation request */
    if (rleft(&r) && r.p[r.pos] != 0x01) {
        while (rleft(&r) >= 2 && !(r.p[r.pos] == '\r' && r.p[r.pos + 1] == '\n')) r.pos++;
        rskip(&r, 2);
    }
    bool neg = false;
    if (rleft(&r) >= 8 && r.p[r.pos] == 0x01) {
        rskip(&r, 4);
        c->protocols = r32(&r);
        neg = true;
    }
    struct wbuf w = {c->out, 0, MAX_PDU};
    w8(&w, 3);
    w8(&w, 0);
    w16be(&w, 0);
    w8(&w, neg ? 14 : 6);
    w8(&w, 0xD0);
    w16be(&w, 0);
    w16be(&w, 0x1234);
    w8(&w, 0);
    if (neg) { /* we only speak standard RDP security, and the client accepts that */
        w8(&w, 0x02);
        w8(&w, 0);
        w16(&w, 8);
        w32(&w, PROTOCOL_RDP);
    }
    put16be(&w, 2, (uint16_t)w.n);
    return send_raw(c, &w);
}

static void client_core(struct rdp *c, struct rbuf *b) {
    rskip(b, 4);
    int w = r16(b), h = r16(b);
    rskip(b, 2 + 2 + 4 + 4);
    /* the computer name, UTF-16 (no surrogates in 15 characters of a NetBIOS name) */
    char *o = c->client_name;
    for (int i = 0; i < 16; i++) {
        uint16_t u = r16(b);
        if (!u || i == 15) {
            rskip(b, (15 - i) * 2);
            break;
        }
        if (u < 0x80) {
            *o++ = (char)u;
        } else if (u < 0x800) {
            *o++ = (char)(0xC0 | u >> 6);
            *o++ = (char)(0x80 | (u & 0x3F));
        } else {
            *o++ = (char)(0xE0 | u >> 12);
            *o++ = (char)(0x80 | (u >> 6 & 0x3F));
            *o++ = (char)(0x80 | (u & 0x3F));
        }
    }
    *o = 0;
    c->width = w;
    c->height = h;
    rskip(b, 4 + 4 + 4 + 64 + 2 + 2 + 4);
    uint16_t high = r16(b), supported = r16(b), early = r16(b);
    if (b->err) {
        c->bpp = 16;
        return;
    }
    if ((early & 0x0002) && (supported & 0x0008)) c->bpp = 32;
    else if (high == 24 && (supported & 0x0001)) c->bpp = 24;
    else c->bpp = 16;
}

static void client_net(struct rdp *c, struct rbuf *b) {
    uint32_t n = r32(b);
    for (uint32_t i = 0; i < n && !b->err; i++) {
        char name[9] = {0};
        for (int j = 0; j < 8; j++) name[j] = (char)r8(b);
        uint32_t opts = r32(b);
        if (c->nchannels == (int)ARRAY_SIZE(c->channel_ids)) continue;
        int k = c->nchannels++;
        c->channel_ids[k] = (uint16_t)(1004 + i);
        memcpy(c->channel_names[k], name, sizeof name);
        c->channel_opts[k] = opts;
        if (!strcmp(name, "cliprdr")) c->clip = k;
        if (!strcmp(name, "drdynvc")) c->dvc = k;
    }
}

static int connect_initial(struct rdp *c, int len) {
    struct rbuf r = {c->in, (uint32_t)len, 7, false};
    if (r8(&r) != 0x7F || r8(&r) != 0x65) return -EINVAL;
    ber_len(&r);
    for (int i = 0; i < 6; i++) { /* the domain selectors, upward flag and three parameter sets */
        r8(&r);
        rskip(&r, ber_len(&r));
    }
    if (r8(&r) != 0x04) return -EINVAL;
    uint32_t ulen = ber_len(&r);
    if (r.err || ulen > rleft(&r)) return -EINVAL;
    /* the GCC conference create request: client data blocks follow the "Duca" key */
    uint32_t end = r.pos + ulen, i = r.pos;
    while (i + 4 <= end && memcmp(r.p + i, "Duca", 4)) i++;
    if (i + 4 > end) return -EINVAL;
    r.pos = i + 4;
    uint32_t blen = per_len(&r);
    uint32_t bend = MIN(end, r.pos + blen);
    while (!r.err && r.pos + 4 <= bend) {
        uint16_t type = r16(&r), size = r16(&r);
        if (size < 4 || r.pos - 4 + size > bend) break;
        struct rbuf b = {r.p, r.pos - 4 + size, r.pos, false};
        if (type == 0xC001) client_core(c, &b);
        else if (type == 0xC003) client_net(c, &b);
        r.pos += size - 4;
    }
    c->user_id = (uint16_t)(1004 + c->nchannels);

    /* server data blocks */
    uint8_t blocks[128];
    struct wbuf sd = {blocks, 0, sizeof blocks};
    w16(&sd, 0x0C01);
    w16(&sd, 16);
    w32(&sd, 0x00080004);
    w32(&sd, c->protocols);
    w32(&sd, 0);
    w16(&sd, 0x0C02);
    w16(&sd, 12);
    w32(&sd, 0); /* encryption method: none */
    w32(&sd, 0); /* encryption level: none */
    int padded = c->nchannels + (c->nchannels & 1);
    w16(&sd, 0x0C03);
    w16(&sd, (uint16_t)(8 + padded * 2));
    w16(&sd, IO_CHANNEL);
    w16(&sd, (uint16_t)c->nchannels);
    for (int k = 0; k < padded; k++) w16(&sd, k < c->nchannels ? c->channel_ids[k] : 0);

    /* GCC conference create response around them */
    static const uint8_t gcc_head[] = {0x00, 0x05, 0x00, 0x14, 0x7C, 0x00, 0x01};
    static const uint8_t gcc_resp[] = {0x14, 0x76, 0x0A, 0x01, 0x01, 0x00, 0x01, 0xC0, 0x00, 'M', 'c', 'D', 'n'};
    uint32_t gcc_len = sizeof gcc_head + 2 + sizeof gcc_resp + 2 + sd.n;
    static const uint8_t domain_params[] = {0x30, 0x1A, 0x02, 0x01, 0x22, 0x02, 0x01, 0x03, 0x02, 0x01,
                                            0x00, 0x02, 0x01, 0x01, 0x02, 0x01, 0x00, 0x02, 0x01, 0x01,
                                            0x02, 0x03, 0x00, 0xFF, 0xF8, 0x02, 0x01, 0x02};
    uint32_t body = 3 + 3 + sizeof domain_params + 4 + gcc_len;

    struct wbuf w = x224_begin(c);
    w8(&w, 0x7F);
    w8(&w, 0x66);
    w8(&w, 0x82);
    w16be(&w, (uint16_t)body);
    static const uint8_t result[] = {0x0A, 0x01, 0x00, 0x02, 0x01, 0x00};
    wbytes(&w, result, sizeof result);
    wbytes(&w, domain_params, sizeof domain_params);
    w8(&w, 0x04);
    w8(&w, 0x82);
    w16be(&w, (uint16_t)gcc_len);
    wbytes(&w, gcc_head, sizeof gcc_head);
    w16be(&w, (uint16_t)(0x8000 | (sizeof gcc_resp + 2 + sd.n)));
    wbytes(&w, gcc_resp, sizeof gcc_resp);
    w16be(&w, (uint16_t)(0x8000 | sd.n));
    wbytes(&w, blocks, sd.n);
    return x224_end(c, &w);
}

static int send_license(struct rdp *c) {
    struct wbuf w = x224_begin(c);
    uint32_t start = sdi_begin(&w, IO_CHANNEL);
    w16(&w, SEC_LICENSE_PKT);
    w16(&w, 0);
    w8(&w, 0xFF); /* ERROR_ALERT */
    w8(&w, 0x03); /* version 3 */
    w16(&w, 16);
    w32(&w, 0x07); /* STATUS_VALID_CLIENT */
    w32(&w, 0x02); /* ST_NO_TRANSITION */
    w16(&w, 0x04); /* BB_ERROR_BLOB, empty */
    w16(&w, 0);
    sdi_end(&w, start);
    return x224_end(c, &w);
}

static void cap(struct wbuf *w, uint16_t type, uint32_t *at) {
    w16(w, type);
    *at = w->n;
    w16(w, 0);
}
static void cap_end(struct wbuf *w, uint32_t at) { put16(w, at, (uint16_t)(w->n - at + 2)); }

static int send_demand_active(struct rdp *c) {
    struct wbuf w = x224_begin(c);
    uint32_t start = sdi_begin(&w, IO_CHANNEL);
    uint32_t sc = w.n;
    w16(&w, 0);
    w16(&w, PDUTYPE_DEMANDACTIVE | 0x10);
    w16(&w, SERVER_CHANNEL);
    w32(&w, SHARE_ID);
    w16(&w, 4);
    uint32_t caplen_at = w.n;
    w16(&w, 0);
    wbytes(&w, "RDP", 4);
    uint32_t caps_start = w.n;
    w16(&w, 9); /* number of capability sets */
    w16(&w, 0);
    uint32_t at;

    cap(&w, 1, &at); /* general */
    w16(&w, 1);
    w16(&w, 3);
    w16(&w, 0x0200);
    w16(&w, 0);
    w16(&w, 0);
    w16(&w, 0x0001 | 0x0004 | 0x0400); /* fast-path output, long credentials, no bitmap compression header */
    w16(&w, 0);
    w16(&w, 0);
    w16(&w, 0);
    w8(&w, 1); /* refresh rect */
    w8(&w, 1); /* suppress output */
    cap_end(&w, at);

    cap(&w, 2, &at); /* bitmap */
    w16(&w, (uint16_t)c->bpp);
    w16(&w, 1);
    w16(&w, 1);
    w16(&w, 1);
    w16(&w, (uint16_t)c->width);
    w16(&w, (uint16_t)c->height);
    w16(&w, 0);
    w16(&w, 1); /* desktop resize */
    w16(&w, 1);
    w8(&w, 0);
    w8(&w, 0);
    w16(&w, 1);
    w16(&w, 0);
    cap_end(&w, at);

    cap(&w, 3, &at); /* order: we draw no orders */
    wzero(&w, 16 + 4);
    w16(&w, 1);
    w16(&w, 20);
    w16(&w, 0);
    w16(&w, 1);
    w16(&w, 0);
    w16(&w, 0x0002 | 0x0008); /* negotiate order support, zero-bounds deltas */
    wzero(&w, 32);
    w16(&w, 0);
    w16(&w, 0);
    w32(&w, 0);
    w32(&w, 480 * 480);
    w16(&w, 0);
    w16(&w, 0);
    w16(&w, 0);
    w16(&w, 0);
    cap_end(&w, at);

    cap(&w, 8, &at); /* pointer */
    w16(&w, 1);
    w16(&w, 8);
    w16(&w, 8);
    cap_end(&w, at);

    cap(&w, 13, &at); /* input */
    w16(&w, 0x0001 | 0x0004 | 0x0010 | 0x0020); /* scancodes, mouse X buttons, Unicode, fast-path input */
    w16(&w, 0);
    w32(&w, 0);
    w32(&w, 0);
    w32(&w, 0);
    w32(&w, 0);
    wzero(&w, 64);
    cap_end(&w, at);

    cap(&w, 20, &at); /* virtual channels */
    w32(&w, 0);
    w32(&w, 1600);
    cap_end(&w, at);

    cap(&w, 9, &at); /* share */
    w16(&w, SERVER_CHANNEL);
    w16(&w, 0);
    cap_end(&w, at);

    cap(&w, 14, &at); /* font */
    w16(&w, 1);
    w16(&w, 0);
    cap_end(&w, at);

    cap(&w, 26, &at); /* multifragment update */
    w32(&w, MAX_PDU);
    cap_end(&w, at);

    put16(&w, caplen_at, (uint16_t)(w.n - caps_start));
    w32(&w, 0); /* session id */
    put16(&w, sc, (uint16_t)(w.n - sc));
    sdi_end(&w, start);
    return x224_end(c, &w);
}

/* The first half of a deactivation-reactivation: a demand active with the new size follows. */
static int send_deactivate_all(struct rdp *c) {
    struct wbuf w = x224_begin(c);
    uint32_t start = sdi_begin(&w, IO_CHANNEL);
    w16(&w, 13);
    w16(&w, PDUTYPE_DEACTIVATEALL | 0x10);
    w16(&w, SERVER_CHANNEL);
    w32(&w, SHARE_ID);
    w16(&w, 1); /* an empty source descriptor */
    w8(&w, 0);
    sdi_end(&w, start);
    return x224_end(c, &w);
}

static void fill_sync(struct rdp *c, struct wbuf *w, void *arg) {
    (void)c;
    (void)arg;
    w16(w, 1);
    w16(w, SERVER_CHANNEL);
}
static void fill_control(struct rdp *c, struct wbuf *w, void *arg) {
    uint16_t action = (uint16_t)(uintptr_t)arg;
    w16(w, action);
    w16(w, action == CTRLACTION_GRANTED_CONTROL ? c->user_id : 0);
    w32(w, action == CTRLACTION_GRANTED_CONTROL ? SERVER_CHANNEL : 0);
}
static void fill_fontmap(struct rdp *c, struct wbuf *w, void *arg) {
    (void)c;
    (void)arg;
    w16(w, 0);
    w16(w, 0);
    w16(w, 3);
    w16(w, 4);
}
static void fill_nothing(struct rdp *c, struct wbuf *w, void *arg) {
    (void)c;
    (void)w;
    (void)arg;
}

/* ---- output ---- */

static void mark_dirty(struct rdp *c, int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->width) w = c->width - x;
    if (y + h > c->height) h = c->height - y;
    if (w <= 0 || h <= 0) return;
    for (int ty = y / TILE; ty <= (y + h - 1) / TILE; ty++)
        for (int tx = x / TILE; tx <= (x + w - 1) / TILE; tx++) c->dirty[ty * c->tiles_x + tx] = 1;
}

static void refresh_all(struct rdp *c) {
    memset(c->dirty, 1, (size_t)c->tiles_x * c->tiles_y);
    memset(c->shadow, 0xA5, (size_t)c->width * c->height * 4); /* differs from anything real */
}

/* One fast-path bitmap update with one uncompressed rectangle (rows bottom-up). */
static int send_bitmap(struct rdp *c, const uint32_t *px, int pitch, int x, int y, int w, int h) {
    int bypp = c->bpp / 8;
    int sw = (w + 3) & ~3; /* rows padded to 4 pixels */
    uint32_t size = (uint32_t)(sw * h * bypp);
    struct wbuf o = {c->out, 0, MAX_PDU};
    w8(&o, 0x00);
    w16be(&o, 0); /* length, patched */
    w8(&o, FP_UPDATE_BITMAP);
    w16(&o, (uint16_t)(4 + 18 + size));
    w16(&o, 1); /* UPDATETYPE_BITMAP */
    w16(&o, 1);
    w16(&o, (uint16_t)x);
    w16(&o, (uint16_t)y);
    w16(&o, (uint16_t)(x + w - 1));
    w16(&o, (uint16_t)(y + h - 1));
    w16(&o, (uint16_t)sw);
    w16(&o, (uint16_t)h);
    w16(&o, (uint16_t)c->bpp);
    w16(&o, 0);
    w16(&o, (uint16_t)size);
    if (o.n + size > o.cap) return -EINVAL;
    uint8_t *d = o.b + o.n;
    for (int j = h - 1; j >= 0; j--) {
        const uint32_t *row = px + (size_t)(y + j) * pitch + x;
        for (int i = 0; i < sw; i++) {
            uint32_t p = row[i < w ? i : w - 1];
            if (bypp == 4) {
                *d++ = (uint8_t)p;
                *d++ = (uint8_t)(p >> 8);
                *d++ = (uint8_t)(p >> 16);
                *d++ = 0xFF;
            } else if (bypp == 3) {
                *d++ = (uint8_t)p;
                *d++ = (uint8_t)(p >> 8);
                *d++ = (uint8_t)(p >> 16);
            } else {
                uint16_t v = (uint16_t)(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
                *d++ = (uint8_t)v;
                *d++ = (uint8_t)(v >> 8);
            }
        }
    }
    o.n += size;
    put16be(&o, 1, (uint16_t)(0x8000 | o.n));
    return send_raw(c, &o);
}

/* Clients take fast-path PDUs of at most 0x3FFF bytes (unfragmented), so a tile goes out in strips. */
#define FP_MAX_PDU 0x3FFF
static int send_tile(struct rdp *c, const uint32_t *px, int pitch, int x, int y, int w, int h) {
    int row_bytes = ((w + 3) & ~3) * (c->bpp / 8);
    int rows = (FP_MAX_PDU - 32) / row_bytes;
    for (int j = 0; j < h; j += rows) {
        int r = send_bitmap(c, px, pitch, x, y + j, w, MIN(rows, h - j));
        if (r) return r;
    }
    return 0;
}

/* the pointer as a 32x32 32bpp colour pointer with an AND mask */
static int send_pointer(struct rdp *c, int shape) {
    static uint32_t img[32 * 32];
    canvas_t cv;
    memset(img, 0, sizeof img);
    gfx_init(&cv, img, 32, 32, 32);
    desktop_draw_cursor(&cv, 8, 8, shape);
    struct wbuf o = {c->out, 0, MAX_PDU};
    w8(&o, 0x00);
    w16be(&o, 0);
    w8(&o, FP_UPDATE_POINTER);
    w16(&o, 0); /* size, patched */
    w16(&o, 32); /* xor bpp */
    w16(&o, 0);  /* cache index */
    w16(&o, 8);  /* hot spot */
    w16(&o, 8);
    w16(&o, 32);
    w16(&o, 32);
    w16(&o, 128);  /* AND mask length */
    w16(&o, 4096); /* XOR mask length */
    for (int y = 31; y >= 0; y--)
        for (int x = 0; x < 32; x++) w32(&o, img[y * 32 + x]); /* alpha is 0 where nothing is drawn */
    for (int y = 31; y >= 0; y--)
        for (int b = 0; b < 4; b++) {
            uint8_t m = 0;
            for (int i = 0; i < 8; i++)
                if (!(img[y * 32 + b * 8 + i] >> 24)) m |= (uint8_t)(0x80 >> i);
            w8(&o, m);
        }
    put16(&o, 4, (uint16_t)(o.n - 6));
    put16be(&o, 1, (uint16_t)(0x8000 | o.n));
    return send_raw(c, &o);
}

static int push_updates(struct rdp *c) {
    struct wm_rect d[16];
    int n = wm_remote_damage(d, 16);
    for (int i = 0; i < n; i++) mark_dirty(c, d[i].x, d[i].y, d[i].w, d[i].h);
    if (!c->active || c->suppress) return 0;
    int shape = wm_cursor_shape();
    if (shape != c->pointer_shape) {
        c->pointer_shape = shape;
        int r = send_pointer(c, shape);
        if (r) return r;
    }
    int pitch;
    const uint32_t *px = wm_remote_frame(&pitch);
    for (int ty = 0; ty < c->tiles_y; ty++)
        for (int tx = 0; tx < c->tiles_x; tx++) {
            if (!c->dirty[ty * c->tiles_x + tx]) continue;
            c->dirty[ty * c->tiles_x + tx] = 0;
            int x = tx * TILE, y = ty * TILE;
            int w = MIN(TILE, c->width - x), h = MIN(TILE, c->height - y);
            bool same = true;
            for (int j = 0; j < h && same; j++)
                same = !memcmp(px + (size_t)(y + j) * pitch + x, c->shadow + (size_t)(y + j) * c->width + x,
                               (size_t)w * 4);
            if (same) continue;
            for (int j = 0; j < h; j++)
                memcpy(c->shadow + (size_t)(y + j) * c->width + x, px + (size_t)(y + j) * pitch + x, (size_t)w * 4);
            int r = send_tile(c, px, pitch, x, y, w, h);
            if (r) return r;
        }
    return 0;
}

/* ---- input ---- */

static void key(bool extended, bool extended1, bool release, uint8_t code) {
    if (extended) input_scancode(0xE0);
    if (extended1) input_scancode(0xE1);
    input_scancode((uint8_t)((code & 0x7F) | (release ? 0x80 : 0)));
}

static void mouse(struct rdp *c, uint16_t flags, int x, int y, bool extended) {
    struct mouse_event e = {0};
    e.absolute = true;
    int sw = MAX(c->width - 1, 1), sh = MAX(c->height - 1, 1);
    x = MAX(0, MIN(x, sw));
    y = MAX(0, MIN(y, sh));
    e.x = (x * MOUSE_ABS_MAX + sw - 1) / sw; /* round up so the compositor maps it back to x */
    e.y = (y * MOUSE_ABS_MAX + sh - 1) / sh;
    if (!extended) {
        if (flags & PTRFLAGS_WHEEL) {
            int rot = flags & 0xFF;
            if (flags & PTRFLAGS_WHEEL_NEGATIVE) rot -= 256;
            e.wheel = rot > 0 ? -MAX(1, rot / 120) : rot < 0 ? MAX(1, -rot / 120) : 0;
        } else if (!(flags & PTRFLAGS_HWHEEL)) {
            int bit = (flags & PTRFLAGS_BUTTON1) ? 1 : (flags & PTRFLAGS_BUTTON2) ? 2 : (flags & PTRFLAGS_BUTTON3) ? 4 : 0;
            if (bit) {
                if (flags & PTRFLAGS_DOWN) c->mouse_buttons |= bit;
                else c->mouse_buttons &= ~bit;
            }
        }
    }
    e.buttons = (uint8_t)c->mouse_buttons;
    input_mouse(&e);
}

static void fastpath_input(struct rdp *c, int len) {
    struct rbuf r = {c->in, (uint32_t)len, 1, false};
    int n = (c->in[0] >> 2) & 0xF;
    if (c->in[1] & 0x80) r8(&r);
    r8(&r);
    if (n == 0) n = r8(&r);
    for (int i = 0; i < n && !r.err; i++) {
        uint8_t h = r8(&r);
        uint8_t flags = h & 0x1F;
        switch (h >> 5) {
        case 0: /* scancode */
            key(flags & 0x02, flags & 0x04, flags & 0x01, r8(&r));
            break;
        case 1: /* mouse */
        case 2: { /* extended mouse buttons */
            uint16_t pf = r16(&r);
            int x = r16(&r), y = r16(&r);
            if (!r.err) mouse(c, pf, x, y, (h >> 5) == 2);
            break;
        }
        case 3: /* synchronize lock keys */
            break;
        case 4: { /* Unicode */
            uint16_t ch = r16(&r);
            if (!r.err) input_unicode(ch, !(flags & 0x01));
            break;
        }
        case 6: /* QoE timestamp */
            r32(&r);
            break;
        default:
            return;
        }
    }
}

static void slowpath_input(struct rdp *c, struct rbuf *r) {
    int n = r16(r);
    r16(r);
    for (int i = 0; i < n && !r->err; i++) {
        r32(r);
        uint16_t type = r16(r);
        uint16_t a = r16(r), b = r16(r), d = r16(r);
        if (r->err) return;
        switch (type) {
        case 0x0004: key(a & 0x0100, a & 0x0200, a & 0x8000, (uint8_t)b); break;
        case 0x0005: input_unicode(b, !(a & 0x8000)); break;
        case 0x8001: mouse(c, a, b, d, false); break;
        case 0x8002: mouse(c, a, b, d, true); break;
        default: break;
        }
    }
}

/* ---- static virtual channels ---- */

#define CHANNEL_FLAG_FIRST         0x01
#define CHANNEL_FLAG_LAST          0x02
#define CHANNEL_FLAG_SHOW_PROTOCOL 0x10
#define CHANNEL_OPTION_SHOW_PROTOCOL 0x00200000
#define CHANNEL_CHUNK 1600 /* what every client takes */

/* Send one channel message, split into chunks. */
static int vc_send(struct rdp *c, int k, const uint8_t *data, uint32_t len) {
    uint32_t show = (c->channel_opts[k] & CHANNEL_OPTION_SHOW_PROTOCOL) ? CHANNEL_FLAG_SHOW_PROTOCOL : 0;
    uint32_t off = 0;
    do {
        uint32_t n = MIN(CHANNEL_CHUNK, len - off);
        struct wbuf w = x224_begin(c);
        uint32_t start = sdi_begin(&w, c->channel_ids[k]);
        w32(&w, len);
        w32(&w, (off == 0 ? CHANNEL_FLAG_FIRST : 0) | (off + n == len ? CHANNEL_FLAG_LAST : 0) | show);
        wbytes(&w, data + off, n);
        sdi_end(&w, start);
        int r = x224_end(c, &w);
        if (r) return r;
        off += n;
    } while (off < len);
    return 0;
}

/* ---- the clipboard (MS-RDPECLIP), text only ----
   Whoever copies announces the formats it has (a format list); the other side asks for the one
   it wants when it needs it. Windows' copy is fetched as soon as it is announced, so a paste in
   Nocturne finds it in the clipboard; ours is announced whenever it changes. */

enum { CB_MONITOR_READY = 1, CB_FORMAT_LIST, CB_FORMAT_LIST_RESPONSE, CB_FORMAT_DATA_REQUEST,
       CB_FORMAT_DATA_RESPONSE, CB_TEMP_DIRECTORY, CB_CLIP_CAPS };
#define CB_RESPONSE_OK   1
#define CB_RESPONSE_FAIL 2
#define CF_TEXT        1
#define CF_UNICODETEXT 13

static int clip_send(struct rdp *c, uint16_t type, uint16_t flags, const void *data, uint32_t len) {
    uint8_t *m = kmalloc(8 + len);
    if (!m) return 0; /* a lost clipboard message is not worth the session */
    struct wbuf w = {m, 0, 8 + len};
    w16(&w, type);
    w16(&w, flags);
    w32(&w, len);
    if (len) wbytes(&w, data, len);
    int r = vc_send(c, c->clip, m, w.n);
    kfree(m);
    return r;
}

static int clip_start(struct rdp *c) {
    if (c->clip < 0) return 0;
    wm_clipboard(&(size_t){0}, &c->clip_seq); /* what we have now is not news */
    static const uint8_t caps[] = {1, 0, 0, 0, /* one set, the general one: */
                                   1, 0, 12, 0, 2, 0, 0, 0, 0, 0, 0, 0}; /* version 2, short format names */
    int r = clip_send(c, CB_CLIP_CAPS, 0, caps, sizeof caps);
    return r ? r : clip_send(c, CB_MONITOR_READY, 0, NULL, 0);
}

/* UTF-8 (LF line ends) to UTF-16LE bytes (CRLF), with a terminating 0; out NULL just counts.
   Returns the size in bytes. */
static uint32_t to_utf16(const char *s, size_t n, uint8_t *out) {
    uint32_t o = 0;
#define PUT(u)                                                                                          \
    do {                                                                                                \
        if (out) {                                                                                      \
            out[o] = (uint8_t)(u);                                                                      \
            out[o + 1] = (uint8_t)((u) >> 8);                                                           \
        }                                                                                               \
        o += 2;                                                                                         \
    } while (0)
    for (size_t i = 0; i < n;) {
        uint8_t b = (uint8_t)s[i];
        uint32_t cp = 0xFFFD;
        int len = 1;
        if (b < 0x80) cp = b;
        else if ((b & 0xE0) == 0xC0 && i + 1 < n) cp = (b & 0x1Fu) << 6 | (s[i + 1] & 0x3F), len = 2;
        else if ((b & 0xF0) == 0xE0 && i + 2 < n)
            cp = (b & 0x0Fu) << 12 | (s[i + 1] & 0x3Fu) << 6 | (s[i + 2] & 0x3F), len = 3;
        else if ((b & 0xF8) == 0xF0 && i + 3 < n)
            cp = (b & 0x07u) << 18 | (s[i + 1] & 0x3Fu) << 12 | (s[i + 2] & 0x3Fu) << 6 | (s[i + 3] & 0x3F), len = 4;
        if (cp == '\n' && (i == 0 || s[i - 1] != '\r')) PUT('\r');
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            PUT(0xD800 | cp >> 10);
            PUT(0xDC00 | (cp & 0x3FF));
        } else {
            PUT(cp);
        }
        i += (size_t)len;
    }
    PUT(0);
#undef PUT
    return o;
}

/* UTF-16LE bytes (up to a 0) to UTF-8 with LF line ends; out NULL just counts. */
static size_t from_utf16(const uint8_t *p, uint32_t n, char *out) {
    size_t o = 0;
    for (uint32_t i = 0; i + 1 < n; i += 2) {
        uint32_t u = p[i] | p[i + 1] << 8;
        if (!u) break;
        if (u == '\r' && i + 3 < n && p[i + 2] == '\n' && !p[i + 3]) continue;
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {
            uint32_t lo = p[i + 2] | p[i + 3] << 8;
            if (lo >= 0xDC00 && lo < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        char b[4];
        int k;
        if (u < 0x80) b[0] = (char)u, k = 1;
        else if (u < 0x800) b[0] = (char)(0xC0 | u >> 6), b[1] = (char)(0x80 | (u & 0x3F)), k = 2;
        else if (u < 0x10000)
            b[0] = (char)(0xE0 | u >> 12), b[1] = (char)(0x80 | (u >> 6 & 0x3F)), b[2] = (char)(0x80 | (u & 0x3F)), k = 3;
        else
            b[0] = (char)(0xF0 | u >> 18), b[1] = (char)(0x80 | (u >> 12 & 0x3F)),
            b[2] = (char)(0x80 | (u >> 6 & 0x3F)), b[3] = (char)(0x80 | (u & 0x3F)), k = 4;
        if (out) memcpy(out + o, b, (size_t)k);
        o += (size_t)k;
    }
    return o;
}

/* the client wants our text */
static int clip_data_request(struct rdp *c, uint32_t format) {
    size_t n;
    const char *text = wm_clipboard(&n, NULL);
    uint8_t *d = NULL;
    uint32_t len = 0;
    if (format == CF_UNICODETEXT) {
        len = to_utf16(text, n, NULL);
        if ((d = kmalloc(len))) to_utf16(text, n, d);
    } else if (format == CF_TEXT) { /* ASCII only, CRLF */
        if ((d = kmalloc(n * 2 + 1)))
            for (size_t i = 0; i < n; i++) {
                uint8_t b = (uint8_t)text[i];
                if (b == '\n' && (i == 0 || text[i - 1] != '\r')) d[len++] = '\r';
                if (b < 0x80) d[len++] = b;
                else if ((b & 0xC0) != 0x80) d[len++] = '?';
            }
        if (d) d[len++] = 0;
    }
    int r = d ? clip_send(c, CB_FORMAT_DATA_RESPONSE, CB_RESPONSE_OK, d, len)
              : clip_send(c, CB_FORMAT_DATA_RESPONSE, CB_RESPONSE_FAIL, NULL, 0);
    kfree(d);
    return r;
}

/* the client's text arrived (the answer to the request clip_message sent) */
static void clip_data(struct rdp *c, const uint8_t *p, uint32_t n, uint32_t format) {
    char *t = NULL;
    size_t len = 0;
    if (format == CF_UNICODETEXT) {
        len = from_utf16(p, n, NULL);
        if (len > WM_CLIPBOARD_MAX || !(t = kmalloc(len + 1))) return;
        from_utf16(p, n, t);
    } else {
        if (!(t = kmalloc(n + 1))) return;
        for (uint32_t i = 0; i < n && p[i]; i++)
            if (!(p[i] == '\r' && i + 1 < n && p[i + 1] == '\n')) t[len++] = p[i] < 0x80 ? (char)p[i] : '?';
    }
    wm_clipboard_set(t, len);
    kfree(t);
    wm_clipboard(&len, &c->clip_seq); /* do not announce it back */
}

static int clip_message(struct rdp *c, const uint8_t *m, uint32_t n) {
    if (n < 8) return 0;
    uint16_t type = (uint16_t)(m[0] | m[1] << 8), flags = (uint16_t)(m[2] | m[3] << 8);
    uint32_t dlen = m[4] | m[5] << 8 | (uint32_t)m[6] << 16 | (uint32_t)m[7] << 24;
    const uint8_t *d = m + 8;
    dlen = MIN(dlen, n - 8);
    switch (type) {
    case CB_FORMAT_LIST: {
        /* 36-byte entries, a format id and a short name: our capabilities leave out long names */
        bool unicode = false, ansi = false;
        for (uint32_t i = 0; i + 4 <= dlen; i += 36) {
            uint32_t id = d[i] | d[i + 1] << 8 | (uint32_t)d[i + 2] << 16 | (uint32_t)d[i + 3] << 24;
            unicode |= id == CF_UNICODETEXT;
            ansi |= id == CF_TEXT;
        }
        c->clip_ready = true;
        int r = clip_send(c, CB_FORMAT_LIST_RESPONSE, CB_RESPONSE_OK, NULL, 0);
        if (r || (!unicode && !ansi)) return r;
        c->clip_wanted = unicode ? CF_UNICODETEXT : CF_TEXT;
        uint8_t req[4] = {(uint8_t)c->clip_wanted, 0, 0, 0};
        return clip_send(c, CB_FORMAT_DATA_REQUEST, 0, req, 4);
    }
    case CB_FORMAT_DATA_REQUEST:
        if (dlen < 4) return 0;
        return clip_data_request(c, d[0] | d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24);
    case CB_FORMAT_DATA_RESPONSE:
        if (flags & CB_RESPONSE_OK) clip_data(c, d, dlen, c->clip_wanted);
        return 0;
    default: /* capabilities, temporary directory, list responses, locks */
        return 0;
    }
}

/* our clipboard changed: tell the client we have text */
static int clip_poll(struct rdp *c) {
    if (c->clip < 0 || !c->clip_ready) return 0;
    size_t n;
    uint32_t seq;
    wm_clipboard(&n, &seq);
    if (seq == c->clip_seq) return 0;
    c->clip_seq = seq;
    uint8_t list[36] = {CF_UNICODETEXT}; /* one short-name entry, no name */
    return clip_send(c, CB_FORMAT_LIST, 0, list, sizeof list);
}

/* ---- dynamic virtual channels (MS-RDPEDYC), for display control (MS-RDPEDISP) ----
   drdynvc carries channels that the server opens by name. The one we open lets the client tell
   us the new size of its window; we take it with a deactivation-reactivation, which hands the
   client a new desktop size the way the connection sequence did. */

enum { DVC_CREATE = 1, DVC_DATA_FIRST, DVC_DATA, DVC_CLOSE, DVC_CAPS };
#define DISP_CHANNEL_ID 1 /* our id for the display control channel */
#define DISP_MONITOR_LAYOUT 2
#define DISP_CAPS 5

static int dvc_start(struct rdp *c) {
    if (c->dvc < 0) return 0;
    static const uint8_t caps[] = {DVC_CAPS << 4, 0, 1, 0}; /* version 1 */
    return vc_send(c, c->dvc, caps, sizeof caps);
}

static int dvc_open_display(struct rdp *c) {
    static const char name[] = "Microsoft::Windows::RDS::DisplayControl";
    uint8_t m[2 + sizeof name] = {DVC_CREATE << 4, DISP_CHANNEL_ID}; /* one-byte channel id */
    memcpy(m + 2, name, sizeof name);
    return vc_send(c, c->dvc, m, sizeof m);
}

static int disp_caps(struct rdp *c) {
    uint8_t m[2 + 20] = {DVC_DATA << 4, DISP_CHANNEL_ID};
    struct wbuf w = {m + 2, 0, 20};
    w32(&w, DISP_CAPS);
    w32(&w, 20);
    w32(&w, 1);    /* monitors */
    w32(&w, 3840); /* the largest area, as wm_remote_resize allows it */
    w32(&w, 2160);
    return vc_send(c, c->dvc, m, sizeof m);
}

/* the client's monitor layout: the primary monitor's size is the window's */
static void disp_message(struct rdp *c, struct rbuf *r) {
    uint32_t type = r32(r);
    r32(r);
    uint32_t size = r32(r), count = r32(r);
    if (r->err || type != DISP_MONITOR_LAYOUT || size < 20) return;
    for (uint32_t i = 0; i < count && i < 16; i++) {
        uint32_t at = r->pos;
        uint32_t flags = r32(r);
        rskip(r, 8);
        int w = (int)r32(r), h = (int)r32(r);
        r->pos = at;
        rskip(r, size);
        if (r->err) return;
        if (i == 0 || (flags & 1)) {
            c->want_w = w;
            c->want_h = h;
        }
    }
    c->resize_at = uptime_ms() + 250;
}

static uint32_t dvc_field(struct rbuf *r, int size_code) {
    return size_code == 0 ? r8(r) : size_code == 1 ? r16(r) : r32(r);
}

static int dvc_message(struct rdp *c, const uint8_t *m, uint32_t n) {
    struct rbuf r = {m, n, 0, false};
    uint8_t h = r8(&r);
    int cmd = h >> 4, sp = (h >> 2) & 3, cb = h & 3;
    if (cmd == DVC_CAPS) return dvc_open_display(c); /* the client's answer to ours */
    uint32_t id = dvc_field(&r, cb);
    if (r.err || id != DISP_CHANNEL_ID) return 0;
    switch (cmd) {
    case DVC_CREATE: /* the answer to our create request */
        c->disp_open = (int32_t)r32(&r) >= 0 && !r.err;
        return c->disp_open ? disp_caps(c) : 0;
    case DVC_DATA_FIRST: { /* only whole messages: a monitor layout is far smaller than a chunk */
        uint32_t total = dvc_field(&r, sp);
        if (total == rleft(&r)) disp_message(c, &r);
        return 0;
    }
    case DVC_DATA: disp_message(c, &r); return 0;
    case DVC_CLOSE: c->disp_open = false; return 0;
    default: return 0;
    }
}

/* Take the size the client asked for, once its window has stopped changing for a moment. */
static int apply_resize(struct rdp *c) {
    if (!c->want_w || !c->active || uptime_ms() < c->resize_at) return 0;
    int w = c->want_w & ~1, h = c->want_h, ow = c->width, oh = c->height;
    c->want_w = c->want_h = 0;
    wm_remote_resize(&w, &h);
    if (w == ow && h == oh) return 0;
    int tx = (w + TILE - 1) / TILE, ty = (h + TILE - 1) / TILE;
    uint32_t *shadow = kmalloc((size_t)w * h * 4);
    uint8_t *dirty = kzalloc((size_t)tx * ty);
    if (!shadow || !dirty) {
        kfree(shadow);
        kfree(dirty);
        wm_remote_resize(&ow, &oh); /* stay as we were */
        return 0;
    }
    kfree(c->shadow);
    kfree(c->dirty);
    c->shadow = shadow;
    c->dirty = dirty;
    c->width = w;
    c->height = h;
    c->tiles_x = tx;
    c->tiles_y = ty;
    c->active = false; /* until the client has confirmed the new size; the font list says so */
    int e = send_deactivate_all(c);
    return e ? e : send_demand_active(c);
}

/* Data on a static virtual channel: reassemble each message, then hand it over. */
static int vc_data(struct rdp *c, uint16_t channel, struct rbuf *r) {
    int k = 0;
    while (k < c->nchannels && c->channel_ids[k] != channel) k++;
    if (k == c->nchannels || (k != c->clip && k != c->dvc)) return 0; /* the others carry nothing we do */
    uint32_t total = r32(r), flags = r32(r);
    if (r->err) return 0;
    if (flags & CHANNEL_FLAG_FIRST) {
        kfree(c->vc[k].buf);
        c->vc[k].buf = NULL;
        if (total > 4 * WM_CLIPBOARD_MAX + 64) return 0;
        c->vc[k].buf = kmalloc(MAX(total, 1u));
        c->vc[k].total = total;
        c->vc[k].len = 0;
    }
    if (!c->vc[k].buf) return 0;
    uint32_t n = MIN(rleft(r), c->vc[k].total - c->vc[k].len);
    memcpy(c->vc[k].buf + c->vc[k].len, r->p + r->pos, n);
    c->vc[k].len += n;
    if (!(flags & CHANNEL_FLAG_LAST)) return 0;
    int e = k == c->clip ? clip_message(c, c->vc[k].buf, c->vc[k].len) : dvc_message(c, c->vc[k].buf, c->vc[k].len);
    kfree(c->vc[k].buf);
    c->vc[k].buf = NULL;
    return e;
}

/* ---- share control PDUs from the client ---- */

static int share_pdu(struct rdp *c, struct rbuf *r) {
    uint32_t base = r->pos;
    uint16_t total = r16(r), type = r16(r) & 0xF;
    r16(r);
    if (r->err || total < 6 || base + total > r->n) return 0;
    r->n = base + total;
    if (type == PDUTYPE_CONFIRMACTIVE) return 0; /* nothing in it changes what we send */
    if (type != PDUTYPE_DATA) return 0;
    rskip(r, 4 + 1 + 1 + 2);
    uint8_t type2 = r8(r);
    rskip(r, 1 + 2);
    if (r->err) return 0;
    switch (type2) {
    case PDUTYPE2_SYNCHRONIZE: return send_data_pdu(c, PDUTYPE2_SYNCHRONIZE, fill_sync, NULL);
    case PDUTYPE2_CONTROL: {
        uint16_t action = r16(r);
        if (action == CTRLACTION_COOPERATE)
            return send_data_pdu(c, PDUTYPE2_CONTROL, fill_control, (void *)(uintptr_t)CTRLACTION_COOPERATE);
        if (action == CTRLACTION_REQUEST_CONTROL)
            return send_data_pdu(c, PDUTYPE2_CONTROL, fill_control, (void *)(uintptr_t)CTRLACTION_GRANTED_CONTROL);
        return 0;
    }
    case PDUTYPE2_FONTLIST: {
        /* the end of the connection sequence, or of a reactivation after a resize */
        int e = send_data_pdu(c, PDUTYPE2_FONTMAP, fill_fontmap, NULL);
        if (!c->started) {
            kprintf("rdp: session with \"%s\" is up, %dx%d at %d bpp\n", c->client_name, c->width, c->height,
                    c->bpp);
            c->started = true;
            if (!e) e = clip_start(c);
            if (!e) e = dvc_start(c);
        }
        c->active = true;
        c->pointer_shape = -1;
        refresh_all(c);
        return e;
    }
    case PDUTYPE2_INPUT: slowpath_input(c, r); return 0;
    case PDUTYPE2_REFRESH_RECT: {
        int n = r8(r);
        rskip(r, 3);
        for (int i = 0; i < n && !r->err; i++) {
            int l = r16(r), t = r16(r), rt = r16(r), b = r16(r);
            mark_dirty(c, l, t, rt - l + 1, b - t + 1);
            /* the client lost these pixels: resend even if unchanged */
            for (int y = MAX(t, 0); y <= b && y < c->height; y++)
                memset(c->shadow + (size_t)y * c->width, 0xA5, (size_t)c->width * 4);
        }
        return 0;
    }
    case PDUTYPE2_SUPPRESS_OUTPUT: {
        bool allow = r8(r) != 0;
        c->suppress = !allow;
        if (allow) refresh_all(c);
        return 0;
    }
    case PDUTYPE2_SHUTDOWN_REQUEST: return send_data_pdu(c, PDUTYPE2_SHUTDOWN_DENIED, fill_nothing, NULL);
    default: return 0;
    }
}

/* Data on the I/O channel: the client info PDU, then share control PDUs. */
static int io_data(struct rdp *c, struct rbuf *r) {
    uint32_t len = rleft(r);
    if (len >= 4) {
        uint16_t flags = (uint16_t)(r->p[r->pos] | r->p[r->pos + 1] << 8);
        uint16_t total = len >= 6 ? (uint16_t)(r->p[r->pos] | r->p[r->pos + 1] << 8) : 0;
        if (flags & SEC_INFO_PKT && total != len) {
            int e = send_license(c);
            return e ? e : send_demand_active(c);
        }
        /* a security header may precede share control PDUs; their length tells */
        if (total != len && len >= 10) {
            uint16_t inner = (uint16_t)(r->p[r->pos + 4] | r->p[r->pos + 5] << 8);
            if (inner == len - 4) rskip(r, 4);
        }
    }
    while (rleft(r) >= 6) {
        struct rbuf one = *r;
        uint16_t total = (uint16_t)(r->p[r->pos] | r->p[r->pos + 1] << 8);
        if (total < 6 || total > rleft(r)) break;
        int e = share_pdu(c, &one);
        if (e) return e;
        r->pos += total;
    }
    return 0;
}

static int mcs_pdu(struct rdp *c, int len) {
    struct rbuf r = {c->in, (uint32_t)len, 7, false};
    uint8_t t = r8(&r) >> 2;
    switch (t) {
    case 1: return 0; /* erect domain */
    case 8: return -EPIPE; /* disconnect provider ultimatum */
    case 10: { /* attach user: confirm with our user id */
        struct wbuf w = x224_begin(c);
        w8(&w, (11 << 2) | 2);
        w8(&w, 0);
        w16be(&w, (uint16_t)(c->user_id - 1001));
        return x224_end(c, &w);
    }
    case 14: { /* channel join: everything the client asked for is fine */
        uint16_t initiator = r16be(&r), channel = r16be(&r);
        struct wbuf w = x224_begin(c);
        w8(&w, (15 << 2) | 2);
        w8(&w, 0);
        w16be(&w, initiator);
        w16be(&w, channel);
        w16be(&w, channel);
        return x224_end(c, &w);
    }
    case 25: { /* send data request */
        r16be(&r);
        uint16_t channel = r16be(&r);
        r8(&r);
        uint32_t dlen = per_len(&r);
        if (r.err || dlen > rleft(&r)) return 0;
        r.n = r.pos + dlen;
        if (channel == IO_CHANNEL) return io_data(c, &r);
        return vc_data(c, channel, &r);
    }
    default: return 0;
    }
}

static void session(void *arg) {
    struct rdp *c = kzalloc(sizeof *c);
    struct hvsock *s = arg;
    if (!c) goto out;
    c->s = s;
    c->clip = c->dvc = -1;
    c->in = kmalloc(MAX_PDU);
    c->out = kmalloc(MAX_PDU);
    if (!c->in || !c->out) goto out;

    /* VMConnect's availability probe connects and hangs up without a word */
    int len = read_pdu(c, 10000);
    if (len <= 0) goto out;
    if (c->in[0] != 3 || connection_request(c, len)) goto out;
    c->bpp = 16;
    /* a client that wants TLS or CredSSP hangs up after our answer and tries again without */
    if ((len = read_pdu(c, 10000)) <= 0 || c->in[0] != 3 || connect_initial(c, len)) goto out;

    /* the desktop takes the size of the client's window */
    if (!wm_remote_attach(&c->width, &c->height)) {
        kprintf("rdp: no desktop to show\n");
        goto out;
    }
    c->attached = true;
    c->tiles_x = (c->width + TILE - 1) / TILE;
    c->tiles_y = (c->height + TILE - 1) / TILE;
    c->shadow = kmalloc((size_t)c->width * c->height * 4);
    c->dirty = kzalloc((size_t)c->tiles_x * c->tiles_y);
    if (!c->shadow || !c->dirty) goto out;

    int err = 0;
    while (!err) {
        if (hvsock_wait(s, 30)) {
            len = read_pdu(c, 5000);
            if (len <= 0) {
                err = len ? len : -EPIPE;
                break;
            }
            if (c->in[0] == 3) err = mcs_pdu(c, len);
            else if ((c->in[0] & 3) == 0) fastpath_input(c, len);
        }
        if (!err) err = apply_resize(c);
        if (!err) err = push_updates(c);
        if (!err) err = clip_poll(c);
    }
    if (c->started && err != -EPIPE) kprintf("rdp: session with \"%s\" failed (%d)\n", c->client_name, err);
    else if (c->started) kprintf("rdp: session with \"%s\" ended\n", c->client_name);
out:
    if (c && c->attached) wm_remote_detach();
    if (c) {
        kfree(c->in);
        kfree(c->out);
        kfree(c->shadow);
        kfree(c->dirty);
        for (int k = 0; k < (int)ARRAY_SIZE(c->vc); k++) kfree(c->vc[k].buf);
        kfree(c);
    }
    hvsock_close(s);
    task_exit(0);
}

static void on_accept(struct hvsock *s, void *arg) {
    (void)arg;
    kthread_create("rdp", session, s);
}

void rdp_init(void) {
    if (hvsock_listen(RDP_PORT, on_accept, NULL) == 0)
        kprintf("rdp: listening on Hyper-V socket port %d (enhanced session)\n", RDP_PORT);
}
