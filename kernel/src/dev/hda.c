/* Intel High Definition Audio の制御器（High Definition Audio Specification 1.0a。節の番号はこれのもの）。この類の PCI
   の機能は、作り手を問わず（chipset のものも graphics card のものも）制御器である。それぞれ link を上げ、codec を読み
   （dev/hda_codec.c）、codec の出口ごとに、変換器が申告する形式で音の層（dev/audio.h）の出口を一つ作る。

   命令（verb）は CORB で送り、答えは RIRB で一つずつ受ける（4.4）。送った者が link の frame ごとに眠って答えを見に行く
   （一度に一つの thread が自分の答えを待つ）。RIRB の割り込みは、pin が何かを告げたことを制御器の thread に知らせる
   だけである（4.4.1、7.3.3.14）。

   制御器は一度に一つの出口を、一つの出力の stream（無ければ双方向のもの）で鳴らす。stream は同じ大きさの区画の輪を
   終わりなく歩く（3.6.2）。輪は出口を開いたサービスと共有し、サービスが書く。区画ごとに割り込みを求め、それでサービスに
   見るよう知らせる（audio_output_period）。輪の中身はここでは見ない。

   位置は、link が送ったと LPIB が言う所から pin の遅れを引いたもので、時計はその直後に engine の下で読む。stream の
   始まりからの frame の数は回り込まない：輪の一周を見落としても数えすぎても、数のどこに frame があるかが変わるだけで、
   link が輪のどの frame にいるかは変わらない。時計は、とても遅れて来た読みが何周を見落としたかだけを言う。

   機能のメモリーはカーネルのもので、カーネルが動かす全部の機能と同じく IOMMU を訳されずに通る。graphics card の音の
   機能は、アプリが動かす画面の機能（dev/pciuser.c）と同じ隔離の組にいることがある。そのときは claim の客
   （iommu_guest）になる：組が持ち主を変える間は止まり、その後は claim の domain に写したメモリーで動き直す。

   電源（hal/hal.h）。suspend は stream を止め（輪の中身は失われ、生成が進む）、codec の function group を D3 にし、
   link を reset に入れる（4.5.8.2）。resume は link を reset から上げ、thread が codec の設定を初めから戻す（4.5.8.1：
   D3 から D0 に戻った codec の設定は残っていると思わない）。開いている出口の道は次の run が引き直す。

   錠の順：claim と IOMMU の mutex → state → 音の層の mutex → stream → verb_lock → engine。
   state：制御器の全体（probe、claim の客で止まっていること、感知）。制御器の thread、claim の客の stop と start、
   suspend と resume が持つ。
   stream：開いている出口と stream。音の層の呼出し（audio_output_ops）、claim の客の stop と start、suspend と resume が
   持つ。link を上げ下げするのは state と stream の両方を持つ者だけなので、どちらか一つを持つ者の間は link は変わらない。
   命令を送るのも、どちらか一つを持つ者だけである。
   engine：CORB と RIRB の位置、命令の答え、pin の告げ、LPIB と時計の対。葉で、下ではレジスターにしか触れない。 */
#include "dev/audio.h"
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/spin.h"
#include "dev/hda_codec.h"
#include "dev/iommu.h"
#include "dev/pci.h"
#include "hal/hal.h"
#include "mm/heap.h"
#include "mm/shared.h"
#include "sys/alarm.h"
#include "sys/clock.h"
#include "sys/object.h"
#include "sys/thread.h"
#include "sys/wait.h"

/* 全体のレジスター（3.3） */
#define GCAP      0x00
#define GCTL      0x08
#define WAKEEN    0x0C
#define STATESTS  0x0E
#define INTCTL    0x20
#define INTSTS    0x24
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP    0x48
#define CORBRP    0x4A
#define CORBCTL   0x4C
#define CORBSIZE  0x4E
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP    0x58
#define RINTCNT   0x5A
#define RIRBCTL   0x5C
#define RIRBSTS   0x5D
#define RIRBSIZE  0x5E
#define SD_FIRST  0x80
#define SD_STRIDE 0x20

#define GCAP_OSS(g) (((g) >> 12) & 15)
#define GCAP_ISS(g) (((g) >> 8) & 15)
#define GCAP_BSS(g) (((g) >> 3) & 31)
#define GCTL_CRST   (1u << 0)
#define GCTL_UNSOL  (1u << 8)
#define INT_GLOBAL  (1u << 31)
#define INT_CONTROLLER (1u << 30)
#define CORBCTL_RUN (1u << 1)
#define CORBRP_RST  (1u << 15)
#define RIRBWP_RST  (1u << 15)
#define RIRBCTL_INT (1u << 0)
#define RIRBCTL_DMA (1u << 1)
#define RIRBSTS_ALL 0x05 /* 答えの割り込みと溢れ */

/* stream の descriptor のレジスター（3.3.35 から） */
#define SD_CTL   0x00
#define SD_CTL2  0x02 /* 三つ目の byte：stripe、優先、向き、stream の番号 */
#define SD_STS   0x03
#define SD_LPIB  0x04
#define SD_CBL   0x08
#define SD_LVI   0x0C
#define SD_FIFOS 0x10
#define SD_FMT   0x12
#define SD_BDPL  0x18
#define SD_BDPU  0x1C
#define CTL_SRST (1u << 0)
#define CTL_RUN  (1u << 1)
#define CTL_IOCE (1u << 2)
#define CTL_FEIE (1u << 3)
#define CTL_DEIE (1u << 4)
#define CTL2_DIR (1u << 3)
#define CTL2_TAG(tag) ((uint8_t)((tag) << 4))
#define STS_BCIS  (1u << 2)
#define STS_FIFOE (1u << 3)
#define STS_DESE  (1u << 4)

