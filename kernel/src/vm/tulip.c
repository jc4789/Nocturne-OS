/* DEC 21140・21143「tulip」の網の実装：Hyper-V の第 1 世代の従来の網の装置（と QEMU の -nic model=tulip）。登録
   （CSR）は I/O の BAR で触る。節と表の番号は Intel の「21143 PCI/CardBus 10/100Mb/s Ethernet LAN Controller Hardware
   Reference Manual」（278074-001）のもの。 */
#include "kernel.h"
#include "net/net.h"
#include "sys/clock.h"
#include "sys/wait.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/spin.h"
#include "hal/hal.h"
#include "dev/pci.h"
#include "mm/heap.h"

#define NRX   32
#define NTX   16
#define BUFSZ 1536
#define OWN   0x80000000u
#define IO_BAR 0

#define CSR0_SWR       1u          /* Software Reset（3.2.2.1、4.3.1） */
#define CSR0_BUS_MODE  0x4800u     /* 8 longword の cache の揃え、burst の長さ 8 */
#define CSR5_STATUS    0x1FFFFu    /* 1 を書いて消す状態の bit（表 3-70） */
#define CSR5_PROCESSES (0x3Fu << 17) /* RS（19:17）と TS（22:20）。000 が止まっている（表 3-68、3-69） */
#define CSR6_SR        (1u << 1)   /* Start/Stop Receive（3.2.2.9） */
#define CSR6_ST        (1u << 13)  /* Start/Stop Transmission Command（3.2.2.9） */
/* 止めの命令から止まるまで待つ限り。過程は今のフレームを終えてから止まる（3.2.2.9 の SR、ST）：10 Mb/s で最大の
   フレームと、半二重の link の衝突の後の待ちが入る長さ。 */
#define STOP_NS NS_PER_S
/* reset の後、最初の取引までの待ち。4.3.1 は 50 PCI clock を求める。PCI の clock を問わず足りる長さ。 */
#define RESET_NS NS_PER_MS

struct tdesc {
    volatile uint32_t status, ctl, buf1, buf2;
};

/* 装置の状態。tulip_mutex と tulip_lock の両方の下で書き、どちらかの下で読む。 */
enum tulip_state {
    TULIP_STOPPED, /* 始める前と、suspend から resume まで：送らず、受けず、登録に触らない */
    TULIP_RUNNING,
    TULIP_BROKEN, /* resume で戻らなかった：送るものは受け取って捨てる */
};

/* 錠の順：網の層の net_lock（mutex）→ tulip_mutex → tulip_lock。
   tulip_mutex：送信（網の層が一つずつ呼ぶ）と、始めること・suspend・resume を一つずつにする。送信の輪と CSR1 は
   この下。
   tulip_lock：割り込みの処理（irq の CPU の上で、受けた各フレームを網の層の列へ写す：net_rx）と状態。受信の輪と
   CSR5・CSR2 は処理のもので、この下で触る。葉。 */
struct tulip {
    struct netif nif; /* 最初：self が装置 */
    uint16_t iobase;
    struct tdesc *rx, *tx;
    uint8_t *rxbuf[NRX], *txbuf[NTX];
    uint32_t rx_ring_bus, tx_ring_bus, buffers_bus;
    unsigned rx_cur, tx_cur;
    struct mutex tulip_mutex;
    struct spinlock tulip_lock;
    enum tulip_state state;
};

static uint32_t csr_rd(struct tulip *c, int n) { return inl((uint16_t)(c->iobase + n * 8)); }
static void csr_wr(struct tulip *c, int n, uint32_t v) { outl((uint16_t)(c->iobase + n * 8), v); }

