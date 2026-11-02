/* カーネルの中の方針の持ち主へ渡す設定の番号と型（docs/nocturne-native-abi.md §12）。

   設定の保存と文書はカーネルの外にある。カーネルは渡された値を覚えて効かせるだけで、保存しない。
   起動から最初の適用までは、装置の申告か領域の既定で動く。値は型のある固定の大きさで渡し、
   文字列の解析をしない。

   適用は SYS_SETTINGS_APPLY（N_CAP_SETTINGS_APPLY の能力が要る）。
   今の値の読出しは SYS_SETTINGS_EFFECTIVE、型と範囲の読出しは SYS_SETTINGS_DESCRIBE。
   今の値が変わったことは SYS_SETTINGS_WATCH で聞く。タスクバーの音量のように、適用の外で値が
   変わることがある。 */
#ifndef COMMON_SETTINGS_ABI_H
#define COMMON_SETTINGS_ABI_H
#include <stdint.h>

enum n_setting_domain {
    N_DOMAIN_BOOT,
    N_DOMAIN_AUDIO,
    N_DOMAIN_DISPLAY,
    N_DOMAIN_WINDOW,
    N_DOMAIN_INPUT,
    N_DOMAIN_POWER,
    N_DOMAIN_NETWORK,
    N_DOMAIN_TIME,
    N_DOMAIN_COUNT
};

/* N_DOMAIN_BOOT：起動より前に要る値。適用は起動の記録（下）へ書き、次の起動から効く。
   今の値は、今回の起動が読んだ値。 */
enum n_boot_key {
    N_BOOT_IOMMU,               /* N_SETTING_BOOL：DMA を IOMMU で訳す。既定 1 */
    N_BOOT_ACPI_AML,            /* N_SETTING_BOOL：ACPI の AML を使う。既定 1 */
    N_BOOT_SMP,                 /* N_SETTING_BOOL：最初の CPU のほかも起こす。既定 1 */
    N_BOOT_RESUME,              /* N_SETTING_BOOL：休止の像があれば戻る。既定 1 */
    N_BOOT_UNISOLATED_DRIVERS,  /* N_SETTING_BOOL：隔離できない装置のドライバーをアプリに許す。既定 0 */
    N_BOOT_KEY_COUNT
};

/* N_DOMAIN_AUDIO：dev/audio */
enum n_audio_key {
    N_AUDIO_VOLUME,             /* N_SETTING_INT 0..100 */
    N_AUDIO_MUTED,              /* N_SETTING_BOOL */
    N_AUDIO_KEY_COUNT
};

/* N_DOMAIN_DISPLAY：dev/pciuser。モードと配置はサービス display の鍵（下の enum n_display_service_key）。 */
enum n_display_key {
    N_DISPLAY_HOLD_SECONDS,     /* N_SETTING_INT 0..：ドライバーが画面を握れる秒数。0 は上限なし */
    N_DISPLAY_KEY_COUNT
};

/* N_DOMAIN_WINDOW：gui/wm */
enum n_window_key {
    N_WINDOW_SMOOTH_GRADIENTS,  /* N_SETTING_BOOL：10 bit で描いて 8 bit の画面へ dither する */
    N_WINDOW_KEY_COUNT
};

/* N_DOMAIN_INPUT：gui/wm のポインター */
enum n_input_key {
    N_INPUT_POINTER_SPEED,      /* N_SETTING_INT -2..2 */
    N_INPUT_KEY_COUNT
};

/* N_DOMAIN_POWER：arch/idle */
enum n_power_key {
    N_POWER_PERFORMANCE_PREFERENCE, /* N_SETTING_INT 0..255：CPU の性能の好み（0 が性能、255 が省電力） */
    N_POWER_KEY_COUNT
};

/* N_DOMAIN_NETWORK：net/net */
enum n_network_key {
    N_NETWORK_DNS_SERVER,       /* N_SETTING_ENDPOINT：既定の resolver。ip 0 は DHCP の言うもの */
    N_NETWORK_TCP_RX_BYTES,     /* N_SETTING_INT：これから開く接続の受信の容量 */
    N_NETWORK_TCP_TX_BYTES,     /* N_SETTING_INT：これから開く接続の送信の容量 */
    N_NETWORK_KEY_COUNT
};

/* N_DOMAIN_TIME：カーネルに持ち主のある値は今は無い。 */
enum n_time_key {
    N_TIME_KEY_COUNT
};

