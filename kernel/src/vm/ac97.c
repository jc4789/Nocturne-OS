/* Intel AC'97 の音（ICH とその互換。QEMU の -device AC97、VirtualBox の既定）：音の層（dev/audio.h）の出口の一つ、
   48 kHz の stereo。PCM の出力の DMA の機関は、自分の 32 個の緩衝の記述子の輪を、開くときに頼まれた長さで、鳴らして
   いるもの（CIV）から最後の有効なもの（LVI）まで辿る。輪は出口を開いたサービスと分け、サービスが書く。終わり無く
   辿らせるため、LVI は CIV の一つ後ろに置く：どの記述子も機関が鳴らし終えると割り込みを頼み（IOC）、それが LVI を
   進めてサービスに見よと言う。割り込みの線の分からない板は、代わりに各緩衝の終わりに alarm を受ける。
   登録と bit は Intel の「I/O Controller Hub 7 (ICH7) High Definition Audio / AC'97 Programmer's Reference Manual」の
   2.2（AC '97 Audio I/O Space）のもの。 */
#include "kernel.h"
#include "dev/audio.h"
#include "dev/pci.h"
#include "hal/hal.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/spin.h"
#include "mm/heap.h"
#include "mm/shared.h"
#include "sys/alarm.h"
#include "sys/clock.h"
#include "sys/object.h"
#include "sys/wait.h"

/* 混合器の登録（NAM、BAR0） */
#define NAM_RESET      0x00
#define NAM_MASTER     0x02
#define NAM_PCM_OUT    0x18
#define NAM_EXT_ID     0x28
#define NAM_EXT_CTRL   0x2A
#define NAM_FRONT_RATE 0x2C
/* bus master の登録（NABM、BAR1）：PCM の出力の箱、全体の制御 */
#define PO_BDBAR  0x10
#define PO_CIV    0x14
#define PO_LVI    0x15
#define PO_SR     0x16
#define PO_PICB   0x18 /* 今の記述子に残る 16 bit の標本 */
#define PO_CR     0x1B
#define GLOB_CNT  0x2C
#define GLOB_STA  0x30

#define CR_RUN     0x01 /* RPBM（2.2.7） */
#define CR_RESET   0x02 /* RR（2.2.7）：RPBM が 0 の時だけ立てる */
#define CR_IOCE    0x10 /* IOC を頼む記述子の終わりに割り込む（2.2.7） */
#define SR_HALTED  0x01 /* DCH（2.2.4）：止めの bit が 0 で DMA の機関が休んでいる */
#define SR_CLEAR   0x1C /* 1 を書いて消す（2.2.4）：最後の有効な緩衝、終わり、FIFO の誤り */
#define BD_IOC     0x8000 /* 記述子の旗：終わりに割り込む */
#define GLOB_COLD_RESET_OFF 0x2 /* AC '97 Cold Reset#（2.2.8）：1 で cold reset の外 */
#define GLOB_S0_READY 0x100     /* ACZ_SDIN0 Codec Ready（2.2.9） */

#define ADDRESS_BITS 32  /* 機関が届く番地の幅：BDBAR と記述子の番地は 32 bit（2.2.1） */
#define NBUF       32    /* 輪の記述子の数 */
#define RATE       48000
#define FRAME      4     /* バイト：16 bit の標本二つ */
#define BUF_MOST   32766 /* 記述子の frame の上限：長さは標本の 16 bit で、最後は偶数 */
/* codec の ready を待つ限り。2.2.9 は bus master を始める前に見よと言い、時の限りを言わない。 */
#define READY_NS (100 * NS_PER_MS)
/* 止めの bit を 0 にしてから DCH を待つ限り。機関は今の取り込みを終えて休む（2.2.4）。 */
#define HALT_NS (10 * NS_PER_MS)

struct bd {
    uint32_t addr;
    uint16_t samples; /* frame でなく 16 bit の標本 */
    uint16_t flags;
} PACKED;

/* 一つの codec。
   錠の順：音の層の mutex → ac97_mutex → engine。HAL の DMA の錠（hal_dma_map・hal_dma_unmap、眠る）は音の層の mutex の
   後、ac97_mutex の外で取る。
   ac97_mutex：音の層の呼出し（open、close、run、status。層が一つずつ呼ぶ）と、suspend・resume を一つずつにする。
   generation、輪の大きさと場所、played、last_civ、last_at、ring_object はこの下。
   engine：割り込みの処理と alarm（どちらもどの CPU でも）と、呼出しの間で、LVI と CR の書込みと、ready と running。
   ready と running は両方の錠の下で書き、どちらかの下で読む。葉。 */
