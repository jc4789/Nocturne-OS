/* Realtek RTL8125B 2.5 Gigabit Ethernet（PCI 10EC:8125）。AM5 と Intel 600・700 系の多くの板に載る口。MAC は mask
   ROM の code で走る：Realtek の driver が読み込む MAC と PHY の microcode の当て布は当てない。

   レジスターの番地、bit の値、初めの手順（out-of-band からの出、MAC の共通の設定、PCI Express の PHY の値、PHY の OCP
   の読み書き）と電源の手順（re_suspend、re_shutdown、re_resume）は Realtek の FreeBSD の driver rtl_bsd_drv_v1102.01
   に従う。ASPM は使わない（その driver の enable_aspm が 0 の道）。その driver は次の文を持つ： */
/*
 * Copyright (c) 1997, 1998
 *	Bill Paul <wpaul@ctr.columbia.edu>.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by Bill Paul.
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Bill Paul AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL Bill Paul OR THE VOICES IN HIS HEAD
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "kernel.h"
#include "net/net.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/spin.h"
#include "dev/entropy.h"
#include "hal/hal.h"
#include "mm/heap.h"
#include "sys/clock.h"
#include "sys/wait.h"

/* MAC のレジスター */
#define IDR0        0x0000
#define MAR0        0x0008
#define MAR4        0x000C
#define TX_RING     0x0020 /* 普通の優先の queue 0、64 bit */
#define INT_CFG0    0x0034
#define CMD         0x0037
#define TXCFG       0x0040
#define RXCFG       0x0044
#define EECMD       0x0050
#define CFG1        0x0052
#define CFG2        0x0053
#define CFG5        0x0056
#define TDFNR       0x0057
#define PHY_STATUS  0x006C
#define PMCH        0x006F
#define INT_CFG1    0x007A
#define EPHYAR      0x0080
#define TPPOLL      0x0090
#define MAC_OCP     0x00B0
#define PHY_OCP     0x00B8
#define TWI_CMD     0x00D2
#define MCU_CMD     0x00D3
#define RX_MAX_SIZE 0x00DA
#define CPLUS_CMD   0x00E0
#define INTR_MIT    0x00E2
#define RX_RING     0x00E4 /* queue 0、64 bit */
#define INTR_MOD    0x0A00
#define IMR_CLEAR   0x0D00
#define ISR         0x0D04
#define IMR_SET     0x0D0C
#define BACKUP_ADDR 0x19E0
#define EEE_TXIDLE  0x6048

#define CMD_TX_EN       0x04
#define CMD_RX_EN       0x08
#define CMD_RESET       0x10
#define CMD_STOP_REQ    0x80
#define EECMD_UNLOCK    0xC0 /* 構成のレジスターへ書ける */
#define MCU_NOW_IS_OOB  0x80
#define MCU_FIFOS_EMPTY 0x30
#define RX_ACCEPT       (0x02 | 0x04 | 0x08) /* 自分の番地、multicast、broadcast */
#define RX_FILTER       0x3Fu                /* 受ける frame の選び（re_clear_all_rx_packet_filter） */
#define TXCFG_INIT      0x03000780           /* frame の間の隙間、DMA の burst は限らない */
#define PMCH_PHY_WAKE   0x80                 /* PHY の電源（RL_FLAG_PHYWAKE_PM の機種） */

/* 8125B の割り込みの割り当て（割り込みの構成の bit 0） */
#define INT_RX0  (1u << 0)
#define INT_TX0  (1u << 16)
#define INT_LINK (1u << 21)
#define INT_MASK (INT_RX0 | INT_TX0 | INT_LINK)

#define LINK_UP  0x02
#define FULL_DUP 0x01

/* descriptor の最初の語：両方の輪 */
#define D_OWN  (1u << 31)
#define D_EOR  (1u << 30)
#define D_FS   (1u << 29)
#define D_LS   (1u << 28)
#define RX_RES (1u << 21) /* 受けた frame の誤りのまとめ */
#define RX_LEN 0x3FFFu
/* 受けた frame の checksum について MAC が言うこと（opts1。IP の版は opts2） */
#define RX_TCPT (1u << 17)
#define RX_UDPT (1u << 18)
#define RX_IPF  (1u << 16)
#define RX_UDPF (1u << 15)
#define RX_TCPF (1u << 14)
#define RX_V4F  (1u << 30)
/* 送る frame が MAC に埋めさせるもの（opts2、8125 の descriptor の形） */
#define TX_IPV4CS (1u << 29)
#define TX_TCPCS  (1u << 30)
#define TX_UDPCS  (1u << 31)

/* PHY（標準のレジスターは OCP の 0xA400 + 2 * 番号に写る、IEEE 802.3 22.2.4） */
#define MII_BMCR     0
#define MII_ANAR     4
#define MII_GBCR     9
#define BMCR_RESET   0x8000
#define BMCR_AUTONEG 0x1000
#define BMCR_PDOWN   0x0800
#define BMCR_RESTART 0x0200
#define ANAR_10      0x0020
#define ANAR_10_FD   0x0040
#define ANAR_100     0x0080
#define ANAR_100_FD  0x0100
#define ANAR_PAUSE   0x0400
#define ANAR_ASYM    0x0800
#define GBCR_1000_FD 0x0200
#define GBCR_1000_HD 0x0100
#define ADV_2500_FD  0x0080

