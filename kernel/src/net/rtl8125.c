/* Realtek RTL8125B 2.5 Gigabit Ethernet (PCI 10EC:8125), the onboard port of many AM5 and
 * Intel 600/700-series boards. The MAC runs on its mask-ROM code: the optional MAC and PHY
 * microcode patches the vendor driver loads are not applied.
 *
 * Register offsets, bit values and the initialization sequence (out-of-band exit, MAC common
 * setup, PCIe PHY parameters, PHY OCP access) follow Realtek's FreeBSD driver
 * rtl_bsd_drv_v1102.01, which carries this notice:
 *
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
#include "net.h"
#include "../arch/cpu.h"
#include "../arch/spin.h"
#include "../dev/pci.h"
#include "../dev/entropy.h"
#include "../dev/pm.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"

#define VEC_RTL 0x4C /* after the NVMe vectors 0x48..0x4B */

/* MAC registers */
#define IDR0        0x0000
#define MAR0        0x0008
#define MAR4        0x000C
#define TX_RING     0x0020 /* normal-priority queue 0, 64-bit */
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
#define RX_RING     0x00E4 /* queue 0, 64-bit */
#define INTR_MOD    0x0A00
#define IMR_CLEAR   0x0D00
#define ISR         0x0D04
#define IMR_SET     0x0D0C
#define BACKUP_ADDR 0x19E0
#define EEE_TXIDLE  0x6048

#define CMD_TX_EN    0x04
#define CMD_RX_EN    0x08
#define CMD_RESET    0x10
#define CMD_STOP_REQ 0x80
#define EECMD_UNLOCK 0xC0 /* write access to the configuration registers */
#define MCU_NOW_IS_OOB 0x80
#define MCU_FIFOS_EMPTY 0x30
#define RX_ACCEPT (0x02 | 0x04 | 0x08) /* own address, multicast, broadcast */
#define TXCFG_INIT 0x03000780 /* interframe gap, unlimited DMA burst */

/* The 8125B's interrupt mapping (interrupt configuration bit 0) */
#define INT_RX0  (1u << 0)
#define INT_TX0  (1u << 16)
#define INT_LINK (1u << 21)
#define INT_MASK (INT_RX0 | INT_TX0 | INT_LINK)

#define LINK_UP   0x02
#define FULL_DUP  0x01

/* Descriptors: first word, both rings */
#define D_OWN (1u << 31)
#define D_EOR (1u << 30)
#define D_FS  (1u << 29)
#define D_LS  (1u << 28)
#define RX_RES (1u << 21) /* receive error summary */
#define RX_LEN 0x3FFFu

/* PHY (standard registers are mapped at OCP 0xA400 + 2 * register) */
#define MII_BMCR 0
#define MII_ANAR 4
#define MII_GBCR 9
#define BMCR_RESET    0x8000
#define BMCR_AUTONEG  0x1000
#define BMCR_RESTART  0x0200
#define ANAR_10       0x0020
#define ANAR_10_FD    0x0040
#define ANAR_100      0x0080
#define ANAR_100_FD   0x0100
#define ANAR_PAUSE    0x0400
#define ANAR_ASYM     0x0800
#define GBCR_1000_FD  0x0200
#define GBCR_1000_HD  0x0100
#define ADV_2500_FD   0x0080

#define BUFSZ 2048
#define FRAME_MAX 1522 /* 1500 + header + VLAN tag + FCS */
#define UDP_PAD 121 /* short IPv4 UDP frames this length or shorter go out padded (8125A/B) */

struct desc {
    uint32_t opts1, opts2;
    uint64_t addr;
};
#define NRX (PAGE_SIZE / sizeof(struct desc))
#define NTX (PAGE_SIZE / sizeof(struct desc))
#define BUFS_PER_PAGE (PAGE_SIZE / BUFSZ)
_Static_assert(sizeof(struct desc) == 16, "RTL8125 descriptor ABI");

/* The receive ring and the link state belong to the net thread (poll_rx, rx_pending); the
   transmit ring to send, which the stack calls one at a time. The interrupt handler (CPU 0) and
   poll_rx, on any CPU, share the interrupt mask and the two latches below: rtl_lock, a leaf
   under the stack's net_lock, guards them; net_rx_notify comes after it is given back. A
   power change (quiesce, resume) runs alone: every other CPU halted, no other task of CPU 0's
   running in the middle. */