/* 値の型。BOOL と INT は int32_t。 */
enum n_setting_type {
    N_SETTING_BOOL,
    N_SETTING_INT,
    N_SETTING_ENDPOINT,     /* struct n_sockaddr（abi.h） */
    N_SETTING_MONITOR_MODE, /* struct n_setting_monitor_mode */
    N_SETTING_MONITOR_PLACE, /* struct n_setting_monitor_place */
    N_SETTING_DEVICE        /* uint64_t：装置の番号（n_setting_device）。0 は無し */
};

#define N_SETTING_LIST 1u       /* SYS_SETTINGS_EFFECTIVE は size の倍数の一覧を返す */
#define N_SETTING_AT_START 2u   /* 適用は次の起動から効く */

/* SYS_SETTINGS_DESCRIBE の答え。 */
struct n_setting_desc {
    uint32_t type;   /* enum n_setting_type */
    uint32_t size;   /* 一つの値のバイト数 */
    uint32_t flags;  /* N_SETTING_* */
    uint32_t reserved;
    int64_t low, high; /* BOOL と INT の範囲 */
};

/* モニターごとの選択。モニターは struct n_setting_id の device（n_screen_device）。width 0 は選択なし。
   format は N_SCANOUT_* で、0 はどれでも。 */
struct n_setting_monitor_mode {
    uint32_t width, height;
    uint32_t refresh_mhz;
    uint32_t format;
};
/* モニターごとの配置。placed 0 は自動（ほかの画面の右）。x、y はほかの画面と同じ座標で、負でもよい。 */
struct n_setting_monitor_place {
    int32_t x, y;
    uint32_t placed;
};

/* ---- 起動の記録 ----
   起動の区画の /boot/settings.rec。loader が module "settings" として持ち込む。二つの枠を持ち、
   新しい世代を古い枠へ書くので、途中で切れても前の記録が残る。全部 0 の文書は「既定のまま」。 */
#define N_BOOT_RECORD_MAGIC 0x54425a4eu /* "NZBT" */
#define N_BOOT_RECORD_SLOT 4096u
#define N_BOOT_RECORD_BYTES (2 * N_BOOT_RECORD_SLOT)
struct n_boot_record {
    uint32_t magic;
    uint32_t check;      /* この欄を 0 にした構造体の CRC-32C */
    uint64_t generation;
    int32_t value[N_BOOT_KEY_COUNT];
};

/* 名前の木の外の私的な領域（SYS_STORE_PRIVATE_OPEN）。利用者ごとに同じ番号の領域を持つ。
   利用者 0 は機械。番号の持ち主はシステムのサービス。 */
enum n_private_region {
    N_REGION_SETTINGS = 1,  /* 設定のサービスの保存 */
    N_REGION_USERS,         /* 利用者とセッションのサービスの台帳 */
    N_REGION_SECRETS,       /* 利用者の秘密 */
    N_REGION_FIRST_FREE = 16
};

/* ---- 設定のサービスの宣言と channel の文 ----
   選択の保存、宣言の表、権限の確かめ、見張りはサービス settings（user/apps/settingsd.c）が持つ。
   settings は領域の意味を持たない。呼ぶ側は libc の nocturne_settings.h を使う。 */

/* カーネルの持ち主の無い時刻の鍵。読む者が見張る。 */
enum n_time_setting_key {
    N_TIME_ZONE_MINUTES,        /* N_SETTING_INT：時計の時刻に足す分 */
};

#define N_DOMAIN_PROGRAM 0xffffffffu /* プログラムの scope の鍵の領域。適用の相手が無い。 */

/* 宣言の型は enum n_setting_type にこの二つを足したもの。 */
#define N_SETTING_ENUM 16u   /* int32_t：選択肢の番号 */
#define N_SETTING_STRING 17u /* UTF-8 のバイト列（終端なし） */