/* CSR9 で bit を打って読む直列の EEPROM（93C46・93C66）。DEC の参照のコードと同じ形 */
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
    spin_take(&c->tulip_lock);
    if (c->state != TULIP_RUNNING) { /* 止めた装置は割り込みを覆ってある：共有の INTx の線のほかの装置のもの */
        spin_give(&c->tulip_lock);
        return IRQ_NONE;
    }
    uint32_t st = csr_rd(c, 5);
    if (!(st & CSR5_STATUS)) { /* INTx の線は共有かもしれない */
        spin_give(&c->tulip_lock);
        return IRQ_NONE;
    }
    csr_wr(c, 5, st & CSR5_STATUS);
    while (!(c->rx[c->rx_cur].status & OWN)) {
        uint32_t s = c->rx[c->rx_cur].status;
        /* 誤りの要約が無く、フレームの最初と最後の記述子 */
        if (!(s & (1u << 15)) && (s & (3u << 8)) == (3u << 8)) {
            uint32_t len = (s >> 16) & 0x3FFF;
            if (len > 4) net_rx(&c->nif, c->rxbuf[c->rx_cur], len - 4, 0); /* CRC を除く */
        }
        c->rx[c->rx_cur].status = OWN;
        c->rx_cur = (c->rx_cur + 1) % NRX;
    }
    if (st & (1u << 7)) csr_wr(c, 2, 0); /* 受信の緩衝が無かった：受信の poll demand */
    spin_give(&c->tulip_lock);
    if (st & 1u) net_rx_notify(); /* 送り終えた：網の層が持っているものの場所ができた */
    return IRQ_HANDLED;
}

/* 受信は割り込みの処理のもの（net_rx）なので、網の層が poll するものは無い。 */
static size_t tulip_poll(struct netif *self, unsigned queue, size_t budget, net_input_fn input) {
    (void)self; (void)queue; (void)budget; (void)input;
    return 0;
}

static bool tulip_rx_pending(struct netif *self, unsigned queue) {
    (void)self; (void)queue;
    return false;
}

/* tulip_mutex の下、動いている装置で。 */
static unsigned tx_post(struct tulip *c, const struct net_tx_frame *frames, unsigned count) {
    unsigned taken = 0;
    while (taken < count) {
        const struct net_tx_frame *f = &frames[taken];
        if (f->length > BUFSZ) { /* 緩衝に入らない：捨てる */
            taken++;
            continue;
        }
        struct tdesc *d = &c->tx[c->tx_cur];
        if (d->status & OWN) break; /* 残りは送り終わりの割り込みを待つ */
        size_t len = 0;
        for (unsigned i = 0; i < f->part_count; i++) {
            memcpy(c->txbuf[c->tx_cur] + len, f->parts[i].data, f->parts[i].length);
            len += f->parts[i].length;
        }
        /* IC、LS、FS と、輪の最後の記述子には TER */
        d->ctl = (uint32_t)len | (1u << 31) | (1u << 30) | (1u << 29) | (c->tx_cur == NTX - 1 ? (1u << 25) : 0);
        d->status = OWN;
        c->tx_cur = (c->tx_cur + 1) % NTX;
        taken++;
    }
    if (taken) csr_wr(c, 1, 0); /* 送信の poll demand、まとめに一度 */
    return taken;
}

/* 止めた装置は一つも受け取らない（網の層が持ち、net_rx_notify の後にまた渡す）。壊れた装置は全部を受け取って捨てる。 */
static unsigned tulip_transmit(struct netif *self, unsigned queue, const struct net_tx_frame *frames, unsigned count) {
    (void)queue;
    struct tulip *c = (struct tulip *)self;
    mutex_lock(&c->tulip_mutex);
    unsigned taken = 0;
    switch (c->state) {
    case TULIP_STOPPED: break;
    case TULIP_RUNNING: taken = tx_post(c, frames, count); break;
    case TULIP_BROKEN: taken = count; break;
    }
    mutex_unlock(&c->tulip_mutex);
    return taken;
}

/* 受信と送信の過程を止める（3.2.2.9：どちらも今のフレームを終えてから止まる）。CSR5 の両方の過程が「止まっている」を
   言えば true（表 3-68、3-69）。眠ってよい thread から。 */