static volatile uint8_t *mmio;
static volatile struct desc *rx, *tx;
static uint8_t *rxbuf[NRX], *txbuf[NTX];
static unsigned rx_cur, tx_cur, tx_clean;
static struct spinlock rtl_lock;
static volatile bool rx_irq_pending, link_event;
static bool link;
static uint64_t link_checked;
static uint32_t macver;
static struct netif nif = {.driver = "rtl8125"};

static uint8_t rd8(uint32_t r) { return mmio_read8(mmio + r); }
static uint16_t rd16(uint32_t r) { return mmio_read16(mmio + r); }
static uint32_t rd32(uint32_t r) { return mmio_read32(mmio + r); }
static void wr8(uint32_t r, uint8_t v) { mmio_write8(mmio + r, v); }
static void wr16(uint32_t r, uint16_t v) { mmio_write16(mmio + r, v); }
static void wr32(uint32_t r, uint32_t v) { mmio_write32(mmio + r, v); }

static void udelay(unsigned us) {
    if (tsc_hz >= 1000000) {
        uint64_t start = rdtsc(), span = tsc_hz / 1000000 * us;
        while (rdtsc() - start < span) pause();
    } else {
        while (us--) io_wait();
    }
}

/* MAC and PHY registers behind the OCP windows, addressed in bytes, one 16-bit word each */
static uint16_t mac_ocp_read(uint16_t a) {
    wr32(MAC_OCP, (uint32_t)(a / 2) << 16);
    return (uint16_t)rd32(MAC_OCP);
}

static void mac_ocp_write(uint16_t a, uint16_t v) { wr32(MAC_OCP, 1u << 31 | (uint32_t)(a / 2) << 16 | v); }

static void mac_ocp_modify(uint16_t a, uint16_t clear, uint16_t set) {
    mac_ocp_write(a, (mac_ocp_read(a) & ~clear) | set);
}

static uint16_t phy_read(uint16_t a) {
    wr32(PHY_OCP, (uint32_t)(a / 2) << 16);
    uint32_t v = 0;
    for (int i = 0; i < 100; i++) {
        udelay(1);
        if ((v = rd32(PHY_OCP)) & (1u << 31)) break;
    }
    return (uint16_t)v;
}

static void phy_write(uint16_t a, uint16_t v) {
    wr32(PHY_OCP, 1u << 31 | (uint32_t)(a / 2) << 16 | v);
    for (int i = 0; i < 100; i++) {
        udelay(1);
        if (!(rd32(PHY_OCP) & (1u << 31))) break;
    }
}

static void phy_modify(uint16_t a, uint16_t clear, uint16_t set) { phy_write(a, (phy_read(a) & ~clear) | set); }
static uint16_t mii_read(unsigned reg) { return phy_read((uint16_t)(0xA400 + reg * 2)); }
static void mii_write(unsigned reg, uint16_t v) { phy_write((uint16_t)(0xA400 + reg * 2), v); }

/* PCI Express PHY */
static void ephy_write(uint8_t a, uint16_t v) {
    wr32(EPHYAR, 1u << 31 | (uint32_t)a << 16 | v);
    for (int i = 0; i < 5; i++) {
        net_delay_ms(1);
        if (!(rd32(EPHYAR) & (1u << 31))) break;
    }
}

static void cfg_unlock(void) { wr8(EECMD, rd8(EECMD) | EECMD_UNLOCK); }
static void cfg_lock(void) { wr8(EECMD, rd8(EECMD) & ~EECMD_UNLOCK); }

static bool wait8(uint32_t r, uint8_t mask, uint8_t want, unsigned tries, unsigned us) {
    for (unsigned i = 0; i < tries; i++) {
        udelay(us);
        if ((rd8(r) & mask) == want) return true;
    }
    return false;
}

static unsigned phy_state(void) { return phy_read(0xA420) & 7; }

static void wait_phy_state(unsigned state) {
    for (int i = 0; i < 100 && phy_state() != state; i++) net_delay_ms(1);
}