#define POLL_NS        NS_PER_S           /* 自分で告げない jack を見る間 */
#define VERB_NS        (10 * NS_PER_MS)   /* これだけ答えない codec は去った（link の次の reset まで） */
#define RESET_NS       (100 * NS_PER_MS)  /* 制御器が reset を出る・入るのに与える一番長い時間 */
#define STOP_NS        NS_PER_MS          /* stream と輪の RUN の bit はこの間に 0 を読む（40 us、3.3.35） */
#define RESET_PULSE_NS (100 * NS_PER_US)  /* codec が link の reset を見る一番短い長さ（5.5.1.2） */
#define CODEC_WAKE_NS  (521 * NS_PER_US)  /* CRST# の後、codec が番地を求め終える（4.3） */
#define FRAME_NS       20834              /* link の frame の長さ（5.3）：レジスターの変わりと答えを見る間 */
#define STREAM_TAG     1u                 /* link の上の stream の番号：制御器ごとに一つ */

/* 輪の頁は一枚で、CORB、続いて RIRB。descriptor の頁は一枚で、stream が持てる数（3.6.2）。frame の輪は別の object で、
   出口が開いている間だけある。 */
#define RINGS_PAGES 1
#define BDL_PAGES   1
#define DMA_PAGES   (RINGS_PAGES + BDL_PAGES)
#define BDL_LOW     2u   /* stream が取る一番少ない項（3.3.38） */
#define BDL_HIGH    256u /* と一番多い項 */
#define BLOCK_ALIGN 128u /* 区画の始まりの揃え（3.6.3） */

struct bdl_entry { /* 3.6.3 */
    uint64_t address;
    uint32_t length;
    uint32_t flags; /* bit 0：終わったら割り込む */
} PACKED;

struct hda;
struct hda_sink {
    struct audio_output output;
    struct hda *h;
    struct hda_output *out;
    struct hda_codec *codec;
    struct hda_sink *next;  /* 制御器の中の次 */
    struct hda_sink *every; /* 機械の中の次（名前のため） */
    uint32_t rates[11];     /* R1 から R11（hda_output_rates） */
    struct n_audio_size sizes[4];
    char key[sizeof "hda-ffff:ff:ff.7-14-65535"]; /* PCI の機能、codec の番地（4.3）、pin の node */
    char label[sizeof(((struct hda_output *)0)->name) + sizeof " 4294967295"]; /* 出口の名前。同じ名前と分ける */
};

struct hda {
    struct hda_link link; /* 先頭：命令が制御器へ戻る道 */
    struct pci_device *pci;
    struct hal_device *hal;
    volatile uint8_t *regs; /* メモリーの BAR（hal_map） */
    uint64_t dma_phys, dma_bus; /* 輪と descriptor の頁：CPU から見た番地と、制御器へ渡す番地 */
    uint32_t *corb;
    volatile uint64_t *rirb;
    struct bdl_entry *bdl;
    struct iommu_guest guest;
    /* link_up が GCAP から書き、変わらない。割り込みは錠なしで読む */
    volatile uint8_t *sd; /* 出力の stream の descriptor のレジスター */
    unsigned sd_index;    /* と、INTCTL と INTSTS でのその bit */
    bool bidirectional;

    /* state の下 */
    struct mutex state;
    bool stopped;   /* claim の客で、使われていない */
    bool probed;
    bool restore;   /* link が reset から上がった：codec に全部をもう一度言う */
    bool responses; /* 制御器の割り込みが開き、答えのたびに一つ来る */
    struct hda_codec *codecs[15]; /* 番地ごと（4.3） */
    struct hda_output *outputs[15];
    struct hda_sink *sinks;
    uint64_t next_look; /* 自分で告げない jack を次に見る時 */
    struct thread *thread;
    struct wait_queue wq;
    uint64_t signals;

    /* state と stream の両方を持って書き、どちらか一つを持って読む */
    uint16_t codec_present; /* link を上げた時に STATESTS が言った codec */
    bool quiet; /* 割り込みが無い：tick が音の層に知らせ、thread は POLL_NS ごとに見る */
    struct alarm tick;
    struct iommu_domain *guest_dom; /* メモリーを写しておく claim の domain */

    /* stream の下：音の層の呼出しのもの */
    struct mutex stream;
    struct hda_sink *open; /* 開いている出口。割り込みは錠なしで読む */
    uint8_t *ring;
    uint64_t ring_phys, ring_bus;
    size_t ring_pages;
    struct object *ring_object;
    uint32_t generation; /* 輪の中身の世代：電源の変化が中身を失わせると進む */
    bool routed, running;
    uint32_t rate, block_frames, blocks;
    unsigned frame_bytes;
    uint64_t pos;       /* stream の始まりからの frame */
    uint32_t last_lpib;
    uint64_t last_at;   /* last_lpib を読んだ時 */
    uint64_t started_at;
    uint32_t guard;
    uint32_t fifo_errors; /* 割り込みが数える */

    struct mutex verb_lock;
    /* engine の下。up は state と stream も持って書く */
    struct spinlock engine;
    bool up; /* link が reset を出て、輪が回っている */
    unsigned corb_entries, rirb_entries, corb_wp, rirb_rp;
    uint16_t silent;    /* 答えなくなった codec の番地 */
    uint16_t sense_due; /* 告げた pin の codec の番地 */
    uint32_t answer;    /* 最後に送った命令の答え */
    bool answered;      /* と、それが来たか */
};

static struct mutex label_mutex;
static struct hda_sink *every_sink;

void *hda_alloc(size_t bytes) { return kzalloc(bytes); }

/* ---- レジスター ---- */

static uint8_t read8(const struct hda *h, unsigned at) { return mmio_read8(h->regs + at); }
static uint16_t read16(const struct hda *h, unsigned at) { return mmio_read16(h->regs + at); }
static uint32_t read32(const struct hda *h, unsigned at) { return mmio_read32(h->regs + at); }
static void write8(const struct hda *h, unsigned at, uint8_t v) { mmio_write8(h->regs + at, v); }
static void write16(const struct hda *h, unsigned at, uint16_t v) { mmio_write16(h->regs + at, v); }
static void write32(const struct hda *h, unsigned at, uint32_t v) { mmio_write32(h->regs + at, v); }
static uint8_t sd_read8(const struct hda *h, unsigned at) { return mmio_read8(h->sd + at); }
static void sd_write8(const struct hda *h, unsigned at, uint8_t v) { mmio_write8(h->sd + at, v); }

