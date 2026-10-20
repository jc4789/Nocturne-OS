/* Intel 8254x "e1000" network driver (QEMU's default NIC, also VirtualBox and VMware). */
#include "kernel.h"
#include "net.h"
#include "../arch/cpu.h"
#include "../dev/pci.h"
#include "../dev/pm.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"

#define REG_CTRL   0x0000
#define REG_EERD   0x0014
#define REG_ICR    0x00C0
#define REG_ITR    0x00C4
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
#define REG_RDTR   0x2820
#define REG_RADV   0x282C
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL    0x5400
#define REG_RAH    0x5404

#define BUFSZ 2048
#define ICR_TXDW   (1u << 0)
#define ICR_LSC    (1u << 2)
#define ICR_RXDMT0 (1u << 4)
#define ICR_RXO    (1u << 6)
#define ICR_RXT0   (1u << 7)
#define RX_IRQS (ICR_RXDMT0 | ICR_RXO | ICR_RXT0)
#define DESC_DD  1u
#define RX_EOP   2u
#define ITR_TICK_NS 256u /* 8254x ITR, manual section 13.4.18 */
#define ITR_DEFAULT_TICKS 651u /* low end of Intel's suggested 651..5580 range */

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

/* Each descriptor ring owns one physical page: 256 legacy descriptors. This
   satisfies the hardware's 16-byte base and 128-byte ring-length alignment;
   packet buffering is backed by these DMA descriptors, not another RX queue. */
#define NRX (PAGE_SIZE / sizeof(struct rx_desc))
#define NTX (PAGE_SIZE / sizeof(struct tx_desc))
#define BUFS_PER_PAGE (PAGE_SIZE / BUFSZ)
_Static_assert(sizeof(struct rx_desc) == 16 && sizeof(struct tx_desc) == 16, "e1000 descriptor ABI");
_Static_assert(PAGE_SIZE % 128 == 0 && PAGE_SIZE % BUFSZ == 0, "e1000 DMA alignment");

static volatile uint8_t *mmio;
static volatile struct rx_desc *rx;
static volatile struct tx_desc *tx;
static uint8_t *rxbuf[NRX], *txbuf[NTX];
static int rx_cur, tx_cur, tx_clean;
static volatile bool rx_irq_pending;
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
    /* DMA ownership remains with the driver until the net thread has consumed
       the frame. Suppress RX interrupts while that thread drains the ring. */
    if (icr & RX_IRQS) wr(REG_IMC, RX_IRQS);
    rx_irq_pending = true;
    net_rx_notify();
}

static bool e1000_rx_pending(void) {
    return rx_irq_pending || (rx[rx_cur].status & DESC_DD);
}

static size_t e1000_poll_rx(void (*input)(const void *, size_t)) {
    wr(REG_IMC, RX_IRQS);
    size_t consumed = 0;
    int returned = -1;
    /* Return ownership only after this drain, so hardware cannot refill the
       descriptors consumed here until the single RDT write below. One ring
       is a finite poll bound; the net thread applies its service time/byte
       budget between polls without extra tail writes for small batches. */
    while (consumed < NRX && (rx[rx_cur].status & DESC_DD)) {
        /* x86 coherent DMA writes the packet before DD. Prevent the compiler
           from moving the packet read ahead of the volatile ownership check. */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint16_t len = rx[rx_cur].len;
        if ((rx[rx_cur].status & RX_EOP) && !rx[rx_cur].errors && len <= BUFSZ)
            input(rxbuf[rx_cur], len);
        rx[rx_cur].status = 0;
        returned = rx_cur;
        rx_cur = (rx_cur + 1) % NRX;
        consumed++;
    }
    uint64_t flags = irq_save();
    rx_irq_pending = false;
    if (returned >= 0) {
        __atomic_thread_fence(__ATOMIC_RELEASE);
        wr(REG_RDT, (uint32_t)returned); /* one tail write for the whole drain */
    }
    wr(REG_IMS, RX_IRQS);
    /* A packet may arrive between the empty check and rearming. Never wait
       for a fresh edge if its descriptor is already visible. */
    if (rx[rx_cur].status & DESC_DD) {
        wr(REG_IMC, RX_IRQS);
        rx_irq_pending = true;
        net_rx_notify();
    }
    irq_restore(flags);
    return consumed;
}

