/* Intel 8254x「e1000」の網の実装（QEMU の既定の NIC、VirtualBox と VMware も）。節の番号は Intel の「PCI/PCI-X Family
   of Gigabit Ethernet Controllers Software Developer's Manual」（317453）のもの。 */
#include "kernel.h"
#include "net/net.h"
#include "sys/clock.h"
#include "sys/wait.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/spin.h"
#include "dev/pci.h"
#include "io/io.h"
#include "mm/heap.h"

#define REG_CTRL  0x0000
#define REG_EERD  0x0014
#define REG_ICR   0x00C0
#define REG_ITR   0x00C4
#define REG_IMS   0x00D0
#define REG_IMC   0x00D8
#define REG_RCTL  0x0100
#define REG_TCTL  0x0400
#define REG_TIPG  0x0410
#define REG_RDBAL 0x2800
#define REG_RDBAH 0x2804
#define REG_RDLEN 0x2808
#define REG_RDH   0x2810
#define REG_RDT   0x2818
#define REG_RDTR  0x2820
#define REG_RADV  0x282C
#define REG_TDBAL 0x3800
#define REG_TDBAH 0x3804
#define REG_TDLEN 0x3808
#define REG_TDH   0x3810
#define REG_TDT   0x3818
#define REG_MTA   0x5200
#define REG_RAL   0x5400
#define REG_RAH   0x5404

#define CTRL_RST          (1u << 26) /* Device Reset（13.4.1） */
#define BUFSZ             2048
#define ICR_TXDW          (1u << 0)
#define ICR_LSC           (1u << 2)
#define ICR_RXDMT0        (1u << 4)
#define ICR_RXO           (1u << 6)
#define ICR_RXT0          (1u << 7)
#define RX_IRQS           (ICR_RXDMT0 | ICR_RXO | ICR_RXT0)
#define DESC_DD           1u
#define RX_EOP            2u
#define ITR_TICK_NS       256u /* ITR の刻み（13.4.18） */
#define ITR_DEFAULT_TICKS 651u /* Intel が勧める 651 から 5580 の低い端 */
/* reset の終わりを待つ限り。13.4.1 は RST を立ててから約 1 µs 後に見よと言い、上の限りを言わない。 */
#define RESET_NS NS_PER_S

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

/* 記述子の輪はそれぞれ一頁：従来の記述子 256 個。装置の求める 16 バイトの基と 128 バイトの輪の長さの揃えを満たす。
   受信の緩衝はこの DMA の記述子で、別の受信の列は無い。 */
#define NRX (PAGE_SIZE / sizeof(struct rx_desc))
#define NTX (PAGE_SIZE / sizeof(struct tx_desc))
_Static_assert(sizeof(struct rx_desc) == 16 && sizeof(struct tx_desc) == 16, "e1000 descriptor ABI");
_Static_assert(PAGE_SIZE % 128 == 0, "e1000 DMA alignment");

/* 装置の状態。e1000_mutex と e1000_lock の両方の下で書き、どちらかの下で読む。 */
enum e1000_state {
    E1000_STOPPED, /* 始める前と、suspend から resume まで：送らず、受けず、登録に触らない */
    E1000_RUNNING,
    E1000_BROKEN, /* resume で戻らなかった：送るものは受け取って捨てる */
};

/* 錠の順：網の層の net_lock（mutex）→ e1000_mutex → e1000_lock。
   e1000_mutex：網の層の呼出し（transmit、poll、rx_pending）と、始めること・suspend・resume を一つずつにする。受信の
   輪は poll、送信の輪は transmit のもので、どちらもこの下で触る。
   e1000_lock：割り込みの処理（どの CPU にも来る）と poll が分ける受信の割り込みの覆いと rx_irq_pending。葉。
   net_rx_notify はこれを放した後に呼ぶ。 */
