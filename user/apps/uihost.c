/* uihost: the interface engine (libc/ui.c) for programs that cannot call C.
   The program launches uihost (found in C:\Nocturne\System) with one end of a message channel
   as handle 3 and sends it the session messages of ui.h, one per channel message;
   it gets back the engine's messages the same way. Two additions for the
   channel: an 's' reply carries the surface's shared pages as its attachment,
   and a message the engine refused is answered by 'x' i32 result, u8 its type.
   uihost ends when the program closes its end (or ends). */
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "ui.h"

#define CHANNEL 3
#define CHANNEL_TOKEN (1ull << 32)

/* Takes every message waiting on the channel; false once it has closed. */
static bool take(struct ui_session *s) {
    for (;;) {
        n_handle attached = -1;
        n_result size = n_channel_receive(CHANNEL, NULL, 0, N_CHANNEL_PEEK, &attached);
        if (size == -N_ERR_NOT_READY) return true;
        if (size < 0) return false;
        uint8_t *buf = malloc(size ? (size_t)size : 1);
        if (!buf) {
            n_channel_receive(CHANNEL, NULL, 0, N_CHANNEL_DISCARD, &attached);
            continue;
        }
        n_result got = n_channel_receive(CHANNEL, buf, (size_t)size, 0, &attached);
        if (attached >= 0) n_handle_close(attached);
        if (got < 0) { free(buf); return got != -N_ERR_CLOSED; }
        n_result r = ui_session_send(s, buf, (size_t)got);
        if (r != 0) {
            uint8_t x[6] = {'x'};
            int32_t result = (int32_t)r;
            memcpy(x + 1, &result, 4);
            x[5] = got ? buf[0] : 0;
            n_channel_send(CHANNEL, x, sizeof x, -1);
        }
        free(buf);
    }
}

/* Sends the engine's messages to the program; false once it has gone. */
static bool give(struct ui_session *s) {
    size_t n;
    const uint8_t *m;
    while ((m = ui_session_peek(s, &n))) {
        n_handle attach = -1;
        if (n >= 9 && m[0] == 's') {
            uint32_t id;
            int32_t result;
            memcpy(&id, m + 1, 4);
            memcpy(&result, m + 5, 4);
            if (result == 0) attach = ui_session_surface_handle(s, id);
        }
        n_result r = n_channel_send(CHANNEL, m, n, attach < 0 ? -1 : attach);
        ui_session_pop(s);
        if (r == -N_ERR_CLOSED) return false;
    }
    return true;
}

int main(void) {
    struct ui_session *s = ui_session_new();
    if (!s) return 1;
    n_handle q = ui_session_queue(s);
    if (n_events_watch(q, CHANNEL, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, CHANNEL_TOKEN) < 0) return 1;
    for (;;) {
        if (!take(s)) break;
        ui_session_pump(s);
        if (!give(s)) break;
        struct n_event ev[16];
        n_events_wait(q, ev, 16, ui_session_timeout(s));
    }
    ui_session_free(s);
    return 0;
}