/* at の size バイトの値と mask の積が want になるまで、link の frame ごとに眠って読み、ns まで待つ。来たら true。 */
static bool wait_bits(const volatile uint8_t *at, unsigned size, uint32_t mask, uint32_t want, uint64_t ns) {
    for (uint64_t until = clock_after(ns);;) {
        uint32_t v = size == 1 ? mmio_read8(at) : size == 2 ? mmio_read16(at) : mmio_read32(at);
        if ((v & mask) == want) return true;
        if (clock_now_ns() >= until) return false;
        sleep_until(clock_after(FRAME_NS));
    }
}

static void signal(struct hda *h) {
    __atomic_add_fetch(&h->signals, 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&h->wq);
}

/* ---- link ---- */

/* engine を持って：CORB と RIRB を置き場に、制御器が持つ一番大きな大きさで置く。まだ回さない（4.4.1.3、4.4.2.2）。 */
static void rings_place(struct hda *h) {
    static const uint16_t sizes[] = {2, 16, 256};
    uint8_t corb_caps = read8(h, CORBSIZE) >> 4, rirb_caps = read8(h, RIRBSIZE) >> 4;
    unsigned corb_size = corb_caps & 4 ? 2 : corb_caps & 2 ? 1 : 0, rirb_size = rirb_caps & 4 ? 2 : rirb_caps & 2 ? 1 : 0;
    write8(h, CORBSIZE, (uint8_t)corb_size);
    write8(h, RIRBSIZE, (uint8_t)rirb_size);
    h->corb_entries = sizes[corb_size];
    h->rirb_entries = sizes[rirb_size];
    uint64_t rings = h->dma_bus, rirb = rings + PAGE_SIZE / 2;
    write32(h, CORBLBASE, (uint32_t)rings);
    write32(h, CORBUBASE, (uint32_t)(rings >> 32));
    write32(h, RIRBLBASE, (uint32_t)rirb);
    write32(h, RIRBUBASE, (uint32_t)(rirb >> 32));
    write16(h, CORBWP, 0);
    write16(h, RIRBWP, RIRBWP_RST);
    write16(h, RINTCNT, 1);
    h->corb_wp = h->rirb_rp = 0;
    write16(h, CORBRP, CORBRP_RST);
}

/* state と stream を持って：制御器の何も走らず割り込まなくなり、link は reset に入る。RUN の bit が 0 を読んでから
   CRST を 0 にする（3.3.7：きれいに始め直すため）。false は RUN の bit か CRST が答えなかった：DMA が走っている
   かもしれない。 */
static bool link_reset(struct hda *h) {
    ASSERT(mutex_held(&h->state) && mutex_held(&h->stream));
    uint32_t caps = read16(h, GCAP);
    unsigned count = GCAP_ISS(caps) + GCAP_OSS(caps) + GCAP_BSS(caps);
    uint64_t f = spin_lock(&h->engine);
    h->up = false;
    spin_unlock(&h->engine, f);
    write32(h, INTCTL, 0);
    for (unsigned i = 0; i < count; i++) write8(h, SD_FIRST + SD_STRIDE * i + SD_CTL, 0);
    write8(h, CORBCTL, 0);
    write8(h, RIRBCTL, 0);
    bool stopped = wait_bits(h->regs + CORBCTL, 1, CORBCTL_RUN, 0, STOP_NS);
    stopped = wait_bits(h->regs + RIRBCTL, 1, RIRBCTL_DMA, 0, STOP_NS) && stopped;
    for (unsigned i = 0; i < count; i++)
        stopped = wait_bits(h->regs + SD_FIRST + SD_STRIDE * i + SD_CTL, 1, CTL_RUN, 0, STOP_NS) && stopped;
    write32(h, GCTL, 0);
    return wait_bits(h->regs + GCTL, 4, GCTL_CRST, 0, RESET_NS) && stopped;
}

/* state と stream を持って：link を reset に入れてから出し、codec を数え、輪を回す（4.2.2、4.3、4.4）。最初、claim の
   後、電源の変化の後に通る。false は link が上がらなかった。 */
static bool link_up(struct hda *h) {
    if (!link_reset(h)) return false;
    sleep_until(clock_after(RESET_PULSE_NS));
    write32(h, GCTL, GCTL_CRST);
    if (!wait_bits(h->regs + GCTL, 4, GCTL_CRST, GCTL_CRST, RESET_NS)) return false;
    sleep_until(clock_after(CODEC_WAKE_NS));
    uint32_t caps = read16(h, GCAP);
    h->bidirectional = !GCAP_OSS(caps);
    h->sd_index = GCAP_ISS(caps); /* stream は入力、出力、双方向の順（3.3.2） */
    h->sd = h->regs + SD_FIRST + SD_STRIDE * h->sd_index;
    h->codec_present = read16(h, STATESTS) & 0x7FFF;
    write16(h, STATESTS, 0x7FFF);
    /* codec の状態の変化で機械も制御器の割り込みも起こさない。値は思い込まずに書く（3.3.8） */
    write16(h, WAKEEN, 0);
    write32(h, GCTL, GCTL_CRST | GCTL_UNSOL);
    uint64_t f = spin_lock(&h->engine);
    rings_place(h);
    spin_unlock(&h->engine, f);
    /* 読みの位置の reset は 1 を読んでから 0 を書き、0 を読む（3.3.21）。1 を見せない制御器は、すぐに済ませている */
    wait_bits(h->regs + CORBRP, 2, CORBRP_RST, CORBRP_RST, STOP_NS);
    write16(h, CORBRP, 0);
    if (!wait_bits(h->regs + CORBRP, 2, CORBRP_RST, 0, STOP_NS)) return false;
    f = spin_lock(&h->engine);
    write8(h, RIRBSTS, RIRBSTS_ALL);
    write8(h, RIRBCTL, RIRBCTL_DMA | RIRBCTL_INT);
    write8(h, CORBCTL, CORBCTL_RUN);
    write32(h, INTCTL, INT_GLOBAL | (1u << h->sd_index));
    h->silent = 0;
    h->up = true;
    spin_unlock(&h->engine, f);
    h->responses = false;
    return true;
}