enum n_setting_scope {
    N_SCOPE_MACHINE, /* 機械に一つ。変えるのは N_CLASS_ADMIN 以上。 */
    N_SCOPE_USER,    /* 利用者ごと。変えるのは本人。 */
    N_SCOPE_DEVICE,  /* 装置ごと（struct n_setting_id の device）。変えるのは N_CLASS_ADMIN 以上。 */
    N_SCOPE_PROGRAM  /* 利用者ごと、プログラムごと。読み書きするのはそのプログラムだけ。 */
};
enum n_setting_apply {
    N_APPLY_NOW,      /* 持ち主へ渡し、その場で効く。 */
    N_APPLY_TRIAL,    /* 期限つきの試し（TRY）を経て確定する。期限が過ぎれば settings が前の値へ戻す。 */
    N_APPLY_RESTART,  /* 持ち主へ渡し、次の起動から効く。 */
    N_APPLY_NONE      /* 適用の相手が無い。保存と見張りだけ。 */
};
#define N_SETTING_NAME_MAX 48u     /* 終端を含む。ASCII の小文字、数字、'.'、'-'。 */
#define N_SETTING_LABEL_MAX 64u    /* 終端を含む。人に見せる名前。 */
#define N_SETTING_ABOUT_MAX 160u   /* 終端を含む。人に見せる説明。 */
#define N_SETTING_OPTIONS_MAX 256u /* enum の選択肢の名前。NUL で区切り、空の名前で終わる。 */
#define N_SETTING_TEXT_MAX 128u    /* 文字列の既定の長さの上限（終端を含む）。 */
#define N_SETTING_VALUE_MAX 65536u /* 値の長さの上限。 */
#define N_SETTING_DECL_SECRET 1u   /* 値を窓に出さない（鍵など）。 */
#define N_SETTING_DECL_CUSTOM 2u   /* 窓の宣言の頁に部品を組まない。専用の部品が扱う。 */

struct n_setting_decl {
    uint32_t domain;   /* enum n_setting_domain か N_DOMAIN_PROGRAM */
    uint32_t key;      /* 領域の中の番号 */
    uint32_t type;     /* enum n_setting_type か N_SETTING_ENUM・N_SETTING_STRING */
    uint8_t scope;     /* enum n_setting_scope */
    uint8_t apply;     /* enum n_setting_apply */
    uint8_t flags;     /* N_SETTING_DECL_* */
    uint8_t reserved;
    int64_t min, max;  /* INT：範囲。STRING：max が最長のバイト数。ENUM：max が選択肢の数 - 1。BOOL：0 と 1。 */
    int64_t fallback;  /* BOOL・INT・ENUM の既定 */
    char fallback_text[N_SETTING_TEXT_MAX]; /* STRING の既定 */
    char name[N_SETTING_NAME_MAX];
    char label[N_SETTING_LABEL_MAX];
    char about[N_SETTING_ABOUT_MAX];
    char options[N_SETTING_OPTIONS_MAX];
};

/* 値の宛先。user と program は 0 で呼ぶ側のもの。program は呼ぶ側の身元の外を指せない。 */
struct n_setting_id {
    uint32_t domain, key;
    uint64_t device;  /* N_SCOPE_DEVICE の装置（n_setting_device）。ほかは 0。 */
    uint32_t user;
    uint32_t program;
};
#define N_SETTING_ANY 0xffffffffu /* N_SETTINGS_WATCH の id：領域か鍵の全部 */