struct e1000 {
    struct netif nif; /* 最初：self が装置 */
    volatile uint8_t *mmio;
    volatile struct rx_desc *rx;
    volatile struct tx_desc *tx;
    uint8_t *rxbuf[NRX], *txbuf[NTX];
    uint64_t rx_ring_bus, tx_ring_bus;
    unsigned rx_cur, tx_cur, tx_clean;
    struct mutex e1000_mutex;
    struct spinlock e1000_lock;
    enum e1000_state state;
    volatile bool rx_irq_pending;
    bool new_eerd;      /* 82574 の形の EERD */
    uint32_t itr_ticks; /* 0：ITR を持たない 82544GC/EI */
};

static uint32_t rd(struct e1000 *c, uint32_t r) { return mmio_read32(c->mmio + r); }
static void wr(struct e1000 *c, uint32_t r, uint32_t v) { mmio_write32(c->mmio + r, v); }

static uint16_t eeprom_read(struct e1000 *c, uint8_t addr) {
    if (c->new_eerd)
        wr(c, REG_EERD, 1 | ((uint32_t)addr << 2));
    else
        wr(c, REG_EERD, 1 | ((uint32_t)addr << 8));
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
    if (c->state != E1000_RUNNING) { /* 止めた装置は割り込みを覆ってある：共有の INTx の線のほかの装置のもの */
        spin_give(&c->e1000_lock);
        return IRQ_NONE;
    }
    uint32_t icr = rd(c, REG_ICR); /* 読むと答えたことになる */
    if (icr) {
        /* 網の thread がフレームを受け取るまで、DMA の持ち主は実装のまま。その thread が輪を空ける間、受信の割り込みを
           覆う。 */
        if (icr & RX_IRQS) wr(c, REG_IMC, RX_IRQS);
        c->rx_irq_pending = true;
    }
    spin_give(&c->e1000_lock);
    if (icr) net_rx_notify();
    return icr ? IRQ_HANDLED : IRQ_NONE; /* INTx の線は共有かもしれない */
}

static bool e1000_rx_pending(struct netif *self, unsigned queue) {
    (void)queue;
    struct e1000 *c = (struct e1000 *)self;
    mutex_lock(&c->e1000_mutex);
    bool pending = c->state == E1000_RUNNING && (c->rx_irq_pending || (c->rx[c->rx_cur].status & DESC_DD));
    mutex_unlock(&c->e1000_mutex);
    return pending;
}

/* e1000_mutex の下、動いている装置で。 */
static size_t rx_drain(struct e1000 *c, size_t budget, net_input_fn input) {
    wr(c, REG_IMC, RX_IRQS);
    size_t consumed = 0;
    int returned = -1;
    /* 持ち主を返すのはこの取出しの後：下の RDT の一度の書込みまで、装置はここで受け取った記述子を埋め直せない。 */
    while (consumed < budget && (c->rx[c->rx_cur].status & DESC_DD)) {
        /* x86 の一貫した DMA は DD より先にフレームを書く。フレームの読みを volatile の持ち主の確かめより前に
           compiler が動かさないようにする。 */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint16_t len = c->rx[c->rx_cur].len;
        if ((c->rx[c->rx_cur].status & RX_EOP) && !c->rx[c->rx_cur].errors && len <= BUFSZ)
            input(&c->nif, c->rxbuf[c->rx_cur], len, 0);
        c->rx[c->rx_cur].status = 0;
        returned = (int)c->rx_cur;
        c->rx_cur = (c->rx_cur + 1) % NRX;
        consumed++;
    }
    uint64_t flags = spin_lock(&c->e1000_lock);
    c->rx_irq_pending = false;
    if (returned >= 0) {
        __atomic_thread_fence(__ATOMIC_RELEASE);
        wr(c, REG_RDT, (uint32_t)returned); /* 取り出し全部に尾の書込みは一度 */
    }
    wr(c, REG_IMS, RX_IRQS);
    /* 空の確かめと覆いを外す間にフレームが来ることがある。その記述子がもう見えていれば、新しい割り込みを待たない。 */
    bool again = c->rx[c->rx_cur].status & DESC_DD;
    if (again) {
        wr(c, REG_IMC, RX_IRQS);
        c->rx_irq_pending = true;
    }
    spin_unlock(&c->e1000_lock, flags);
    if (again) net_rx_notify();
    return consumed;
}