/* engine を持って：RIRB にあるもの。読むのは答えを待つ命令でも、pin の告げを探す thread でもよい：命令の答えは命令の
   ために取っておき、pin の告げは印を付ける。 */
static void rirb_take(struct hda *h) {
    unsigned wp = read16(h, RIRBWP) & (h->rirb_entries - 1);
    /* RINTCNT の数に入った答えは、割り込みがあっても無くても、見たと言うまで残る。その間、命令を送らない制御器がある */
    if (h->rirb_rp != wp) write8(h, RIRBSTS, RIRBSTS_ALL);
    while (h->rirb_rp != wp) {
        unsigned at = (h->rirb_rp + 1) & (h->rirb_entries - 1);
        h->rirb_rp = at;
        iommu_cache_flush(&h->rirb[at], sizeof h->rirb[at]);
        uint64_t entry = h->rirb[at];
        unsigned extended = (unsigned)(entry >> 32);
        if (extended & 0x10) h->sense_due |= (uint16_t)(1u << (extended & 15));
        else {
            h->answer = (uint32_t)entry;
            h->answered = true;
        }
    }
}

/* 命令を一つ（struct hda_link）：engine の下で書き、答えを link の frame ごとに眠って見に行く。呼ぶ者は state か
   stream を持つ：その間 link は変わらない。 */
static uint32_t link_verb(struct hda_link *link, unsigned codec, unsigned nid, uint32_t command) {
    struct hda *h = (struct hda *)link;
    uint32_t answer = 0;
    bool got = false, announced = false;
    mutex_lock(&h->verb_lock);
    uint64_t f = spin_lock(&h->engine);
    bool sent = h->up && !(h->silent & (1u << codec));
    if (sent) {
        h->answered = false;
        unsigned at = (h->corb_wp + 1) & (h->corb_entries - 1);
        h->corb[at] = (uint32_t)codec << 28 | (uint32_t)nid << 20 | command;
        iommu_cache_flush(&h->corb[at], sizeof h->corb[at]);
        write16(h, CORBWP, (uint16_t)at);
        h->corb_wp = at;
    }
    spin_unlock(&h->engine, f);
    for (uint64_t until = clock_after(VERB_NS); sent && !got;) {
        sleep_until(clock_after(FRAME_NS));
        f = spin_lock(&h->engine);
        rirb_take(h);
        got = h->answered;
        if (got) answer = h->answer;
        announced = announced || h->sense_due;
        bool late = !got && clock_now_ns() >= until;
        if (late) h->silent |= (uint16_t)(1u << codec);
        spin_unlock(&h->engine, f);
        if (late) {
            kprintf("audio: HD Audio codec %u at %04x:%02x:%02x.%x does not answer verb %x of node %u\n", codec,
                    PCI_ADDRESS_ARGS(h->pci->address), command, nid);
            break;
        }
    }
    mutex_unlock(&h->verb_lock);
    if (announced) signal(h);
    return answer;
}

/* ---- 割り込み ---- */

static void tell_open(struct hda *h) {
    struct hda_sink *o = __atomic_load_n(&h->open, __ATOMIC_ACQUIRE);
    if (o) audio_output_period(&o->output);
}

/* 区画を一つ鳴らした、か stream の誤り。答えが来た：待つ命令が無ければ pin の告げ。 */
static enum irq_result hda_irq(void *context) {
    struct hda *h = context;
    uint32_t status = read32(h, INTSTS);
    if (!status || status == UINT32_MAX) return IRQ_NONE;
    if (status & (1u << h->sd_index)) {
        uint8_t stream = sd_read8(h, SD_STS);
        sd_write8(h, SD_STS, stream & (STS_BCIS | STS_FIFOE | STS_DESE));
        if (stream & (STS_FIFOE | STS_DESE)) __atomic_add_fetch(&h->fifo_errors, 1, __ATOMIC_RELAXED);
        tell_open(h);
    }
    if (status & INT_CONTROLLER) {
        write8(h, RIRBSTS, read8(h, RIRBSTS) & RIRBSTS_ALL);
        signal(h);
    }
    return IRQ_HANDLED;
}

static void hda_tick(struct alarm *a, struct regs *r) {
    (void)r;
    tell_open((struct hda *)((char *)a - offsetof(struct hda, tick)));
}

/* state と stream を持って：割り込みを、バスが渡す形で開く（hal_irq_enable：機能が message を送れるならそれ。claim の
   domain の客は送れない）。無ければ tick と thread が代わりに見る。 */
static void irq_on(struct hda *h) {
    ASSERT(mutex_held(&h->state) && mutex_held(&h->stream));
    struct irq_spec spec = {.name = "HD Audio", .handler = hda_irq, .context = h};
    h->quiet = !hal_irq_enable(h->hal, &spec, 1, NULL);
}

/* state と stream を持って。割り込みの元はもう切ってある。 */
static void irq_off(struct hda *h) {
    ASSERT(mutex_held(&h->state) && mutex_held(&h->stream));
    if (!h->quiet) hal_irq_disable(h->hal);
    h->quiet = true;
}

/* ---- stream ----
   ここから hda_status までは stream を持って走る。 */

static uint32_t block_bytes(const struct hda *h) { return h->block_frames * h->frame_bytes; }
static uint32_t ring_frames(const struct hda *h) { return h->block_frames * h->blocks; }
static uint64_t block_ns(const struct hda *h) { return (uint64_t)h->block_frames * NS_PER_S / h->rate; }

/* frame の数え直し。 */
static void stream_idle(struct hda *h) {
    h->pos = 0;
    h->last_lpib = 0;
    h->running = false;
}

