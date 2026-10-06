#include "webnet.h"
#include "webnet_wire.h"
#include "http.h"
#include "nocturne.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

#define REQUEST_MAX 64
#define QUEUE_BYTES (32u * 1024u * 1024u)
#define PUMP_BYTES (256u * 1024u)
struct request {
    struct request *next;
    uint64_t id, generation, deadline;
    int priority;
    webnet_callback callback;
    void *opaque;
    unsigned char *tx, *rx;
    size_t tx_len, tx_pos, rx_len, rx_pos;
    struct webnet_wire_response head;
    size_t head_pos;
    bool cancelled, finishing;
    char *cookie_site;
    enum webnet_credentials credentials;
    bool top_level;
    char error[160];
};
struct slot { struct request *rq; int pid, in, out; };
struct webnet {
    struct request *queue;
    struct slot slots[2];
    uint64_t serial;
    unsigned count;
    size_t queued_bytes;
    webcookie_jar *cookies;
};

static void request_free(struct request *r) { free(r->tx); free(r->rx); free(r->cookie_site); free(r); }
static void load_cookie_psl(webcookie_jar *jar) {
    FILE *f = fopen("/usr/share/browser/public_suffix_list.dat", "r");
    if (!f) return; /* Jar falls back to host-only, not a guessed suffix table. */
    char *text = malloc(WEBCOOKIE_PSL_MAX + 1);
    if (text) { size_t n = fread(text, 1, WEBCOOKIE_PSL_MAX + 1, f); if (n <= WEBCOOKIE_PSL_MAX) webcookie_psl_load(jar, text, n); free(text); }
    fclose(f);
}
static void slot_close(struct slot *s) {
    if (s->in >= 0) close(s->in);
    if (s->out >= 0) close(s->out);
    s->in = s->out = -1;
}
static void slot_stop(struct slot *s, const char *error) {
    if (s->rq->finishing) return;
    if (error) snprintf(s->rq->error, sizeof s->rq->error, "%s", error);
    s->rq->finishing = true;
    slot_close(s);
    if (s->pid > 0) kill(s->pid);
}
webnet *webnet_create(void) {
    webnet *n = calloc(1, sizeof *n);
    if (n) {
        n->cookies = webcookie_create();
        if (!n->cookies) { free(n); return NULL; }
        load_cookie_psl(n->cookies);
        for (int i = 0; i < 2; i++) n->slots[i].in = n->slots[i].out = -1;
    }
    return n;
}
void webnet_free(webnet *n) {
    if (!n) return;
    for (int i = 0; i < 2; i++) {
        struct slot *s = &n->slots[i];
        if (!s->rq) continue;
        slot_close(s);
        if (s->pid > 0) { kill(s->pid); waitpid(s->pid, NULL, 0); }
        request_free(s->rq);
    }
    while (n->queue) { struct request *r = n->queue; n->queue = r->next; request_free(r); }
    webcookie_free(n->cookies); free(n);
}
long webnet_cookie_get(webnet *n, const char *url, char *out, size_t cap) {
    struct webcookie_context c = {url, url, "GET", false, false, false};
    return n ? webcookie_get(n->cookies, &c, out, cap, time(NULL)) : -1;
}
int webnet_cookie_set(webnet *n, const char *url, const char *value) {
    struct webcookie_context c = {url, url, "GET", false, false, false};
    return n && value ? webcookie_set(n->cookies, &c, value, strlen(value), time(NULL)) : -1;
}
uint64_t webnet_submit(webnet *n, const struct webnet_request *q, webnet_callback cb, void *opaque) {
    if (!n || !q || !q->url || !cb || n->count >= REQUEST_MAX || q->kind > WEBNET_FETCH ||
        q->kind < WEBNET_NAVIGATION || q->body_len > WEBNET_BODY_LIMIT || (q->body_len && !q->body) ||
        q->credentials < WEBNET_CREDENTIALS_OMIT || q->credentials > WEBNET_CREDENTIALS_INCLUDE) return 0;
    const char *origin = q->origin ? q->origin : "";
    const char *method = q->method ? q->method : "GET";
    const char *headers = q->headers ? q->headers : "";
    size_t ul = strlen(q->url), ol = strlen(origin), ml = strlen(method), hl = strlen(headers);
    if (!ul || ul >= WEBNET_URL_MAX || ol >= WEBNET_URL_MAX || ml >= 8 || hl >= WEBNET_HEADERS_MAX ||
        (strcmp(method, "GET") && strcmp(method, "POST")) || (!strcmp(method, "GET") && q->body_len)) return 0;
    int64_t cookie_now=time(NULL);
    long cookie_len = q->credentials == WEBNET_CREDENTIALS_OMIT ? 0 : webcookie_export(n->cookies, NULL, 0, cookie_now);
    if (cookie_len < 0) return 0;
    size_t len = sizeof(struct webnet_wire_request) + ul + ol + ml + hl + q->body_len + (size_t)cookie_len;
    if (len > QUEUE_BYTES - n->queued_bytes) return 0;
    struct request *r = calloc(1, sizeof *r);
    if (!r) return 0;
    r->tx = malloc(len);
    if (!r->tx) { free(r); return 0; }
    r->cookie_site = strdup(origin);
    if (!r->cookie_site) { request_free(r); return 0; }
    r->credentials = q->credentials; r->top_level = q->kind == WEBNET_NAVIGATION;
    r->id = ++n->serial;
    if (!r->id) r->id = ++n->serial;
    r->generation = q->generation;
    r->deadline = uptime_ms() + WEBNET_TIMEOUT_MS;
    r->callback = cb; r->opaque = opaque; r->tx_len = len;
    r->priority = q->kind == WEBNET_NAVIGATION ? 0 :
                  (q->kind == WEBNET_CLASSIC || q->kind == WEBNET_MODULE) ? 1 :
                  q->kind == WEBNET_FETCH ? 2 : 3;
    struct webnet_wire_request h = {0};
    h.magic = WEBNET_MAGIC; h.kind = q->kind;
    h.user_navigation = (q->user_navigation ? WEBNET_WIRE_USER_NAVIGATION : 0) |
                        (q->force_preflight ? WEBNET_WIRE_FORCE_PREFLIGHT : 0);
    h.credentials = q->credentials; h.cookie_len = (uint32_t)cookie_len;
    h.id = r->id; h.generation = r->generation; h.deadline = r->deadline;
    h.url_len = ul; h.origin_len = ol; h.method_len = ml; h.headers_len = hl; h.body_len = q->body_len;
    memcpy(r->tx, &h, sizeof h);
    unsigned char *p = r->tx + sizeof h;
    memcpy(p, q->url, ul); p += ul; memcpy(p, origin, ol); p += ol;
    memcpy(p, method, ml); p += ml; memcpy(p, headers, hl); p += hl;
    if (q->body_len) memcpy(p, q->body, q->body_len);
    p += q->body_len;
    if (cookie_len && webcookie_export(n->cookies, p, (size_t)cookie_len, cookie_now) != cookie_len) { request_free(r); return 0; }
    struct request **at = &n->queue;
    while (*at && (*at)->priority <= r->priority) at = &(*at)->next;
    r->next = *at; *at = r;
    n->queued_bytes += len; n->count++;
    return r->id;
}
static bool start(struct slot *s, struct request *r) {
    int ip[2] = {-1,-1}, op[2] = {-1,-1}, nullfd = -1;
    const char *stage = "request pipe";
    int saved_errno;
    if (pipe(ip) < 0) goto fail;
    stage = "response pipe";
    if (pipe(op) < 0) goto fail;
    stage = "stderr descriptor";
    if ((nullfd = open("/dev/null", O_WRONLY)) < 0) goto fail;
    int map[3] = {ip[0], op[1], nullfd};
    char *argv[] = {"webfetch", NULL};
    stage = "spawn";
    s->pid = spawn("/bin/webfetch", argv, map, 0);
    saved_errno = errno;
    close(ip[0]); ip[0] = -1; close(op[1]); op[1] = -1; close(nullfd); nullfd = -1;
    if (s->pid < 0) { errno = saved_errno; goto fail; }
    s->in = ip[1]; s->out = op[0]; s->rq = r;
    if (fcntl(s->in, F_SETFL, O_NONBLOCK) < 0 || fcntl(s->out, F_SETFL, O_NONBLOCK) < 0) {
        slot_stop(s, "Cannot make worker pipes nonblocking");
    }
    return true;
fail:
    saved_errno = errno;
    for (int i = 0; i < 2; i++) { if (ip[i] >= 0) close(ip[i]); if (op[i] >= 0) close(op[i]); }
    if (nullfd >= 0) close(nullfd);
    s->pid = 0; s->rq = r; s->in = s->out = -1;
    r->finishing = true;
    snprintf(r->error, sizeof r->error, "Cannot start /bin/webfetch: %s (errno %d)", stage, saved_errno);
    return false;
}
static bool cookie_events(webnet *n, struct request *r) {
    size_t pos = (size_t)r->head.url_len + r->head.headers_len + r->head.body_len + r->head.error_len + 4;
    const unsigned char *p = r->rx + pos, *end = p + r->head.cookie_len;
    /* Validate the whole private event stream before applying any update. */
    for (int pass = 0; pass < 2; pass++) {
        p = r->rx + pos;
        while (p < end) {
            if ((size_t)(end-p) < sizeof(struct webnet_wire_cookie)) return false;
            struct webnet_wire_cookie e; memcpy(&e,p,sizeof e); p += sizeof e;
            if (!e.url_len || e.url_len >= WEBNET_URL_MAX || e.value_len > WEBCOOKIE_FIELD_MAX ||
                e.redirect_cross_site > 1 || e.reserved || e.received < 0 || e.received > time(NULL) + 60 ||
                (size_t)e.url_len + e.value_len > (size_t)(end-p) || memchr(p,0,(size_t)e.url_len+e.value_len)) return false;
            char url[WEBNET_URL_MAX]; memcpy(url,p,e.url_len); url[e.url_len]=0;
            if (r->credentials == WEBNET_CREDENTIALS_OMIT) return false;
            if (r->credentials == WEBNET_CREDENTIALS_SAME_ORIGIN) {
                struct url a,b;
                if (!url_parse(url,&a) || !url_parse(r->cookie_site,&b) || a.tls!=b.tls || a.port!=b.port || strcasecmp(a.host,b.host)) return false;
            }
            if (pass) {
                struct webcookie_context c = {url, *r->cookie_site ? r->cookie_site : r->top_level ? NULL : "", "GET", r->top_level, true, e.redirect_cross_site != 0};
                if (webcookie_set(n->cookies,&c,(const char *)p+e.url_len,e.value_len,e.received) < 0) return false;
            }
            p += (size_t)e.url_len + e.value_len;
        }
    }
    return true;
}
static void complete(webnet *n, struct slot *s) {
    struct request *r = s->rq;
    s->rq = NULL; s->pid = 0; slot_close(s); n->count--;
    if (r->tx) n->queued_bytes -= r->tx_len;
    if (!r->cancelled) {
        if (!r->error[0] && r->rx && !cookie_events(n,r)) snprintf(r->error,sizeof r->error,"Invalid cookie update or out of memory");
        struct webnet_response response = {0};
        response.final_url = ""; response.headers = ""; response.body = ""; response.error = r->error;
        if (!r->error[0] && r->rx) {
            char *p = (char *)r->rx;
            response.status = (int)r->head.status;
            response.final_url = p; p += r->head.url_len + 1;
            response.headers = p; p += r->head.headers_len + 1;
            response.body = p; response.body_len = r->head.body_len; p += r->head.body_len + 1;
            response.error = p;
        }
        r->callback(n, r->id, r->generation, &response, r->opaque);
    }
    request_free(r);
}
/* Wire data has no terminators; insert them after the complete payload has arrived. */
static bool unpack(struct request *r) {
    const struct webnet_wire_response *h = &r->head;
    size_t wire_len = (size_t)h->url_len + h->headers_len + h->body_len + h->error_len + h->cookie_len;
    unsigned char *out = malloc(wire_len + 4);
    if (!out) return false;
    size_t sizes[4] = {h->url_len, h->headers_len, h->body_len, h->error_len};
    size_t src = 0, dst = 0;
    for (int i = 0; i < 4; i++) {
        if (i != 2 && memchr(r->rx + src, 0, sizes[i])) { free(out); return false; }
        memcpy(out + dst, r->rx + src, sizes[i]); out[dst + sizes[i]] = 0;
        src += sizes[i]; dst += sizes[i] + 1;
    }
    memcpy(out+dst,r->rx+src,h->cookie_len);
    free(r->rx); r->rx = out;
    return true;
}
void webnet_pump(webnet *n, uint64_t now) {
    if (!n) return;
    /* Expired queued requests also finish through a normal callback, never during submit. */
    struct request **qp = &n->queue;
    while (*qp) {
        struct request *r = *qp;
        if (now < r->deadline) { qp = &r->next; continue; }
        *qp = r->next; r->next = NULL;
        struct slot temp = {r, 0, -1, -1};
        snprintf(r->error, sizeof r->error, "Request deadline exceeded"); complete(n, &temp);
        qp = &n->queue; /* callbacks may have changed the queue */
    }
    for (int i = 0; i < 2; i++) {
        struct slot *s = &n->slots[i];
        if (!s->rq && n->queue) {
            struct request *r = n->queue; n->queue = r->next; r->next = NULL; start(s, r);
        }
        struct request *r = s->rq;
        if (!r) continue;
        if (!r->finishing && now >= r->deadline) slot_stop(s, "Request deadline exceeded");
        if (!r->finishing) {
            struct n_pollfd pf[2] = {{s->out, N_POLLIN, 0}, {s->in, N_POLLOUT, 0}};
            if (poll(pf, s->in >= 0 ? 2 : 1, 0) < 0) slot_stop(s, "Worker poll failed");
            size_t budget = PUMP_BYTES;
            while (!r->finishing && s->in >= 0 && budget && (pf[1].revents & N_POLLOUT)) {
                size_t take = MIN(r->tx_len - r->tx_pos, budget);
                ssize_t k = write(s->in, r->tx + r->tx_pos, take);
                if (k < 0 && (errno == EAGAIN || errno == EINTR)) break;
                if (k <= 0) { slot_stop(s, "Worker request pipe closed"); break; }
                r->tx_pos += k; budget -= k;
                if (r->tx_pos == r->tx_len) {
                    close(s->in); s->in = -1;
                    n->queued_bytes -= r->tx_len; free(r->tx); r->tx = NULL;
                }
            }
            budget = PUMP_BYTES;
            while (!r->finishing && budget && (pf[0].revents & (N_POLLIN | N_POLLHUP))) {
                void *dest; size_t take;
                if (r->head_pos < sizeof r->head) {
                    dest = (unsigned char *)&r->head + r->head_pos; take = sizeof r->head - r->head_pos;
                } else { dest = r->rx + r->rx_pos; take = r->rx_len - r->rx_pos; }
                take = MIN(take, budget);
                ssize_t k = read(s->out, dest, take);
                if (k < 0 && (errno == EAGAIN || errno == EINTR)) break;
                if (k <= 0) { slot_stop(s, "Worker response ended early"); break; }
                budget -= k;
                if (r->head_pos < sizeof r->head) {
                    r->head_pos += k;
                    if (r->head_pos == sizeof r->head) {
                        struct webnet_wire_response *h = &r->head;
                        if (h->magic != WEBNET_MAGIC || h->id != r->id || h->generation != r->generation ||
                            h->url_len >= WEBNET_URL_MAX || h->headers_len >= WEBNET_HEADERS_MAX ||
                            h->body_len > WEBNET_BODY_LIMIT || h->error_len >= sizeof r->error || h->status > 999 ||
                            h->cookie_len > WEBNET_COOKIE_EVENTS_MAX) {
                            slot_stop(s, "Invalid worker response"); break;
                        }
                        r->rx_len = (size_t)h->url_len + h->headers_len + h->body_len + h->error_len + h->cookie_len;
                        r->rx = malloc(r->rx_len + 1);
                        if (!r->rx) { slot_stop(s, "Out of memory reading response"); break; }
                    }
                } else r->rx_pos += k;
                if (r->head_pos == sizeof r->head && r->rx_pos == r->rx_len) {
                    if (!unpack(r)) { slot_stop(s, "Invalid response or out of memory"); break; }
                    r->finishing = true; slot_close(s);
                }
            }
        }
        if (r->finishing) {
            /* Even a child that produced its frame must not hold a slot past the deadline. */
            if (s->pid > 0 && now >= r->deadline) kill(s->pid);
            int status = 0;
            int reaped = s->pid > 0 ? waitpid(s->pid, &status, WNOHANG) : -1;
            if (reaped != 0) complete(n, s);
        }
    }
}
bool webnet_busy(const webnet *n) { return n && n->count != 0; }
int webnet_timeout(const webnet *n, uint64_t now) {
    if (!webnet_busy(n)) return -1;
    int ms = 10;
    for (const struct request *r = n->queue; r; r = r->next) {
        if (now >= r->deadline) return 0;
        if (r->deadline - now < (uint64_t)ms) ms = (int)(r->deadline - now);
    }
    for (int i = 0; i < 2; i++) if (n->slots[i].rq) {
        const struct request *r = n->slots[i].rq;
        if (r->finishing || now >= r->deadline) return 0;
        if (r->deadline - now < (uint64_t)ms) ms = (int)(r->deadline - now);
    }
    return ms;
}
static void cancel(webnet *n, uint64_t key, bool generation) {
    if (!n) return;
    struct request **p = &n->queue;
    while (*p) {
        struct request *r = *p;
        if ((generation ? r->generation : r->id) != key) { p = &r->next; continue; }
        *p = r->next; n->count--; n->queued_bytes -= r->tx_len; request_free(r);
    }
    for (int i = 0; i < 2; i++) {
        struct slot *s = &n->slots[i];
        if (s->rq && (generation ? s->rq->generation : s->rq->id) == key) {
            s->rq->cancelled = true; slot_stop(s, NULL);
            if (s->pid > 0) kill(s->pid);
        }
    }
    if (generation) {
        /* Document teardown needs stopped children, not merely pending kill requests.
           Signal both first; Nocturne wakes blocked killed tasks so wait can reap them.
           Individual fetch aborts still use the nonblocking pump/reap path above. */
        for (int i = 0; i < 2; i++) {
            struct slot *s = &n->slots[i];
            if (!s->rq || s->rq->generation != key) continue;
            if (s->pid > 0) waitpid(s->pid, NULL, 0);
            complete(n, s); /* cancelled requests never invoke their callback */
        }
    }
}
void webnet_cancel(webnet *n, uint64_t id) { cancel(n, id, false); }
void webnet_cancel_generation(webnet *n, uint64_t generation) { cancel(n, generation, true); }
