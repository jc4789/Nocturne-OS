/* デスクトップのプロセス：一つのセッションの利用者の身元で動き、その利用者のプログラムを起こす。
   子は身元を継ぐので、ここから起こしたものは全部このセッションのもの。
   依頼は二つの道から来る：session から渡された channel（handle N_SERVICE_HANDLE_CONTROL。
   添付は答えを待つ相手の channel）と、窓の管理者のメニューとアイコン（SYS_DESKTOP_ATTACH）。
   どちらの形も struct n_session_request。この app が止まるとセッションが終わる。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "nocturne_services.h"

#define REQUEST_MAX (sizeof(struct n_session_request) + 2 * N_SESSION_PATH_MAX)

static n_result launch_one(const struct n_session_request *q, size_t bytes) {
    if (bytes < sizeof *q || q->op != N_SESSION_LAUNCH || q->path_bytes >= N_SESSION_PATH_MAX ||
        q->argument_bytes >= N_SESSION_PATH_MAX || bytes != sizeof *q + q->path_bytes + q->argument_bytes)
        return -N_ERR_PROTOCOL;
    char path[N_SESSION_PATH_MAX], argument[N_SESSION_PATH_MAX];
    const char *text = (const char *)(q + 1);
    memcpy(path, text, q->path_bytes);
    path[q->path_bytes] = 0;
    memcpy(argument, text + q->path_bytes, q->argument_bytes);
    argument[q->argument_bytes] = 0;
    char folder[sizeof "FOLDER=" + N_ACCOUNT_FOLDER_MAX];
    snprintf(folder, sizeof folder, "FOLDER=%s", n_user_folder());
    const char *arguments[2] = {path, argument}, *settings[1] = {folder};
    int32_t handles[3] = {-1, -1, -1};
    struct n_launch launch = {.arguments = arguments, .settings = settings, .handles = handles,
                              .argument_count = q->argument_bytes ? 2 : 1, .setting_count = 1, .handle_count = 3};
    n_handle app = n_app_launch(path, &launch);
    if (app < 0) return app;
    n_handle_close(app);
    return 0;
}

int main(void) {
    char *message = malloc(REQUEST_MAX);
    n_handle queue = n_events_create();
    n_handle ends[2];
    if (!message || queue < 0 || n_channel_pair(ends) < 0) return 1;
    enum { FROM_SESSION = 1, FROM_MENU };
    n_events_watch(queue, N_SERVICE_HANDLE_CONTROL, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, FROM_SESSION);
    n_events_watch(queue, ends[0], N_EVENT_RECEIVABLE, FROM_MENU);
    n_result attached = __syscall(SYS_DESKTOP_ATTACH, ends[1], 0, 0, 0, 0);
    if (attached < 0) printf("desktop: the menu does not reach this session (%s)\n", n_result_text(attached));
    n_handle_close(ends[1]);
    fflush(stdout);
    for (;;) {
        struct n_event reports[2];
        n_result got = n_events_wait(queue, reports, 2, N_CLOCK_NEVER);
        if (got < 0) return 1;
        for (n_result i = 0; i < got; i++) {
            n_handle from = reports[i].token == FROM_SESSION ? N_SERVICE_HANDLE_CONTROL : ends[0];
            n_handle client = -1;
            n_result bytes = n_channel_receive(from, message, REQUEST_MAX, 0, &client);
            if (bytes == -N_ERR_NOT_READY) continue;
            if (bytes == -N_ERR_CLOSED && from == N_SERVICE_HANDLE_CONTROL) return 0;
            if (bytes < 0) continue;
            n_result r = launch_one((const void *)message, (size_t)bytes);
            if (client >= 0) {
                struct n_session_reply reply = {.result = r};
                n_channel_send(client, &reply, sizeof reply, -1);
                n_handle_close(client);
            } else if (r < 0) {
                printf("desktop: cannot start a program (%s)\n", n_result_text(r));
                fflush(stdout);
            }
        }
    }
}