static size_t e1000_poll(struct netif *self, unsigned queue, size_t budget, net_input_fn input) {
    (void)queue;
    struct e1000 *c = (struct e1000 *)self;
    mutex_lock(&c->e1000_mutex);
    size_t consumed = c->state == E1000_RUNNING ? rx_drain(c, budget, input) : 0;
    mutex_unlock(&c->e1000_mutex);
    return consumed;
}

/* e1000_mutex の下、動いている装置で。 */
static unsigned tx_post(struct e1000 *c, const struct net_tx_frame *frames, unsigned count) {
    while (c->tx_clean != c->tx_cur && (c->tx[c->tx_clean].status & DESC_DD)) c->tx_clean = (c->tx_clean + 1) % NTX;
    unsigned taken = 0, posted = 0;
    while (taken < count) {
        const struct net_tx_frame *f = &frames[taken];
        if (f->length > BUFSZ) { /* 緩衝に入らない：捨てる */
            taken++;
            continue;
        }
        unsigned next = (c->tx_cur + 1) % NTX;
        volatile struct tx_desc *d = &c->tx[c->tx_cur];
        /* TDH == TDT は装置の輪が空だという意味なので、DMA がまだ一つも送り終えていなくても満と空を分けるために記述子を
           一つ残す。 */
        if (next == c->tx_clean || !(d->status & DESC_DD)) break; /* 残りは送り終わりを待つ */
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
        wr(c, REG_TDT, c->tx_cur); /* まとめに尾の書込みは一度 */
    }
    return taken;
}

/* 止めた装置は一つも受け取らない（網の層が持ち、net_rx_notify の後にまた渡す）。壊れた装置は全部を受け取って捨てる。 */
static unsigned e1000_transmit(struct netif *self, unsigned queue, const struct net_tx_frame *frames, unsigned count) {
    (void)queue;
    struct e1000 *c = (struct e1000 *)self;
    mutex_lock(&c->e1000_mutex);
    unsigned taken = 0;
    switch (c->state) {
    case E1000_STOPPED:
        break;
    case E1000_RUNNING:
        taken = tx_post(c, frames, count);
        break;
    case E1000_BROKEN:
        taken = count;
        break;
    }
    mutex_unlock(&c->e1000_mutex);
    return taken;
}

static uint32_t e1000_itr_ticks(void) {
    uint64_t us;
    if (!test_number("e1000-itr-us", &us) || us > (uint64_t)UINT16_MAX * ITR_TICK_NS / 1000) return ITR_DEFAULT_TICKS;
    return (uint32_t)((us * 1000 + ITR_TICK_NS - 1) / ITR_TICK_NS);
}

/* 全部の登録と状態機械を電源の入りの値へ（13.4.1 の RST、14.7 の Global Reset）。PCI の構成は変わらない。割り込みの
   登録も初めの値（覆った）になる。DMA は走らない。終われば true。眠ってよい thread から。 */
static bool hw_reset(struct e1000 *c) {
    wr(c, REG_IMC, 0xFFFFFFFF);
    wr(c, REG_CTRL, rd(c, REG_CTRL) | CTRL_RST);
    clock_spin(NS_PER_US); /* 13.4.1：RST を立ててから約 1 µs、どの登録も見ない */
    for (uint64_t until = clock_after(RESET_NS); (rd(c, REG_CTRL) & CTRL_RST) && clock_now_ns() < until;)
        sleep_until(clock_after(NS_PER_MS));
    return !(rd(c, REG_CTRL) & CTRL_RST);
}

