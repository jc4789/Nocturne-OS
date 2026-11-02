/* サービスの仕組みの libc の口（user/libc/services.c）。
   docs/nocturne-native-abi.md §13。型は common/services_abi.h と common/authority_abi.h。 */
#pragma once
#include "nocturne.h"
#include "services_abi.h"
#include <cdefs.h>
NOCTURNE_BEGIN_DECLS

/* ---- カーネルが証明する身元と能力 ---- */
n_result n_identity_self(struct n_identity *out);
/* channel のもう一端を持つプロセスの身元。 */
n_result n_channel_peer(n_handle channel, struct n_identity *out);
/* cap から what の能力を切り出す。 */
n_handle n_cap_derive(n_handle cap, enum n_cap what);

/* 名前の木の外の私的な領域 region の文書（読み書き。無ければ空で作られる）。cap：N_CAP_STORE_PRIVATE。 */
n_handle n_store_private_open(n_handle cap, const char *region);

/* ---- アプリの側 ---- */
/* 名前のサービスとの channel。名前は "名前" か "名前:引数"。相手はサービスの管理者が起こす。
   宣言に無い相手の要求や、諦めたサービスへの要求は、channel が閉じる（N_ERR_CLOSED）。 */
n_handle n_service_connect(const char *name);

/* ---- 接続の口 ----
   channel の一通が新しい channel の片端を運ぶ。名前の登録を通らない、親と子の私的な道にも使う。 */
/* 接続の口へ新しい channel の片端を送り、もう片端を返す。argument は NULL でよい。 */
n_handle n_connector_connect(n_handle connector, const char *argument);
/* 接続の口の次の相手。無ければ N_ERR_NOT_READY、止める指示なら N_ERR_CANCELLED。
   口が閉じていれば N_ERR_CLOSED。argument は NULL でよい。 */
n_handle n_connector_accept(n_handle connector, char *argument, size_t cap);

/* ---- サービスの側 ----
   n_service_begin を最初に呼ぶ。能力を受け取り、以後は N_SERVICE_HANDLE_CONTROL を
   N_EVENT_RECEIVABLE で見張り、n_connector_accept で相手を受ける。 */
n_result n_service_begin(void);
/* 宣言で渡された能力。宣言に無ければ N_ERR_DENIED。閉じない。 */
n_handle n_service_cap(enum n_cap what);
/* 依存する者を起こしてよいと管理者に知らせる。 */
n_result n_service_ready(void);

/* ---- 利用者 ---- */
/* 一つの要求と一つの応答。LIST は呼ばない：n_accounts_list を使う。 */
n_result n_accounts_call(const struct n_accounts_request *request, struct n_accounts_reply *reply);
/* 全部の利用者の新しい配列（free する）。数を返す。 */
n_result n_accounts_list(struct n_account **out);

/* ---- サービスの一覧と操作（設定の窓の「サービス」の頁） ---- */
/* 全部の実例の新しい配列（free する）。数を返す。 */
n_result n_services_list(struct n_service_status **out);
/* N_SERVICES_RESTART か N_SERVICES_STOP か N_SERVICES_STOP_SESSION。 */
n_result n_services_control(uint32_t op, const char *name, const char *argument, uint32_t session);

NOCTURNE_END_DECLS