#define BUFSZ     2048
#define FRAME_MAX 1522 /* 1500 + 頭 + VLAN の札 + FCS */
#define UDP_PAD   121 /* MAC に checksum を埋めさせない短い IPv4 UDP の frame はこの長さで出す（8125A/B） */
#define LINK_LOOK NS_PER_S /* link の割り込みのほかに、poll はこの間ごとに link の状態を読む */

struct desc {
    uint32_t opts1, opts2;
    uint64_t addr;
};
#define NRX (PAGE_SIZE / sizeof(struct desc))
#define NTX (PAGE_SIZE / sizeof(struct desc))
_Static_assert(sizeof(struct desc) == 16, "RTL8125 descriptor ABI");

/* 受信の輪と link の状態は網の thread（poll、rx_pending）のもの、送信の輪は transmit のもの。網の層は呼出しを net_lock
   の下で一つずつにする（net/net.h）。割り込みと poll はどの CPU でも走り、割り込みの mask と下の二つの印を共有する：
   rtl_lock（葉）が守り、net_rx_notify は放してから呼ぶ。
   電源の walk の間も網の層は呼ぶ。網の層の呼出しは path を持って asleep を見て、立っていれば装置に触れない。suspend は
   asleep を立ててから装置を止め、resume は装置を立て直してから下ろす。その間、装置に触れるのは電源の thread と割り込み
   だけである。錠の順：net_lock → path → rtl_lock。 */
struct rtl8125 {
    struct netif nif; /* 先頭：self が装置 */
    volatile uint8_t *mmio;
    volatile struct desc *rx, *tx;
    uint64_t rx_ring_bus, tx_ring_bus;
    uint8_t *rxbuf[NRX], *txbuf[NTX];
    unsigned rx_cur, tx_cur, tx_clean;
    struct mutex path;
    bool asleep; /* path の下 */
    struct spinlock rtl_lock;
    volatile bool rx_irq_pending, link_event;
    bool link;
    uint64_t link_checked; /* check_link が最後に PHY を読んだ時 */
    uint32_t macver;
};

static uint8_t rd8(struct rtl8125 *c, uint32_t r) { return mmio_read8(c->mmio + r); }
static uint16_t rd16(struct rtl8125 *c, uint32_t r) { return mmio_read16(c->mmio + r); }
static uint32_t rd32(struct rtl8125 *c, uint32_t r) { return mmio_read32(c->mmio + r); }
static void wr8(struct rtl8125 *c, uint32_t r, uint8_t v) { mmio_write8(c->mmio + r, v); }
static void wr16(struct rtl8125 *c, uint32_t r, uint16_t v) { mmio_write16(c->mmio + r, v); }
static void wr32(struct rtl8125 *c, uint32_t r, uint32_t v) { mmio_write32(c->mmio + r, v); }

/* size バイトのレジスター r の値と mask の積が want になるまで、step ごとに眠ってから読み（Realtek の手順と同じく、
   一歩の後に見る）、ns まで待つ。来たら true。last があれば最後に読んだ値を入れる。 */
static bool wait_reg(struct rtl8125 *c, uint32_t r, unsigned size, uint32_t mask, uint32_t want, uint64_t step,
                     uint64_t ns, uint32_t *last) {
    uint64_t until = clock_after(ns);
    for (;;) {
        sleep_until(clock_after(step));
        uint32_t v = size == 1 ? rd8(c, r) : size == 2 ? rd16(c, r) : rd32(c, r);
        if (last) *last = v;
        if ((v & mask) == want) return true;
        if (clock_now_ns() >= until) return false;
    }
}

/* OCP の窓の奥の MAC と PHY のレジスター。番地は byte で、一つが 16 bit の語 */
static uint16_t mac_ocp_read(struct rtl8125 *c, uint16_t a) {
    wr32(c, MAC_OCP, (uint32_t)(a / 2) << 16);
    return (uint16_t)rd32(c, MAC_OCP);
}

static void mac_ocp_write(struct rtl8125 *c, uint16_t a, uint16_t v) {
    wr32(c, MAC_OCP, 1u << 31 | (uint32_t)(a / 2) << 16 | v);
}

static void mac_ocp_modify(struct rtl8125 *c, uint16_t a, uint16_t clear, uint16_t set) {
    mac_ocp_write(c, a, (mac_ocp_read(c, a) & ~clear) | set);
}

/* 値は印（bit 31）を見せた読みのもの（re_real_ocp_phy_read）。 */
static uint16_t phy_read(struct rtl8125 *c, uint16_t a) {
    wr32(c, PHY_OCP, (uint32_t)(a / 2) << 16);
    uint32_t v = 0;
    wait_reg(c, PHY_OCP, 4, 1u << 31, 1u << 31, NS_PER_US, 100 * NS_PER_US, &v);
    return (uint16_t)v;
}

static void phy_write(struct rtl8125 *c, uint16_t a, uint16_t v) {
    wr32(c, PHY_OCP, 1u << 31 | (uint32_t)(a / 2) << 16 | v);
    wait_reg(c, PHY_OCP, 4, 1u << 31, 0, NS_PER_US, 100 * NS_PER_US, NULL);
}

