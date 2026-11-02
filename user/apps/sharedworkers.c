/* 共有の worker の窓口（セッションごとに一つ）：鍵（起源、名前、URL）ごとに一つの
   browserjsworker --shared-server を持ち、ブラウザーの channel を渡す。worker は相手が全部
   いなくなると自分で終わり、そのとき鍵も消える。 */
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "nocturne_services.h"

#define WORKER_PROGRAM N_FOLDER_SYSTEM "\\browserjsworker"

struct worker {
    struct worker *next;
    char *key;
    n_handle app, connector; /* connector は worker の接続の口 */
};
struct waiting {
    struct waiting *next;
    n_handle channel;
    bool gone;
};
static struct worker *workers;
static struct waiting *waitings;
static n_handle queue;

enum { TOKEN_CONTROL, TOKEN_WORKER, TOKEN_WAITING };
#define TOKEN_MASK 3ull
#define TOKEN(p, kind) ((uint64_t)(uintptr_t)(p) | (uint64_t)(kind))

static struct worker *worker_start(const char *key, const char *url) {
    struct worker *w = calloc(1, sizeof *w);
    n_handle ends[2];
    if (!w || !(w->key = strdup(key)) || n_channel_pair(ends) < 0) {
        if (w) free(w->key);
        free(w);
        return NULL;
    }
    const char *arguments[3] = {"browserjsworker", "--shared-server", url};
    const int32_t handles[1] = {ends[1]};
    struct n_launch launch = {.arguments = arguments, .argument_count = 3, .handles = handles, .handle_count = 1};
    w->app = n_app_launch(WORKER_PROGRAM, &launch);
    n_handle_close(ends[1]);
    if (w->app < 0) {
        n_handle_close(ends[0]);
        free(w->key);
        free(w);
        return NULL;
    }
    w->connector = ends[0];
    w->next = workers;
    workers = w;
    n_events_watch(queue, w->app, N_EVENT_APP_ENDED, TOKEN(w, TOKEN_WORKER));
    return w;
}

static void worker_ended(struct worker *ended) {
    for (struct worker **at = &workers; *at; at = &(*at)->next) {
        if (*at != ended) continue;
        *at = ended->next;
        n_events_unwatch(queue, TOKEN(ended, TOKEN_WORKER));
        n_handle_close(ended->app);
        n_handle_close(ended->connector);
        free(ended->key);
        free(ended);
        return;
    }
}

/* 一人の要求：鍵の worker を見つけるか起こし、答えて、channel を渡す。 */
static void request_serve(n_handle channel, const char *message, size_t bytes) {
    const struct n_sharedworker_request *q = (const void *)message;
    struct n_sharedworker_reply reply = {.result = -N_ERR_PROTOCOL};
    char *key = NULL, *url = NULL;
    if (bytes >= sizeof *q && q->key_bytes && bytes == sizeof *q + (size_t)q->key_bytes + q->url_bytes) {
        key = strndup(message + sizeof *q, q->key_bytes);
        url = strndup(message + sizeof *q + q->key_bytes, q->url_bytes);
        reply.result = key && url ? 0 : -N_ERR_NO_MEMORY;
    }
    struct worker *w = NULL;
    if (reply.result >= 0) {
        for (w = workers; w && strcmp(w->key, key); w = w->next) {}
        if (!w) {
            w = worker_start(key, url);
            reply.created = w != NULL;
            if (!w) reply.result = -N_ERR_NO_APP;
        }
    }
    free(key);
    free(url);
    /* 答えが先：worker がこの channel に書くのは接続の後。 */
    n_channel_send(channel, &reply, sizeof reply, -1);
    if (w) {
        struct n_service_control message_out = {.kind = N_SERVICE_CTL_CONNECTION};
        n_channel_send(w->connector, &message_out, sizeof message_out, channel);
    }
    n_handle_close(channel);
}

static void waiting_ready(struct waiting *wait) {
    n_result size = n_channel_receive(wait->channel, NULL, 0, N_CHANNEL_PEEK, NULL);
    if (size == -N_ERR_NOT_READY) return;
    char *message = size >= 0 ? malloc(size ? (size_t)size : 1) : NULL;
    n_result got = message ? n_channel_receive(wait->channel, message, (size_t)size, 0, NULL) : -N_ERR_NO_MEMORY;
    n_events_unwatch(queue, TOKEN(wait, TOKEN_WAITING));
    if (got >= 0) request_serve(wait->channel, message, (size_t)got);
    else n_handle_close(wait->channel);
    free(message);
    wait->gone = true;
}

int main(void) {
    queue = n_events_create();
    if (queue < 0 || n_service_begin() < 0) return 1;
    n_events_watch(queue, N_SERVICE_HANDLE_CONTROL, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, TOKEN(NULL, TOKEN_CONTROL));
    n_service_ready();
    for (;;) {
        struct n_event reports[32];
        n_result got = n_events_wait(queue, reports, 32, N_CLOCK_NEVER);
        if (got < 0) return 1;
        for (n_result i = 0; i < got; i++) {
            uint64_t kind = reports[i].token & TOKEN_MASK;
            void *p = (void *)(uintptr_t)(reports[i].token & ~TOKEN_MASK);
            if (kind == TOKEN_WORKER) {
                worker_ended(p);
            } else if (kind == TOKEN_WAITING) {
                if (!((struct waiting *)p)->gone) waiting_ready(p);
            } else {
                for (;;) {
                    n_handle channel = n_connector_accept(N_SERVICE_HANDLE_CONTROL, NULL, 0);
                    if (channel == -N_ERR_NOT_READY) break;
                    if (channel < 0) return 0;
                    struct waiting *wait = calloc(1, sizeof *wait);
                    if (!wait) {
                        n_handle_close(channel);
                        continue;
                    }
                    wait->channel = channel;
                    wait->next = waitings;
                    waitings = wait;
                    n_events_watch(queue, channel, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, TOKEN(wait, TOKEN_WAITING));
                }
            }
        }
        /* 報告の組の後に掃除する：同じ組の後の報告がまだ指しているかもしれない */
        for (struct waiting **at = &waitings; *at;) {
            struct waiting *wait = *at;
            if (wait->gone) {
                *at = wait->next;
                free(wait);
            } else {
                at = &wait->next;
            }
        }
    }
}