static int e1000_send(const void *frame, size_t len) {
    if (len > BUFSZ) return -N_ERR_INVALID;
    uint64_t flags = irq_save();
    while (tx_clean != tx_cur && (tx[tx_clean].status & DESC_DD))
        tx_clean = (tx_clean + 1) % NTX;
    int next = (tx_cur + 1) % NTX;
    volatile struct tx_desc *d = &tx[tx_cur];
    /* TDH == TDT denotes an empty hardware ring, so retain one descriptor to
       distinguish full from empty even before DMA has completed any send. */
    if (next == tx_clean || !(d->status & DESC_DD)) {
        irq_restore(flags);
        return -N_ERR_NOT_READY; /* local backpressure, not a packet lost on the wire */
    }
    memcpy(txbuf[tx_cur], frame, len);
    d->len = (uint16_t)len;
    d->cmd = 0x0B; /* EOP | IFCS | RS */
    d->status = 0;
    tx_cur = next;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    wr(REG_TDT, tx_cur);
    irq_restore(flags);
    return 0;
}

static uint32_t e1000_itr_ticks(void) {
    extern const char *kernel_cmdline;
    const char prefix[] = "e1000-itr-us=";
    uint32_t ticks = ITR_DEFAULT_TICKS;
    if (!kernel_cmdline) return ticks;
    for (const char *p = kernel_cmdline; *p; p++) {
        if ((p != kernel_cmdline && p[-1] != ' ') || strncmp(p, prefix, sizeof prefix - 1)) continue;
        const char *q = p + sizeof prefix - 1;
        uint64_t us = 0;
        bool valid = *q >= '0' && *q <= '9';
        while (*q >= '0' && *q <= '9') {
            unsigned digit = *q++ - '0';
            if (us > (UINT64_MAX - digit) / 10) valid = false;
            if (valid) us = us * 10 + digit;
        }
        if (!valid || (*q && *q != ' ') || us > (uint64_t)UINT16_MAX * ITR_TICK_NS / 1000) continue;
        ticks = (uint32_t)((us * 1000 + ITR_TICK_NS - 1) / ITR_TICK_NS);
    }
    return ticks;
}

/* Power change (dev/pm.h): no more interrupts, receive or transmit DMA. */
static bool e1000_quiesce(void *arg) {
    (void)arg;
    wr(REG_IMC, 0xFFFFFFFF);
    wr(REG_RCTL, 0);
    wr(REG_TCTL, 0);
    /* The 8254x has no bit that says its DMA went idle (EN reads back what was written): neither
       engine fetches another descriptor, and the one in flight lands within this wait. */
    for (int i = 0; i < 100; i++) io_wait(); /* ~1 us each */
    return true;
}

static uint64_t rx_ring_phys, tx_ring_phys;
static uint32_t itr_ticks; /* 0: the 82544GC/EI, which has no ITR */

static void hw_reset(void) {
    wr(REG_IMC, 0xFFFFFFFF);
    wr(REG_CTRL, rd(REG_CTRL) | (1u << 26)); /* device reset */
    net_delay_ms(10);
    for (int i = 0; i < 1000 && (rd(REG_CTRL) & (1u << 26)); i++) net_delay_ms(1);
    wr(REG_IMC, 0xFFFFFFFF);
    uint32_t ctrl = rd(REG_CTRL);
    ctrl |= (1u << 6) | (1u << 5);                  /* SLU, ASDE */
    ctrl &= ~((1u << 3) | (1u << 31) | (1u << 7)); /* LRST, PHY_RST, ILOS */
    wr(REG_CTRL, ctrl);
}

/* After the reset: the address, both rings from their first descriptor, the interrupt rate and
   the interrupts. At probe and after a sleep, which lost every register. */
