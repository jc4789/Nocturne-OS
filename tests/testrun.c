/* 試験の媒体のサービス "tests"。製品の媒体には無い（scripts/test.py が C:\Nocturne\System に置く）。
   試験の利用者を一人用意して自動で入るようにし、その身元で C:\Tests\autorun.sh を実行し、電源を切る。
   シェルは受けた能力を子へ渡すので、試験のプログラムは n_cap_find で能力を引く。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "testrun.h"

#define SH N_FOLDER_SYSTEM "\\sh"
#define AUTORUN "C:\\Tests\\autorun.sh"
#define TEST_USER "Test"

int main(void) {
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
    n_service_ready();
    struct n_entry e;
    if (n_entry_info(AUTORUN, &e) >= 0) {
        n_handle log = n_system_log_open();
        const char *arguments[2] = {"sh", AUTORUN};
        int32_t handles[3 + N_CAP_COUNT] = {-1, log, log};
        size_t caps = n_caps_list(handles + 3);
        struct n_identity identity = {.user = reply.account.user, .cls = reply.account.cls};
        struct n_launch launch = {.arguments = arguments, .handles = handles, .argument_count = 2,
                                  .handle_count = (uint32_t)(3 + caps), .folder = reply.account.folder, .identity = &identity,
                                  .identity_grant = n_cap_find(N_CAP_IDENTITY_GRANT)};
        n_handle app = n_app_launch(SH, &launch);
        if (log >= 0) n_handle_close(log);
        if (app >= 0) n_app_result(app, N_CLOCK_NEVER, NULL);
    }
    poweroff(n_cap_find(N_CAP_POWER));
    return 1;
}