static void phy_modify(struct rtl8125 *c, uint16_t a, uint16_t clear, uint16_t set) {
    phy_write(c, a, (phy_read(c, a) & ~clear) | set);
}
static uint16_t mii_read(struct rtl8125 *c, unsigned reg) { return phy_read(c, (uint16_t)(0xA400 + reg * 2)); }
static void mii_write(struct rtl8125 *c, unsigned reg, uint16_t v) { phy_write(c, (uint16_t)(0xA400 + reg * 2), v); }

/* PCI Express の PHY */
static void ephy_write(struct rtl8125 *c, uint8_t a, uint16_t v) {
    wr32(c, EPHYAR, 1u << 31 | (uint32_t)a << 16 | v);
    wait_reg(c, EPHYAR, 4, 1u << 31, 0, NS_PER_MS, 5 * NS_PER_MS, NULL);
}

static void cfg_unlock(struct rtl8125 *c) { wr8(c, EECMD, rd8(c, EECMD) | EECMD_UNLOCK); }
static void cfg_lock(struct rtl8125 *c) { wr8(c, EECMD, rd8(c, EECMD) & ~EECMD_UNLOCK); }

static unsigned phy_state(struct rtl8125 *c) { return phy_read(c, 0xA420) & 7; }

static void wait_phy_state(struct rtl8125 *c, unsigned state) {
    uint64_t until = clock_after(100 * NS_PER_MS);
    while (phy_state(c) != state && clock_now_ns() < until) sleep_until(clock_after(NS_PER_MS));
}

/* MAC を firmware の out-of-band（wake と管理）の型から driver の型へ。 */
static void exit_oob(struct rtl8125 *c) {
    cfg_lock(c);
    wr8(c, 0xF2, rd8(c, 0xF2) | 0x08);
    sleep_until(clock_after(2 * NS_PER_MS));
    if (rd8(c, CMD) & (CMD_TX_EN | CMD_RX_EN)) {
        sleep_until(clock_after(100 * NS_PER_US));
        wr8(c, CMD, rd8(c, CMD) & ~(CMD_TX_EN | CMD_RX_EN));
    }
    wr8(c, CMD, rd8(c, CMD) | CMD_STOP_REQ);
    if (!wait_reg(c, MCU_CMD, 1, MCU_FIFOS_EMPTY, MCU_FIFOS_EMPTY, 50 * NS_PER_US, 150 * NS_PER_MS, NULL))
        kprintf("rtl8125: FIFOs did not drain\n");
    wait_reg(c, INTR_MIT, 2, 0x103, 0x103, 50 * NS_PER_US, 150 * NS_PER_MS, NULL);
    mac_ocp_modify(c, 0xC0B4, 1, 0); /* MAC の MCU の frame の選びを reset する */
    mac_ocp_modify(c, 0xC0B4, 0, 1);
    wr8(c, MCU_CMD, rd8(c, MCU_CMD) & ~MCU_NOW_IS_OOB);
    mac_ocp_modify(c, 0xE8DE, 1u << 14, 0);
    wait_reg(c, TWI_CMD, 2, 1u << 9, 1u << 9, 100 * NS_PER_US, NS_PER_MS, NULL);
    mac_ocp_write(c, 0xC0AA, 0x07D0);
    mac_ocp_write(c, 0xC0A6, 0x01B5);
    mac_ocp_write(c, 0xC01E, 0x5555);
    wait_reg(c, TWI_CMD, 2, 1u << 9, 1u << 9, 100 * NS_PER_US, NS_PER_MS, NULL);
    if (mac_ocp_read(c, 0xD42C) & (1u << 8)) { /* PHY の節電（UPS）の状態から戻る */
        wait_phy_state(c, 2);
        mac_ocp_modify(c, 0xD42C, 1u << 8, 0);
        phy_modify(c, 0xA466, 1, 0);
        phy_modify(c, 0xA468, 0x0A, 0);
    }
}

/* 受けるのを止め、割り込みを切り、MAC を reset する（re_stop_txrx、re_reset）。reset は全部の DMA を終わらせる。reset
   が済んだら true。 */
static bool mac_reset(struct rtl8125 *c) {
    wr32(c, RXCFG, rd32(c, RXCFG) & ~RX_FILTER);
    wr32(c, IMR_CLEAR, 0xFFFFFFFF);
    wr32(c, ISR, 0xFFFFFFFF);
    sleep_until(clock_after(2 * NS_PER_MS + 200 * NS_PER_US));
    wr8(c, CMD, CMD_RESET);
    if (wait_reg(c, CMD, 1, CMD_RESET, 0, 10 * NS_PER_US, 10 * NS_PER_MS, NULL)) return true;
    kprintf("rtl8125: reset did not complete\n");
    return false;
}

static void aspm_clkreq_off(struct rtl8125 *c) {
    wr8(c, 0xF1, rd8(c, 0xF1) & ~0x80);
    wr8(c, CFG5, rd8(c, CFG5) & ~0x01);
    wr8(c, CFG2, rd8(c, CFG2) & ~0x80);
}

/* MAC と PHY を初めから立てる。firmware が渡したままでも、電源を失った後でも、走っていても同じ（exit_oob が送受信を
   止め、mac_reset が DMA を終わらせる）。 */
