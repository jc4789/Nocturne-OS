/* システムのサービスの口の型。全部が型のある記録で、channel の一通が一つの記録。
   身元は記録に書かない。受ける側が n_channel_peer で確かめる。接続の要求の形は authority_abi.h。 */
#ifndef COMMON_SERVICES_ABI_H
#define COMMON_SERVICES_ABI_H
#include <stdint.h>
#include "authority_abi.h"

#define N_SERVICE_NAME_MAX 64u
#define N_SERVICE_ARGUMENT_MAX 16u
#define N_SERVICE_PROGRAM_MAX 64u
#define N_SERVICE_MANAGER "services" /* サービスの管理者の口の名前 */

/* 宣言のサービスは handle 3 に管理者との channel を受ける。受ける向きは受け口（n_inlet_accept）で、
   管理者がこの channel を閉じたら片付けて終わる。送る向きは準備の知らせ一通（n_service_ready）。 */
#define N_SERVICE_HANDLE_CONTROL 3
struct n_service_ready_message { uint32_t kind; };
#define N_SERVICE_READY 1u

/* ---- サービスの管理者の口（N_SERVICE_MANAGER） ---- */
enum n_services_op {
    N_SERVICES_LIST = 1, /* 全部の実例。応答の後に count 通の n_service_status。 */
    N_SERVICES_STATUS,   /* name と argument の実例。応答の後に count 通の n_service_status。 */
    N_SERVICES_RESTART,  /* N_CLASS_ADMIN 以上。 */
    N_SERVICES_STOP,     /* N_CLASS_ADMIN 以上。 */
    N_SERVICES_WATCH,    /* 以後、状態が変わるたびに n_service_status を一通送る。 */
};
enum n_service_state {
    N_SERVICE_STOPPED = 0, /* 動いていない。要求が来れば起こす。 */
    N_SERVICE_WAITING,     /* 依存の準備を待っている。 */
    N_SERVICE_STARTING,    /* 起こした。準備の知らせを待っている。 */
    N_SERVICE_RUNNING,     /* 準備の知らせを受けた。 */
    N_SERVICE_BACKOFF,     /* 落ちた。宣言の間隔の後に起こし直す。 */
    N_SERVICE_FAILED,      /* 起こし直しを諦めた。RESTART の依頼を待つ。 */
    N_SERVICE_ABSENT,      /* プログラムがこの媒体に無い（宣言で許されている）。 */
};
struct n_services_request {
    uint32_t op; /* enum n_services_op */
    uint32_t reserved;
    char name[N_SERVICE_NAME_MAX];
    char argument[N_SERVICE_ARGUMENT_MAX];
};
struct n_services_reply {
    int64_t result; /* 0 か、負の N_ERR_* */
    uint32_t count; /* この後に続く n_service_status の数 */
    uint32_t reserved;
};
struct n_service_status {
    char name[N_SERVICE_NAME_MAX];
    char argument[N_SERVICE_ARGUMENT_MAX];
    char program[N_SERVICE_PROGRAM_MAX];
    uint32_t cls;      /* enum n_class */
    uint32_t state;    /* enum n_service_state */
    uint32_t restarts; /* 今の健康な期間に起こし直した回数 */
    uint32_t restarts_allowed;
    int64_t app;         /* 動いている app の番号。無ければ 0。 */
    int64_t last_status; /* 最後に終わったときの結果 */
    uint64_t state_since; /* その状態に入った monotonic の時刻（ns） */
};

/* ---- 利用者（"accounts"） ---- */
#define N_ACCOUNT_NAME_MAX 64u      /* nul を含む */
#define N_ACCOUNT_PASSWORD_MAX 256u /* nul を含む */
#define N_ACCOUNT_FOLDER_MAX 96u    /* nul を含む。"C:\Users\" と名前。 */
enum n_accounts_op {
    N_ACCOUNTS_LIST = 1, /* 応答の後に count 通の n_account。 */
    N_ACCOUNTS_SELF,     /* 相手自身の n_account。システムの身元は N_ERR_NOT_FOUND。 */
    N_ACCOUNTS_CREATE,   /* 一人目は誰が頼んでも admin になる。二人目からは N_CLASS_ADMIN 以上。 */
    N_ACCOUNTS_VERIFY,   /* name と password が合えば、その n_account。N_CLASS_SYSTEM。 */
    N_ACCOUNTS_SET_CLASS, /* user の級を cls にする。N_CLASS_ADMIN 以上。最後の admin は下げられない。 */
    N_ACCOUNTS_PREPARE,  /* user のフォルダーと一時フォルダーを用意する。N_CLASS_SYSTEM。 */
};
struct n_account {
    uint32_t user;       /* 1 から */
    uint32_t cls;        /* enum n_class：N_CLASS_USER か N_CLASS_ADMIN */
    uint32_t auto_login; /* 起動のときに合言葉なしで入る */
    uint32_t has_password;
    char name[N_ACCOUNT_NAME_MAX];
    char folder[N_ACCOUNT_FOLDER_MAX];
};
struct n_accounts_request {
    uint32_t op;   /* enum n_accounts_op */
    uint32_t user;
    uint32_t cls;
    uint32_t auto_login;
    char name[N_ACCOUNT_NAME_MAX];
    char password[N_ACCOUNT_PASSWORD_MAX];
};
struct n_accounts_reply {
    int64_t result;
    uint32_t count;
    uint32_t reserved;
    struct n_account account;
};