/* 走っていた stream が止まる（RUN を 0 にしてから 0 を読むまで 40 us、3.3.35）。 */
static void stream_stop(struct hda *h) {
    if (!h->running) return;
    sd_write8(h, SD_CTL, 0);
    if (!wait_bits(h->sd + SD_CTL, 1, CTL_RUN, 0, STOP_NS))
        kprintf("audio: HD Audio at %04x:%02x:%02x.%x: its stream does not stop\n", PCI_ADDRESS_ARGS(h->pci->address));
    if (h->quiet) alarm_cancel(&h->tick);
    stream_idle(h);
}

/* 電源の変化：stream は止まり、輪の中身は失われる。開いている出口の持ち主は次の status で新しい生成を見て run し直す
   （dev/audio.h）。codec の道も link の reset で失われる。 */
static void stream_lose(struct hda *h) {
    stream_stop(h);
    h->generation++;
    h->routed = false;
}

/* link が上がっていれば、開いている出口の道を codec に引く（link の reset で codec は忘れる）。上がっていなければ false。 */
static bool stream_route(struct hda *h, struct hda_sink *o) {
    if (!h->up) return false;
    if (!h->routed) hda_output_enable(o->out, STREAM_TAG);
    h->routed = true;
    return true;
}

/* link が上がって：この形式の descriptor と stream のレジスター（4.5.3）。最初の frame から走る。false は stream の
   reset が答えなかった（3.3.35）：走らない。 */
static bool stream_start(struct hda *h, struct hda_sink *o) {
    uint32_t bytes = block_bytes(h);
    for (unsigned i = 0; i < h->blocks; i++)
        h->bdl[i] = (struct bdl_entry){.address = h->ring_bus + i * bytes, .length = bytes, .flags = 1};
    iommu_cache_flush(h->bdl, h->blocks * sizeof *h->bdl);
    uint64_t bdl = h->dma_bus + RINGS_PAGES * PAGE_SIZE;
    sd_write8(h, SD_CTL, CTL_SRST);
    bool reset = wait_bits(h->sd + SD_CTL, 1, CTL_SRST, CTL_SRST, STOP_NS);
    sd_write8(h, SD_CTL, 0);
    reset = wait_bits(h->sd + SD_CTL, 1, CTL_SRST, 0, STOP_NS) && reset;
    if (!reset) {
        kprintf("audio: HD Audio at %04x:%02x:%02x.%x: its stream does not reset\n", PCI_ADDRESS_ARGS(h->pci->address));
        return false;
    }
    mmio_write32(h->sd + SD_CBL, ring_frames(h) * h->frame_bytes);
    mmio_write16(h->sd + SD_LVI, (uint16_t)(h->blocks - 1));
    mmio_write16(h->sd + SD_FMT, hda_output_format(o->out));
    mmio_write32(h->sd + SD_BDPL, (uint32_t)bdl);
    mmio_write32(h->sd + SD_BDPU, (uint32_t)(bdl >> 32));
    sd_write8(h, SD_CTL2, CTL2_TAG(STREAM_TAG) | (h->bidirectional ? CTL2_DIR : 0));
    sd_write8(h, SD_STS, STS_BCIS | STS_FIFOE | STS_DESE);
    /* FIFO の中身は、制御器が取ってきて link がまだ鳴らしていないもの（3.3.41） */
    h->guard = mmio_read16(h->sd + SD_FIFOS) / h->frame_bytes + 1;
    sd_write8(h, SD_CTL, CTL_RUN | CTL_IOCE | CTL_FEIE | CTL_DEIE);
    h->last_at = h->started_at = clock_now_ns();
    h->running = true;
    return true;
}

/* link が frame の数のどこにいるか。LPIB は輪を回る。半周より遅れて来たら、時計が何周したかを言う。 */
static void position_update(struct hda *h) {
    /* 位置と時計は対：二つの読みの間に割り込みも横取りも入らない */
    uint64_t f = spin_lock(&h->engine);
    uint32_t lpib = mmio_read32(h->sd + SD_LPIB) / h->frame_bytes;
    uint64_t now = clock_now_ns();
    spin_unlock(&h->engine, f);
    uint32_t frames = ring_frames(h);
    if (lpib >= frames) return; /* 読める正しい位置が無い */
    uint64_t delta = (lpib + frames - h->last_lpib) % frames;
    uint64_t elapsed = (now - h->last_at) * h->rate / NS_PER_S;
    if (elapsed > delta + frames / 2) delta += (elapsed - delta + frames / 2) / frames * frames;
    h->pos += delta;
    h->last_lpib = lpib;
    h->last_at = now;
}

static int hda_open(struct audio_output *output, const struct n_audio_device_format *format, struct audio_ring *ring) {
    struct hda_sink *o = output->priv;
    struct hda *h = o->h;
    uint16_t word = hda_format_word(format->rate, format->bits, format->channels);
    if (!word) return -N_ERR_INVALID;
    unsigned frame_bytes = (unsigned)format->channels * format->container;
    /* 区画はどれも揃えの境から始まり、frame を丸ごと持つ */
    unsigned step = 1;
    while ((step * frame_bytes) % BLOCK_ALIGN) step *= 2;
    uint64_t frames = ALIGN_UP((uint64_t)ring->block_frames, step);
    uint32_t blocks = MAX(MIN(ring->blocks, BDL_HIGH), BDL_LOW);
    uint64_t bytes = frames * frame_bytes * blocks;
    if (bytes > UINT32_MAX) return -N_ERR_TOO_LARGE; /* 輪の長さの欄（3.3.37） */
    size_t pages = ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE;
    /* 4 GiB より下：どの制御器も届く（64OK は任意、3.3.2） */
    uint64_t phys;
    struct object *memory = shared_kernel_contig(pages, 32, &phys);
    if (!memory) return -N_ERR_NO_MEMORY;
    mutex_lock(&h->stream);
    int r = h->open ? -N_ERR_BUSY : h->guest_dom && !iommu_map(h->guest_dom, NULL, phys, pages) ? -N_ERR_IO : 0;
    if (!r) {
        h->ring = phys_to_virt(phys);
        iommu_cache_flush(h->ring, pages * PAGE_SIZE);
        h->ring_phys = phys;
        h->ring_bus = hal_dma_address(h->hal, phys);
        h->ring_pages = pages;
        h->ring_object = memory;
        hda_output_select(o->out, word);
        h->rate = format->rate;
        h->frame_bytes = frame_bytes;
        h->block_frames = (uint32_t)frames;
        h->blocks = blocks;
        stream_idle(h);
        stream_route(h, o);
        __atomic_store_n(&h->open, o, __ATOMIC_RELEASE);
        *ring = (struct audio_ring){.memory = memory, .block_frames = (uint32_t)frames, .blocks = blocks,
                                    .flush_bytes = cpu_features.clflush_bytes};
    }
    mutex_unlock(&h->stream);
    if (r) object_unref(memory);
    return r;
}

