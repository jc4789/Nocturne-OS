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
    N_CAP_SHELL,           /* 窓の層と画面の端の予約、窓の一覧を持つ（シェル）。 */
    N_CAP_NETWORK,         /* adapter の住所と経路を適用し、adapter に縛った UDP を開く。 */
    N_CAP_CLOCK,           /* 壁時計と RTC を動かす。 */
    N_CAP_DISPLAY,         /* 画面のモード、配置、主画面、倍率を適用する（サービス display）。 */
    N_CAP_COUNT
};

/* 最初の process が根の能力を受ける handle の番号（0・1・2 は入力・出力・ログ）。 */
#define N_INITIAL_ROOT_HANDLE 3

/* SYS_SERVICE_CONNECT がサービスの管理者の channel へ渡す要求。後ろに名前が name_length バイト続き、
   添付が依頼者につながる channel の端。カーネルは名前を解かず、覚えない。管理者は同じ形の
   メッセージをサービスの受け口へ送る。 */
struct n_service_request {
    struct n_identity from; /* 依頼者の身元。カーネルが書く。 */
    uint32_t name_length;
    uint32_t reserved;
};

/* サービスの管理者が受ける名前。ここへつないだ channel に、受け付けたい名前をメッセージで送り、
   受け口へ送る側の channel の端を添付する。管理者は int32_t を一つ返す：0 か -N_ERR_*。
   名前を誰に許すかは管理者が from で決める。受け口の受ける側が全部閉じると名前は消える。 */
#define N_SERVICE_BIND "Nocturne.Service.Bind"
#endif
