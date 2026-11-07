/* 試験の媒体のサービス "tests"。製品の媒体には無い。試験の媒体を作るとき、build/host/store.exe put で
   C:\Nocturne\System に置く。
   試験の利用者を一人用意して自動で入るようにし、その身元で C:\Tests\autorun.sh を実行し、電源を切る。
   シェルは受けた能力を子へ渡すので、試験のプログラムは n_cap_find で能力を引く。能力の呼出しは公開の呼出しでない
   ので、それを使う試験（pcitest、nvrmtest）はホストで組んで C:\Nocturne\System に置く（sys/proc.c）。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "testrun.h"
#include "nocturne_settings.h"

#define SH N_FOLDER_SYSTEM "\\sh"
#define AUTORUN "C:\\Tests\\autorun.sh"
#define TEST_USER "Test"
#define DNS_SERVER "C:\\Tests\\dns-server"

/* 試験の道具の resolver（a.b.c.d:port）を機械の設定にする。 */
static void dns_server_set(void) {
    FILE *f = fopen(DNS_SERVER, "r");
    if (!f) return;
    char line[64] = "";
    unsigned a, b, c, d, port;
    bool read = fgets(line, sizeof line, f) && sscanf(line, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) == 5;
    fclose(f);
    if (!read) return;
    struct n_sockaddr resolver = {.ip = a << 24 | b << 16 | c << 8 | d, .port = (uint16_t)port};
    n_result r = n_setting_set(n_setting_at(N_DOMAIN_NETWORK, N_NETWORK_DNS_SERVER), &resolver, sizeof resolver, NULL);
    if (r < 0) printf("testrun: the DNS server was not set (%s)\n", n_result_text(r));
}

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
    n_setting_session(reply.account.user); /* セッションの無い試験の間、試験の利用者が前面 */
    n_service_ready();
    dns_server_set();
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
