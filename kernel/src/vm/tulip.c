/* DEC 21140/21143 "tulip" network driver: the Hyper-V Generation 1 *legacy network adapter*
   (and QEMU's -nic model=tulip). Registers (CSRs) are accessed through the I/O BAR. */
#include "kernel.h"
#include "net/net.h"
#include "sys/clock.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "hal/hal.h"
#include "dev/pci.h"
#include "mm/heap.h"

#define NRX   32
#define NTX   16
#define BUFSZ 1536
#define OWN   0x80000000u
#define IO_BAR 0

struct tdesc {
    volatile uint32_t status, ctl, buf1, buf2;
};

/* The receive ring belongs to the interrupt handler (on its irq's CPU), which copies each frame into the
   stack's queue (net_rx); the transmit ring to transmit, which the stack calls one at a time. The
   two share no state and no register (CSR5 and CSR2 are the handler's, CSR1 transmit's), so
   neither needs a lock of the driver's own. */
struct tulip {
    struct netif nif; /* first: self is the device */
    uint16_t iobase;
    struct tdesc *rx, *tx;
    uint8_t *rxbuf[NRX], *txbuf[NTX];
    unsigned rx_cur, tx_cur;
};

static uint32_t csr_rd(struct tulip *c, int n) { return inl((uint16_t)(c->iobase + n * 8)); }
static void csr_wr(struct tulip *c, int n, uint32_t v) { outl((uint16_t)(c->iobase + n * 8), v); }

/* serial EEPROM (93C46/93C66) bit-banged through CSR9, as in the DEC reference code */
#define EE_SHIFT_CLK 0x02
#define EE_CS        0x01
#define EE_DATA_WR   0x04
#define EE_DATA_RD   0x08
#define EE_ENB       (0x4800 | EE_CS)

static uint32_t eeprom_read(struct tulip *c, int location, int addr_len) {
    uint32_t retval = 0;
    int cmd = location | (6 << addr_len);
    csr_wr(c, 9, EE_ENB & ~EE_CS);
    csr_wr(c, 9, EE_ENB);
    for (int i = 4 + addr_len; i >= 0; i--) {
        uint32_t bit = (cmd & (1 << i)) ? EE_DATA_WR : 0;
        csr_wr(c, 9, EE_ENB | bit);
        csr_rd(c, 9);
        csr_wr(c, 9, EE_ENB | bit | EE_SHIFT_CLK);
        csr_rd(c, 9);
        retval = (retval << 1) | ((csr_rd(c, 9) & EE_DATA_RD) ? 1 : 0);
    }
    csr_wr(c, 9, EE_ENB);
    csr_rd(c, 9);
    for (int i = 16; i > 0; i--) {
        csr_wr(c, 9, EE_ENB | EE_SHIFT_CLK);
        csr_rd(c, 9);
        retval = (retval << 1) | ((csr_rd(c, 9) & EE_DATA_RD) ? 1 : 0);
        csr_wr(c, 9, EE_ENB);
        csr_rd(c, 9);
    }
    csr_wr(c, 9, EE_ENB & ~EE_CS);
    return retval;
}

static enum irq_result tulip_irq(void *context) {
    struct tulip *c = context;
    uint32_t st = csr_rd(c, 5);
    if (!(st & 0x1FFFF)) return IRQ_NONE; /* its INTx line may be shared */
    csr_wr(c, 5, st & 0x1FFFF); /* write-one-to-clear */
    while (!(c->rx[c->rx_cur].status & OWN)) {
        uint32_t s = c->rx[c->rx_cur].status;
        /* no error summary, first and last descriptor of the frame */
        if (!(s & (1u << 15)) && (s & (3u << 8)) == (3u << 8)) {
            uint32_t len = (s >> 16) & 0x3FFF;
            if (len > 4) net_rx(&c->nif, c->rxbuf[c->rx_cur], len - 4, 0); /* strip CRC */
        }
        c->rx[c->rx_cur].status = OWN;
        c->rx_cur = (c->rx_cur + 1) % NRX;
    }
    if (st & (1u << 7)) csr_wr(c, 2, 0); /* receive buffer unavailable: poll demand */
    if (st & 1u) net_rx_notify();         /* a transmit finished: room for what the stack kept */
    return IRQ_HANDLED;
}