static bool hw_stop(struct tulip *c) {
    csr_wr(c, 6, csr_rd(c, 6) & ~(CSR6_SR | CSR6_ST));
    for (uint64_t until = clock_after(STOP_NS); (csr_rd(c, 5) & CSR5_PROCESSES) && clock_now_ns() < until;)
        sleep_until(clock_after(NS_PER_MS));
    return !(csr_rd(c, 5) & CSR5_PROCESSES);
}

/* 4.3.1：software reset は全部の処理を止め、全部の内の状態と登録を初めに戻す（構成の登録を除く）。持つ記述子は
   無くなる。CSR0 を書くのは両方の過程が止まっている時だけ（表 3-44）：hw_stop の後に呼ぶ。最後に CSR0 へ bus の形を
   書く（4.3.4 の 3）。 */
static void hw_reset(struct tulip *c) {
    csr_wr(c, 0, CSR0_SWR);
    sleep_until(clock_after(RESET_NS));
    csr_wr(c, 0, CSR0_BUS_MODE);
}

/* 両方の輪を最初の記述子から：受信は全部を装置に渡し、送信の最初は設定のフレーム（自分の番地と broadcast の完全な
   filter）。 */
static void rings_init(struct tulip *c) {
    for (int i = 0; i < NRX; i++) {
        c->rx[i].buf1 = c->buffers_bus + i * BUFSZ;
        c->rx[i].buf2 = 0;
        c->rx[i].ctl = BUFSZ | (i == NRX - 1 ? (1u << 25) : 0); /* RER */
        c->rx[i].status = OWN;
    }
    for (int i = 0; i < NTX; i++) {
        c->tx[i].buf1 = c->buffers_bus + (NRX + i) * BUFSZ;
        c->tx[i].buf2 = 0;
        c->tx[i].ctl = i == NTX - 1 ? (1u << 25) : 0; /* TER */
        c->tx[i].status = 0;
    }
    c->rx_cur = 0;
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
}

/* 4.3.4 の手順で装置を初めから動かす。attach と resume が tulip_mutex の下で呼ぶ。過程が止まらなければ false。状態を
   動いているにしてから割り込みの覆いを外す：その後の割り込みは処理が答える。 */
static bool tulip_start(struct tulip *c) {
    if (!hw_stop(c)) return false;
    hw_reset(c);
    rings_init(c);
    csr_wr(c, 3, c->rx_ring_bus);
    csr_wr(c, 4, c->tx_ring_bus);
    uint64_t f = spin_lock(&c->tulip_lock);
    c->state = TULIP_RUNNING;
    csr_wr(c, 5, CSR5_STATUS);
    csr_wr(c, 7, (1u << 16) | (1u << 15) | (1u << 6) | (1u << 7) | 1u); /* NIS、AIS、RI、RU、TI */
    spin_unlock(&c->tulip_lock, f);
    /* store-and-forward、送受信の始め、全二重、promiscuous（フレームはソフトウェアで選ぶ） */
    csr_wr(c, 6, (1u << 21) | CSR6_ST | (1u << 9) | (1u << 6) | CSR6_SR);
    csr_wr(c, 1, 0);
    csr_wr(c, 2, 0);
    return true;
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
    if (!hw_stop(c)) { /* firmware が動かしたままのものを止める */
        kprintf("net: tulip does not stop\n");
        r = -N_ERR_IO;
        goto fail;
    }
    hw_reset(c);

    int alen = (eeprom_read(c, 0xFF, 8) & 0x40000) ? 8 : 6;
    uint8_t ee[128];
    for (int i = 0; i < 64; i++) {
        uint32_t w = eeprom_read(c, i, alen);
        ee[i * 2] = (uint8_t)w;
        ee[i * 2 + 1] = (uint8_t)(w >> 8);
    }
    /* DEC の形の SROM は番地を 20 バイト目に置く。古い板は最初に置く */
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

    /* 記述子とそれが指す緩衝を、4 GiB より下の一つの塊に：記述子と緩衝の指は 32 bit しかない */
    uint64_t bus;
    uint8_t *block = hal_dma_alloc(d, PAGE_SIZE + (NRX + NTX) * BUFSZ, 32, &bus);
    if (!block) {
        kprintf("net: tulip DMA allocation failed\n");
        goto fail;
    }
    c->rx = (struct tdesc *)block;
    c->tx = c->rx + NRX;
    c->rx_ring_bus = (uint32_t)bus;
    c->tx_ring_bus = (uint32_t)(bus + NRX * sizeof(struct tdesc));
    c->buffers_bus = (uint32_t)(bus + PAGE_SIZE);
    for (int i = 0; i < NRX; i++) c->rxbuf[i] = block + PAGE_SIZE + i * BUFSZ;
    for (int i = 0; i < NTX; i++) c->txbuf[i] = block + PAGE_SIZE + (NRX + i) * BUFSZ;

    if (!hal_irq_enable(d, &(struct irq_spec){.name = "tulip", .handler = tulip_irq, .context = c}, 1, NULL)) {
        kprintf("net: tulip has no interrupt route\n");
        r = -N_ERR_UNSUPPORTED;
        goto fail;
    }
    mutex_lock(&c->tulip_mutex);
    bool started = tulip_start(c);
    mutex_unlock(&c->tulip_mutex);
    if (!started) {
        kprintf("net: tulip does not stop\n");
        r = -N_ERR_IO;
        goto fail;
    }
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
    hal_irq_disable(d); /* 処理の記憶が去る前に */
    d->driver_data = NULL;
    kfree(c);
    return r;
}

