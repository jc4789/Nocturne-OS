#ifndef COMMON_AUTHORITY_ABI_H
#define COMMON_AUTHORITY_ABI_H
#include <stdint.h>
/* プロセスの権限の級。上の級ほど広い。 */
enum n_class { N_CLASS_USER = 0, N_CLASS_ADMIN = 1, N_CLASS_SYSTEM = 2 };
/* カーネルが証明するプロセスの身元。アプリは書けない。 */
struct n_identity {
    uint32_t user;    /* 利用者の番号。0 はシステム。 */
    uint32_t session; /* セッションの番号。0 はセッションの外。 */
    uint32_t cls;     /* enum n_class */
    uint32_t program; /* サービスの管理者が割り当てるプログラムの番号。 */
};
/* 能力。特権は handle で持つ。 */
enum n_cap {
    N_CAP_ROOT = 0,        /* 全部を切り出せる根。 */
    N_CAP_SERVICE_ROOT,    /* サービスの名前の登録を受ける。 */
    N_CAP_IDENTITY_GRANT,  /* 子に別の利用者の身元を与える。 */
    N_CAP_PCI_CLAIM,
    N_CAP_POWER,
    N_CAP_SCREEN_READ,
    N_CAP_SETTINGS_APPLY,  /* カーネルが持つ領域へ設定を適用する。 */
    N_CAP_STORE_PRIVATE,   /* 名前の木の外の私的な領域を開く。 */
    N_CAP_COUNT
};
#endif