static void hw_init(struct rtl8125 *c) {
    exit_oob(c);
    mac_reset(c);
    mac_ocp_modify(c, 0xE080, 1u << 1, 0); /* EEE+ を切る */
    cfg_unlock(c);
    mac_ocp_modify(c, 0xC0B6, 1, 0); /* magic packet で起こさない */
    aspm_clkreq_off(c);
    wr8(c, CFG2, rd8(c, CFG2) | 0x20);
    cfg_lock(c);

    /* boot ROM が残したかもしれない MAC の microcode の当て布の breakpoint：MAC は ROM の code で走る */
    for (uint16_t a = 0xFC28; a <= 0xFC48; a += 2) mac_ocp_write(c, a, 0);
    sleep_until(clock_after(3 * NS_PER_MS));
    mac_ocp_write(c, 0xFC26, 0);

    /* PHY の電源を入れる */
    wr8(c, PMCH, rd8(c, PMCH) | 0xC0);
    mii_write(c, MII_BMCR, BMCR_AUTONEG);
    wait_phy_state(c, 3);

    /* PCI Express の PHY の値 */
    static const uint16_t ephy_b1[][2] = {
        {0x06, 0x001F}, {0x0A, 0xB66B}, {0x01, 0xA852}, {0x24, 0x0008}, {0x2F, 0x6052}, {0x0D, 0xF716}, {0x20, 0xD477},
        {0x21, 0x4477}, {0x22, 0x0013}, {0x23, 0xBB66}, {0x0B, 0xA909}, {0x29, 0xFF04}, {0x1B, 0x1EA0}, {0x46, 0x001F},
        {0x4A, 0xB66B}, {0x41, 0xA84A}, {0x64, 0x000C}, {0x6F, 0x604A}, {0x4D, 0xF716}, {0x60, 0xD477}, {0x61, 0x4477},
        {0x62, 0x0013}, {0x63, 0xBB66}, {0x4B, 0xA909}, {0x69, 0xFF04}, {0x5B, 0x1EA0},
    };
    static const uint16_t ephy_b2[][2] = {
        {0x0B, 0xA908}, {0x1E, 0x20EB}, {0x22, 0x0023}, {0x02, 0x60C2}, {0x29, 0xFF00},
        {0x4B, 0xA908}, {0x5E, 0x28EB}, {0x62, 0x0023}, {0x42, 0x60C2}, {0x69, 0xFF00},
    };
    if (c->macver == 0x64000000)
        for (size_t i = 0; i < ARRAY_SIZE(ephy_b1); i++) ephy_write(c, (uint8_t)ephy_b1[i][0], ephy_b1[i][1]);
    else
        for (size_t i = 0; i < ARRAY_SIZE(ephy_b2); i++) ephy_write(c, (uint8_t)ephy_b2[i][0], ephy_b2[i][1]);

    /* MAC の共通の設定 */
    cfg_unlock(c);
    aspm_clkreq_off(c);
    wr16(c, CPLUS_CMD, 0x2060);
    wr32(c, TXCFG, TXCFG_INIT);
    wr8(c, CFG2, rd8(c, CFG2) | 0x20);
    mac_ocp_write(c, 0xE098, 0xC302);
    wr16(c, 0x382, 0x221B);
    wr8(c, 0x4500, 0);
    wr16(c, 0x4800, 0);
    wr8(c, CFG1, rd8(c, CFG1) & ~0x10);
    wr8(c, 0xF2, rd8(c, 0xF2) & ~0x08);
    wr8(c, TDFNR, 0x10);
    mac_ocp_write(c, 0xC140, 0xFFFF);
    mac_ocp_write(c, 0xC142, 0xFFFF);
    mac_ocp_modify(c, 0xEB58, 1, 0); /* 16 byte の descriptor の形 */
    mac_ocp_modify(c, 0xE614, 0x0700, 2u << 8);
    mac_ocp_modify(c, 0xE63E, 0x0C00, 0);
    mac_ocp_modify(c, 0xE63E, 0x0030, 2u << 4);
    mac_ocp_modify(c, 0xC0B4, 0, 0x000C);
    mac_ocp_modify(c, 0xEB6A, 0x00FF, 0x0033);
    mac_ocp_modify(c, 0xEB50, 0x03E0, 0x0040);
    mac_ocp_modify(c, 0xE056, 0x00F0, 0);
    mac_ocp_modify(c, 0xE040, 1u << 12, 0);
    mac_ocp_modify(c, 0xEA1C, 0x0003, 0x0001);
    mac_ocp_write(c, 0xE0C0, 0x4400);
    mac_ocp_modify(c, 0xE052, 0x0088, 0x0060);
    mac_ocp_modify(c, 0xC0AC, 0, 0x1F80);
    mac_ocp_modify(c, 0xD430, 0x0FFF, 0x045F);
    wr8(c, 0xD0, rd8(c, 0xD0) | 0x40);
    wr8(c, 0xF2, rd8(c, 0xF2) | 0x40);
    wr8(c, 0xD0, rd8(c, 0xD0) | 0x80);
    mac_ocp_modify(c, 0xEA1C, 1u << 2, 0);
    mac_ocp_modify(c, 0xEB54, 0, 1);
    clock_spin(NS_PER_US); /* 二つの書込みの間の 1 us */
    mac_ocp_modify(c, 0xEB54, 1, 0);
    wr16(c, 0x1880, rd16(c, 0x1880) & ~0x30);
    for (uint32_t r = 0x0A00; r < 0x0A80; r += 4) wr32(c, r, 0);
    wr8(c, INT_CFG0, (rd8(c, INT_CFG0) & ~0x07) | 0x01); /* 8125B の割り込みの割り当て */
    wr16(c, INT_CFG1, 0);
    wr32(c, INTR_MOD, 0x00630063);
    cfg_lock(c);
}