struct ac97 {
    struct audio_output output; /* 最初：音の層が返してくる */
    struct mutex ac97_mutex;
    struct spinlock engine;
    bool ready;   /* codec と PCM の出力の箱が整っている（suspend から resume までは false：登録に触らない） */
    bool running; /* 機関が鳴らしている */
    uint32_t generation;
    uint16_t nam, nabm;
    struct bd *bdl_view;
    uint32_t bdl_bus;
    bool has_irq;        /* 割り込みの線がある。無ければ alarm が各緩衝の終わりに埋め直す */
    struct alarm refill; /* 鳴っている緩衝の終わり */
    struct hal_device *hal;
    uint32_t buf_frames; /* 開いたものの */
    uint64_t buf_ns;
    uint64_t played; /* 機関が始めてから終えた緩衝の frame */
    int last_civ;
    uint64_t last_at;
    struct object *ring_object; /* 開いた輪：音の層の共有 object。HAL を通して機関へ渡してある */
    uint64_t ring_phys, ring_pages;
};

static struct ac97 *ac97_of(struct audio_output *output) { return (struct ac97 *)output; }

/* engine の下：LVI を鳴っているものの一つ後ろに置き、機関がそこへ着かないようにする。 */
static void ring_keep(struct ac97 *c) {
    outb(c->nabm + PO_LVI, (uint8_t)((inb(c->nabm + PO_CIV) + NBUF - 1) & (NBUF - 1)));
}

/* 機関の割り込み：緩衝が鳴り終えた。状態の bit にここで答えるので、この後の機関の動きはまた割り込む。線はほかの装置の
   ものでもありうる：その割り込みはこの機関の状態の bit を見つけない。 */
static enum irq_result ac97_irq(void *context) {
    struct ac97 *c = context;
    spin_take(&c->engine);
    if (!c->ready) { /* 止めた機関は割り込まない：共有の線のほかの装置のもの */
        spin_give(&c->engine);
        return IRQ_NONE;
    }
    uint16_t status = inw(c->nabm + PO_SR) & SR_CLEAR;
    if (status) {
        outw(c->nabm + PO_SR, status);
        if (c->running) ring_keep(c);
    }
    spin_give(&c->engine);
    if (!status) return IRQ_NONE;
    audio_output_period(&c->output);
    return IRQ_HANDLED;
}

static void ac97_refill(struct alarm *a, struct regs *r) {
    (void)r;
    struct ac97 *c = (struct ac97 *)((char *)a - __builtin_offsetof(struct ac97, refill));
    uint64_t f = spin_lock(&c->engine);
    bool on = c->ready && c->running;
    if (on) ring_keep(c);
    spin_unlock(&c->engine, f);
    if (on) alarm_set(&c->refill, clock_after(c->buf_ns));
    audio_output_period(&c->output);
}

/* engine の下：機関を止める（RPBM を 0、割り込みの許しも 0）。立っていた状態に答え、線を下ろす。 */
static void engine_pause(struct ac97 *c) {
    c->running = false;
    outb(c->nabm + PO_CR, 0);
    outw(c->nabm + PO_SR, SR_CLEAR);
}

/* ac97_mutex の下、engine_pause の後：DMA の機関が休んだと DCH が言うまで待つ（2.2.4）。眠る。 */
static bool engine_halted(struct ac97 *c) {
    for (uint64_t until = clock_after(HALT_NS); !(inw(c->nabm + PO_SR) & SR_HALTED) && clock_now_ns() < until;)
        sleep_until(clock_after(NS_PER_MS / 10));
    return inw(c->nabm + PO_SR) & SR_HALTED;
}

/* ac97_mutex の下、止まった機関で：PCM の出力の箱の bus master の登録を初めに戻し（RR、2.2.7）、記述子を渡し直す。 */
static bool box_reset(struct ac97 *c) {
    outb(c->nabm + PO_CR, CR_RESET);
    for (uint64_t until = clock_after(HALT_NS); (inb(c->nabm + PO_CR) & CR_RESET) && clock_now_ns() < until;)
        sleep_until(clock_after(NS_PER_MS / 10));
    bool reset = !(inb(c->nabm + PO_CR) & CR_RESET);
    outl(c->nabm + PO_BDBAR, c->bdl_bus);
    return reset;
}

