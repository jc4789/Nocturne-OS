/* 試験の媒体のサービス "tests"。製品の媒体には無い（scripts/test.py が C:\Nocturne\System に置く）。
   試験の利用者を一人用意して自動で入るようにし、その身元で C:\Tests\autorun.sh を実行し、電源を切る。
   実行の間、試験のプログラムに能力を貸す（tests/testrun.h）。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "testrun.h"

#define SH N_FOLDER_SYSTEM "\\sh"
#define AUTORUN "C:\\Tests\\autorun.sh"
#define TEST_USER "Test"

static void lend_caps(void *arg) {
    (void)arg;
    for (;;) {
        n_handle queue = n_events_create();
        if (queue < 0) return;
        n_events_watch(queue, N_SERVICE_HANDLE_CONTROL, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, 1);
        for (;;) {
            struct n_event report;
            if (n_events_wait(queue, &report, 1, N_CLOCK_NEVER) < 0) return;
            n_handle channel;
            while ((channel = n_connector_accept(N_SERVICE_HANDLE_CONTROL, NULL, 0)) >= 0) {
                struct testrun_request q;
                int64_t result = -N_ERR_PROTOCOL;
                n_handle cap = -1;
                if (n_channel_receive(channel, &q, sizeof q, N_CHANNEL_WAIT, NULL) == sizeof q && q.cap < N_CAP_COUNT) {
                    cap = n_service_cap((enum n_cap)q.cap);
                    result = cap < 0 ? cap : 0;
                }
                n_channel_send(channel, &result, sizeof result, cap);
                n_handle_close(channel);
            }
            if (channel != -N_ERR_NOT_READY) return;
        }
    }
}

int main(void) {
    if (n_service_begin() < 0) return 1;
    struct n_accounts_request q = {.op = N_ACCOUNTS_CREATE, .cls = N_CLASS_ADMIN, .auto_login = 1};
    strlcpy(q.name, TEST_USER, sizeof q.name);
    struct n_accounts_reply reply;
    n_result r = n_accounts_call(&q, &reply);
    if (r == -N_ERR_EXISTS || r == -N_ERR_DENIED) {
        /* 同じ媒体の二度目の起動：同じ利用者が残っている */
        q.op = N_ACCOUNTS_VERIFY;
        r = n_accounts_call(&q, &reply);
    }
    if (r < 0) {
        printf("testrun: no test user (%s)\n", n_result_text(r));
        fflush(stdout);
        return 1;
    }
    struct n_accounts_request prepare = {.op = N_ACCOUNTS_PREPARE, .user = reply.account.user};
    struct n_accounts_reply prepared;
    n_accounts_call(&prepare, &prepared);
    n_result lender = thread_create(lend_caps, NULL);
    if (lender >= 0) thread_detach((int)lender);
    n_service_ready();
    struct n_entry e;
    if (n_entry_info(AUTORUN, &e) >= 0) {
        n_handle log = n_system_log_open();
        char folder[sizeof "FOLDER=" + N_ACCOUNT_FOLDER_MAX];
        snprintf(folder, sizeof folder, "FOLDER=%s", reply.account.folder);
        const char *arguments[2] = {"sh", AUTORUN}, *settings[1] = {folder};
        int32_t handles[3] = {-1, log, log};
        struct n_identity identity = {.user = reply.account.user, .cls = reply.account.cls};
        struct n_launch launch = {.arguments = arguments, .settings = settings, .handles = handles, .argument_count = 2,
                                  .setting_count = 1, .handle_count = 3, .identity = &identity,
                                  .identity_cap = n_service_cap(N_CAP_IDENTITY_GRANT)};
        n_handle app = n_app_launch(SH, &launch);
        if (log >= 0) n_handle_close(log);
        if (app >= 0) n_app_result(app, N_CLOCK_NEVER, NULL);
    }
    poweroff();
    return 1;
}