static void start_autoneg(struct rtl8125 *c) {
    phy_modify(c, 0xA428, 1u << 9, 0); /* "giga lite" を使わない */
    phy_modify(c, 0xA5EA, 1, 0);
    phy_modify(c, 0xA5D4, 0, ADV_2500_FD);
    mii_write(c, MII_GBCR, (mii_read(c, MII_GBCR) & ~GBCR_1000_HD) | GBCR_1000_FD);
    mii_write(c, MII_ANAR,
              (mii_read(c, MII_ANAR) & ~0x0FE0u) | ANAR_10 | ANAR_10_FD | ANAR_100 | ANAR_100_FD | ANAR_PAUSE |
                  ANAR_ASYM);
    mii_write(c, MII_BMCR, BMCR_RESET | BMCR_AUTONEG | BMCR_RESTART);
}

/* 両方の輪を最初の descriptor から MAC に渡し、受信と送信を入れる。緩衝はもとの場所のまま。 */
static void rings_start(struct rtl8125 *c) {
    for (size_t i = 0; i < NRX; i++) {
        c->rx[i].opts2 = 0;
        c->rx[i].opts1 = D_OWN | (i == NRX - 1 ? D_EOR : 0) | BUFSZ;
    }
    for (size_t i = 0; i < NTX; i++) c->tx[i].opts1 = i == NTX - 1 ? D_EOR : 0;
    c->rx_cur = c->tx_cur = c->tx_clean = 0;
    c->rx_irq_pending = false;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    wr32(c, TX_RING, (uint32_t)c->tx_ring_bus);
    wr32(c, TX_RING + 4, (uint32_t)(c->tx_ring_bus >> 32));
    wr32(c, RX_RING, (uint32_t)c->rx_ring_bus);
    wr32(c, RX_RING + 4, (uint32_t)(c->rx_ring_bus >> 32));
    wr16(c, RX_MAX_SIZE, FRAME_MAX);
    wr16(c, EEE_TXIDLE, 1500 + 14 + 0x20);
    wr32(c, MAR0, 0xFFFFFFFF);
    wr32(c, MAR4, 0xFFFFFFFF);
    wr32(c, RXCFG, 0x40C00C00 | RX_ACCEPT);
    wr8(c, CMD, CMD_TX_EN | CMD_RX_EN);
}

/* hw_init の後、データの道を閉じて：局の番地、両方の輪、割り込み、自動交渉。attach と resume が通る。 */
static void rtl_run(struct rtl8125 *c) {
    cfg_unlock(c);
    wr32(c, IDR0, c->nif.mac[0] | c->nif.mac[1] << 8 | c->nif.mac[2] << 16 | (uint32_t)c->nif.mac[3] << 24);
    wr16(c, IDR0 + 4, (uint16_t)(c->nif.mac[4] | c->nif.mac[5] << 8));
    cfg_lock(c);
    rings_start(c);
    c->link = false; /* 自動交渉が済むと check_link がまた言う */
    wr32(c, ISR, 0xFFFFFFFF);
    wr32(c, IMR_SET, INT_MASK);
    start_autoneg(c);
}

/* データの道を閉じる：この後、網の層の呼出しは装置に触れない。 */
static void path_close(struct rtl8125 *c) {
    mutex_lock(&c->path);
    c->asleep = true;
    mutex_unlock(&c->path);
}

/* データの道を開き、網の thread に輪と、待たせた送信を見させる。 */
static void path_open(struct rtl8125 *c) {
    mutex_lock(&c->path);
    c->asleep = false;
    mutex_unlock(&c->path);
    net_rx_notify();
}

static void check_link(struct rtl8125 *c) {
    uint32_t s = rd32(c, PHY_STATUS);
    bool up = s != 0xFFFFFFFF && (s & LINK_UP);
    c->link_checked = clock_now_ns();
    if (up == c->link) return;
    c->link = up;
    if (!up) {
        kprintf("rtl8125: link down\n");
        return;
    }
    uint32_t cfg = rd32(c, TXCFG);
    if (s & FULL_DUP)
        cfg = (cfg | 1u << 24 | 1u << 25) & ~(1u << 19);
    else
        cfg = (cfg | 1u << 25) & ~(1u << 19 | 1u << 24);
    wr32(c, TXCFG, cfg);
    const char *speed = s & 0x400 ? "2500" : s & 0x10 ? "1000" : s & 0x08 ? "100" : s & 0x04 ? "10" : "?";
    kprintf("rtl8125: link up, %s Mbit/s %s duplex\n", speed, s & FULL_DUP ? "full" : "half");
}