/* Take the MAC from the firmware's out-of-band (wake/management) mode to driver mode. */
static void exit_oob(void) {
    cfg_lock();
    wr8(0xF2, rd8(0xF2) | 0x08);
    net_delay_ms(2);
    if (rd8(CMD) & (CMD_TX_EN | CMD_RX_EN)) {
        udelay(100);
        wr8(CMD, rd8(CMD) & ~(CMD_TX_EN | CMD_RX_EN));
    }
    wr8(CMD, rd8(CMD) | CMD_STOP_REQ);
    if (!wait8(MCU_CMD, MCU_FIFOS_EMPTY, MCU_FIFOS_EMPTY, 3000, 50)) kprintf("rtl8125: FIFOs did not drain\n");
    for (int i = 0; i < 3000; i++) {
        udelay(50);
        if ((rd16(INTR_MIT) & 0x103) == 0x103) break;
    }
    mac_ocp_modify(0xC0B4, 1, 0); /* reset the MAC MCU packet filter */
    mac_ocp_modify(0xC0B4, 0, 1);
    wr8(MCU_CMD, rd8(MCU_CMD) & ~MCU_NOW_IS_OOB);
    mac_ocp_modify(0xE8DE, 1u << 14, 0);
    for (int i = 0; i < 10 && !(rd16(TWI_CMD) & (1u << 9)); i++) udelay(100);
    mac_ocp_write(0xC0AA, 0x07D0);
    mac_ocp_write(0xC0A6, 0x01B5);
    mac_ocp_write(0xC01E, 0x5555);
    for (int i = 0; i < 10 && !(rd16(TWI_CMD) & (1u << 9)); i++) udelay(100);
    if (mac_ocp_read(0xD42C) & (1u << 8)) { /* resuming from the PHY's power-saving (UPS) state */
        wait_phy_state(2);
        mac_ocp_modify(0xD42C, 1u << 8, 0);
        phy_modify(0xA466, 1, 0);
        phy_modify(0xA468, 0x0A, 0);
    }
}

static void mac_reset(void) {
    wr32(RXCFG, rd32(RXCFG) & ~0x3Fu); /* accept nothing while the reset runs */
    wr32(IMR_CLEAR, 0xFFFFFFFF);
    wr32(ISR, 0xFFFFFFFF);
    net_delay_ms(2);
    udelay(200);
    wr8(CMD, CMD_RESET);
    if (!wait8(CMD, CMD_RESET, 0, 1000, 10)) kprintf("rtl8125: reset did not complete\n");
}

/* Power change (dev/pm.h): interrupts off, then the reset ends every DMA. Polled waits only. */
static bool rtl_quiesce(void *arg) {
    (void)arg;
    wr32(IMR_CLEAR, 0xFFFFFFFF);
    wr32(RXCFG, rd32(RXCFG) & ~0x3Fu);
    wr8(CMD, rd8(CMD) & ~(CMD_TX_EN | CMD_RX_EN));
    udelay(100);
    wr8(CMD, CMD_RESET);
    return wait8(CMD, CMD_RESET, 0, 1000, 10);
}

static void aspm_clkreq_off(void) {
    wr8(0xF1, rd8(0xF1) & ~0x80);
    wr8(CFG5, rd8(CFG5) & ~0x01);
    wr8(CFG2, rd8(CFG2) & ~0x80);
}

