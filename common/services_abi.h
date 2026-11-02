/* システムのサービスの口の型。全部が型のある記録で、channel の一通が一つの記録。
   身元は記録に書かない。受ける側が n_channel_peer で確かめる。接続の要求の形は authority_abi.h。 */
#ifndef COMMON_SERVICES_ABI_H
#define COMMON_SERVICES_ABI_H
#include <stdint.h>
#include "abi.h"
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

/* ---- 網（"network"）：DHCP、名前引きと cache、固定の住所、経路の選択。 ---- */
enum n_network_op {
    N_NETWORK_RESOLVE = 1, /* 要求の後ろに名前が name_length バイト。応答の後ろに uint32_t の住所が count 個。 */
    N_NETWORK_LINKS,       /* 応答の後ろに struct n_network_link が count 個、それぞれ可変長。 */
    N_NETWORK_RESOLVERS,   /* 応答の後ろに struct n_sockaddr が count 個：機械の名前引きが聞くサーバー */
};
struct n_network_request {
    uint32_t op;
    uint32_t name_length;
    struct n_sockaddr resolver; /* ip 0 は機械の resolver */
};
enum n_dns_status {
    N_DNS_OK, N_DNS_NXDOMAIN, N_DNS_NODATA, N_DNS_SERVFAIL, N_DNS_REFUSED,
    N_DNS_FORMAT, N_DNS_TIMEOUT, N_DNS_UNREACHABLE, N_DNS_CANCELLED, N_DNS_RESOURCE,
};
struct n_network_reply {
    int32_t result;  /* 0 か、負の N_ERR_* */
    uint32_t status; /* enum n_dns_status */
    uint32_t count;
    uint32_t ttl;    /* 残りの秒 */
};
enum n_link_state {
    N_LINK_ABSENT,   /* 装置が無い */
    N_LINK_SEARCHING, /* DHCP の答えを待っている */
    N_LINK_LEASED,   /* DHCP の住所がある */
    N_LINK_FIXED,    /* 固定の住所 */
};
/* 一つの adapter の構成。後ろに uint32_t の住所が dns_count + ntp_count 個続く（DNS、NTP の順）。住所と
   マスクはカーネルの一覧（n_net_adapters）にある。 */
struct n_network_link {
    uint32_t adapter;
    uint32_t state;   /* enum n_link_state */
    uint32_t gateway; /* 0 は無し */
    uint32_t lease_seconds; /* N_LINK_LEASED：借りた住所の残り */
    uint32_t dns_count, ntp_count;
};

/* ---- 名前引きの結果（net_resolve_all）。count は全部の数で、渡した場所より多いことがある。 ---- */
struct n_dns_result { uint32_t version, status, count, ttl; };
#endif
