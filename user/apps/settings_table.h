/* 設定の宣言の表：カーネルの領域の鍵と、持ち主の無い時刻の鍵。settingsd が読み、tests/check_settings.py
   が調べる。ユーザー空間のサービスとプログラムの鍵は、それぞれが N_SETTINGS_DECLARE で足す。 */
#pragma once
#include "settings_abi.h"

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
    {.domain = N_DOMAIN_DISPLAY, .key = N_DISPLAY_MODE, .type = N_SETTING_MONITOR_MODE, .scope = N_SCOPE_DEVICE,
     .apply = N_APPLY_TRIAL, .flags = N_SETTING_DECL_CUSTOM, .name = "display.mode", .label = "解像度と拍",
     .about = "モニターごとの解像度とリフレッシュレートです。"},
    INT_SETTING(N_DOMAIN_DISPLAY, N_DISPLAY_HOLD_SECONDS, N_SCOPE_MACHINE, N_APPLY_NOW, "display.hold-seconds",
                "ドライバーが画面を握る秒数", "ドライバーが画面を握れる秒数です。0 は上限なしです。", 0, 86400, 0),
    /* 窓 */
    BOOL_SETTING(N_DOMAIN_WINDOW, N_WINDOW_SMOOTH_GRADIENTS, N_SCOPE_USER, N_APPLY_NOW, "window.smooth-gradients",
                 "なめらかな階調", "10 bit で描いて 8 bit の画面へ dither します。プロセッサーの時間を使います。", 0),
    /* 入力 */
    INT_SETTING(N_DOMAIN_INPUT, N_INPUT_POINTER_SPEED, N_SCOPE_USER, N_APPLY_NOW, "input.pointer-speed",
                "ポインターの速さ", "同じマウスの動きでポインターが進む量です。", -2, 2, 0),
    /* 電源 */
    INT_SETTING(N_DOMAIN_POWER, N_POWER_PERFORMANCE_PREFERENCE, N_SCOPE_MACHINE, N_APPLY_NOW, "power.performance",
                "CPU の性能の好み", "0 が性能、255 が省電力です。", 0, 255, 128),
    /* 網 */
    {.domain = N_DOMAIN_NETWORK, .key = N_NETWORK_DNS_SERVER, .type = N_SETTING_ENDPOINT, .scope = N_SCOPE_MACHINE,
     .apply = N_APPLY_NOW, .name = "network.dns-server", .label = "DNS のサーバー",
     .about = "既定の resolver です。0.0.0.0 は DHCP の言うものを使います。"},
    INT_SETTING(N_DOMAIN_NETWORK, N_NETWORK_TCP_RX_BYTES, N_SCOPE_MACHINE, N_APPLY_NOW, "network.tcp-receive",
                "TCP の受信の容量", "これから開く接続の受信の容量（バイト）です。", 4096, INT32_MAX, 1048576),
    INT_SETTING(N_DOMAIN_NETWORK, N_NETWORK_TCP_TX_BYTES, N_SCOPE_MACHINE, N_APPLY_NOW, "network.tcp-send",
                "TCP の送信の容量", "これから開く接続の送信の容量（バイト）です。", 4096, INT32_MAX, 1048576),
    /* 時刻 */
    INT_SETTING(N_DOMAIN_TIME, N_TIME_ZONE_MINUTES, N_SCOPE_USER, N_APPLY_NONE, "time.zone-minutes",
                "時間帯", "時計の時刻に足す分です。localtime がこの値を読みます。", -720, 840, 0),
};
#undef BOOL_SETTING
#undef INT_SETTING