/* reset の後：link、番地、最初の記述子からの両方の輪、割り込みの頻度。割り込みの覆いは外さない（e1000_start）。 */
static void hw_start(struct e1000 *c) {
    uint32_t ctrl = rd(c, REG_CTRL);
    ctrl |= (1u << 6) | (1u << 5);                 /* SLU、ASDE */
    ctrl &= ~((1u << 3) | (1u << 31) | (1u << 7)); /* LRST、PHY_RST、ILOS */
    wr(c, REG_CTRL, ctrl);
    const uint8_t *mac = c->nif.mac;
    wr(c, REG_RAL, mac[0] | mac[1] << 8 | mac[2] << 16 | (uint32_t)mac[3] << 24);
    wr(c, REG_RAH, mac[4] | mac[5] << 8 | 0x80000000u);
    for (int i = 0; i < 128; i++) wr(c, REG_MTA + i * 4, 0);
    for (size_t i = 0; i < NRX; i++) c->rx[i].status = 0;
    for (size_t i = 0; i < NTX; i++) {
        c->tx[i].status = 1; /* DD：空き */
        c->tx[i].cmd = 0;
    }
    c->rx_cur = c->tx_cur = c->tx_clean = 0;
    c->rx_irq_pending = false;
    wr(c, REG_RDBAL, (uint32_t)c->rx_ring_bus);
    wr(c, REG_RDBAH, (uint32_t)(c->rx_ring_bus >> 32));
    wr(c, REG_RDLEN, NRX * sizeof *c->rx);
    wr(c, REG_RDH, 0);
    wr(c, REG_RDT, NRX - 1);
    wr(c, REG_RCTL, (1u << 1) | (1u << 15) | (1u << 26)); /* EN、BAM、SECRC。2048 バイトの緩衝 */
    wr(c, REG_TDBAL, (uint32_t)c->tx_ring_bus);
    wr(c, REG_TDBAH, (uint32_t)(c->tx_ring_bus >> 32));
    wr(c, REG_TDLEN, NTX * sizeof *c->tx);
    wr(c, REG_TDH, 0);
    wr(c, REG_TDT, 0);
    wr(c, REG_TCTL, (1u << 1) | (1u << 3) | (15u << 4) | (64u << 12)); /* EN、PSP、CT、COLD */
    wr(c, REG_TIPG, 10 | (8 << 10) | (6 << 20));
    /* Intel は RDTR・RADV でなく ITR を勧める（受信の遅れの errata）。82544GC/EI（0x100C）は ITR を持たない。 */
    wr(c, REG_RDTR, 0);
    if (c->itr_ticks) {
        wr(c, REG_RADV, 0);
        wr(c, REG_ITR, c->itr_ticks);
    }
}

/* 装置を reset から動かす。attach と resume が e1000_mutex の下で呼ぶ。状態を動いているにしてから覆いを外す：その後の
   割り込みは処理が答える。 */
static bool e1000_start(struct e1000 *c) {
    if (!hw_reset(c)) return false;
    hw_start(c);
    uint64_t f = spin_lock(&c->e1000_lock);
    c->state = E1000_RUNNING;
    rd(c, REG_ICR);
    wr(c, REG_IMS, ICR_TXDW | ICR_LSC | RX_IRQS);
    spin_unlock(&c->e1000_lock, f);
    return true;
}

