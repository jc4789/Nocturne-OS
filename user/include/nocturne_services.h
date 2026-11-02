/* システムのサービスの libc の口（user/libc/services.c）。docs/nocturne-native-abi.md §15。
   型は common/services_abi.h。身元、能力、受け口、接続は nocturne.h。 */
#pragma once
#include "nocturne.h"
#include "services_abi.h"
#include <cdefs.h>
NOCTURNE_BEGIN_DECLS

/* 名前の木の外の私的な領域の文書（enum n_private_region、§3）。user 0 は機械の領域。
   access は n_document_open と同じで、0 は N_DOCUMENT_EDIT。cap：N_CAP_STORE_PRIVATE。 */
n_handle n_store_private_open(n_handle cap, uint32_t region, uint32_t user, int access);

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

/* ---- セッション ---- */
/* 呼ぶ側のセッションを終え、ログインの窓へ戻る。 */
n_result n_session_end(void);

/* その身元のアプリを止める（N_CAP_IDENTITY_GRANT）。まだ終わっていない数を返す。 */
n_result n_session_stop(n_handle cap, uint32_t session);

/* ---- プログラムの台帳 ---- */
struct n_program {
    uint32_t id, icon, flags;
    const char *name, *path, *argument;
};
struct n_program_claim {
    uint32_t id, current;
    const char *type;
};
/* 新しい配列（文字列ごと一度の free で放す）。数を返す。 */
n_result n_programs_list(struct n_program **out);
n_result n_programs_claims(struct n_program_claim **out);
/* プログラムを起こす。document があれば最初の引数になる。アプリの番号を返す。 */
n_result n_program_run(const struct n_program *p, const char *document);
/* 文書を、呼ぶ側の利用者がその種類を開くプログラムで開く。 */
n_result n_program_open_document(const char *path);
/* 管理者の操作。足したプログラムの番号を返す。 */
n_result n_program_register(const char *name, const char *path, const char *argument, uint32_t icon, uint32_t flags);
n_result n_program_claim(uint32_t id, const char *type);
n_result n_program_remove(uint32_t id);
/* 呼ぶ側の利用者の、種類の既定を決める。 */
n_result n_program_choose(uint32_t id, const char *type);

/* ---- 機械 ---- */
/* N_MACHINE_POWER_OFF か N_MACHINE_REBOOT。戻るのは失敗のときだけ。 */
n_result n_machine_power(uint32_t op);
/* 利用者が選んだ電源の変更（N_MACHINE_POWER_OFF、N_MACHINE_REBOOT、N_MACHINE_SLEEP、N_MACHINE_HIBERNATE）：
   すぐ戻り、進み方はシェルが n_power_state で読む。 */
n_result n_machine_choose(uint32_t op);
/* 画面の画素の新しい配列（free する）。 */
n_result n_machine_screen(uint32_t **pixels, int *width, int *height);

NOCTURNE_END_DECLS