static void hw_init(void) {
    exit_oob();
    mac_reset();
    mac_ocp_modify(0xE080, 1u << 1, 0); /* EEE+ off */
    cfg_unlock();
    mac_ocp_modify(0xC0B6, 1, 0); /* no magic-packet wake */
    aspm_clkreq_off();
    wr8(CFG2, rd8(CFG2) | 0x20);
    cfg_lock();

    /* Breakpoints of a MAC microcode patch a boot ROM may have left: the MAC runs its ROM code. */
    for (uint16_t a = 0xFC28; a <= 0xFC48; a += 2) mac_ocp_write(a, 0);
    net_delay_ms(3);
    mac_ocp_write(0xFC26, 0);

    /* PHY power up */
    wr8(PMCH, rd8(PMCH) | 0xC0);
    mii_write(MII_BMCR, BMCR_AUTONEG);
    wait_phy_state(3);

    /* PCI Express PHY parameters */
    static const uint16_t ephy_b1[][2] = {
        {0x06, 0x001F}, {0x0A, 0xB66B}, {0x01, 0xA852}, {0x24, 0x0008}, {0x2F, 0x6052}, {0x0D, 0xF716},
        {0x20, 0xD477}, {0x21, 0x4477}, {0x22, 0x0013}, {0x23, 0xBB66}, {0x0B, 0xA909}, {0x29, 0xFF04},
        {0x1B, 0x1EA0}, {0x46, 0x001F}, {0x4A, 0xB66B}, {0x41, 0xA84A}, {0x64, 0x000C}, {0x6F, 0x604A},
        {0x4D, 0xF716}, {0x60, 0xD477}, {0x61, 0x4477}, {0x62, 0x0013}, {0x63, 0xBB66}, {0x4B, 0xA909},
        {0x69, 0xFF04}, {0x5B, 0x1EA0},
    };
    static const uint16_t ephy_b2[][2] = {
        {0x0B, 0xA908}, {0x1E, 0x20EB}, {0x22, 0x0023}, {0x02, 0x60C2}, {0x29, 0xFF00},
        {0x4B, 0xA908}, {0x5E, 0x28EB}, {0x62, 0x0023}, {0x42, 0x60C2}, {0x69, 0xFF00},
    };
    if (macver == 0x64000000)
        for (size_t i = 0; i < ARRAY_SIZE(ephy_b1); i++) ephy_write((uint8_t)ephy_b1[i][0], ephy_b1[i][1]);
    else
        for (size_t i = 0; i < ARRAY_SIZE(ephy_b2); i++) ephy_write((uint8_t)ephy_b2[i][0], ephy_b2[i][1]);

    /* MAC common setup */
    cfg_unlock();
    aspm_clkreq_off();
    wr16(CPLUS_CMD, 0x2060);
    wr32(TXCFG, TXCFG_INIT);
    wr8(CFG2, rd8(CFG2) | 0x20);
    mac_ocp_write(0xE098, 0xC302);
    wr16(0x382, 0x221B);
    wr8(0x4500, 0);
    wr16(0x4800, 0);
    wr8(CFG1, rd8(CFG1) & ~0x10);
    wr8(0xF2, rd8(0xF2) & ~0x08);
    wr8(TDFNR, 0x10);
    mac_ocp_write(0xC140, 0xFFFF);
    mac_ocp_write(0xC142, 0xFFFF);
    mac_ocp_modify(0xEB58, 1, 0); /* the 16-byte descriptor format */
    mac_ocp_modify(0xE614, 0x0700, 2u << 8);
    mac_ocp_modify(0xE63E, 0x0C00, 0);
    mac_ocp_modify(0xE63E, 0x0030, 2u << 4);
    mac_ocp_modify(0xC0B4, 0, 0x000C);
    mac_ocp_modify(0xEB6A, 0x00FF, 0x0033);
    mac_ocp_modify(0xEB50, 0x03E0, 0x0040);
    mac_ocp_modify(0xE056, 0x00F0, 0);
    mac_ocp_modify(0xE040, 1u << 12, 0);
    mac_ocp_modify(0xEA1C, 0x0003, 0x0001);
    mac_ocp_write(0xE0C0, 0x4400);
    mac_ocp_modify(0xE052, 0x0088, 0x0060);
    mac_ocp_modify(0xC0AC, 0, 0x1F80);
    mac_ocp_modify(0xD430, 0x0FFF, 0x045F);
    wr8(0xD0, rd8(0xD0) | 0x40);
    wr8(0xF2, rd8(0xF2) | 0x40);
    wr8(0xD0, rd8(0xD0) | 0x80);
    mac_ocp_modify(0xEA1C, 1u << 2, 0);
    mac_ocp_modify(0xEB54, 0, 1);
    udelay(1);
    mac_ocp_modify(0xEB54, 1, 0);
    wr16(0x1880, rd16(0x1880) & ~0x30);
    for (uint32_t r = 0x0A00; r < 0x0A80; r += 4) wr32(r, 0);
    wr8(INT_CFG0, (rd8(INT_CFG0) & ~0x07) | 0x01); /* the 8125B interrupt mapping */
    wr16(INT_CFG1, 0);
    wr32(INTR_MOD, 0x00630063);
    cfg_lock();
}

static void start_autoneg(void) {
    phy_modify(0xA428, 1u << 9, 0); /* no "giga lite" */
    phy_modify(0xA5EA, 1, 0);
    phy_modify(0xA5D4, 0, ADV_2500_FD);
    mii_write(MII_GBCR, (mii_read(MII_GBCR) & ~GBCR_1000_HD) | GBCR_1000_FD);
    mii_write(MII_ANAR, (mii_read(MII_ANAR) & ~0x0FE0u) | ANAR_10 | ANAR_10_FD | ANAR_100 | ANAR_100_FD |
                            ANAR_PAUSE | ANAR_ASYM);
    mii_write(MII_BMCR, BMCR_RESET | BMCR_AUTONEG | BMCR_RESTART);
}