static enum irq_result rtl_irq(void *context) {
    struct rtl8125 *c = context;
    spin_take(&c->rtl_lock);
    uint32_t isr = rd32(c, ISR);
    bool ours = isr && isr != 0xFFFFFFFF;
    if (ours) {
        /* 輪は網の thread が空ける：poll が入れ直すまで割り込みは切ったまま */
        wr32(c, IMR_CLEAR, 0xFFFFFFFF);
        wr32(c, ISR, isr);
        if (isr & INT_LINK) c->link_event = true;
        c->rx_irq_pending = true;
    }
    spin_give(&c->rtl_lock);
    if (ours) net_rx_notify();
    return ours ? IRQ_HANDLED : IRQ_NONE;
}

static bool rtl_rx_pending(struct netif *self, unsigned queue) {
    (void)queue;
    struct rtl8125 *c = (struct rtl8125 *)self;
    mutex_lock(&c->path);
    bool pending = !c->asleep && (c->rx_irq_pending || c->link_event || !(c->rx[c->rx_cur].opts1 & D_OWN));
    mutex_unlock(&c->path);
    return pending;
}

/* MAC が確かめたこと：IPv4 の frame の頭と TCP か UDP の checksum が正しければ NET_RX_CSUM_OK。 */
static unsigned rx_checks(uint32_t opts1, uint32_t opts2) {
    if (!(opts2 & RX_V4F) || (opts1 & RX_IPF)) return 0;
    if ((opts1 & RX_TCPT) && !(opts1 & RX_TCPF)) return NET_RX_CSUM_OK;
    if ((opts1 & RX_UDPT) && !(opts1 & RX_UDPF)) return NET_RX_CSUM_OK;
    return 0;
}

static size_t rtl_poll(struct netif *self, unsigned queue, size_t budget, net_input_fn input) {
    (void)queue;
    struct rtl8125 *c = (struct rtl8125 *)self;
    mutex_lock(&c->path);
    if (c->asleep) {
        mutex_unlock(&c->path);
        return 0;
    }
    uint64_t flags = spin_lock(&c->rtl_lock);
    bool event = c->link_event;
    c->link_event = false;
    spin_unlock(&c->rtl_lock, flags);
    if (event || clock_now_ns() >= c->link_checked + LINK_LOOK) check_link(c);
    (void)rd8(c, INT_CFG0); /* レジスターの読みは、NIC がその前に書いた descriptor を先に見せる */
    size_t consumed = 0;
    while (consumed < budget && !(c->rx[c->rx_cur].opts1 & D_OWN)) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint32_t o = c->rx[c->rx_cur].opts1, o2 = c->rx[c->rx_cur].opts2;
        unsigned len = (o & RX_LEN) >= 4 ? (o & RX_LEN) - 4 : 0; /* FCS を除く */
        if (!(o & RX_RES) && (o & D_FS) && (o & D_LS) && len >= 14 && len <= BUFSZ)
            input(self, c->rxbuf[c->rx_cur], len, rx_checks(o, o2));
        c->rx[c->rx_cur].opts2 = 0;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        c->rx[c->rx_cur].opts1 = D_OWN | (c->rx_cur == NRX - 1 ? D_EOR : 0) | BUFSZ;
        c->rx_cur = (c->rx_cur + 1) % NRX;
        consumed++;
    }
    flags = spin_lock(&c->rtl_lock);
    c->rx_irq_pending = false;
    wr32(c, IMR_SET, INT_MASK);
    /* 最後に見てから入れ直すまでに frame が着いたかもしれない：新しい割り込みを待たない */
    bool again = !(c->rx[c->rx_cur].opts1 & D_OWN);
    if (again) {
        wr32(c, IMR_CLEAR, 0xFFFFFFFF);
        c->rx_irq_pending = true;
    }
    spin_unlock(&c->rtl_lock, flags);
    mutex_unlock(&c->path);
    if (again) net_rx_notify();
    return consumed;
}

/* frame を一つ次の descriptor に入れ、MAC に渡す。MAC が送れるより長い frame は捨てる。 */
static void tx_post(struct rtl8125 *c, const struct net_tx_frame *frame) {
    uint8_t *b = c->txbuf[c->tx_cur];
    size_t len = 0;
    for (unsigned i = 0; i < frame->part_count; i++) {
        memcpy(b + len, frame->parts[i].data, frame->parts[i].length);
        len += frame->parts[i].length;
    }
    uint32_t opts2 = 0;
    if (frame->offload & NET_TX_IP_CSUM) opts2 |= TX_IPV4CS;
    if (frame->offload & NET_TX_TCP_CSUM)
        opts2 |= TX_TCPCS;
    else if (frame->offload & NET_TX_UDP_CSUM)
        opts2 |= TX_UDPCS;
    /* 8125A/B は短い IPv4 UDP の frame を詰め物なしでは壊す。MAC が自分で checksum を埋めるなら要らない（Realtek の
       driver の re_8125_pad） */
    size_t min = !(frame->offload & (NET_TX_TCP_CSUM | NET_TX_UDP_CSUM)) && len >= 24 && b[12] == 0x08 &&
                         b[13] == 0x00 && b[23] == 17
                     ? UDP_PAD
                     : 60;
    if (len < min) {
        memset(b + len, 0, min - len);
        len = min;
    }
    c->tx[c->tx_cur].opts2 = opts2;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    c->tx[c->tx_cur].opts1 = D_OWN | D_FS | D_LS | (c->tx_cur == NTX - 1 ? D_EOR : 0) | (uint32_t)len;
    c->tx_cur = (c->tx_cur + 1) % NTX;
}