static void hda_close(struct audio_output *output) {
    struct hda_sink *o = output->priv;
    struct hda *h = o->h;
    mutex_lock(&h->stream);
    stream_stop(h);
    __atomic_store_n(&h->open, NULL, __ATOMIC_RELEASE);
    if (h->routed) hda_output_disable(o->out);
    h->routed = false;
    if (h->guest_dom) iommu_unmap(h->guest_dom, NULL, h->ring_phys, h->ring_pages);
    struct object *memory = h->ring_object;
    h->ring_object = NULL;
    h->ring = NULL;
    mutex_unlock(&h->stream);
    object_unref(memory);
}

static int hda_run(struct audio_output *output, bool run) {
    struct hda_sink *o = output->priv;
    struct hda *h = o->h;
    int r = 0;
    mutex_lock(&h->stream);
    if (!run) stream_stop(h);
    else if (!stream_route(h, o)) r = -N_ERR_NOT_READY; /* 電源の変化か claim の間：resume が period で知らせる */
    else if (!h->running) {
        if (!stream_start(h, o)) r = -N_ERR_IO;
        else if (h->quiet) alarm_set(&h->tick, clock_after(block_ns(h)));
    }
    mutex_unlock(&h->stream);
    return r;
}

static void hda_status(struct audio_output *output, struct n_audio_device_status *status) {
    struct hda_sink *o = output->priv;
    struct hda *h = o->h;
    mutex_lock(&h->stream);
    if (h->running) {
        position_update(h);
        unsigned delay = o->out->delay;
        status->position = h->pos > delay ? h->pos - delay : 0;
        status->at_ns = h->last_at;
        status->guard_frames = h->guard + delay;
        if (h->quiet) alarm_set(&h->tick, clock_after(block_ns(h)));
    } else status->at_ns = clock_now_ns();
    status->generation = h->generation;
    status->running = h->running;
    status->errors = __atomic_load_n(&h->fifo_errors, __ATOMIC_RELAXED);
    mutex_unlock(&h->stream);
}

static const struct audio_output_ops hda_ops = {.open = hda_open, .close = hda_close, .run = hda_run, .status = hda_status};

/* ---- 出口 ---- */

/* 出口の名前。機械のほかの出口が同じ名前なら番号を付ける。 */
static void sink_label(struct hda_sink *o) {
    mutex_lock(&label_mutex);
    for (unsigned n = 1;; n++) {
        if (n == 1) strlcpy(o->label, o->out->name, sizeof o->label);
        else ksnprintf(o->label, sizeof o->label, "%s %u", o->out->name, n);
        bool taken = false;
        for (struct hda_sink *s = every_sink; s && !taken; s = s->every) taken = s != o && !strcmp(s->label, o->label);
        if (!taken) break;
    }
    mutex_unlock(&label_mutex);
}

static enum n_audio_jack jack_of(const struct hda_output *out) {
    return !out->senses ? N_AUDIO_JACK_NONE : out->plugged ? N_AUDIO_JACK_PLUGGED : N_AUDIO_JACK_EMPTY;
}

static bool sink_add(struct hda *h, struct hda_codec *codec, struct hda_output *out) {
    static const enum n_audio_kind kinds[] = {[HDA_OUTPUT_SPEAKER] = N_AUDIO_KIND_SPEAKER, [HDA_OUTPUT_HEADPHONE] = N_AUDIO_KIND_HEADPHONE,
                                              [HDA_OUTPUT_LINE] = N_AUDIO_KIND_LINE, [HDA_OUTPUT_DISPLAY] = N_AUDIO_KIND_DISPLAY,
                                              [HDA_OUTPUT_DIGITAL] = N_AUDIO_KIND_DIGITAL, [HDA_OUTPUT_OTHER] = N_AUDIO_KIND_OTHER};
    struct hda_sink *o = kzalloc(sizeof *o);
    if (!o) return false;
    o->h = h;
    o->out = out;
    o->codec = codec;
    hda_output_sense(out);
    mutex_lock(&label_mutex);
    o->every = every_sink;
    every_sink = o;
    mutex_unlock(&label_mutex);
    sink_label(o);
    o->output.ops = &hda_ops;
    o->output.priv = o;
    o->output.name = o->label;
    ksnprintf(o->key, sizeof o->key, "hda-%04x:%02x:%02x.%x-%u-%u", PCI_ADDRESS_ARGS(h->pci->address), codec->address, out->pin->nid);
    o->output.key = o->key;
    o->output.kind = kinds[out->kind];
    o->output.jack = jack_of(out);
    o->output.rates = o->rates;
    o->output.rate_count = hda_output_rates(out, o->rates);
    uint8_t bits[sizeof o->sizes / sizeof *o->sizes];
    o->output.size_count = hda_output_sizes(out, bits);
    for (uint32_t i = 0; i < o->output.size_count; i++) /* 収まる一番小さな容れ物（4.5.1） */
        o->sizes[i] = (struct n_audio_size){.bits = bits[i], .container = bits[i] == 16 ? 2 : 4};
    o->output.sizes = o->sizes;
    o->output.channels_low = o->output.channels_high = out->channels;
    struct hda_sink **tail = &h->sinks;
    while (*tail) tail = &(*tail)->next;
    *tail = o;
    kprintf("audio: HD Audio codec %u pin %u: \"%s\" (%s)\n", codec->address, out->pin->nid, o->label,
            !out->senses ? "no jack sense" : out->plugged ? "plugged in" : "nothing plugged in");
    audio_output_add(&o->output);
    hal_offer(h->hal, N_OFFER_AUDIO, (uint32_t)codec->address << 8 | out->pin->nid, o->label);
    return true;
}

