/* 設定の口（user/libc/settings.c）。値はサービス settings が持ち、持ち主へ適用する。

   値の形：BOOL・INT・ENUM は int32_t、STRING は UTF-8 のバイト列、ENDPOINT と MONITOR_MODE はその構造体。
   変更の関数は最後の引数に enum n_setting_reason を返す（NULL 可）。 */
#pragma once
#include <nocturne.h>
#include <settings_abi.h>
#include <cdefs.h>
NOCTURNE_BEGIN_DECLS

/* 自分の鍵の宛先。 */
static inline struct n_setting_id n_setting_at(uint32_t domain, uint32_t key) {
    struct n_setting_id id = {.domain = domain, .key = key};
    return id;
}
/* 自分のプログラムの scope の鍵の宛先。 */
static inline struct n_setting_id n_setting_of_program(uint32_t key) { return n_setting_at(N_DOMAIN_PROGRAM, key); }
/* 装置を指す番号。同じ文字列（モニターの識別子など）は同じ番号。 */
uint64_t n_setting_device(const char *text);

/* 値を out へ（cap まで）。値の長さを返す。cap が足りなければ -N_ERR_TOO_LARGE。 */
n_result n_setting_get(struct n_setting_id id, void *out, size_t cap);
n_result n_setting_get_int(struct n_setting_id id, int32_t *out);
n_result n_setting_set(struct n_setting_id id, const void *value, size_t length, uint32_t *reason);
n_result n_setting_set_int(struct n_setting_id id, int32_t value, uint32_t *reason);
/* 今の値が expected のときだけ書く。違えば -N_ERR_BUSY（N_SETTING_NOT_THE_SAME）。 */
n_result n_setting_set_if(struct n_setting_id id, const void *expected, size_t expected_length, const void *value,
                          size_t length, uint32_t *reason);

/* 期限つきの試し。revert_to は保存された値が無いときに戻す先（NULL 可）。 */
n_result n_setting_try(struct n_setting_id id, const void *value, size_t length, const void *revert_to, size_t revert_length,
                       uint32_t trial_ms, uint32_t *reason);
n_result n_setting_commit(struct n_setting_id id, uint32_t *reason);
n_result n_setting_revert(struct n_setting_id id, uint32_t *reason);

/* 見張り：受け取れる channel を返す（struct n_settings_changed が一通ずつ届く）。domain か key に N_SETTING_ANY。
   閉じるまで有効。n_setting_watch_take は届いた一通を取り、無ければ -N_ERR_NOT_READY。 */
n_handle n_setting_watch(uint32_t domain, uint32_t key);
n_result n_setting_watch_take(n_handle watch, struct n_settings_changed *out);

/* 見える宣言の全部を新しい配列で返す（free する）。数を返す。 */
n_result n_setting_list(struct n_setting_decl **out);

/* プログラムの scope の宣言。プログラムの始めに一度。同じ宣言を繰り返してよい。 */
n_result n_setting_declare(const struct n_setting_decl *declarations, size_t count, uint32_t *reason);

/* 領域の持ち主（システムのサービス）。n_setting_owner_open は専用の接続を開いて領域の鍵を宣言し、その受け取れる
   channel を返す。接続が閉じれば持ち主でなくなるので、サービスは止まり、起こし直されて宣言し直す。適用の依頼が
   届くと n_setting_owner_take が取る：id と値（cap まで）を書いて serial を返し、無ければ -N_ERR_NOT_READY。
   答えは n_setting_owner_answer。値は n_setting_get で読む（別の接続）。答える前に試しの値は読めない。 */
n_handle n_setting_owner_open(const struct n_setting_decl *declarations, size_t count, uint32_t *reason);
n_result n_setting_owner_take(n_handle owner, struct n_setting_id *id, void *value, size_t cap, uint32_t *length, uint32_t *serial);
n_result n_setting_owner_answer(n_handle owner, uint32_t serial, n_result result);

/* 前面の利用者が変わった。N_CLASS_SYSTEM のセッションのサービスだけが呼ぶ。 */
n_result n_setting_session(uint32_t user);

NOCTURNE_END_DECLS