/* ac97_mutex の下：鳴っていれば止め、箱を初めに戻す。 */
static int engine_stop(struct ac97 *c) {
    if (!c->has_irq) alarm_cancel(&c->refill);
    uint64_t f = spin_lock(&c->engine);
    bool ready = c->ready;
    if (ready) engine_pause(c);
    else c->running = false;
    spin_unlock(&c->engine, f);
    if (!ready) return 0;
    return engine_halted(c) && box_reset(c) ? 0 : -N_ERR_IO;
}

static void ac97_status(struct audio_output *output, struct n_audio_device_status *status) {
    struct ac97 *c = ac97_of(output);
    mutex_lock(&c->ac97_mutex);
    status->generation = c->generation;
    status->running = c->running;
    status->at_ns = clock_now_ns();
    if (!c->running) {
        mutex_unlock(&c->ac97_mutex);
        return;
    }
    /* PICB と CIV は別の登録：後に読んだ CIV が同じなら PICB はその CIV のもの。読む間に記述子を一つ越えるのは普通：
       新しい CIV でもう一度。 */
    uint64_t f = spin_lock(&c->engine);
    int confirmed = inb(c->nabm + PO_CIV) & (NBUF - 1);
    int civ = confirmed;
    unsigned remaining = 0;
    uint64_t at = status->at_ns;
    for (int tries = 0; tries < 2; tries++) {
        civ = confirmed;
        remaining = inw(c->nabm + PO_PICB);
        at = clock_now_ns();
        confirmed = inb(c->nabm + PO_CIV) & (NBUF - 1);
        if (confirmed == civ) break;
    }
    spin_unlock(&c->engine, f);
    uint64_t moved = (uint64_t)((civ + NBUF - c->last_civ) & (NBUF - 1));
    uint64_t late = (at - c->last_at) / c->buf_ns;
    if (late > moved + NBUF / 2) moved += (late - moved + NBUF / 2) / NBUF * NBUF; /* 誰も見ない間の輪の周回 */
    c->played += moved * c->buf_frames;
    c->last_civ = civ;
    c->last_at = at;
    if (remaining > c->buf_frames * 2) remaining = c->buf_frames * 2;
    status->position = c->played + c->buf_frames - remaining / 2;
    status->at_ns = at;
    status->guard_frames = remaining / 2 + c->buf_frames; /* 機関はその後の記述子を取り込んである */
    mutex_unlock(&c->ac97_mutex);
}

static int ac97_open(struct audio_output *output, const struct n_audio_device_format *format, struct audio_ring *ring) {
    struct ac97 *c = ac97_of(output);
    (void)format; /* 申告した一つ */
    uint32_t frames = MAX(MIN(ring->block_frames, BUF_MOST), 2u);
    uint64_t pages = ALIGN_UP((uint64_t)frames * FRAME * NBUF, PAGE_SIZE) / PAGE_SIZE, phys, bus;
    /* 輪は音の層とサービスも持つ共有 object：機関が届く番地の中に取り、開いている間 HAL を通して渡す */
    struct object *memory = shared_kernel_contig(pages, ADDRESS_BITS, &phys);
    if (!memory) return -N_ERR_NO_MEMORY;
    if (!hal_dma_map(c->hal, phys, pages, &bus)) {
        object_unref(memory);
        return -N_ERR_NO_MEMORY;
    }
    mutex_lock(&c->ac97_mutex);
    for (uint32_t i = 0; i < NBUF; i++) {
        c->bdl_view[i].addr = (uint32_t)(bus + (uint64_t)i * frames * FRAME);
        c->bdl_view[i].samples = (uint16_t)(frames * 2);
        c->bdl_view[i].flags = BD_IOC;
    }
    c->buf_frames = frames;
    c->buf_ns = frames * NS_PER_S / RATE;
    c->ring_object = memory;
    c->ring_phys = phys;
    c->ring_pages = pages;
    mutex_unlock(&c->ac97_mutex);
    *ring = (struct audio_ring){.memory = memory, .block_frames = frames, .blocks = NBUF};
    return 0;
}

/* 止まった機関から始める：最初の記述子から鳴らし、最後の記述子は機関が進むまで有効。眠りの間（ready が false）は
   始められない：resume の周期の知らせが、また見る時を言う。 */
