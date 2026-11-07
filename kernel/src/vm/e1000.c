/* Intel 8254x "e1000" network driver (QEMU's default NIC, also VirtualBox and VMware). */
#include "kernel.h"
#include "net/net.h"
#include "sys/clock.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/spin.h"
#include "dev/pci.h"
#include "hal/hal.h"
#include "mm/heap.h"

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

/* Each descriptor ring owns one page: 256 legacy descriptors. This satisfies the hardware's
   16-byte base and 128-byte ring-length alignment; packet buffering is backed by these DMA
   descriptors, not another RX queue. */
#define NRX (PAGE_SIZE / sizeof(struct rx_desc))
#define NTX (PAGE_SIZE / sizeof(struct tx_desc))
_Static_assert(sizeof(struct rx_desc) == 16 && sizeof(struct tx_desc) == 16, "e1000 descriptor ABI");
_Static_assert(PAGE_SIZE % 128 == 0, "e1000 DMA alignment");

/* The receive ring belongs to the net thread (poll, rx_pending), the transmit ring to transmit,
   which the stack calls one at a time. The interrupt handler and poll, each on any CPU,
   share the receive interrupt mask and rx_irq_pending: e1000_lock, a leaf under the stack's
   net_lock, guards them; net_rx_notify comes after it is given back. A power change runs
   alone (every other CPU halted). */
struct e1000 {
    struct netif nif; /* first: self is the device */
    volatile uint8_t *mmio;
    volatile struct rx_desc *rx;
    volatile struct tx_desc *tx;
    uint8_t *rxbuf[NRX], *txbuf[NTX];
    uint64_t rx_ring_bus, tx_ring_bus;
    unsigned rx_cur, tx_cur, tx_clean;
    struct spinlock e1000_lock;
    volatile bool rx_irq_pending;
    bool new_eerd;      /* 82574-style EERD layout */
    uint32_t itr_ticks; /* 0: the 82544GC/EI, which has no ITR */
};

static uint32_t rd(struct e1000 *c, uint32_t r) { return mmio_read32(c->mmio + r); }
static void wr(struct e1000 *c, uint32_t r, uint32_t v) { mmio_write32(c->mmio + r, v); }

static uint16_t eeprom_read(struct e1000 *c, uint8_t addr) {
    if (c->new_eerd) wr(c, REG_EERD, 1 | ((uint32_t)addr << 2));
    else wr(c, REG_EERD, 1 | ((uint32_t)addr << 8));
    uint32_t done = c->new_eerd ? 2 : 16;
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(c, REG_EERD);
        if (v & done) return (uint16_t)(v >> 16);
        pause();
    }
    return 0xFFFF;
}

static enum irq_result e1000_irq(void *context) {
    struct e1000 *c = context;
    spin_take(&c->e1000_lock);
    uint32_t icr = rd(c, REG_ICR); /* reading acknowledges */
    if (icr) {
        /* DMA ownership remains with the driver until the net thread has consumed
           the frame. Suppress RX interrupts while that thread drains the ring. */
        if (icr & RX_IRQS) wr(c, REG_IMC, RX_IRQS);
        c->rx_irq_pending = true;
    }
    spin_give(&c->e1000_lock);
    if (icr) net_rx_notify();
    return icr ? IRQ_HANDLED : IRQ_NONE; /* its INTx line may be shared */
}

static bool e1000_rx_pending(struct netif *self, unsigned queue) {
    (void)queue;
    struct e1000 *c = (struct e1000 *)self;
    return c->rx_irq_pending || (c->rx[c->rx_cur].status & DESC_DD);
}

static size_t e1000_poll(struct netif *self, unsigned queue, size_t budget, net_input_fn input) {
    (void)queue;
    struct e1000 *c = (struct e1000 *)self;
    wr(c, REG_IMC, RX_IRQS);
    size_t consumed = 0;
    int returned = -1;
    /* Return ownership only after this drain, so hardware cannot refill the
       descriptors consumed here until the single RDT write below. */
    while (consumed < budget && (c->rx[c->rx_cur].status & DESC_DD)) {
        /* x86 coherent DMA writes the packet before DD. Prevent the compiler
           from moving the packet read ahead of the volatile ownership check. */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint16_t len = c->rx[c->rx_cur].len;
        if ((c->rx[c->rx_cur].status & RX_EOP) && !c->rx[c->rx_cur].errors && len <= BUFSZ)
            input(self, c->rxbuf[c->rx_cur], len, 0);
        c->rx[c->rx_cur].status = 0;
        returned = (int)c->rx_cur;
        c->rx_cur = (c->rx_cur + 1) % NRX;
        consumed++;
    }
    uint64_t flags = spin_lock(&c->e1000_lock);
    c->rx_irq_pending = false;
    if (returned >= 0) {
        __atomic_thread_fence(__ATOMIC_RELEASE);
        wr(c, REG_RDT, (uint32_t)returned); /* one tail write for the whole drain */
    }
    wr(c, REG_IMS, RX_IRQS);
    /* A packet may arrive between the empty check and rearming. Never wait
       for a fresh edge if its descriptor is already visible. */
    bool again = c->rx[c->rx_cur].status & DESC_DD;
    if (again) {
        wr(c, REG_IMC, RX_IRQS);
        c->rx_irq_pending = true;
    }
    spin_unlock(&c->e1000_lock, flags);
    if (again) net_rx_notify();
    return consumed;
}

