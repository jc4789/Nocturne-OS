/* Intel 8254x "e1000" network driver (QEMU's default NIC, also VirtualBox and VMware). */
#include "kernel.h"
#include "net.h"
#include "../arch/cpu.h"
#include "../dev/pci.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"

#define REG_CTRL   0x0000
#define REG_EERD   0x0014
#define REG_ICR    0x00C0
#define REG_IMS    0x00D0
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL    0x5400
#define REG_RAH    0x5404

#define NRX   32
#define NTX   16
#define BUFSZ 2048

struct rx_desc {
    uint64_t addr;
    uint16_t len, csum;
    uint8_t status, errors;
    uint16_t special;
} PACKED;

struct tx_desc {
    uint64_t addr;
    uint16_t len;
    uint8_t cso, cmd, status, css;
    uint16_t special;
} PACKED;

static volatile uint8_t *mmio;
static volatile struct rx_desc *rx;
static volatile struct tx_desc *tx;
static uint8_t *rxbuf[NRX], *txbuf[NTX];
static int rx_cur, tx_cur;
static bool new_eerd; /* 82574-style EERD layout */
static struct netif nif = {.driver = "e1000"};

static uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(mmio + r); }
static void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(mmio + r) = v; }

static uint16_t eeprom_read(uint8_t addr) {
    if (new_eerd) wr(REG_EERD, 1 | ((uint32_t)addr << 2));
    else wr(REG_EERD, 1 | ((uint32_t)addr << 8));
    uint32_t done = new_eerd ? 2 : 16;
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(REG_EERD);
        if (v & done) return (uint16_t)(v >> 16);
        pause();
    }
    return 0xFFFF;
}

static void e1000_irq(struct regs *r) {
    (void)r;
    uint32_t icr = rd(REG_ICR); /* reading acknowledges */
    if (!icr) return;
    while (rx[rx_cur].status & 1) {
        if ((rx[rx_cur].status & 2) && !rx[rx_cur].errors) net_rx(rxbuf[rx_cur], rx[rx_cur].len);
        rx[rx_cur].status = 0;
        wr(REG_RDT, rx_cur);
        rx_cur = (rx_cur + 1) % NRX;
    }
}

static int e1000_send(const void *frame, size_t len) {
    if (len > BUFSZ) return -EINVAL;
    volatile struct tx_desc *d = &tx[tx_cur];
    for (int i = 0; i < 1000000 && !(d->status & 1); i++) pause();
    if (!(d->status & 1)) return -EBUSY;
    memcpy(txbuf[tx_cur], frame, len);
    d->len = (uint16_t)len;
    d->cmd = 0x0B; /* EOP | IFCS | RS */
    d->status = 0;
    tx_cur = (tx_cur + 1) % NTX;
    wr(REG_TDT, tx_cur);
    return 0;
}

bool e1000_probe(void) {
    static const uint16_t ids[] = {0x100E, 0x100F, 0x1004, 0x100C, 0x1015, 0x10D3};
    struct pci_device *d = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(ids) && !d; i++) {
        d = pci_find(0x8086, ids[i]);
        if (d) new_eerd = ids[i] == 0x10D3;
    }
    if (!d || (d->bar[0] & 1)) return false;
    pci_enable_busmaster(d);
    uint64_t phys = d->bar[0] & ~0xFULL;
    if ((d->bar[0] & 6) == 4) phys |= (uint64_t)d->bar[1] << 32;
    mmio = vmm_map_mmio(phys, 0x20000);

    wr(REG_IMC, 0xFFFFFFFF);
    wr(REG_CTRL, rd(REG_CTRL) | (1u << 26)); /* device reset */
    net_delay_ms(10);
    for (int i = 0; i < 1000 && (rd(REG_CTRL) & (1u << 26)); i++) net_delay_ms(1);
    wr(REG_IMC, 0xFFFFFFFF);
    uint32_t ctrl = rd(REG_CTRL);
    ctrl |= (1u << 6) | (1u << 5);                  /* SLU, ASDE */
    ctrl &= ~((1u << 3) | (1u << 31) | (1u << 7)); /* LRST, PHY_RST, ILOS */
    wr(REG_CTRL, ctrl);

    uint32_t ral = rd(REG_RAL), rah = rd(REG_RAH);
    if ((rah & 0x80000000u) && (ral || (rah & 0xFFFF))) {
        for (int i = 0; i < 4; i++) nif.mac[i] = (uint8_t)(ral >> (i * 8));
        nif.mac[4] = (uint8_t)rah;
        nif.mac[5] = (uint8_t)(rah >> 8);
    } else {
        for (int i = 0; i < 3; i++) {
            uint16_t w = eeprom_read((uint8_t)i);
            nif.mac[i * 2] = (uint8_t)w;
            nif.mac[i * 2 + 1] = (uint8_t)(w >> 8);
        }
        wr(REG_RAL, nif.mac[0] | nif.mac[1] << 8 | nif.mac[2] << 16 | (uint32_t)nif.mac[3] << 24);
        wr(REG_RAH, nif.mac[4] | nif.mac[5] << 8 | 0x80000000u);
    }
    for (int i = 0; i < 128; i++) wr(REG_MTA + i * 4, 0);

    /* descriptor rings and buffers */
    uint64_t ring = pmm_alloc_zeroed();
    rx = phys_to_virt(ring);
    tx = (void *)((uint8_t *)phys_to_virt(ring) + 2048);
    for (int i = 0; i < NRX; i += 2) {
        uint64_t p = pmm_alloc();
        for (int k = 0; k < 2; k++) {
            rxbuf[i + k] = (uint8_t *)phys_to_virt(p) + k * BUFSZ;
            rx[i + k].addr = p + k * BUFSZ;
            rx[i + k].status = 0;
        }
    }
    for (int i = 0; i < NTX; i += 2) {
        uint64_t p = pmm_alloc();
        for (int k = 0; k < 2; k++) {
            txbuf[i + k] = (uint8_t *)phys_to_virt(p) + k * BUFSZ;
            tx[i + k].addr = p + k * BUFSZ;
            tx[i + k].status = 1; /* DD: free */
            tx[i + k].cmd = 0;
        }
    }
    wr(REG_RDBAL, (uint32_t)ring);
    wr(REG_RDBAH, (uint32_t)(ring >> 32));
    wr(REG_RDLEN, NRX * 16);
    wr(REG_RDH, 0);
    wr(REG_RDT, NRX - 1);
    wr(REG_RCTL, (1u << 1) | (1u << 15) | (1u << 26)); /* EN, BAM, SECRC; 2048-byte buffers */
    wr(REG_TDBAL, (uint32_t)(ring + 2048));
    wr(REG_TDBAH, (uint32_t)((ring + 2048) >> 32));
    wr(REG_TDLEN, NTX * 16);
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    wr(REG_TCTL, (1u << 1) | (1u << 3) | (15u << 4) | (64u << 12)); /* EN, PSP, CT, COLD */
    wr(REG_TIPG, 10 | (8 << 10) | (6 << 20));

    irq_register(d->irq, e1000_irq);
    pic_unmask(d->irq);
    wr(REG_IMS, (1u << 2) | (1u << 4) | (1u << 6) | (1u << 7)); /* LSC, RXDMT0, RXO, RXT0 */
    rd(REG_ICR);
    nif.send = e1000_send;
    kprintf("net: e1000 (%04x) irq %d mac %02x:%02x:%02x:%02x:%02x:%02x\n", d->device, d->irq, nif.mac[0],
            nif.mac[1], nif.mac[2], nif.mac[3], nif.mac[4], nif.mac[5]);
    net_register(&nif);
    return true;
}