/* Both rings from their first descriptor, handed to the MAC, then receive and transmit on: at
   probe and after a sleep, which reset the MAC (the buffers stay where they were). */
static uint64_t rx_ring_phys, tx_ring_phys;

static void rings_start(void) {
    for (size_t i = 0; i < NRX; i++) {
        rx[i].opts2 = 0;
        rx[i].opts1 = D_OWN | (i == NRX - 1 ? D_EOR : 0) | BUFSZ;
    }
    for (size_t i = 0; i < NTX; i++) tx[i].opts1 = i == NTX - 1 ? D_EOR : 0;
    rx_cur = tx_cur = tx_clean = 0;
    rx_irq_pending = false;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    wr32(TX_RING, (uint32_t)tx_ring_phys);
    wr32(TX_RING + 4, (uint32_t)(tx_ring_phys >> 32));
    wr32(RX_RING, (uint32_t)rx_ring_phys);
    wr32(RX_RING + 4, (uint32_t)(rx_ring_phys >> 32));
    wr16(RX_MAX_SIZE, FRAME_MAX);
    wr16(EEE_TXIDLE, 1500 + 14 + 0x20);
    wr32(MAR0, 0xFFFFFFFF);
    wr32(MAR4, 0xFFFFFFFF);
    wr32(RXCFG, 0x40C00C00 | RX_ACCEPT);
    wr8(CMD, CMD_TX_EN | CMD_RX_EN);
}

static bool rtl_resume(void *arg) {
    (void)arg;
    hw_init();
    cfg_unlock();
    wr32(IDR0, nif.mac[0] | nif.mac[1] << 8 | nif.mac[2] << 16 | (uint32_t)nif.mac[3] << 24);
    wr16(IDR0 + 4, (uint16_t)(nif.mac[4] | nif.mac[5] << 8));
    cfg_lock();
    rings_start();
    uint8_t cmd = rd8(CMD); /* out of reset, both engines on (rings_start's last write) */
    bool back = !(cmd & CMD_RESET) && (cmd & (CMD_TX_EN | CMD_RX_EN)) == (CMD_TX_EN | CMD_RX_EN);
    link = false; /* check_link reports it again once autonegotiation is done */
    wr32(ISR, 0xFFFFFFFF);
    wr32(IMR_SET, INT_MASK);
    start_autoneg();
    net_rx_notify();
    return back;
}

static struct pm_device rtl_pm = {.name = "rtl8125", .stage = PM_STAGE_IO, .quiesce = rtl_quiesce,
                                  .resume = rtl_resume};

static void check_link(void) {
    uint32_t s = rd32(PHY_STATUS);
    bool up = s != 0xFFFFFFFF && (s & LINK_UP);
    link_checked = uptime_ms();
    if (up == link) return;
    link = up;
    if (!up) {
        kprintf("rtl8125: link down\n");
        return;
    }
    uint32_t cfg = rd32(TXCFG);
    if (s & FULL_DUP) cfg = (cfg | 1u << 24 | 1u << 25) & ~(1u << 19);
    else cfg = (cfg | 1u << 25) & ~(1u << 19 | 1u << 24);
    wr32(TXCFG, cfg);
    const char *speed = s & 0x400 ? "2500" : s & 0x10 ? "1000" : s & 0x08 ? "100" : s & 0x04 ? "10" : "?";
    kprintf("rtl8125: link up, %s Mbit/s %s duplex\n", speed, s & FULL_DUP ? "full" : "half");
}

static void rtl_irq(struct regs *r) {
    (void)r;
    spin_take(&rtl_lock);
    uint32_t isr = rd32(ISR);
    bool ours = isr && isr != 0xFFFFFFFF;
    if (ours) {
        /* The net thread drains the rings: interrupts stay off until poll_rx rearms them. */
        wr32(IMR_CLEAR, 0xFFFFFFFF);
        wr32(ISR, isr);
        if (isr & INT_LINK) link_event = true;
        rx_irq_pending = true;
    }
    spin_give(&rtl_lock);
    if (ours) net_rx_notify();
}

