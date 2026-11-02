/* システムログの保存：カーネルの記録の輪を C: の機械の場所へ写す。一人の利用者のものではない。
   この起動の分が System.log、前の起動の分が System-previous.log。シリアルの無い機械では、
   電源と一緒に輪が消えるので、止まる前に何を言ったかを読めるのはここだけ。
   一回の起動で KEEP_MOST バイトまで保存する：C: には他の用途がある。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "nocturne_services.h"

#define LOG_FOLDER "C:\\Nocturne\\Logs"
#define LOG_NOW LOG_FOLDER "\\System.log"
#define LOG_BEFORE LOG_FOLDER "\\System-previous.log"
#define KEEP_PIECE 65536u /* 一度に輪から取る量 */
#define KEEP_MOST (32ull << 20)

static void keep_note(const char *text) { n_document_append(LOG_NOW, text, strlen(text)); }

int main(void) {
    n_result r, made = n_folder_create(LOG_FOLDER);
    if (made < 0 && made != -N_ERR_EXISTS) {
        printf("systemlog: no folder for the log (%s)\n", n_result_text(made));
        return 1;
    }
    r = n_entry_replace(LOG_NOW, LOG_BEFORE);
    if (r < 0 && r != -N_ERR_NOT_FOUND) printf("systemlog: the previous log stays (%s)\n", n_result_text(r));
    n_handle log = n_system_log_open();
    char *piece = malloc(KEEP_PIECE);
    if (log < 0 || !piece) return 1;
    n_service_ready();

    n_handle queue = n_events_create();
    if (queue < 0) return 1;
    enum { LOG_TOKEN = 1, CONTROL_TOKEN };
    n_events_watch(queue, log, N_EVENT_RECEIVABLE, LOG_TOKEN);
    n_events_watch(queue, N_SERVICE_HANDLE_CONTROL, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, CONTROL_TOKEN);
    unsigned long long kept = 0;
    for (;;) {
        struct n_event reports[2];
        n_result got = n_events_wait(queue, reports, 2, N_CLOCK_NEVER);
        if (got < 0) return 1;
        for (n_result i = 0; i < got; i++) {
            if (reports[i].token == CONTROL_TOKEN) {
                /* 受ける相手はいない。閉じられたら終わる。 */
                n_handle stranger = n_inlet_accept(N_SERVICE_HANDLE_CONTROL, 0);
                if (stranger == -N_ERR_CLOSED) return 0;
                if (stranger >= 0) n_handle_close(stranger);
                continue;
            }
            n_result n = n_channel_receive(log, piece, KEEP_PIECE, 0, NULL);
            if (n == -N_ERR_NOT_READY) continue;
            if (n <= 0) {
                printf("systemlog: the system log stopped (%s)\n", n_result_text(n));
                return 1;
            }
            if (kept + (unsigned long long)n > KEEP_MOST) {
                keep_note("\n[systemlog: this start said more than the kept size; the rest is not kept here]\n");
                n_events_unwatch(queue, LOG_TOKEN);
                continue;
            }
            r = n_document_append(LOG_NOW, piece, (size_t)n);
            if (r < 0) {
                printf("systemlog: the log cannot be kept on C: (%s)\n", n_result_text(r));
                return 1;
            }
            kept += (unsigned long long)n;
        }
    }
}