/* state を持って、link が上がって：各 codec が持つもの。 */
static void probe(struct hda *h) {
    uint16_t present = h->codec_present;
    unsigned found = 0;
    for (unsigned at = 0; at < 15; at++) {
        h->codecs[at] = NULL;
        if (!(present & (1u << at))) continue;
        h->codecs[at] = hda_codec_read(&h->link, at);
        if (!h->codecs[at]) {
            kprintf("audio: HD Audio codec %u at %04x:%02x:%02x.%x has no audio function that can be read\n", at, PCI_ADDRESS_ARGS(h->pci->address));
            continue;
        }
        h->outputs[at] = hda_codec_outputs(h->codecs[at]);
        hda_codec_power(h->codecs[at], h->outputs[at], 1);
    }
    for (unsigned at = 0; at < 15; at++) {
        if (!h->codecs[at]) continue;
        for (struct hda_output *o = h->outputs[at]; o; o = o->next)
            if (sink_add(h, h->codecs[at], o)) found++;
    }
    h->probed = true;
    kprintf("audio: HD Audio at %04x:%02x:%02x.%x (%04x:%04x): codecs %x, %u outputs, %s\n", PCI_ADDRESS_ARGS(h->pci->address),
            h->pci->vendor, h->pci->device, present, found, h->quiet ? "timed refill" : "interrupts");
}

/* state を持って：告げた codec の出口と、look なら自分で告げない出口。 */
static void sense(struct hda *h, uint16_t codecs, bool look) {
    for (struct hda_sink *o = h->sinks; o; o = o->next) {
        bool due = codecs & (1u << o->codec->address) || (look && o->out->senses && (h->quiet || !o->out->announces));
        if (!due || !hda_output_sense(o->out)) continue;
        sink_label(o);
        audio_output_state(&o->output, jack_of(o->out), o->label);
    }
}

/* codec を読み、その後は codec が告げることに答え、link が reset から上がったら codec に全部をもう一度言う。自分の
   thread で走る：codec を読むには数千の命令が要る。link が下りている間（claim の客、電源の変化）は何もしない。 */
static void hda_thread(void *arg) {
    struct hda *h = arg;
    for (;;) {
        uint64_t seen = __atomic_load_n(&h->signals, __ATOMIC_SEQ_CST), wake = CLOCK_NEVER;
        mutex_lock(&h->state);
        if (h->up && !h->probed) probe(h);
        if (h->up && h->probed) {
            uint16_t due = 0;
            if (h->restore) {
                h->restore = false;
                for (unsigned at = 0; at < 15; at++)
                    if (h->codecs[at]) hda_codec_power(h->codecs[at], h->outputs[at], 1);
                due = UINT16_MAX;
            }
            uint64_t f = spin_lock(&h->engine);
            rirb_take(h);
            due |= h->sense_due;
            h->sense_due = 0;
            spin_unlock(&h->engine, f);
            if (!h->responses) { /* ここからは答えは少なく、どれも pin の告げでありうる */
                write32(h, INTCTL, read32(h, INTCTL) | INT_CONTROLLER);
                h->responses = true;
            }
            bool looks = false;
            for (struct hda_sink *o = h->sinks; o; o = o->next) looks = looks || (o->out->senses && (h->quiet || !o->out->announces));
            uint64_t now = clock_now_ns();
            bool look = looks && now >= h->next_look;
            if (look) h->next_look = now + POLL_NS;
            sense(h, due, look);
            if (looks) wake = h->next_look;
        }
        mutex_unlock(&h->state);
        wq_wait_unless(&h->wq, &h->signals, seen, wake);
    }
}

/* ---- 電源 ---- */

/* hal.h の suspend。止め方は目標によらず同じ：link の reset が DMA と割り込みを止め、resume はどの場合も link の
   reset から codec を戻す。function group は link の clock を止める前に D3 にする（7.3.3.10：codec は D0 から D3 へ
   移る音の乱れを自分で抑える。clock の止まった codec は完全な reset から始め直し、resume はそうする）。claim の客で
   止まっていれば、link はもう下りている。
   止まらなければ false：眠りと休止の取りやめで resume が続き、link を上げ直す。 */
static bool hda_suspend(struct hal_device *d, enum hal_power_target target) {
    (void)target;
    struct hda *h = d->driver_data;
    bool stopped = true;
    mutex_lock(&h->state);
    if (!h->stopped) {
        mutex_lock(&h->stream);
        stream_lose(h);
        for (unsigned at = 0; at < 15; at++)
            if (h->codecs[at]) hda_codec_sleep(h->codecs[at]);
        stopped = link_reset(h);
        mutex_unlock(&h->stream);
    }
    mutex_unlock(&h->state);
    if (!stopped) kprintf("audio: HD Audio at %04x:%02x:%02x.%x does not stop\n", PCI_ADDRESS_ARGS(h->pci->address));
    signal(h);
    tell_open(h); /* 開いた出口の持ち主が、止まったことと新しい生成を見る */
    return stopped;
}

/* hal.h の resume。装置は電源を失っていても（眠りと休止の後）、そのままでも（取りやめと写しの後）よい：stream を
   止めたことにして、link を reset から上げる。codec の設定は thread が初めから戻し（4.5.8.1）、開いた出口は持ち主が
   新しい生成で run し直す。 */
static bool hda_resume(struct hal_device *d) {
    struct hda *h = d->driver_data;
    bool back = true;
    mutex_lock(&h->state);
    if (!h->stopped) {
        mutex_lock(&h->stream);
        stream_lose(h);
        back = link_up(h);
        h->restore = back;
        mutex_unlock(&h->stream);
    }
    mutex_unlock(&h->state);
    if (!back) {
        kprintf("audio: HD Audio at %04x:%02x:%02x.%x does not come out of reset after sleeping\n", PCI_ADDRESS_ARGS(h->pci->address));
        hal_failed(d, -N_ERR_IO);
    }
    signal(h);
    tell_open(h);
    return back;
}