/* 割り込みを覆い、両方の過程を止め（3.2.2.9、表 3-68、3-69）、software reset で全部の状態を初めに戻す（4.3.1）。過程が
   止まらなければ CSR0 は書けない（表 3-44）：止められなかった。三つの目標で同じ。 */
static bool tulip_suspend(struct hal_device *d, enum hal_power_target target) {
    (void)target;
    struct tulip *c = d->driver_data;
    mutex_lock(&c->tulip_mutex);
    uint64_t f = spin_lock(&c->tulip_lock);
    c->state = TULIP_STOPPED;
    csr_wr(c, 7, 0);
    csr_wr(c, 5, CSR5_STATUS); /* 立っていた INTx を下ろす：処理はもう答えない */
    spin_unlock(&c->tulip_lock, f);
    bool stopped = hw_stop(c);
    if (stopped) hw_reset(c);
    mutex_unlock(&c->tulip_mutex);
    if (!stopped) kprintf("net: tulip at %s does not stop\n", d->location);
    return stopped;
}

/* 電源を失った装置も保った装置も、4.3.4 の手順で初めから動かす。網の層が持っていたフレームを渡し直させる。 */
static bool tulip_resume(struct hal_device *d) {
    struct tulip *c = d->driver_data;
    mutex_lock(&c->tulip_mutex);
    bool started = tulip_start(c);
    if (!started) {
        uint64_t f = spin_lock(&c->tulip_lock);
        c->state = TULIP_BROKEN;
        spin_unlock(&c->tulip_lock, f);
    }
    mutex_unlock(&c->tulip_mutex);
    net_rx_notify();
    if (started) return true;
    kprintf("net: tulip at %s does not stop\n", d->location);
    hal_failed(d, -N_ERR_IO);
    return false;
}

/* 21140：Hyper-V の従来の網の装置。21143：QEMU。 */
static const struct n_hal_rule tulip_rules[] = {N_PCI_ID(0x1011, 0x0009), N_PCI_ID(0x1011, 0x0019)};
const struct hal_driver tulip_driver = {
    .name = "tulip",
    .rules = tulip_rules,
    .rule_count = ARRAY_SIZE(tulip_rules),
    .attach = tulip_attach,
    .suspend = tulip_suspend,
    .resume = tulip_resume,
    .after_sleep = HAL_AFTER_SLEEP_RESUME,
};