/* 道が閉じている間は一つも取らない：網の層が持っておき、path_open の知らせで渡し直す（net/net.h）。 */
static unsigned rtl_transmit(struct netif *self, unsigned queue, const struct net_tx_frame *frames, unsigned count) {
    (void)queue;
    struct rtl8125 *c = (struct rtl8125 *)self;
    mutex_lock(&c->path);
    if (c->asleep) {
        mutex_unlock(&c->path);
        return 0;
    }
    while (c->tx_clean != c->tx_cur && !(c->tx[c->tx_clean].opts1 & D_OWN)) c->tx_clean = (c->tx_clean + 1) % NTX;
    unsigned taken = 0, posted = 0;
    while (taken < count) {
        if (frames[taken].length > FRAME_MAX - 4) { /* MAC が送れない：捨てる */
            taken++;
            continue;
        }
        if ((c->tx_cur + 1) % NTX == c->tx_clean) break; /* 場所が無い：残りは送り終わりを待つ */
        tx_post(c, &frames[taken++]);
        posted++;
    }
    if (posted) wr16(c, TPPOLL, 1); /* 普通の優先の queue 0：まとめて一度の呼び鈴 */
    mutex_unlock(&c->path);
    return taken;
}

/* ---- 電源 ---- */

/* D3 の前の MAC の設定（re_hw_d3_para の RTL8125B の分）：EEE+ を入れ（re_set_eeeplus）、MAC の側で LTR を切り
   （_re_set_enable_ltr）、L1 off を切る（re_set_enable_l1off）。ASPM を使わないので LTR の要求は出していない
   （re_issue_ltr_no_req は何もしない）。構成の空間（Device Control 2 の LTR の許し）は HAL のもので、ここでは書かない。 */
static void d3_prepare(struct rtl8125 *c) {
    mac_ocp_modify(c, 0xE080, 0, 1u << 1);
    mac_ocp_modify(c, 0xE032, (1u << 0) | (1u << 1), 0);
    mac_ocp_modify(c, 0xE094, 0xFF00, 0);
    mac_ocp_modify(c, 0xE092, 0x00FF, 0);
}

/* wake を使わない眠りと電源断の PHY（re_setwol、re_phy_power_down）：PHY を power down にし、PHY の wake の電源を
   切る。 */
static void phy_power_down(struct rtl8125 *c) {
    mii_write(c, MII_BMCR, BMCR_AUTONEG | BMCR_PDOWN);
    wr8(c, PMCH, rd8(c, PMCH) & ~PMCH_PHY_WAKE);
}

/* hal.h の suspend（re_suspend と re_shutdown）。道を閉じてから MAC を reset する：DMA と割り込みが止まる。眠りと電源断
   では装置が D3 に入るか電源を失うので、D3 の前の設定と PHY の電源断を続ける。休止の写しでは電源が残り、resume が
   初めから立て直す。reset が済まなければ false：眠りと休止の取りやめで resume が続き、立て直す。 */
static bool rtl_suspend(struct hal_device *d, enum hal_power_target target) {
    struct rtl8125 *c = d->driver_data;
    path_close(c);
    if (!mac_reset(c)) return false;
    switch (target) {
    case HAL_POWER_SLEEP:
    case HAL_POWER_OFF:
        d3_prepare(c);
        phy_power_down(c);
        break;
    case HAL_POWER_IMAGE:
        break;
    }
    return true;
}

/* hal.h の resume（re_resume）。装置は電源を失っていても、そのままでもよい：道を閉じて hw_init（re_hw_init）がどちら
   からも初めから立て、道を開く。局の番地と輪の場所は記憶にあり、電源を失っても失われない。MAC が reset を出て送受信が
   入ったら true。 */
static bool rtl_resume(struct hal_device *d) {
    struct rtl8125 *c = d->driver_data;
    path_close(c);
    hw_init(c);
    rtl_run(c);
    uint8_t cmd = rd8(c, CMD);
    bool back = !(cmd & CMD_RESET) && (cmd & (CMD_TX_EN | CMD_RX_EN)) == (CMD_TX_EN | CMD_RX_EN);
    path_open(c);
    if (!back) {
        kprintf("rtl8125: did not come back after sleeping\n");
        hal_failed(d, -N_ERR_IO);
    }
    return back;
}

static bool valid_mac(const uint8_t *m) { return !(m[0] & 1) && (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]); }

