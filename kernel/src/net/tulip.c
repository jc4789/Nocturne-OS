/* DEC 21140/21143 "tulip" network driver: the Hyper-V Generation 1 *legacy network adapter*
   (and QEMU's -nic model=tulip). Registers (CSRs) are accessed through the I/O BAR. */
#include "kernel.h"
#include "net.h"
#include "../arch/cpu.h"
#include "../dev/pci.h"
#include "../mm/pmm.h"

#define NRX   32
#define NTX   16
#define BUFSZ 1536
#define OWN   0x80000000u

struct tdesc {
    volatile uint32_t status, ctl, buf1, buf2;
};

static uint16_t iobase;
static struct tdesc *rx, *tx;
static uint8_t *rxbuf[NRX], *txbuf[NTX];
static int rx_cur, tx_cur;
static struct netif nif = {.driver = "tulip"};

static uint32_t csr_rd(int n) { return inl((uint16_t)(iobase + n * 8)); }
static void csr_wr(int n, uint32_t v) { outl((uint16_t)(iobase + n * 8), v); }

/* serial EEPROM (93C46/93C66) bit-banged through CSR9, as in the DEC reference code */
#define EE_SHIFT_CLK 0x02
#define EE_CS        0x01
#define EE_DATA_WR   0x04
#define EE_DATA_RD   0x08
#define EE_ENB       (0x4800 | EE_CS)

static uint32_t eeprom_read(int location, int addr_len) {
    uint32_t retval = 0;
    int cmd = location | (6 << addr_len);
    csr_wr(9, EE_ENB & ~EE_CS);
    csr_wr(9, EE_ENB);
    for (int i = 4 + addr_len; i >= 0; i--) {
        uint32_t bit = (cmd & (1 << i)) ? EE_DATA_WR : 0;
        csr_wr(9, EE_ENB | bit);
        csr_rd(9);
        csr_wr(9, EE_ENB | bit | EE_SHIFT_CLK);
        csr_rd(9);
        retval = (retval << 1) | ((csr_rd(9) & EE_DATA_RD) ? 1 : 0);
    }
    csr_wr(9, EE_ENB);
    csr_rd(9);
    for (int i = 16; i > 0; i--) {
        csr_wr(9, EE_ENB | EE_SHIFT_CLK);
        csr_rd(9);
        retval = (retval << 1) | ((csr_rd(9) & EE_DATA_RD) ? 1 : 0);
        csr_wr(9, EE_ENB);
        csr_rd(9);
    }
    csr_wr(9, EE_ENB & ~EE_CS);
    return retval;
}

static void tulip_irq(struct regs *r) {
    (void)r;
    uint32_t st = csr_rd(5);
    if (!(st & 0x1FFFF)) return;
    csr_wr(5, st & 0x1FFFF); /* write-one-to-clear */
    while (!(rx[rx_cur].status & OWN)) {
        uint32_t s = rx[rx_cur].status;
        /* no error summary, first and last descriptor of the frame */
        if (!(s & (1u << 15)) && (s & (3u << 8)) == (3u << 8)) {
            uint32_t len = (s >> 16) & 0x3FFF;
            if (len > 4) net_rx(rxbuf[rx_cur], len - 4); /* strip CRC */
        }
        rx[rx_cur].status = OWN;
        rx_cur = (rx_cur + 1) % NRX;
    }
    if (st & (1u << 7)) csr_wr(2, 0); /* receive buffer unavailable: poll demand */
}

static int tulip_send(const void *frame, size_t len) {
    if (len > BUFSZ) return -EINVAL;
    struct tdesc *d = &tx[tx_cur];
    for (int i = 0; i < 1000000 && (d->status & OWN); i++) pause();
    if (d->status & OWN) return -EBUSY;
    memcpy(txbuf[tx_cur], frame, len);
    d->ctl = (uint32_t)len | (1u << 30) | (1u << 29) | (tx_cur == NTX - 1 ? (1u << 25) : 0); /* LS, FS, TER */
    d->status = OWN;
    tx_cur = (tx_cur + 1) % NTX;
    csr_wr(1, 0); /* transmit poll demand */
    return 0;
}