/* Receive is the interrupt handler's (net_rx), so there is nothing for the stack to poll. */
static size_t tulip_poll(struct netif *self, unsigned queue, size_t budget, net_input_fn input) {
    (void)self; (void)queue; (void)budget; (void)input;
    return 0;
}

static bool tulip_rx_pending(struct netif *self, unsigned queue) {
    (void)self; (void)queue;
    return false;
}

static unsigned tulip_transmit(struct netif *self, unsigned queue, const struct net_tx_frame *frames, unsigned count) {
    (void)queue;
    struct tulip *c = (struct tulip *)self;
    unsigned taken = 0;
    while (taken < count) {
        const struct net_tx_frame *f = &frames[taken];
        if (f->length > BUFSZ) { /* it does not fit a buffer: dropped */
            taken++;
            continue;
        }
        struct tdesc *d = &c->tx[c->tx_cur];
        if (d->status & OWN) break; /* the rest waits for the completion interrupt */
        size_t len = 0;
        for (unsigned i = 0; i < f->part_count; i++) {
            memcpy(c->txbuf[c->tx_cur] + len, f->parts[i].data, f->parts[i].length);
            len += f->parts[i].length;
        }
        /* IC, LS, FS, and TER on the last descriptor of the ring */
        d->ctl = (uint32_t)len | (1u << 31) | (1u << 30) | (1u << 29) | (c->tx_cur == NTX - 1 ? (1u << 25) : 0);
        d->status = OWN;
        c->tx_cur = (c->tx_cur + 1) % NTX;
        taken++;
    }
    if (taken) csr_wr(c, 1, 0); /* transmit poll demand, once for the batch */
    return taken;
}

/* Power change (hal.h): interrupts off, receive and transmit stopped (CSR7, CSR6), until both
   processes report "stopped" in CSR5 (bits 17-19 and 20-22). */
static bool tulip_quiesce(struct hal_device *d) {
    struct tulip *c = d->driver_data;
    csr_wr(c, 7, 0);
    csr_wr(c, 6, 0);
    for (int i = 0; i < 10000; i++) {
        if (!(csr_rd(c, 5) & (0x3Fu << 17))) return true;
        io_wait(); /* ~1 us each */
    }
    return false;
}