static int rtl_attach(struct hal_device *d) {
    struct rtl8125 *c = kzalloc(sizeof *c);
    if (!c) {
        kprintf("rtl8125: no memory for a device\n");
        return -N_ERR_NO_MEMORY;
    }
    d->driver_data = c;
    int r = -N_ERR_UNSUPPORTED;
    unsigned bar = 0;
    while (bar < 6 && !hal_resource(d, N_HAL_MEMORY, bar)) bar++;
    if (bar == 6) {
        kprintf("rtl8125: no memory register window\n");
        goto fail;
    }
    if (!(c->mmio = hal_map(d, bar))) {
        r = -N_ERR_NO_MEMORY;
        goto fail;
    }
    hal_enable(d, HAL_ENABLE_MEMORY | HAL_ENABLE_DMA);

    uint32_t txcfg = rd32(c, TXCFG);
    c->macver = txcfg & 0xFCF00000;
    if (txcfg == 0xFFFFFFFF || (c->macver != 0x64000000 && c->macver != 0x64100000)) {
        kprintf("rtl8125: chip version %08x not supported yet; only the RTL8125B is\n", txcfg);
        goto fail;
    }

    for (int i = 0; i < 4; i++) c->nif.mac[i] = (uint8_t)(rd32(c, BACKUP_ADDR) >> (i * 8));
    c->nif.mac[4] = (uint8_t)rd16(c, BACKUP_ADDR + 4);
    c->nif.mac[5] = (uint8_t)(rd16(c, BACKUP_ADDR + 4) >> 8);
    if (!valid_mac(c->nif.mac))
        for (int i = 0; i < 6; i++) c->nif.mac[i] = rd8(c, IDR0 + i);
    if (!valid_mac(c->nif.mac)) {
        entropy_get(c->nif.mac, 6);
        c->nif.mac[0] = (c->nif.mac[0] & 0xFE) | 0x02; /* 局所に決めた番地 */
        kprintf("rtl8125: no station address stored, using a random one\n");
    }

    hw_init(c);
    phy_write(c, 0xA436, 0x801E);
    uint16_t phy_code = phy_read(c, 0xA438);

    /* MAC は 64 bit の番地を読み書きする：輪のレジスターと descriptor の番地は 64 bit で、Realtek の driver は全部の DMA の
       tag を BUS_SPACE_MAXADDR で作る。輪、続いてそれが指す緩衝：それぞれ一つの塊で、緩衝は並べて */
    hal_dma_bits(d, 64);
    uint64_t ring_bus, buffer_bus;
    uint8_t *rings = hal_dma_alloc(d, 2 * PAGE_SIZE, &ring_bus);
    uint8_t *buffers = hal_dma_alloc(d, (NRX + NTX) * BUFSZ, &buffer_bus);
    if (!rings || !buffers) {
        kprintf("rtl8125: DMA allocation failed\n");
        r = -N_ERR_NO_MEMORY;
        goto fail;
    }
    c->rx = (volatile struct desc *)rings;
    c->tx = (volatile struct desc *)(rings + PAGE_SIZE);
    c->rx_ring_bus = ring_bus;
    c->tx_ring_bus = ring_bus + PAGE_SIZE;
    for (size_t i = 0; i < NRX; i++) {
        c->rxbuf[i] = buffers + i * BUFSZ;
        c->rx[i].addr = buffer_bus + i * BUFSZ;
    }
    for (size_t i = 0; i < NTX; i++) {
        c->txbuf[i] = buffers + (NRX + i) * BUFSZ;
        c->tx[i].addr = buffer_bus + (NRX + i) * BUFSZ;
    }

    /* 割り込む PCI Express の機能は MSI か MSI-X を持つ（PCIe 6.0、6.1.4）。chip の割り込みの mask は輪が立つまで
       切ったまま（mac_reset） */
    if (!hal_irq_enable(d, &(struct irq_spec){.name = "RTL8125", .handler = rtl_irq, .context = c}, 1, NULL)) {
        kprintf("rtl8125: no interrupt message could be enabled\n");
        r = -N_ERR_IO;
        goto fail;
    }

    /* 網の層は net_register から呼ぶ：輪が立つまで道を閉じておく */
    c->asleep = true;
    c->nif.driver = "rtl8125";
    c->nif.tx_queues = c->nif.rx_queues = 1;
    c->nif.offloads = NET_OFFLOAD_CSUM_TX | NET_OFFLOAD_CSUM_RX;
    c->nif.transmit = rtl_transmit;
    c->nif.rx_pending = rtl_rx_pending;
    c->nif.poll = rtl_poll;
    net_register(d, &c->nif);
    rtl_run(c);
    path_open(c);
    kprintf("net: rtl8125 (RTL8125B, %08x, PHY code %04x) mac %02x:%02x:%02x:%02x:%02x:%02x\n", txcfg, phy_code,
            c->nif.mac[0], c->nif.mac[1], c->nif.mac[2], c->nif.mac[3], c->nif.mac[4], c->nif.mac[5]);
    return 0;
fail:
    hal_irq_disable(d); /* handler の記憶が消える前に */
    d->driver_data = NULL;
    kfree(c);
    return r;
}

static const struct n_hal_rule rtl_rules[] = {N_PCI_ID(0x10EC, 0x8125)};
static const struct hal_driver rtl8125_driver = {
    .name = "rtl8125",
    .rules = rtl_rules,
    .rule_count = ARRAY_SIZE(rtl_rules),
    .attach = rtl_attach,
    .suspend = rtl_suspend,
    .resume = rtl_resume,
    .after_sleep = HAL_AFTER_SLEEP_RESUME,
};
HAL_DRIVER(rtl8125_driver);