/* ---- セッション（"session"） ---- */
enum n_session_op {
    N_SESSION_END = 1, /* 相手のセッションを終え、ログインの窓へ戻る。 */
};
struct n_session_request { uint32_t op; };
struct n_session_reply { int64_t result; };

/* ---- プログラムの台帳（"programs"） ----
   文の長さは決めない。要求は一通で、頭に種類、名前、パス、引数の文字列（nul 終わり、バイト数は nul を含む）が続く。
   応答は一通で、頭に count 個の記録が続く。 */
enum n_icon {
    N_ICON_TERMINAL, N_ICON_FILES, N_ICON_PAINT, N_ICON_CLOCK, N_ICON_SNAKE, N_ICON_FRACTAL, N_ICON_CUBE,
    N_ICON_LIFE, N_ICON_MONITOR, N_ICON_EDITOR, N_ICON_ABOUT, N_ICON_CALC, N_ICON_POWER, N_ICON_REBOOT,
    N_ICON_TETRIS, N_ICON_MINES, N_ICON_NET, N_ICON_PIANO, N_ICON_SOUND, N_ICON_SETTINGS, N_ICON_SLEEP,
    N_ICON_HIBERNATE, N_ICON_LOGOUT, N_ICON_GENERIC, N_ICON_COUNT
};
enum n_programs_op {
    N_PROGRAMS_LIST = 1, /* 全部のプログラム。記録は n_program_head。 */
    N_PROGRAMS_CLAIMS,   /* 全部の種類の組。記録は n_program_claim_head。 */
    N_PROGRAMS_FOR_TYPE, /* 相手の利用者が種類を開くプログラム。記録は一つ。 */
    N_PROGRAMS_CHOOSE,   /* 相手の利用者の既定を id にする。 */
    N_PROGRAMS_REGISTER, /* C:\Programs の中のプログラムを足す。N_CLASS_ADMIN 以上。記録は足したもの。 */
    N_PROGRAMS_CLAIM,    /* id が種類を開けると言う。N_CLASS_ADMIN 以上。 */
    N_PROGRAMS_REMOVE,   /* 登録のプログラムを消す。N_CLASS_ADMIN 以上。 */
};
#define N_PROGRAM_MENU 1u    /* 開始メニューに出す */
#define N_PROGRAM_DESKTOP 2u /* デスクトップに印を出す */
#define N_PROGRAM_SYSTEM 4u  /* システムのプログラム。消せない。 */
#define N_PROGRAM_ANY_TYPE "*" /* どの種類も開けるという種類。文書の種類は小文字の拡張子で点なし。 */
struct n_programs_request {
    uint32_t op;   /* enum n_programs_op */
    uint32_t id;
    uint32_t icon; /* enum n_icon */
    uint32_t flags;
    uint32_t type_bytes, name_bytes, path_bytes, argument_bytes;
};
struct n_programs_reply {
    int64_t result;
    uint32_t count;
    uint32_t reserved;
};
/* 続けて名前、パス、引数。引数は無ければ空で、起こすときの最初の引数になる。 */
struct n_program_head {
    uint32_t id, icon, flags;
    uint32_t name_bytes, path_bytes, argument_bytes;
};
/* 続けて種類。current はその種類を相手の利用者が今開くプログラム。 */
struct n_program_claim_head {
    uint32_t id, current, type_bytes;
};

/* ---- 機械（"machine"）：電源と画面の読取り。能力はこのサービスだけが持つ。 ---- */
enum n_machine_op {
    N_MACHINE_POWER_OFF = 1,
    N_MACHINE_REBOOT,
    N_MACHINE_SCREEN, /* 応答の添付が 0xAARRGGBB の画素の共有ページ。 */
    N_MACHINE_SLEEP,
    N_MACHINE_HIBERNATE,
};
/* 利用者が選んだ：アプリは窓を残して取りやめられ、応答はすぐ返る（N_POWER_CHOSEN、abi.h）。 */
#define N_MACHINE_CHOSEN 1u
struct n_machine_request { uint32_t op; uint32_t flags; };
struct n_machine_reply {
    int64_t result;
    int32_t width, height;
};
#endif