static int e1000_attach(struct io_device *d) {
    struct pci_device *pci = pci_device_of(d);
    if (!io_resource(d, N_HAL_MEMORY, 0)) return -N_ERR_UNSUPPORTED;
    struct e1000 *c = kzalloc(sizeof *c);
    if (!c) return -N_ERR_NO_MEMORY;
    d->driver_data = c;
    int r = -N_ERR_NO_MEMORY;
    c->new_eerd = pci->device == 0x10D3;
    if (!(c->mmio = io_map(d, 0))) goto fail;
    io_enable(d, IO_ENABLE_MEMORY | IO_ENABLE_DMA);
    if (!hw_reset(c)) { /* firmware が動かしたままのものを止める */
        kprintf("net: e1000 does not come out of reset\n");
        r = -N_ERR_IO;
        goto fail;
    }

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

    /* 制御器が出す番地は全部 64 bit（2.4。手引きの機種で 64 bit の番地を持たないのは 82547GI/EI だけで、規則に無い）。
       輪と、それが指す緩衝：一つずつの塊で、緩衝は続けて */
    io_dma_bits(d, 64);
    uint64_t ring_bus, buffer_bus;
    uint8_t *rings = io_dma_alloc(d, 2 * PAGE_SIZE, &ring_bus);
    uint8_t *buffers = io_dma_alloc(d, (NRX + NTX) * BUFSZ, &buffer_bus);
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

    if (!io_irq_enable(d, &(struct irq_spec){.name = "e1000", .handler = e1000_irq, .context = c}, 1, NULL)) {
        kprintf("net: e1000 has no interrupt route\n");
        r = -N_ERR_UNSUPPORTED;
        goto fail;
    }
    mutex_lock(&c->e1000_mutex);
    bool started = e1000_start(c);
    mutex_unlock(&c->e1000_mutex);
    if (!started) {
        kprintf("net: e1000 does not come out of reset\n");
        r = -N_ERR_IO;
        goto fail;
    }
    c->nif.driver = "e1000";
    c->nif.tx_queues = c->nif.rx_queues = 1;
    c->nif.transmit = e1000_transmit;
    c->nif.rx_pending = e1000_rx_pending;
    c->nif.poll = e1000_poll;
    net_register(d, &c->nif);
    net_rx_notify(); /* 載る前に受けたフレームを網の thread に見させる */
    kprintf("net: e1000 (%04x) mac %02x:%02x:%02x:%02x:%02x:%02x\n", pci->device, mac[0], mac[1], mac[2], mac[3],
            mac[4], mac[5]);
    return 0;
fail:
    io_irq_disable(d); /* 処理の記憶が去る前に */
    d->driver_data = NULL;
    kfree(c);
    return r;
}

/* 6.3.1.4：D3 の前に、送受信を止め、進行中のバスの取引を終わらせる。受信と送信を止め（13.4.22、13.4.33）、reset で
   全部の状態機械を初めに戻す（13.4.1：PCI の reset と同じ、構成の空間を除く）。輪に残った送るフレームはここで落ちる
   （TCP が送り直す）。三つの目標で同じ。 */
static bool e1000_suspend(struct io_device *d, enum io_power_target target) {
    (void)target;
    struct e1000 *c = d->driver_data;
    mutex_lock(&c->e1000_mutex);
    uint64_t f = spin_lock(&c->e1000_lock);
    c->state = E1000_STOPPED;
    wr(c, REG_IMC, 0xFFFFFFFF);
    rd(c, REG_ICR); /* 立っていた INTx を下ろす：処理はもう答えない */
    spin_unlock(&c->e1000_lock, f);
    wr(c, REG_RCTL, 0);
    wr(c, REG_TCTL, 0);
    bool stopped = hw_reset(c);
    mutex_unlock(&c->e1000_mutex);
    if (!stopped) kprintf("net: e1000 at %s does not come out of reset\n", d->location);
    return stopped;
}

/* 電源を失った装置も保った装置も、reset から始め直す。網の層が持っていたフレームを渡し直させる。 */
static bool e1000_resume(struct io_device *d) {
    struct e1000 *c = d->driver_data;
    mutex_lock(&c->e1000_mutex);
    bool started = e1000_start(c);
    if (!started) {
        uint64_t f = spin_lock(&c->e1000_lock);
        c->state = E1000_BROKEN;
        spin_unlock(&c->e1000_lock, f);
    }
    mutex_unlock(&c->e1000_mutex);
    net_rx_notify();
    if (started) return true;
    kprintf("net: e1000 at %s does not come out of reset\n", d->location);
    io_failed(d, -N_ERR_IO);
    return false;
}

static const struct n_hal_rule e1000_rules[] = {N_PCI_ID(0x8086, 0x100E), N_PCI_ID(0x8086, 0x100F),
                                                N_PCI_ID(0x8086, 0x1004), N_PCI_ID(0x8086, 0x100C),
                                                N_PCI_ID(0x8086, 0x1015), N_PCI_ID(0x8086, 0x10D3)};
const struct io_driver e1000_driver = {
    .name = "e1000",
    .rules = e1000_rules,
    .rule_count = ARRAY_SIZE(e1000_rules),
    .attach = e1000_attach,
    .suspend = e1000_suspend,
    .resume = e1000_resume,
    .after_sleep = IO_AFTER_SLEEP_RESUME,
};