static bool rtl_rx_pending(void) {
    return rx_irq_pending || link_event || !(rx[rx_cur].opts1 & D_OWN);
}

static size_t rtl_poll_rx(void (*input)(const void *, size_t)) {
    uint64_t flags = spin_lock(&rtl_lock);
    bool event = link_event;
    link_event = false;
    spin_unlock(&rtl_lock, flags);
    if (event || uptime_ms() - link_checked >= 1000) check_link();
    (void)rd8(INT_CFG0); /* a register read orders the descriptor writes the NIC posted before it */
    size_t consumed = 0;
    while (consumed < NRX && !(rx[rx_cur].opts1 & D_OWN)) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint32_t o = rx[rx_cur].opts1;
        unsigned len = (o & RX_LEN) >= 4 ? (o & RX_LEN) - 4 : 0; /* without the FCS */
        if (!(o & RX_RES) && (o & D_FS) && (o & D_LS) && len >= 14 && len <= BUFSZ) input(rxbuf[rx_cur], len);
        rx[rx_cur].opts2 = 0;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        rx[rx_cur].opts1 = D_OWN | (rx_cur == NRX - 1 ? D_EOR : 0) | BUFSZ;
        rx_cur = (rx_cur + 1) % NRX;
        consumed++;
    }
    flags = spin_lock(&rtl_lock);
    rx_irq_pending = false;
    wr32(IMR_SET, INT_MASK);
    /* A frame may have landed between the last check and rearming: never wait for a new edge. */
    bool again = !(rx[rx_cur].opts1 & D_OWN);
    if (again) {
        wr32(IMR_CLEAR, 0xFFFFFFFF);
        rx_irq_pending = true;
    }
    spin_unlock(&rtl_lock, flags);
    if (again) net_rx_notify();
    return consumed;
}

static int rtl_send(const void *frame, size_t len) {
    if (len > FRAME_MAX - 4) return -N_ERR_INVALID;
    while (tx_clean != tx_cur && !(tx[tx_clean].opts1 & D_OWN)) tx_clean = (tx_clean + 1) % NTX;
    unsigned next = (tx_cur + 1) % NTX;
    if (next == tx_clean) return -N_ERR_NOT_READY; /* local backpressure */
    uint8_t *b = txbuf[tx_cur];
    memcpy(b, frame, len);
    const uint8_t *f = frame;
    /* The 8125A/B corrupt short IPv4 UDP frames unless they are padded (vendor erratum). */
    size_t min = len >= 24 && f[12] == 0x08 && f[13] == 0x00 && f[23] == 17 ? UDP_PAD : 60;
    if (len < min) {
        memset(b + len, 0, min - len);
        len = min;
    }
    tx[tx_cur].opts2 = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    tx[tx_cur].opts1 = D_OWN | D_FS | D_LS | (tx_cur == NTX - 1 ? D_EOR : 0) | (uint32_t)len;
    tx_cur = next;
    wr16(TPPOLL, 1); /* normal-priority queue 0 */
    return 0;
}

static bool valid_mac(const uint8_t *m) {
    return !(m[0] & 1) && (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]);
}

static void free_dma(uint64_t rx_ring, uint64_t tx_ring) {
    for (size_t i = 0; i < NRX; i += BUFS_PER_PAGE)
        if (rxbuf[i]) pmm_free(hhdm_virt_to_phys(rxbuf[i]));
    for (size_t i = 0; i < NTX; i += BUFS_PER_PAGE)
        if (txbuf[i]) pmm_free(hhdm_virt_to_phys(txbuf[i]));
    if (rx_ring) pmm_free(rx_ring);
    if (tx_ring) pmm_free(tx_ring);
    memset(rxbuf, 0, sizeof rxbuf);
    memset(txbuf, 0, sizeof txbuf);
    rx = tx = NULL;
}