static void hw_start(void) {
    wr(REG_RAL, nif.mac[0] | nif.mac[1] << 8 | nif.mac[2] << 16 | (uint32_t)nif.mac[3] << 24);
    wr(REG_RAH, nif.mac[4] | nif.mac[5] << 8 | 0x80000000u);
    for (int i = 0; i < 128; i++) wr(REG_MTA + i * 4, 0);
    for (size_t i = 0; i < NRX; i++) rx[i].status = 0;
    for (size_t i = 0; i < NTX; i++) {
        tx[i].status = 1; /* DD: free */
        tx[i].cmd = 0;
    }
    rx_cur = tx_cur = tx_clean = 0;
    rx_irq_pending = false;
    wr(REG_RDBAL, (uint32_t)rx_ring_phys);
    wr(REG_RDBAH, (uint32_t)(rx_ring_phys >> 32));
    wr(REG_RDLEN, NRX * sizeof *rx);
    wr(REG_RDH, 0);
    wr(REG_RDT, NRX - 1);
    wr(REG_RCTL, (1u << 1) | (1u << 15) | (1u << 26)); /* EN, BAM, SECRC; 2048-byte buffers */
    wr(REG_TDBAL, (uint32_t)tx_ring_phys);
    wr(REG_TDBAH, (uint32_t)(tx_ring_phys >> 32));
    wr(REG_TDLEN, NTX * sizeof *tx);
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    wr(REG_TCTL, (1u << 1) | (1u << 3) | (15u << 4) | (64u << 12)); /* EN, PSP, CT, COLD */
    wr(REG_TIPG, 10 | (8 << 10) | (6 << 20));

    /* Intel explicitly recommends ITR instead of RDTR/RADV (receive-delay
       errata). ITR is not implemented by the 82544GC/EI, device 0x100C. */
    wr(REG_RDTR, 0);
    if (itr_ticks) {
        wr(REG_RADV, 0);
        wr(REG_ITR, itr_ticks);
    }
    rd(REG_ICR);
    wr(REG_IMS, ICR_TXDW | ICR_LSC | RX_IRQS);
}

static bool e1000_resume(void *arg) {
    (void)arg;
    hw_reset();
    bool reset = !(rd(REG_CTRL) & (1u << 26));
    hw_start();
    net_rx_notify(); /* whatever the net thread was waiting for is gone: let it look again */
    return reset && (rd(REG_RCTL) & (1u << 1)) && (rd(REG_TCTL) & (1u << 1));
}

static struct pm_device e1000_pm = {.name = "e1000", .stage = PM_STAGE_IO, .quiesce = e1000_quiesce,
                                    .resume = e1000_resume};

static void e1000_free_dma(uint64_t rx_ring, uint64_t tx_ring) {
    for (size_t i = 0; i < NRX; i += BUFS_PER_PAGE)
        if (rxbuf[i]) pmm_free(hhdm_virt_to_phys(rxbuf[i]));
    for (size_t i = 0; i < NTX; i += BUFS_PER_PAGE)
        if (txbuf[i]) pmm_free(hhdm_virt_to_phys(txbuf[i]));
    if (rx_ring) pmm_free(rx_ring);
    if (tx_ring) pmm_free(tx_ring);
    memset(rxbuf, 0, sizeof rxbuf);
    memset(txbuf, 0, sizeof txbuf);
    rx = NULL;
    tx = NULL;
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
    hw_reset();

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
    }

    /* descriptor rings and buffers */
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
    if (d->device != 0x100C) {
        itr_ticks = e1000_itr_ticks();
        kprintf("net: e1000 ITR %u x %u ns (e1000-itr-us=0 disables)\n", itr_ticks, ITR_TICK_NS);
    }

    nif.send = e1000_send;
    nif.rx_pending = e1000_rx_pending;
    nif.poll_rx = e1000_poll_rx;
    net_register(&nif);
    pm_register(&e1000_pm);
    irq_register(d->irq, e1000_irq);
    pic_unmask(d->irq);
    hw_start();
    kprintf("net: e1000 (%04x) irq %d mac %02x:%02x:%02x:%02x:%02x:%02x\n", d->device, d->irq, nif.mac[0],
            nif.mac[1], nif.mac[2], nif.mac[3], nif.mac[4], nif.mac[5]);
    return true;
no_dma:
    e1000_free_dma(rx_ring, tx_ring);
    kprintf("net: e1000 DMA allocation failed\n");
    return false;
}