static int tulip_attach(struct hal_device *d) {
    struct pci_device *pci = pci_device_of(d);
    uint16_t iobase = hal_port(d, IO_BAR);
    if (!iobase) return -N_ERR_UNSUPPORTED;
    struct tulip *c = kzalloc(sizeof *c);
    if (!c) return -N_ERR_NO_MEMORY;
    d->driver_data = c;
    c->iobase = iobase;
    int r = -N_ERR_NO_MEMORY;
    hal_enable(d, HAL_ENABLE_PORTS | HAL_ENABLE_DMA);

    csr_wr(c, 0, 1); /* software reset */
    clock_spin(10 * NS_PER_MS);
    csr_wr(c, 0, 0);
    clock_spin(10 * NS_PER_MS);

    int alen = (eeprom_read(c, 0xFF, 8) & 0x40000) ? 8 : 6;
    uint8_t ee[128];
    for (int i = 0; i < 64; i++) {
        uint32_t w = eeprom_read(c, i, alen);
        ee[i * 2] = (uint8_t)w;
        ee[i * 2 + 1] = (uint8_t)(w >> 8);
    }
    /* DEC-format SROMs keep the address at byte 20; old boards put it first (Linux uses the same test) */
    int off = 0;
    for (int i = 0; i < 8; i++)
        if (ee[i] != ee[16 + i]) off = 20;
    if (ee[0] == 0xFF && ee[1] == 0xFF && ee[2] == 0) off = 2;
    memcpy(c->nif.mac, ee + off, 6);
    bool zero = true, ff = true;
    for (int i = 0; i < 6; i++) {
        if (c->nif.mac[i]) zero = false;
        if (c->nif.mac[i] != 0xFF) ff = false;
    }
    if (zero || ff) {
        static const uint8_t fallback[6] = {0x00, 0x15, 0x5D, 0x4E, 0x43, 0x54};
        memcpy(c->nif.mac, fallback, 6);
    }

    /* the descriptors and the buffers they point at, in one block below 4 GiB: the chip has only
       32-bit descriptor and buffer pointers */
    uint64_t bus;
    uint8_t *block = hal_dma_alloc(d, PAGE_SIZE + (NRX + NTX) * BUFSZ, 32, &bus);
    if (!block) {
        kprintf("net: tulip DMA allocation failed\n");
        goto fail;
    }
    c->rx = (struct tdesc *)block;
    c->tx = c->rx + NRX;
    uint64_t rx_ring = bus, tx_ring = bus + NRX * sizeof(struct tdesc);
    uint8_t *buffers = block + PAGE_SIZE;
    uint64_t buffers_bus = bus + PAGE_SIZE;
    for (int i = 0; i < NRX; i++) {
        c->rxbuf[i] = buffers + i * BUFSZ;
        c->rx[i].buf1 = (uint32_t)(buffers_bus + i * BUFSZ);
        c->rx[i].buf2 = 0;
        c->rx[i].ctl = BUFSZ | (i == NRX - 1 ? (1u << 25) : 0); /* RER */
        c->rx[i].status = OWN;
    }
    for (int i = 0; i < NTX; i++) {
        c->txbuf[i] = buffers + (NRX + i) * BUFSZ;
        c->tx[i].buf1 = (uint32_t)(buffers_bus + (NRX + i) * BUFSZ);
        c->tx[i].buf2 = 0;
        c->tx[i].ctl = i == NTX - 1 ? (1u << 25) : 0; /* TER */
        c->tx[i].status = 0;
    }
    csr_wr(c, 0, 0x4800); /* 8-longword cache alignment, burst length 8 */
    csr_wr(c, 3, (uint32_t)rx_ring);
    csr_wr(c, 4, (uint32_t)tx_ring);

    /* setup frame: perfect filter with our address and broadcast */
    uint32_t *sf = (uint32_t *)c->txbuf[0];
    for (int e = 0; e < 16; e++) {
        const uint8_t *a = e == 0 ? c->nif.mac : (const uint8_t *)"\xff\xff\xff\xff\xff\xff";
        sf[e * 3 + 0] = a[0] | a[1] << 8;
        sf[e * 3 + 1] = a[2] | a[3] << 8;
        sf[e * 3 + 2] = a[4] | a[5] << 8;
    }
    c->tx[0].ctl = 192 | (1u << 27); /* SET */
    c->tx[0].status = OWN;
    c->tx_cur = 1;

    if (!hal_irq_enable(d, &(struct irq_spec){.name = "tulip", .handler = tulip_irq, .context = c}, 1, NULL)) {
        kprintf("net: tulip has no interrupt route\n");
        r = -N_ERR_UNSUPPORTED;
        goto fail;
    }
    csr_wr(c, 5, 0x1FFFF);
    csr_wr(c, 7, (1u << 16) | (1u << 15) | (1u << 6) | (1u << 7) | 1u); /* NIS, AIS, RI, RU, TI */
    /* store-and-forward, start TX/RX, full duplex, promiscuous (frames are filtered in software) */
    csr_wr(c, 6, (1u << 21) | (1u << 13) | (1u << 9) | (1u << 6) | (1u << 1));
    csr_wr(c, 1, 0);
    csr_wr(c, 2, 0);
    c->nif.driver = "tulip";
    c->nif.tx_queues = 1;
    c->nif.rx_queues = 1;
    c->nif.transmit = tulip_transmit;
    c->nif.poll = tulip_poll;
    c->nif.rx_pending = tulip_rx_pending;
    kprintf("net: tulip (%04x) mac %02x:%02x:%02x:%02x:%02x:%02x\n", pci->device, c->nif.mac[0], c->nif.mac[1],
            c->nif.mac[2], c->nif.mac[3], c->nif.mac[4], c->nif.mac[5]);
    net_register(d, &c->nif);
    return 0;
fail:
    hal_irq_disable(d); /* before the handler's memory goes */
    d->driver_data = NULL;
    kfree(c);
    return r;
}

/* 21140: Hyper-V's legacy adapter. 21143: QEMU. */
static const struct n_hal_rule tulip_rules[] = {N_PCI_ID(0x1011, 0x0009), N_PCI_ID(0x1011, 0x0019)};
const struct hal_driver tulip_driver = {
    .name = "tulip",
    .rules = tulip_rules,
    .rule_count = ARRAY_SIZE(tulip_rules),
    .attach = tulip_attach,
    .quiesce = tulip_quiesce,
};
