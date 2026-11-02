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

/* N_DOMAIN_DISPLAY：gui/screenmode と dev/pciuser */
enum n_display_key {
    N_DISPLAY_MODE,             /* N_SETTING_MONITOR_MODE の一覧：モニターごとの利用者の選択。適用は一件ずつ */
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
    N_SETTING_MONITOR_MODE  /* struct n_setting_monitor_mode */
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

/* モニターごとの選択。monitor は struct n_screen_modes の monitor と同じ。 */
struct n_setting_monitor_mode {
    char monitor[32];
    uint32_t width, height;
    uint32_t refresh_mhz;
    uint32_t reserved;
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
#endif
