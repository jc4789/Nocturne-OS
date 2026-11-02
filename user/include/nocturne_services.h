/* サービスへつなぐ口（user/libc/services.c）。 */
#pragma once
#include <nocturne.h>
#include <services_abi.h>
#include <cdefs.h>
NOCTURNE_BEGIN_DECLS

/* name（"名前" か "名前:引数"）のサービスへの channel。動いていなければ管理者が起こす。 */
n_handle n_service_connect(const char *name);

NOCTURNE_END_DECLS
