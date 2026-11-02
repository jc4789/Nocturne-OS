/* システムのサービスの口の型。全部が型のある記録で、channel の一通が一つの記録。
   身元は記録に書かない。受ける側が SYS_CHANNEL_PEER で確かめる。 */
#ifndef COMMON_SERVICES_ABI_H
#define COMMON_SERVICES_ABI_H
#include <stdint.h>
#include "authority_abi.h"

/* サービスの名前は "名前" か "名前:引数"。引数は実例を選ぶ（GPU ごとの "gpu:0100" など）。 */
#define N_SERVICE_NAME_MAX 64u      /* ":" より前。nul を含む。 */
#define N_SERVICE_ARGUMENT_MAX 64u  /* ":" より後。nul を含む。 */
#define N_SERVICE_TEXT_MAX (N_SERVICE_NAME_MAX + N_SERVICE_ARGUMENT_MAX)
#define N_SERVICE_PROGRAM_MAX 128u
#define N_SERVICE_MANAGER "services" /* サービスの管理者自身の口の名前 */

/* SYS_SERVICE_REGISTER の channel にカーネルが積む、SYS_SERVICE_CONNECT 一回分の要求。
   添付は新しい channel のサービス側の端。その端の SYS_CHANNEL_PEER が要求した者の身元を返す。 */
struct n_service_request {
    uint32_t name_bytes;           /* nul を含まない */
    uint32_t reserved;
    char name[N_SERVICE_TEXT_MAX]; /* "名前" か "名前:引数" */
};

/* 管理者がサービスを起こすときの handle の番号。0、1、2 は入力、出力、ログ。
   この channel が管理者とサービスの間の唯一の道で、能力も接続もここを通る。 */
#define N_SERVICE_HANDLE_CONTROL 3

enum n_service_control_kind {
    N_SERVICE_CTL_CAP = 1,    /* 管理者から。添付は value の能力（enum n_cap）。 */
    N_SERVICE_CTL_CAPS_DONE,  /* 管理者から。能力はこれで全部。 */
    N_SERVICE_CTL_CONNECTION, /* 管理者から。添付は一人の相手の channel。argument はその引数。 */
    N_SERVICE_CTL_STOP,       /* 管理者から。片付けて終わる。 */
    N_SERVICE_CTL_READY,      /* サービスから。依存する者を起こしてよい。 */
};
struct n_service_control {
    uint32_t kind;  /* enum n_service_control_kind */
    uint32_t value;
    char argument[N_SERVICE_ARGUMENT_MAX];
};

/* ---- サービスの管理者の口（N_SERVICE_MANAGER） ---- */
enum n_services_op {
    N_SERVICES_LIST = 1,     /* 全部の実例。応答の後に count 通の n_service_status。 */
    N_SERVICES_STATUS,       /* name の実例。応答の後に count 通の n_service_status。 */
    N_SERVICES_RESTART,      /* N_CLASS_ADMIN 以上。 */
    N_SERVICES_STOP,         /* N_CLASS_ADMIN 以上。 */
    N_SERVICES_STOP_SESSION, /* N_CLASS_SYSTEM。session の実例を全部止める。 */
    N_SERVICES_WATCH,        /* 以後、状態が変わるたびに n_service_status を一通送る。 */
};
enum n_service_state {
    N_SERVICE_STOPPED = 0, /* 動いていない。要求が来れば起こす。 */
    N_SERVICE_WAITING,     /* 依存の準備を待っている。 */
    N_SERVICE_STARTING,    /* 起こした。READY を待っている。 */
    N_SERVICE_RUNNING,     /* READY を受けた。 */
    N_SERVICE_BACKOFF,     /* 落ちた。宣言の間隔の後に起こし直す。 */
    N_SERVICE_FAILED,      /* 起こし直しを諦めた。RESTART の依頼を待つ。 */
    N_SERVICE_ABSENT,      /* プログラムがこの媒体に無い（宣言で許されている）。 */
};
struct n_services_request {
    uint32_t op;      /* enum n_services_op */
    uint32_t session; /* 実例の選択。0 はセッションの外。STOP_SESSION の相手。 */
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
    uint32_t user;
    uint32_t session;
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
    N_ACCOUNTS_VERIFY,   /* name と password が合えば、その n_account。 */
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
    N_SESSION_LAUNCH = 1, /* 相手のセッションのデスクトップにプログラムを起こさせる。 */
    N_SESSION_END,        /* 相手のセッションを終える。 */
};
#define N_SESSION_PATH_MAX 4096u
/* 頭に n_session_request、続けて path と argument（どちらも nul を含まず、指定の長さ）。
   デスクトップのプロセスへ渡すときも同じ形。 */
struct n_session_request {
    uint32_t op;
    uint32_t path_bytes;
    uint32_t argument_bytes;
    uint32_t reserved;
};
struct n_session_reply { int64_t result; };

/* ---- 共有の worker（"sharedworkers"） ---- */
/* 頭に n_sharedworker_request、続けて key と url（nul を含まず、指定の長さ）。この channel は
   応答の後に worker の手に移り、同じ channel で worker と話す。 */
struct n_sharedworker_request {
    uint32_t key_bytes;
    uint32_t url_bytes;
};
struct n_sharedworker_reply {
    int32_t result;
    uint32_t created; /* この要求で worker を新しく起こした */
};
#endif
