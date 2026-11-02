/* 試験のプログラムの利用者の道具。能力は試験の媒体のサービス "tests"（tests/testrun.c）からシェルを通って継ぐ。 */
#ifndef TESTS_TESTRUN_H
#define TESTS_TESTRUN_H
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "nocturne_services.h"

/* 名前の試験の利用者（無ければ作る）。呼ぶ者は試験の利用者で admin。 */
static inline n_result test_account(const char *name, struct n_account *out) {
    struct n_accounts_request q = {.op = N_ACCOUNTS_CREATE, .cls = N_CLASS_USER};
    strlcpy(q.name, name, sizeof q.name);
    struct n_accounts_reply reply;
    n_result r = n_accounts_call(&q, &reply);
    if (r >= 0) {
        *out = reply.account;
        return 0;
    }
    if (r != -N_ERR_EXISTS) return r;
    struct n_account *list;
    n_result count = n_accounts_list(&list);
    if (count < 0) return count;
    r = -N_ERR_NOT_FOUND;
    for (n_result i = 0; i < count; i++)
        if (!strcasecmp(list[i].name, name)) {
            *out = list[i];
            r = 0;
        }
    free(list);
    return r;
}
/* launch の子を、その利用者の身元で起こす（一度きりの n_app_launch）。 */
static inline n_handle test_launch_as(const struct n_account *a, const char *path, const struct n_launch *launch) {
    struct n_identity self, identity;
    if (n_identity_self(&self) < 0) return -N_ERR_DENIED;
    identity = (struct n_identity){.user = a->user, .session = self.session, .cls = a->cls};
    n_handle cap = n_cap_find(N_CAP_IDENTITY_GRANT);
    if (cap < 0) return cap;
    struct n_launch l = *launch;
    l.identity = &identity;
    l.identity_grant = cap;
    return n_app_launch(path, &l);
}
#endif