bool tulip_probe(void) {
    struct pci_device *d = pci_find(0x1011, 0x0009); /* 21140: Hyper-V legacy adapter */
    if (!d) d = pci_find(0x1011, 0x0019);            /* 21143: QEMU */
    if (!d || !(d->bar[0] & 1)) return false;
    pci_enable_busmaster(d);
    iobase = (uint16_t)(d->bar[0] & ~3u);

    csr_wr(0, 1); /* software reset */
    net_delay_ms(10);
    csr_wr(0, 0);
    net_delay_ms(10);

    int alen = (eeprom_read(0xFF, 8) & 0x40000) ? 8 : 6;
    uint8_t ee[128];
    for (int i = 0; i < 64; i++) {
        uint32_t w = eeprom_read(i, alen);
        ee[i * 2] = (uint8_t)w;
        ee[i * 2 + 1] = (uint8_t)(w >> 8);
    }
    /* DEC-format SROMs keep the address at byte 20; old boards put it first (Linux uses the same test) */
    int off = 0;
    for (int i = 0; i < 8; i++)
        if (ee[i] != ee[16 + i]) off = 20;
    if (ee[0] == 0xFF && ee[1] == 0xFF && ee[2] == 0) off = 2;
    memcpy(nif.mac, ee + off, 6);
    bool zero = true, ff = true;
    for (int i = 0; i < 6; i++) {
        if (nif.mac[i]) zero = false;
        if (nif.mac[i] != 0xFF) ff = false;
    }
    if (zero || ff) {
        static const uint8_t fallback[6] = {0x00, 0x15, 0x5D, 0x4E, 0x43, 0x54};
        memcpy(nif.mac, fallback, 6);
    }

    /* rings must live below 4 GiB: the tulip only has 32-bit descriptor pointers */
    uint64_t ring = pmm_alloc_zeroed();
    if (ring >> 32) return false;
    rx = phys_to_virt(ring);
    tx = (struct tdesc *)((uint8_t *)phys_to_virt(ring) + NRX * sizeof(struct tdesc));
    uint64_t tx_ring = ring + NRX * sizeof(struct tdesc);
    for (int i = 0; i < NRX; i += 2) {
        uint64_t p = pmm_alloc();
        for (int k = 0; k < 2; k++) {
            rxbuf[i + k] = (uint8_t *)phys_to_virt(p) + k * 2048;
            rx[i + k].buf1 = (uint32_t)(p + k * 2048);
            rx[i + k].buf2 = 0;
            rx[i + k].ctl = BUFSZ | (i + k == NRX - 1 ? (1u << 25) : 0); /* RER */
            rx[i + k].status = OWN;
        }
    }
    for (int i = 0; i < NTX; i += 2) {
        uint64_t p = pmm_alloc();
        for (int k = 0; k < 2; k++) {
            txbuf[i + k] = (uint8_t *)phys_to_virt(p) + k * 2048;
            tx[i + k].buf1 = (uint32_t)(p + k * 2048);
            tx[i + k].buf2 = 0;
            tx[i + k].ctl = i + k == NTX - 1 ? (1u << 25) : 0; /* TER */
            tx[i + k].status = 0;
        }
    }
    csr_wr(0, 0x4800); /* 8-longword cache alignment, burst length 8 */
    csr_wr(3, (uint32_t)ring);
    csr_wr(4, (uint32_t)tx_ring);

    /* setup frame: perfect filter with our address and broadcast */
    uint32_t *sf = (uint32_t *)txbuf[0];
    for (int e = 0; e < 16; e++) {
        const uint8_t *a = e == 0 ? nif.mac : (const uint8_t *)"\xff\xff\xff\xff\xff\xff";
        sf[e * 3 + 0] = a[0] | a[1] << 8;
        sf[e * 3 + 1] = a[2] | a[3] << 8;
        sf[e * 3 + 2] = a[4] | a[5] << 8;
    }
    tx[0].ctl = 192 | (1u << 27); /* SET */
    tx[0].status = OWN;
    tx_cur = 1;

    irq_register(d->irq, tulip_irq);
    pic_unmask(d->irq);
    csr_wr(5, 0x1FFFF);
    csr_wr(7, (1u << 16) | (1u << 15) | (1u << 6) | (1u << 7)); /* NIS, AIS, RI, RU */
    /* store-and-forward, start TX/RX, full duplex, promiscuous (frames are filtered in software) */
    csr_wr(6, (1u << 21) | (1u << 13) | (1u << 9) | (1u << 6) | (1u << 1));
    csr_wr(1, 0);
    csr_wr(2, 0);
    nif.send = tulip_send;
    kprintf("net: tulip (%04x) irq %d mac %02x:%02x:%02x:%02x:%02x:%02x\n", d->device, d->irq, nif.mac[0],
            nif.mac[1], nif.mac[2], nif.mac[3], nif.mac[4], nif.mac[5]);
    net_register(&nif);
    return true;
}