static int engine_start(struct ac97 *c) {
    uint64_t f = spin_lock(&c->engine);
    bool ready = c->ready;
    spin_unlock(&c->engine, f);
    if (!ready) return -N_ERR_NOT_READY;
    if (!box_reset(c)) return -N_ERR_IO;
    c->played = 0;
    c->last_civ = 0;
    c->last_at = clock_now_ns();
    f = spin_lock(&c->engine);
    outb(c->nabm + PO_LVI, NBUF - 1);
    outb(c->nabm + PO_CR, c->has_irq ? CR_RUN | CR_IOCE : CR_RUN);
    c->running = true;
    spin_unlock(&c->engine, f);
    if (!c->has_irq) alarm_set(&c->refill, clock_after(c->buf_ns));
    return 0;
}

static int ac97_run(struct audio_output *output, bool run) {
    struct ac97 *c = ac97_of(output);
    mutex_lock(&c->ac97_mutex);
    int r = 0;
    if (run != c->running) r = run ? engine_start(c) : engine_stop(c);
    mutex_unlock(&c->ac97_mutex);
    return r;
}

/* ほかの出口が鳴らす：機関は居る所で止まる。 */
static void ac97_close(struct audio_output *output) {
    struct ac97 *c = ac97_of(output);
    mutex_lock(&c->ac97_mutex);
    engine_stop(c);
    struct object *memory = c->ring_object;
    uint64_t phys = c->ring_phys, pages = c->ring_pages;
    c->ring_object = NULL;
    mutex_unlock(&c->ac97_mutex);
    hal_dma_unmap(c->hal, phys, pages); /* 機関は止まり、もう輪を読まない */
    object_unref(memory);
}

/* codec を cold reset の外へ出して ready を待ち（2.2.8、2.2.9）、0 dB と 48 kHz にし、PCM の出力の箱を初めに戻す。
   電源を失った制御器では Cold Reset# が 0 に戻っているのでここで codec が初めから立ち上がる。電源を保った制御器では
   Cold Reset# は 1 のまま（2.2.8）で、混合器の値を書き直すだけになる。attach と resume が ac97_mutex の下で呼ぶ。眠る。 */
static bool codec_start(struct ac97 *c) {
    outl(c->nabm + GLOB_CNT, GLOB_COLD_RESET_OFF); /* 割り込みの許しは 0 */
    for (uint64_t until = clock_after(READY_NS); !(inl(c->nabm + GLOB_STA) & GLOB_S0_READY) && clock_now_ns() < until;)
        sleep_until(clock_after(NS_PER_MS));
    if (!(inl(c->nabm + GLOB_STA) & GLOB_S0_READY)) return false;
    outw(c->nam + NAM_RESET, 1);
    outw(c->nam + NAM_MASTER, 0x0000);  /* 0 dB、消音でない */
    outw(c->nam + NAM_PCM_OUT, 0x0808); /* 0 dB */
    if (inw(c->nam + NAM_EXT_ID) & 1) { /* 可変の rate：48 kHz にする */
        outw(c->nam + NAM_EXT_CTRL, inw(c->nam + NAM_EXT_CTRL) | 1);
        outw(c->nam + NAM_FRONT_RATE, RATE);
    }
    outb(c->nabm + PO_CR, 0); /* RR は RPBM が 0 の時だけ（2.2.7） */
    if (!engine_halted(c) || !box_reset(c)) return false;
    uint64_t f = spin_lock(&c->engine);
    c->ready = true;
    spin_unlock(&c->engine, f);
    return true;
}

static const struct audio_output_ops ac97_ops = {.open = ac97_open, .close = ac97_close, .run = ac97_run, .status = ac97_status};