/* ---- claim の客 ---- */

/* 組が持ち主を変える：出口が去り、続いて link と、それが送る全部が止まる。 */
static void hda_guest_stop(void *ctx) {
    struct hda *h = ctx;
    mutex_lock(&h->state);
    if (!h->stopped) {
        for (struct hda_sink *o = h->sinks; o; o = o->next) audio_output_available(&o->output, false);
        mutex_lock(&h->stream);
        h->guest_dom = NULL;
        h->stopped = true;
        link_reset(h);
        irq_off(h);
        mutex_unlock(&h->stream);
        pci_disable_busmaster(h->pci, NULL);
    }
    mutex_unlock(&h->state);
}

/* dom：メモリーをこれから写しておく claim の domain。NULL：組は元のまま。 */
static void hda_guest_start(struct iommu_domain *dom, void *ctx) {
    struct hda *h = ctx;
    mutex_lock(&h->state);
    if (h->stopped) {
        if (dom && !iommu_map(dom, NULL, h->dma_phys, DMA_PAGES)) {
            kprintf("audio: HD Audio at %04x:%02x:%02x.%x: its memory cannot be mapped in its group's domain\n", PCI_ADDRESS_ARGS(h->pci->address));
        } else {
            mutex_lock(&h->stream);
            h->guest_dom = dom;
            hal_enable(h->hal, HAL_ENABLE_MEMORY | HAL_ENABLE_DMA);
            h->stopped = false;
            bool up = link_up(h);
            if (up) irq_on(h);
            mutex_unlock(&h->stream);
            if (up) {
                h->restore = true;
                for (struct hda_sink *o = h->sinks; o; o = o->next) audio_output_available(&o->output, true);
            } else kprintf("audio: HD Audio at %04x:%02x:%02x.%x does not come out of reset\n", PCI_ADDRESS_ARGS(h->pci->address));
            signal(h);
        }
    }
    mutex_unlock(&h->state);
}

/* ---- 見つける ---- */

static int hda_attach(struct hal_device *hal) {
    struct pci_device *d = pci_device_of(hal);
    if (!hal_resource(hal, N_HAL_MEMORY, 0)) return -N_ERR_NOT_FOUND;
    struct hda *h = kzalloc(sizeof *h);
    if (!h) {
        kprintf("audio: HD Audio at %04x:%02x:%02x.%x found, but no memory for it\n", PCI_ADDRESS_ARGS(d->address));
        return -N_ERR_NO_MEMORY;
    }
    volatile uint8_t *regs = hal_map(hal, 0);
    /* 4 GiB より下：どの制御器も届く（64OK は任意、3.3.2） */
    uint64_t bus;
    uint8_t *memory = regs ? hal_dma_alloc(hal, DMA_PAGES * PAGE_SIZE, 32, &bus) : NULL;
    if (!memory) {
        kprintf("audio: HD Audio at %04x:%02x:%02x.%x found, but no memory for it\n", PCI_ADDRESS_ARGS(d->address));
        kfree(h);
        return -N_ERR_NO_MEMORY;
    }
    hal->driver_data = h;
    h->link.verb = link_verb;
    h->pci = d;
    h->hal = hal;
    h->regs = regs;
    h->dma_bus = bus;
    h->dma_phys = hhdm_virt_to_phys(memory);
    iommu_cache_flush(memory, DMA_PAGES * PAGE_SIZE);
    h->corb = (uint32_t *)memory;
    h->rirb = (volatile uint64_t *)(memory + PAGE_SIZE / 2);
    h->bdl = (struct bdl_entry *)(memory + RINGS_PAGES * PAGE_SIZE);
    h->frame_bytes = 4; /* 形式が開くまで */
    alarm_init(&h->tick, hda_tick);
    hal_enable(hal, HAL_ENABLE_MEMORY | HAL_ENABLE_DMA);
    mutex_lock(&h->state);
    mutex_lock(&h->stream);
    bool up = link_up(h);
    uint32_t caps = up ? read16(h, GCAP) : 0;
    /* 鳴らす stream が無い制御器は driver が動かさない：アプリが動かしてよい */
    int r = !up ? -N_ERR_IO : !GCAP_OSS(caps) && !GCAP_BSS(caps) ? HAL_DECLINE : 0;
    if (r) kprintf("audio: HD Audio at %04x:%02x:%02x.%x %s\n", PCI_ADDRESS_ARGS(d->address), up ? "has no stream to play on" : "does not come out of reset");
    if (!r) {
        irq_on(h);
        h->guest = (struct iommu_guest){.stop = hda_guest_stop, .start = hda_guest_start, .ctx = h};
        d->guest = &h->guest;
        h->thread = kthread_create("hda", hda_thread, h);
        if (!h->thread) {
            kprintf("audio: HD Audio at %04x:%02x:%02x.%x: no memory for its thread\n", PCI_ADDRESS_ARGS(d->address));
            d->guest = NULL;
            irq_off(h);
            r = -N_ERR_NO_MEMORY;
        }
    }
    if (r) link_reset(h);
    mutex_unlock(&h->stream);
    mutex_unlock(&h->state);
    if (r) {
        hal->driver_data = NULL;
        kfree(h);
    }
    return r;
}

/* 音の類の PCI の機能の全部：chipset のものも graphics card のものも。 */
static const struct n_hal_rule hda_rules[] = {N_PCI_CLASS(0x04, 0x03)};
static const struct hal_driver hda_driver = {
    .name = "hda",
    .rules = hda_rules,
    .rule_count = ARRAY_SIZE(hda_rules),
    .attach = hda_attach,
    .suspend = hda_suspend,
    .resume = hda_resume,
    .after_sleep = HAL_AFTER_SLEEP_RESUME,
};
HAL_DRIVER(hda_driver);