bool rtl8125_probe(void) {
    struct pci_device *d = pci_find(0x10EC, 0x8125);
    if (!d) return false;
    uint64_t base = 0, size = 0;
    for (unsigned i = 0; i < 6 && !base; i++)
        if ((base = pci_bar_address(d, i))) size = pci_bar_size(d, i);
    if (!base) {
        kprintf("rtl8125: no memory register window\n");
        return false;
    }
    uint8_t pm = pci_find_cap(d, 0x01);
    if (pm) { /* the firmware may have left the function in a sleep state */
        uint32_t pmcsr = pci_read32(d->bus, d->dev, d->func, pm + 4);
        if (pmcsr & 3) {
            pci_write32(d->bus, d->dev, d->func, pm + 4, pmcsr & ~3u);
            net_delay_ms(10);
        }
    }
    if (!(mmio = vmm_map_mmio(base, size && size < 0x10000 ? size : 0x10000))) return false;
    pci_enable_busmaster(d);

    uint32_t txcfg = rd32(TXCFG);
    macver = txcfg & 0xFCF00000;
    if (txcfg == 0xFFFFFFFF || (macver != 0x64000000 && macver != 0x64100000)) {
        kprintf("rtl8125: chip version %08x (rev %02x) not supported yet; only the RTL8125B is\n", txcfg,
                pci_read32(d->bus, d->dev, d->func, 8) & 0xFF);
        return false;
    }

    for (int i = 0; i < 4; i++) nif.mac[i] = (uint8_t)(rd32(BACKUP_ADDR) >> (i * 8));
    nif.mac[4] = (uint8_t)rd16(BACKUP_ADDR + 4);
    nif.mac[5] = (uint8_t)(rd16(BACKUP_ADDR + 4) >> 8);
    if (!valid_mac(nif.mac))
        for (int i = 0; i < 6; i++) nif.mac[i] = rd8(IDR0 + i);
    if (!valid_mac(nif.mac)) {
        entropy_get(nif.mac, 6);
        nif.mac[0] = (nif.mac[0] & 0xFE) | 0x02; /* locally administered */
        kprintf("rtl8125: no station address stored, using a random one\n");
    }

    hw_init();
    phy_write(0xA436, 0x801E);
    uint16_t phy_code = phy_read(0xA438);

    cfg_unlock();
    wr32(IDR0, nif.mac[0] | nif.mac[1] << 8 | nif.mac[2] << 16 | (uint32_t)nif.mac[3] << 24);
    wr16(IDR0 + 4, (uint16_t)(nif.mac[4] | nif.mac[5] << 8));
    cfg_lock();

    uint64_t rx_ring = pmm_alloc_zeroed(), tx_ring = pmm_alloc_zeroed();
    if (!rx_ring || !tx_ring) goto no_dma;
    rx = phys_to_virt(rx_ring);
    tx = phys_to_virt(tx_ring);
    for (size_t i = 0; i < NRX; i += BUFS_PER_PAGE) {
        uint64_t p = pmm_alloc();
        if (!p) goto no_dma;
        for (size_t k = 0; k < BUFS_PER_PAGE; k++) {
            rxbuf[i + k] = (uint8_t *)phys_to_virt(p) + k * BUFSZ;
            rx[i + k].addr = p + k * BUFSZ;
        }
    }
    for (size_t i = 0; i < NTX; i += BUFS_PER_PAGE) {
        uint64_t p = pmm_alloc();
        if (!p) goto no_dma;
        for (size_t k = 0; k < BUFS_PER_PAGE; k++) {
            txbuf[i + k] = (uint8_t *)phys_to_virt(p) + k * BUFSZ;
            tx[i + k].addr = p + k * BUFSZ;
        }
    }
    rx_ring_phys = rx_ring;
    tx_ring_phys = tx_ring;
    rings_start();

    nif.send = rtl_send;
    nif.rx_pending = rtl_rx_pending;
    nif.poll_rx = rtl_poll_rx;
    vector_register(VEC_RTL, rtl_irq);
    bool msi = pci_msi_enable(d, VEC_RTL);
    if (!msi) {
        irq_register(d->irq, rtl_irq);
        pic_unmask(d->irq);
    }
    net_register(&nif);
    pm_register(&rtl_pm);
    wr32(ISR, 0xFFFFFFFF);
    wr32(IMR_SET, INT_MASK);
    start_autoneg();
    kprintf("net: rtl8125 (RTL8125B, %08x, PHY code %04x) %s mac %02x:%02x:%02x:%02x:%02x:%02x\n", txcfg, phy_code,
            msi ? "MSI" : "INTx", nif.mac[0], nif.mac[1], nif.mac[2], nif.mac[3], nif.mac[4], nif.mac[5]);
    return true;
no_dma:
    free_dma(rx_ring, tx_ring);
    kprintf("rtl8125: DMA allocation failed\n");
    return false;
}