/* 混合器の登録はポートの BAR 0、bus master のものはポートの BAR 1。 */
static int ac97_attach(struct hal_device *d) {
    struct pci_device *pci = pci_device_of(d);
    uint16_t nam = hal_port(d, 0), nabm = hal_port(d, 1);
    if (!nam || !nabm) return -N_ERR_UNSUPPORTED;
    struct ac97 *c = kzalloc(sizeof *c);
    if (!c) return -N_ERR_NO_MEMORY;
    c->hal = d;
    c->nam = nam;
    c->nabm = nabm;
    d->driver_data = c;
    hal_enable(d, HAL_ENABLE_PORTS | HAL_ENABLE_DMA);
    int r = -N_ERR_NO_MEMORY;

    hal_dma_bits(d, ADDRESS_BITS);
    uint64_t bus;
    c->bdl_view = hal_dma_alloc(d, PAGE_SIZE, &bus);
    if (!c->bdl_view) {
        kprintf("audio: AC'97 found, but no memory it reaches for its descriptors\n");
        goto fail;
    }
    c->bdl_bus = (uint32_t)bus;
    alarm_init(&c->refill, ac97_refill);
    c->has_irq = hal_irq_enable(d, &(struct irq_spec){.name = "AC'97", .handler = ac97_irq, .context = c}, 1, NULL) == 1;
    mutex_lock(&c->ac97_mutex);
    bool started = codec_start(c);
    mutex_unlock(&c->ac97_mutex);
    if (!started) {
        kprintf("audio: AC'97 (%04x): the codec is not ready\n", pci->device);
        r = -N_ERR_IO;
        goto fail;
    }
    c->output.ops = &ac97_ops;
    static const uint32_t rates[] = {RATE};
    static const struct n_audio_size sizes[] = {{16, FRAME / 2}};
    c->output.kind = N_AUDIO_KIND_LINE;
    c->output.jack = N_AUDIO_JACK_NONE;
    c->output.name = "AC'97";
    c->output.key = "ac97";
    c->output.rates = rates;
    c->output.rate_count = 1;
    c->output.sizes = sizes;
    c->output.size_count = 1;
    c->output.channels_low = c->output.channels_high = 2;
    audio_output_add(&c->output);
    hal_offer(d, N_OFFER_AUDIO, 0, "AC'97");
    kprintf("audio: AC'97 (%04x) at ports %#x/%#x, %s, 48 kHz stereo\n", pci->device, nam, nabm,
            c->has_irq ? "interrupt" : "no interrupt (timed refill)");
    return 0;
fail:
    hal_irq_disable(d); /* 処理の記憶が去る前に */
    d->driver_data = NULL;
    kfree(c);
    return r;
}

/* 機関を止め（RPBM を 0、2.2.7）、DMA が休むのを DCH で見る（2.2.4）。輪の位置は失われる：status は新しい世代で
   止まったと言い（dev/audio.h）、サービスは resume の後に始め直す。三つの目標で同じ。 */
static bool ac97_suspend(struct hal_device *d, enum hal_power_target target) {
    (void)target;
    struct ac97 *c = d->driver_data;
    mutex_lock(&c->ac97_mutex);
    if (!c->has_irq) alarm_cancel(&c->refill);
    uint64_t f = spin_lock(&c->engine);
    engine_pause(c);
    c->ready = false;
    spin_unlock(&c->engine, f);
    bool halted = engine_halted(c);
    c->generation++;
    mutex_unlock(&c->ac97_mutex);
    audio_output_period(&c->output);
    if (!halted) kprintf("audio: AC'97 at %s did not halt\n", d->location);
    return halted;
}

/* 電源を失った制御器も保った制御器も codec_start で整える。サービスに見させ、始め直させる。 */
static bool ac97_resume(struct hal_device *d) {
    struct ac97 *c = d->driver_data;
    mutex_lock(&c->ac97_mutex);
    bool started = codec_start(c);
    mutex_unlock(&c->ac97_mutex);
    audio_output_period(&c->output);
    if (started) return true;
    kprintf("audio: AC'97 at %s: the codec is not ready\n", d->location);
    hal_failed(d, -N_ERR_IO);
    return false;
}

static const struct n_hal_rule ac97_rules[] = {N_PCI_ID(0x8086, 0x2415), N_PCI_ID(0x8086, 0x2425), N_PCI_ID(0x8086, 0x2445),
                                               N_PCI_ID(0x8086, 0x2485), N_PCI_ID(0x8086, 0x24C5), N_PCI_ID(0x8086, 0x24D5),
                                               N_PCI_ID(0x8086, 0x266E), N_PCI_ID(0x8086, 0x27DE), N_PCI_ID(0x8086, 0x7195)};
const struct hal_driver ac97_driver = {
    .name = "ac97",
    .rules = ac97_rules,
    .rule_count = ARRAY_SIZE(ac97_rules),
    .attach = ac97_attach,
    .suspend = ac97_suspend,
    .resume = ac97_resume,
    .after_sleep = HAL_AFTER_SLEEP_RESUME,
};