static unsigned e1000_transmit(struct netif *self, unsigned queue, const struct net_tx_frame *frames, unsigned count) {
    (void)queue;
    struct e1000 *c = (struct e1000 *)self;
    while (c->tx_clean != c->tx_cur && (c->tx[c->tx_clean].status & DESC_DD)) c->tx_clean = (c->tx_clean + 1) % NTX;
    unsigned taken = 0, posted = 0;
    while (taken < count) {
        const struct net_tx_frame *f = &frames[taken];
        if (f->length > BUFSZ) { /* it does not fit a buffer: dropped */
            taken++;
            continue;
        }
        unsigned next = (c->tx_cur + 1) % NTX;
        volatile struct tx_desc *d = &c->tx[c->tx_cur];
        /* TDH == TDT denotes an empty hardware ring, so retain one descriptor to
           distinguish full from empty even before DMA has completed any send. */
        if (next == c->tx_clean || !(d->status & DESC_DD)) break; /* the rest waits for a completion */
        size_t len = 0;
        for (unsigned i = 0; i < f->part_count; i++) {
            memcpy(c->txbuf[c->tx_cur] + len, f->parts[i].data, f->parts[i].length);
            len += f->parts[i].length;
        }
        d->len = (uint16_t)len;
        d->cmd = 0x0B; /* EOP | IFCS | RS */
        d->status = 0;
        c->tx_cur = next;
        taken++;
        posted++;
    }
    if (posted) {
        __atomic_thread_fence(__ATOMIC_RELEASE);
        wr(c, REG_TDT, c->tx_cur); /* one tail write for the batch */
    }
    return taken;
}

static uint32_t e1000_itr_ticks(void) {
    uint64_t us;
    if (!test_number("e1000-itr-us", &us) || us > (uint64_t)UINT16_MAX * ITR_TICK_NS / 1000)
        return ITR_DEFAULT_TICKS;
    return (uint32_t)((us * 1000 + ITR_TICK_NS - 1) / ITR_TICK_NS);
}

static void hw_reset(struct e1000 *c) {
    wr(c, REG_IMC, 0xFFFFFFFF);
    wr(c, REG_CTRL, rd(c, REG_CTRL) | (1u << 26)); /* device reset */
    clock_spin(10 * NS_PER_MS);
    for (uint64_t until = clock_after(NS_PER_S); (rd(c, REG_CTRL) & (1u << 26)) && clock_now_ns() < until;)
        clock_spin(NS_PER_MS);
    wr(c, REG_IMC, 0xFFFFFFFF);
    uint32_t ctrl = rd(c, REG_CTRL);
    ctrl |= (1u << 6) | (1u << 5);                  /* SLU, ASDE */
    ctrl &= ~((1u << 3) | (1u << 31) | (1u << 7)); /* LRST, PHY_RST, ILOS */
    wr(c, REG_CTRL, ctrl);
}

/* After the reset: the address, both rings from their first descriptor, the interrupt rate and
   the interrupts. At probe and after a sleep, which lost every register. */
static void hw_start(struct e1000 *c) {
    const uint8_t *mac = c->nif.mac;
    wr(c, REG_RAL, mac[0] | mac[1] << 8 | mac[2] << 16 | (uint32_t)mac[3] << 24);
    wr(c, REG_RAH, mac[4] | mac[5] << 8 | 0x80000000u);
    for (int i = 0; i < 128; i++) wr(c, REG_MTA + i * 4, 0);
    for (size_t i = 0; i < NRX; i++) c->rx[i].status = 0;
    for (size_t i = 0; i < NTX; i++) {
        c->tx[i].status = 1; /* DD: free */
        c->tx[i].cmd = 0;
    }
    c->rx_cur = c->tx_cur = c->tx_clean = 0;
    c->rx_irq_pending = false;
    wr(c, REG_RDBAL, (uint32_t)c->rx_ring_bus);
    wr(c, REG_RDBAH, (uint32_t)(c->rx_ring_bus >> 32));
    wr(c, REG_RDLEN, NRX * sizeof *c->rx);
    wr(c, REG_RDH, 0);
    wr(c, REG_RDT, NRX - 1);
    wr(c, REG_RCTL, (1u << 1) | (1u << 15) | (1u << 26)); /* EN, BAM, SECRC; 2048-byte buffers */
    wr(c, REG_TDBAL, (uint32_t)c->tx_ring_bus);
    wr(c, REG_TDBAH, (uint32_t)(c->tx_ring_bus >> 32));
    wr(c, REG_TDLEN, NTX * sizeof *c->tx);
    wr(c, REG_TDH, 0);
    wr(c, REG_TDT, 0);
    wr(c, REG_TCTL, (1u << 1) | (1u << 3) | (15u << 4) | (64u << 12)); /* EN, PSP, CT, COLD */
    wr(c, REG_TIPG, 10 | (8 << 10) | (6 << 20));

    /* Intel explicitly recommends ITR instead of RDTR/RADV (receive-delay
       errata). ITR is not implemented by the 82544GC/EI, device 0x100C. */
    wr(c, REG_RDTR, 0);
    if (c->itr_ticks) {
        wr(c, REG_RADV, 0);
        wr(c, REG_ITR, c->itr_ticks);
    }
    rd(c, REG_ICR);
    wr(c, REG_IMS, ICR_TXDW | ICR_LSC | RX_IRQS);
}

