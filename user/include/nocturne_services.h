/* システムのサービスの libc の口（user/libc/services.c）。docs/nocturne-native-abi.md §15。
   型は common/services_abi.h。身元、能力、受け口、接続は nocturne.h。 */
#pragma once
#include "nocturne.h"
#include "services_abi.h"
#include <cdefs.h>
NOCTURNE_BEGIN_DECLS

/* 名前の木の外の私的な領域 region の文書（読み書き。無ければ空で作られる）。cap：N_CAP_STORE_PRIVATE。 */
n_handle n_store_private_open(n_handle cap, const char *region);

/* ---- 宣言のサービスの側 ---- */
/* 依存する者を起こしてよいと管理者に知らせる。相手は n_inlet_accept(N_SERVICE_HANDLE_CONTROL, ...) で受ける。 */
n_result n_service_ready(void);

/* ---- 利用者 ---- */
/* 一つの要求と一つの応答。LIST は n_accounts_list を使う。 */
n_result n_accounts_call(const struct n_accounts_request *request, struct n_accounts_reply *reply);
/* 全部の利用者の新しい配列（free する）。数を返す。 */
n_result n_accounts_list(struct n_account **out);

/* ---- サービスの一覧と操作（設定の窓の「サービス」の頁） ---- */
/* 全部の実例の新しい配列（free する）。数を返す。 */
n_result n_services_list(struct n_service_status **out);
/* N_SERVICES_RESTART か N_SERVICES_STOP。 */
n_result n_services_control(uint32_t op, const char *name, const char *argument);

/* ---- 機械 ---- */
/* N_MACHINE_POWER_OFF か N_MACHINE_REBOOT。戻るのは失敗のときだけ。 */
n_result n_machine_power(uint32_t op);
/* 画面の画素の新しい配列（free する）。 */
n_result n_machine_screen(uint32_t **pixels, int *width, int *height);

NOCTURNE_END_DECLS
