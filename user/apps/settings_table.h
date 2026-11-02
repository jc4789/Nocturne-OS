/* 設定の宣言の表：カーネルの領域の鍵と、適用の無い鍵（N_APPLY_NONE）。適用の無い鍵は持ち主のサービスが
   見張りで読む。settingsd が読み、tests/check_settings.py が調べる。プログラムの鍵は、それぞれが
   N_SETTINGS_DECLARE で足す。 */
#pragma once
#include "settings_abi.h"
#include "abi.h"

#define BOOL_SETTING(dom, k, sc, how, nm, lb, ab, def) \
    {.domain = dom, .key = k, .type = N_SETTING_BOOL, .scope = sc, .apply = how, .min = 0, .max = 1, .fallback = def, \
     .name = nm, .label = lb, .about = ab}
#define INT_SETTING(dom, k, sc, how, nm, lb, ab, lo, hi, def) \
    {.domain = dom, .key = k, .type = N_SETTING_INT, .scope = sc, .apply = how, .min = lo, .max = hi, .fallback = def, \
     .name = nm, .label = lb, .about = ab}

static const struct n_setting_decl settings_table[] = {
    /* 起動 */
    BOOL_SETTING(N_DOMAIN_BOOT, N_BOOT_IOMMU, N_SCOPE_MACHINE, N_APPLY_RESTART, "boot.iommu", "IOMMU",
                 "DMA を IOMMU で訳します。次の起動から効きます。", 1),
    BOOL_SETTING(N_DOMAIN_BOOT, N_BOOT_ACPI_AML, N_SCOPE_MACHINE, N_APPLY_RESTART, "boot.acpi-aml", "ACPI の AML",
                 "ACPI の AML を使います。次の起動から効きます。", 1),
    BOOL_SETTING(N_DOMAIN_BOOT, N_BOOT_SMP, N_SCOPE_MACHINE, N_APPLY_RESTART, "boot.smp", "全部の CPU",
                 "最初の CPU のほかも起こします。次の起動から効きます。", 1),
    BOOL_SETTING(N_DOMAIN_BOOT, N_BOOT_RESUME, N_SCOPE_MACHINE, N_APPLY_RESTART, "boot.resume", "休止からの復帰",
                 "休止の像があれば、そこへ戻ります。次の起動から効きます。", 1),
    BOOL_SETTING(N_DOMAIN_BOOT, N_BOOT_UNISOLATED_DRIVERS, N_SCOPE_MACHINE, N_APPLY_RESTART, "boot.unisolated-drivers",
                 "隔離できない装置のドライバー", "隔離できない装置のドライバーをアプリに許します。次の起動から効きます。", 0),
    /* 画面 */
    INT_SETTING(N_DOMAIN_DISPLAY, N_DISPLAY_HOLD_SECONDS, N_SCOPE_MACHINE, N_APPLY_NOW, "display.hold-seconds",
                "ドライバーが画面を握る秒数", "ドライバーが画面を握れる秒数です。0 は上限なしです。", 0, 86400, 0),
    /* 窓 */
    BOOL_SETTING(N_DOMAIN_WINDOW, N_WINDOW_SMOOTH_GRADIENTS, N_SCOPE_USER, N_APPLY_NOW, "window.smooth-gradients",
                 "なめらかな階調", "10 bit で描いて 8 bit の画面へ dither します。プロセッサーの時間を使います。", 0),
    /* 入力 */
    INT_SETTING(N_DOMAIN_INPUT, N_INPUT_POINTER_SPEED, N_SCOPE_USER, N_APPLY_NOW, "input.pointer-speed",
                "ポインターの速さ", "同じマウスの動きでポインターが進む量です。", -2, 2, 0),
    BOOL_SETTING(N_DOMAIN_INPUT, N_INPUT_POINTER_ACCEL, N_SCOPE_USER, N_APPLY_NOW, "input.pointer-accel",
                 "ポインターの加速", "速く動かすと大きく進みます。", 1),
    BOOL_SETTING(N_DOMAIN_INPUT, N_INPUT_SCROLL_NATURAL, N_SCOPE_USER, N_APPLY_NOW, "input.scroll-natural",
                 "スクロールの向きを逆にする", "ホイールを回す向きと頁の動く向きを入れ替えます。", 0),
    INT_SETTING(N_DOMAIN_INPUT, N_INPUT_DOUBLE_CLICK_MS, N_SCOPE_USER, N_APPLY_NOW, "input.double-click-ms",
                "ダブルクリックの間隔", "続けて二度押して一続きとみなす時間（ミリ秒）です。",
                1, INT32_MAX, N_INPUT_DOUBLE_CLICK_DEFAULT),
    INT_SETTING(N_DOMAIN_INPUT, N_INPUT_REPEAT_DELAY_MS, N_SCOPE_USER, N_APPLY_NOW, "input.repeat-delay-ms",
                "キーの繰返しまでの時間", "押し続けて繰返しが始まるまでの時間（ミリ秒）です。",
                1, INT32_MAX, N_INPUT_REPEAT_DELAY_DEFAULT),
    INT_SETTING(N_DOMAIN_INPUT, N_INPUT_REPEAT_INTERVAL_MS, N_SCOPE_USER, N_APPLY_NOW, "input.repeat-interval-ms",
                "キーの繰返しの間隔", "繰返しの一回ごとの時間（ミリ秒）です。",
                1, INT32_MAX, N_INPUT_REPEAT_INTERVAL_DEFAULT),
    {.domain = N_DOMAIN_INPUT, .key = N_INPUT_LAYOUT, .type = N_SETTING_ENUM, .scope = N_SCOPE_USER,
     .apply = N_APPLY_NOW, .max = 2, .fallback = N_LAYOUT_US, .name = "input.layout", .label = "キーボードの配列",
     .about = "キーと文字の対応です。", .options = "US\0日本語\0UK"},
    {.domain = N_DOMAIN_INPUT, .key = N_INPUT_DEVICE, .type = N_SETTING_INPUT_DEVICE, .scope = N_SCOPE_DEVICE,
     .apply = N_APPLY_NOW, .flags = N_SETTING_DECL_CUSTOM, .name = "input.device", .label = "装置ごとの設定",
     .about = "装置ごとに既定と違う値を持てます。"},
    /* 電源 */
    INT_SETTING(N_DOMAIN_POWER, N_POWER_PERFORMANCE_PREFERENCE, N_SCOPE_MACHINE, N_APPLY_NOW, "power.performance",
                "CPU の性能の好み", "0 が性能、255 が省電力です。CPU が CPPC か HWP を持つときに効きます。", 0, 255, 128),
    /* 網 */
    {.domain = N_DOMAIN_NETWORK, .key = N_NETWORK_DNS_SERVER, .type = N_SETTING_ENDPOINT, .scope = N_SCOPE_MACHINE,
     .apply = N_APPLY_NONE, .name = "network.dns-server", .label = "DNS のサーバー",
     .about = "名前を引く先です。0.0.0.0 は DHCP の言うものを使います。"},
    {.domain = N_DOMAIN_NETWORK, .key = N_NETWORK_FIXED_ADDRESS, .type = N_SETTING_IPV4, .scope = N_SCOPE_DEVICE,
     .apply = N_APPLY_NONE, .flags = N_SETTING_DECL_CUSTOM, .name = "network.fixed-address", .label = "固定の住所",
     .about = "adapter ごとの住所、マスク、ゲートウェイです。住所 0.0.0.0 は DHCP を使います。"},
    {.domain = N_DOMAIN_NETWORK, .key = N_NETWORK_PROXY_HOST, .type = N_SETTING_STRING, .scope = N_SCOPE_MACHINE,
     .apply = N_APPLY_NONE, .max = DNS_NAME_TEXT_BYTES, .name = "network.proxy-host", .label = "HTTP proxy",
     .about = "HTTP と HTTPS の接続が通る proxy の名前か住所です。空なら直接つなぎます。"},
    INT_SETTING(N_DOMAIN_NETWORK, N_NETWORK_PROXY_PORT, N_SCOPE_MACHINE, N_APPLY_NONE, "network.proxy-port",
                "HTTP proxy のポート", "proxy の TCP のポートです。", 1, 65535, 8080),
    INT_SETTING(N_DOMAIN_NETWORK, N_NETWORK_TCP_RX_BYTES, N_SCOPE_MACHINE, N_APPLY_NOW, "network.tcp-receive",
                "TCP の受信の容量", "これから開く接続の受信の容量（バイト）です。", 4096, INT32_MAX, 1048576),
    INT_SETTING(N_DOMAIN_NETWORK, N_NETWORK_TCP_TX_BYTES, N_SCOPE_MACHINE, N_APPLY_NOW, "network.tcp-send",
                "TCP の送信の容量", "これから開く接続の送信の容量（バイト）です。", 4096, INT32_MAX, 1048576),
    /* 時刻 */
    INT_SETTING(N_DOMAIN_TIME, N_TIME_ZONE_MINUTES, N_SCOPE_USER, N_APPLY_NONE, "time.zone-minutes",
                "時間帯", "時計の時刻に足す分です。localtime がこの値を読みます。", -720, 840, 0),
    BOOL_SETTING(N_DOMAIN_TIME, N_TIME_SYNC, N_SCOPE_MACHINE, N_APPLY_NONE, "time.sync", "時刻を網で合わせる",
                 "NTP のサーバーに聞いて時計と RTC を合わせます。", 1),
    {.domain = N_DOMAIN_TIME, .key = N_TIME_SERVER, .type = N_SETTING_STRING, .scope = N_SCOPE_MACHINE,
     .apply = N_APPLY_NONE, .max = DNS_NAME_TEXT_BYTES, .fallback_text = "pool.ntp.org", .name = "time.server",
     .label = "NTP のサーバー", .about = "DHCP が NTP のサーバーを言わないときに聞く名前です。"},
    /* 言語と地域 */
    {.domain = N_DOMAIN_LANGUAGE, .key = N_LANGUAGE_UI, .type = N_SETTING_ENUM, .scope = N_SCOPE_USER,
     .apply = N_APPLY_NONE, .max = 1, .fallback = N_LANGUAGE_JAPANESE, .name = "language.ui", .label = "言語",
     .about = "シェル、設定の窓、ウェブの既定の言語です。", .options = "日本語\0English"},
    {.domain = N_DOMAIN_LANGUAGE, .key = N_LANGUAGE_REGION, .type = N_SETTING_ENUM, .scope = N_SCOPE_USER,
     .apply = N_APPLY_NONE, .max = 2, .fallback = N_REGION_JAPAN, .name = "language.region", .label = "地域",
     .about = "日付と数の書式の元です。", .options = "日本\0アメリカ\0イギリス"},
    {.domain = N_DOMAIN_LANGUAGE, .key = N_LANGUAGE_DATE_ORDER, .type = N_SETTING_ENUM, .scope = N_SCOPE_USER,
     .apply = N_APPLY_NONE, .max = 3, .fallback = N_DATE_OF_REGION, .name = "language.date-order", .label = "日付の順",
     .about = "年月日の並びです。", .options = "地域に従う\0年/月/日\0月/日/年\0日/月/年"},
    {.domain = N_DOMAIN_LANGUAGE, .key = N_LANGUAGE_CLOCK, .type = N_SETTING_ENUM, .scope = N_SCOPE_USER,
     .apply = N_APPLY_NONE, .max = 2, .fallback = N_CLOCK_OF_REGION, .name = "language.clock", .label = "時刻の書式",
     .about = "24 時間か 12 時間かです。", .options = "地域に従う\00024 時間\00012 時間"},
    {.domain = N_DOMAIN_LANGUAGE, .key = N_LANGUAGE_NUMBER, .type = N_SETTING_ENUM, .scope = N_SCOPE_USER,
     .apply = N_APPLY_NONE, .max = 3, .fallback = N_NUMBER_OF_REGION, .name = "language.number", .label = "数の書式",
     .about = "桁の区切りと小数点です。", .options = "地域に従う\0001,234.5\0001.234,5\0001 234,5"},
    {.domain = N_DOMAIN_LANGUAGE, .key = N_LANGUAGE_LOGIN, .type = N_SETTING_ENUM, .scope = N_SCOPE_MACHINE,
     .apply = N_APPLY_NONE, .max = 1, .fallback = N_LANGUAGE_JAPANESE, .name = "language.login", .label = "ログインの窓の言語",
     .about = "利用者が入る前の窓の言葉です。", .options = "日本語\0English"},
};
#undef BOOL_SETTING
#undef INT_SETTING