static int e1000_attach(struct hal_device *d) {
    struct pci_device *pci = pci_device_of(d);
    if (!hal_resource(d, N_HAL_MEMORY, 0)) return -N_ERR_UNSUPPORTED;
    struct e1000 *c = kzalloc(sizeof *c);
    if (!c) return -N_ERR_NO_MEMORY;
    d->driver_data = c;
    int r = -N_ERR_NO_MEMORY;
    c->new_eerd = pci->device == 0x10D3;
    if (!(c->mmio = hal_map(d, 0))) goto fail;
    hal_enable(d, HAL_ENABLE_MEMORY | HAL_ENABLE_DMA);
    hw_reset(c);

    uint8_t *mac = c->nif.mac;
    uint32_t ral = rd(c, REG_RAL), rah = rd(c, REG_RAH);
    if ((rah & 0x80000000u) && (ral || (rah & 0xFFFF))) {
        for (int i = 0; i < 4; i++) mac[i] = (uint8_t)(ral >> (i * 8));
        mac[4] = (uint8_t)rah;
        mac[5] = (uint8_t)(rah >> 8);
    } else {
        for (int i = 0; i < 3; i++) {
            uint16_t w = eeprom_read(c, (uint8_t)i);
            mac[i * 2] = (uint8_t)w;
            mac[i * 2 + 1] = (uint8_t)(w >> 8);
        }
    }

    /* the rings, then the buffers they point at: one block each, the buffers after one another */
    uint64_t ring_bus, buffer_bus;
    uint8_t *rings = hal_dma_alloc(d, 2 * PAGE_SIZE, 0, &ring_bus);
    uint8_t *buffers = hal_dma_alloc(d, (NRX + NTX) * BUFSZ, 0, &buffer_bus);
    if (!rings || !buffers) {
        kprintf("net: e1000 DMA allocation failed\n");
        goto fail;
    }
    c->rx = (volatile struct rx_desc *)rings;
    c->tx = (volatile struct tx_desc *)(rings + PAGE_SIZE);
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
    if (pci->device != 0x100C) {
        c->itr_ticks = e1000_itr_ticks();
        kprintf("net: e1000 ITR %u x %u ns (e1000-itr-us=0 disables)\n", c->itr_ticks, ITR_TICK_NS);
    }

    if (!hal_irq_enable(d, &(struct irq_spec){.name = "e1000", .handler = e1000_irq, .context = c}, 1, NULL)) {
        kprintf("net: e1000 has no interrupt route\n");
        r = -N_ERR_UNSUPPORTED;
        goto fail;
    }
    c->nif.driver = "e1000";
    c->nif.tx_queues = c->nif.rx_queues = 1;
    c->nif.transmit = e1000_transmit;
    c->nif.rx_pending = e1000_rx_pending;
    c->nif.poll = e1000_poll;
    net_register(d, &c->nif);
    hw_start(c);
    kprintf("net: e1000 (%04x) mac %02x:%02x:%02x:%02x:%02x:%02x\n", pci->device, mac[0], mac[1], mac[2], mac[3], mac[4],
            mac[5]);
    return 0;
fail:
    hal_irq_disable(d); /* before the handler's memory goes */
    d->driver_data = NULL;
    kfree(c);
    return r;
}

static const struct n_hal_rule e1000_rules[] = {N_PCI_ID(0x8086, 0x100E), N_PCI_ID(0x8086, 0x100F), N_PCI_ID(0x8086, 0x1004),
                                                N_PCI_ID(0x8086, 0x100C), N_PCI_ID(0x8086, 0x1015), N_PCI_ID(0x8086, 0x10D3)};
const struct hal_driver e1000_driver = {
    .name = "e1000",
    .rules = e1000_rules,
    .rule_count = ARRAY_SIZE(e1000_rules),
    .attach = e1000_attach,
};