enum n_settings_op {
    N_SETTINGS_GET = 1, /* 値を返す。無ければ今の値か既定。 */
    N_SETTINGS_SET,     /* 検証、適用、保存、見張りへの知らせ。失敗すれば元の値へ戻す。argument が 1 なら、値の後の
                           extra バイトが今の値と同じときだけ書く（読んで直す者が他の変更を消さない）。 */
    N_SETTINGS_TRY,     /* 検証、適用。argument はミリ秒の期限。COMMIT が来なければ settings が戻す。
                           値の後に extra バイトの「戻す先」を続けてよい（保存された値が無いときに使う）。 */
    N_SETTINGS_COMMIT,  /* 試しの値を保存して確定する。 */
    N_SETTINGS_REVERT,  /* 試しをやめて前の値へ戻す。 */
    N_SETTINGS_WATCH,   /* この接続を見張りにする。以後は struct n_settings_changed が届く。 */
    N_SETTINGS_LIST,    /* 見える宣言の一覧。argument は始めの番号。応答は struct n_setting_decl の並び。 */
    N_SETTINGS_DECLARE, /* 宣言を足す。値は struct n_setting_decl。領域の宣言はこの接続を適用の相手にする。 */
    N_SETTINGS_APPLY,   /* settings から領域の持ち主へ：この値を適用する。応答は struct n_settings_reply。 */
    N_SETTINGS_SESSION  /* セッションのサービスから：argument の利用者が前面になった。その人の値を適用する。 */
};
/* 値が通らなかった理由。呼ぶ側へそのまま返す。 */
enum n_setting_reason {
    N_SETTING_OK,
    N_SETTING_UNKNOWN_KEY,    /* 宣言が無い */
    N_SETTING_WRONG_TYPE,     /* 値の長さが型に合わない */
    N_SETTING_OUT_OF_RANGE,   /* 範囲の外。選択肢の外。長すぎる。 */
    N_SETTING_NOT_ALLOWED,    /* 級か本人の条件を満たさない */
    N_SETTING_APPLY_REFUSED,  /* 持ち主が値を受け付けなかった。result に持ち主の答え。 */
    N_SETTING_APPLY_GONE,     /* 持ち主がいない、または期限までに答えない */
    N_SETTING_SAVE_FAILED,    /* 適用は戻した。result に保存の答え。 */
    N_SETTING_NO_TRIAL,       /* 試しの中でない（期限で戻った後を含む） */
    N_SETTING_NEEDS_TRIAL,    /* N_APPLY_TRIAL の鍵に SET が来た */
    N_SETTING_BUSY,           /* 同じ鍵の適用の答えを待っている */
    N_SETTING_BAD_DECLARATION, /* 宣言の型、範囲、既定、名前が正しくない */
    N_SETTING_NOT_THE_SAME    /* N_SETTINGS_SET の条件：今の値が読んだ値と違う */
};
struct n_settings_request {
    uint32_t op;       /* enum n_settings_op */
    uint32_t serial;   /* 応答に写す */
    uint32_t length;   /* 続く値のバイト数 */
    uint32_t argument;
    uint32_t extra;    /* TRY：値の後の戻す先のバイト数 */
    uint32_t reserved;
    struct n_setting_id id;
    /* 値がここに続く */
};
struct n_settings_reply {
    uint32_t op, serial;
    int32_t result;    /* 0 か -N_ERR_* */
    uint32_t reason;   /* enum n_setting_reason */
    uint32_t length;   /* 続くバイト数 */
    uint32_t more;     /* LIST：続きの始めの番号。0 は終わり。TRY：期限までのミリ秒。 */
    /* 値がここに続く */
};
enum n_setting_change {
    N_SETTING_CHANGED,    /* 新しい値が効いた（保存済み） */
    N_SETTING_ON_TRIAL,   /* 試しの値が効いた（未保存）。remaining_ms が期限まで。 */
    N_SETTING_REVERTED    /* 試しが終わり前の値へ戻った */
};
struct n_settings_changed {
    struct n_setting_id id;
    uint32_t what;         /* enum n_setting_change */
    uint32_t remaining_ms;
};
#define N_SETTINGS_SERVICE "settings" /* サービスの名前 */

/* サービス display の鍵（N_DOMAIN_DISPLAY。user/apps/display.c が宣言し、適用を受ける）。装置はモニターの身元。 */
enum n_display_service_key {
    N_DISPLAY_MODE = N_DISPLAY_KEY_COUNT, /* N_SETTING_MONITOR_MODE、N_SCOPE_DEVICE、試しつき */
    N_DISPLAY_PLACE,                      /* N_SETTING_MONITOR_PLACE、N_SCOPE_DEVICE */
    N_DISPLAY_SCALE,                      /* N_SETTING_INT、N_SCOPE_DEVICE：百分率 */
    N_DISPLAY_PRIMARY                     /* N_SETTING_DEVICE、N_SCOPE_MACHINE：主画面のモニター。0 は画面 0 */
};
/* サービス machine の鍵（N_DOMAIN_POWER。user/apps/machine.c が宣言し、適用を受ける）。 */
enum n_power_service_key {
    N_POWER_BUTTON = N_POWER_KEY_COUNT, /* N_SETTING_ENUM（enum n_power_answer）：電源ボタン */
    N_POWER_LID,                        /* N_SETTING_ENUM（同じ）：蓋を閉じたとき */
    N_POWER_IDLE_MINUTES,               /* N_SETTING_INT：入力の無い時間がこれを超えたら。0 はしない */
    N_POWER_IDLE_ANSWER,                /* N_SETTING_ENUM（同じ）：そのときにすること */
    N_POWER_ALLOW_SLEEP,                /* N_SETTING_BOOL：眠りを許す */
    N_POWER_ALLOW_HIBERNATE,            /* N_SETTING_BOOL：休止を許す */
    N_POWER_CLOSE_SECONDS,              /* N_SETTING_INT：窓が一つも閉じない時間がこれを超えたら取りやめる */
    N_POWER_AGAIN_SECONDS               /* N_SETTING_INT：取りやめからこの間にもう一度選ぶと、窓を待ち切って止まる */
};
enum n_power_answer { N_POWER_ANSWER_NOTHING, N_POWER_ANSWER_SLEEP, N_POWER_ANSWER_HIBERNATE, N_POWER_ANSWER_OFF };
#define N_SETTINGS_MESSAGE_MAX \
    (sizeof(struct n_settings_request) + 2 * N_SETTING_VALUE_MAX) /* TRY は値と戻す先を運ぶ */
#endif
