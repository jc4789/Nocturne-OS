/* Intel High Definition Audio controllers (High Definition Audio Specification 1.0a; section numbers
   are its): every PCI function of the class says it is one, whoever made it, a chipset's or a
   graphics card's. Each gets its link up, its codecs read (dev/hda_codec.c), and each output of its
   codecs becomes an output of the sound layer (dev/audio.h), with the formats its converter declares.

   Verbs go through the CORB and come back through the RIRB one at a time, polled (one thread at a
   time waits for its answer); the RIRB's interrupt only tells the controller's thread that a pin has
   announced something (4.4.1, 7.3.3.14).

   A controller plays one output at a time on one output stream (or a bidirectional one where it has
   no other), through a ring of equal blocks that the stream walks without end. The ring is shared
   with the service that opened the output, which writes it. Each block asks for an interrupt, which
   tells that service to look (audio_output_period); nothing here looks at the ring's content.

   The position is what LPIB says the link has sent, less the pin's delay, with the clock read right
   after it under `engine`. A count of frames from the stream's start that never wraps comes of it: a
   turn of the ring missed or counted too many only changes where on the count the frames lie, not which
   frame of the ring the link is at. The clock only tells how many turns a report that comes very late
   has missed.

   A function's memory comes from the kernel and it passes the IOMMU untranslated, as every function
   the kernel runs. A graphics card's sound function may share an isolation group with the card's
   display function, which an app drives (dev/pciuser.c): it then is that claim's guest (iommu_guest):
   stopped while the group changes hands, and on again with its memory mapped in the claim's domain.

   Locks, in order: the claim's and IOMMU's mutexes, `state` (what the controller is doing as a whole:
   probing, stopped for a claim, sensing), the sound layer's mutex, verb_lock, `engine`.
   `engine` is a leaf: nothing is called under it but registers. A power change (quiesce and resume, on
   a CPU that may not wait for any of the mutexes) comes between any two steps of the threads: it
   changes up and power_gen and stops the hardware under `engine`, and each of them writes what
   starts something only under `engine`, having seen the generation it began with there. */
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

/* global registers (3.3) */
#define GCAP      0x00
#define GCTL      0x08
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
#define RIRBSTS_ALL 0x05 /* response interrupt, overrun */

/* a stream descriptor's registers (3.3.35 on) */
#define SD_CTL   0x00
#define SD_CTL2  0x02 /* its third byte: stripe, priority, direction, stream number */
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

#define POLL_NS     NS_PER_S           /* between looks at a jack that does not announce itself */
#define VERB_NS     (10 * NS_PER_MS)   /* a codec silent this long is gone (until the link's next reset) */
#define RESET_NS    (100 * NS_PER_MS)  /* the longest a controller may be given to leave or enter a reset */
#define STOP_NS     NS_PER_MS          /* a stream's or ring's run bit reads 0 by then (40 us, 3.3.35) */
#define CODEC_WAKE_NS (521 * NS_PER_US) /* after CRST# for codecs to ask for an address (4.3) */
#define STREAM_TAG  1u                 /* the stream's number on the link: there is one per controller */

/* one page of rings: the CORB, then the RIRB; a page of descriptors, as many as a stream has (3.6.2). The
   ring of frames is an object of its own, there while an output is open. */
#define RINGS_PAGES 1
#define BDL_PAGES   1
#define DMA_PAGES   (RINGS_PAGES + BDL_PAGES)
#define BDL_LOW     2u   /* entries a stream takes at the least (3.3.38) */
#define BDL_HIGH    256u /* and at the most */
#define BLOCK_ALIGN 128u /* of a buffer's start (3.6.3) */

struct bdl_entry { /* 3.6.3 */
    uint64_t address;
    uint32_t length;
    uint32_t flags; /* bit 0: interrupt on completion */
} PACKED;

struct hda;
struct hda_sink {
    struct audio_output output;
    struct hda *h;
    struct hda_output *out;
    struct hda_codec *codec;
    struct hda_sink *next;  /* of its controller */
    struct hda_sink *every; /* of the machine (labels) */
    uint32_t rates[11];     /* R1 to R11 (hda_output_rates) */
    struct n_audio_size sizes[4];
    char key[sizeof "hda-ffff:ff:ff.7-14-65535"]; /* its PCI function, its codec's address (4.3) and its pin's node */
    char label[sizeof(((struct hda_output *)0)->name) + sizeof " 4294967295"]; /* the output's name, told from its namesakes */
};

struct hda {
    struct hda_link link; /* first: the verbs' way back to it */
    struct pci_device *pci;
    struct hal_device *hal;
    volatile uint8_t *regs; /* its memory BAR (hal_map) */
    volatile uint8_t *sd;   /* the output stream descriptor's registers */
    unsigned sd_index;      /* and its bit in INTCTL and INTSTS */
    bool bidirectional;
    uint64_t dma_phys, dma_bus; /* the page of rings and descriptors: where the CPU's memory is, and what the controller is told */
    uint32_t *corb;
    volatile uint64_t *rirb;
    struct bdl_entry *bdl;
    uint8_t *ring;          /* what is open's */
    uint64_t ring_phys, ring_bus;
    size_t ring_pages;
    struct object *ring_object;
    struct iommu_domain *guest_dom; /* the claim's domain its memory has to be in; written with `state` */
    struct iommu_guest guest;

    struct mutex state;
    bool stopped;      /* a claim's guest out of use; written with `engine` too */
    bool probed;
    bool quiet;        /* no interrupt: tick wakes the audio thread, and the thread looks every POLL_NS */
    struct alarm tick;
    struct hda_codec *codecs[15];      /* by address (4.3) */
    struct hda_output *outputs[15];
    struct hda_sink *sinks;
    struct thread *thread;
    struct wait_queue wq;
    uint64_t signals;
    uint64_t next_look; /* at a jack that does not announce itself */
    bool restore;      /* the link was reset: the codecs are to be told again */
    bool responses;    /* the controller's interrupt is on, and each response comes with one */

    struct mutex verb_lock;
    struct spinlock engine;
    bool up;           /* the link is out of reset and the rings run */
    uint64_t power_gen;
    unsigned corb_entries, rirb_entries, corb_wp, rirb_rp;
    uint16_t silent;   /* the addresses of codecs that stopped answering */
    uint16_t codec_present;
    uint16_t sense_due;
    uint32_t answer;        /* `engine`: the answer to the verb last sent */
    bool answered;          /* and whether it came */

    /* the sound layer's calls', one at a time (audio_output_ops), with what they do only ever after a
       power change seen in seen_gen */
    struct hda_sink *open;          /* the output that is open; read without a lock by the interrupt */
    uint64_t seen_gen;
    uint32_t generation;            /* of the ring's content: a power change lost it */
    bool routed, running;
    uint32_t rate, block_frames, blocks;
    unsigned frame_bytes;
    uint64_t pos;                   /* frames from the stream's start */
    uint32_t last_lpib;
    uint64_t last_at;               /* when last_lpib was read */
    uint64_t started_at;
    uint32_t guard;
    uint32_t fifo_errors;           /* the interrupt's */
};

static struct mutex label_mutex;
static struct hda_sink *every_sink;

void *hda_alloc(size_t bytes) { return kzalloc(bytes); }

/* ---- registers ---- */

static uint8_t read8(const struct hda *h, unsigned at) { return mmio_read8(h->regs + at); }
static uint16_t read16(const struct hda *h, unsigned at) { return mmio_read16(h->regs + at); }
static uint32_t read32(const struct hda *h, unsigned at) { return mmio_read32(h->regs + at); }
static void write8(const struct hda *h, unsigned at, uint8_t v) { mmio_write8(h->regs + at, v); }
static void write16(const struct hda *h, unsigned at, uint16_t v) { mmio_write16(h->regs + at, v); }
static void write32(const struct hda *h, unsigned at, uint32_t v) { mmio_write32(h->regs + at, v); }
static uint8_t sd_read8(const struct hda *h, unsigned at) { return mmio_read8(h->sd + at); }
static void sd_write8(const struct hda *h, unsigned at, uint8_t v) { mmio_write8(h->sd + at, v); }

static bool wait32(const struct hda *h, unsigned at, uint32_t mask, uint32_t want, uint64_t ns) {
    for (uint64_t until = clock_after(ns);;) {
        if ((read32(h, at) & mask) == want) return true;
        if (clock_now_ns() >= until) return false;
        pause();
    }
}

static bool wait16(const struct hda *h, unsigned at, uint16_t mask, uint16_t want, uint64_t ns) {
    for (uint64_t until = clock_after(ns);;) {
        if ((read16(h, at) & mask) == want) return true;
        if (clock_now_ns() >= until) return false;
        pause();
    }
}

static void signal(struct hda *h) {
    __atomic_add_fetch(&h->signals, 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&h->wq);
}

/* ---- the link ---- */

/* engine held: the CORB and the RIRB where they are, in the largest sizes the controller has, not
   running yet (4.4.1, 4.4.2). */
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

/* engine held: nothing of the controller's is to run any more, nor to interrupt. */
static void streams_halt(struct hda *h) {
    uint32_t caps = read16(h, GCAP);
    unsigned count = GCAP_ISS(caps) + GCAP_OSS(caps) + GCAP_BSS(caps);
    write32(h, INTCTL, 0);
    for (unsigned i = 0; i < count; i++) write8(h, SD_FIRST + SD_STRIDE * i + SD_CTL, 0);
    write8(h, CORBCTL, 0);
    write8(h, RIRBCTL, 0);
}

/* After streams_halt: the link goes down once the run bits read 0, as they are to before a reset
   (4.2.2). False when one does not: DMA may still run. */
static bool link_down(struct hda *h) {
    uint32_t caps = read16(h, GCAP);
    unsigned count = GCAP_ISS(caps) + GCAP_OSS(caps) + GCAP_BSS(caps);
    uint64_t until = clock_after(STOP_NS);
    bool stopped = wait32(h, CORBCTL, CORBCTL_RUN, 0, STOP_NS) && wait32(h, RIRBCTL, RIRBCTL_DMA, 0, STOP_NS);
    for (unsigned i = 0; i < count; i++)
        while (mmio_read8(h->regs + SD_FIRST + SD_STRIDE * i + SD_CTL) & CTL_RUN) {
            if (clock_now_ns() >= until) {
                stopped = false;
                break;
            }
            pause();
        }
    write32(h, GCTL, 0);
    return stopped;
}

/* engine held: writes what its step wants unless a power change came since gen; whether it did. */
#define STEP(h, gen, ok, ...)                     \
    do {                                          \
        uint64_t step_f = spin_lock(&(h)->engine); \
        (ok) = (ok) && (gen) == (h)->power_gen;   \
        if (ok) { __VA_ARGS__; }                  \
        spin_unlock(&(h)->engine, step_f);        \
    } while (0)

/* The link out of reset with its rings running: at first, after a sleep and after a claim. It polls the
   clock and sleeps nowhere. Each step is one under `engine`: a power change that comes between them
   does this itself, and this gives up. False when the link is not up in the end. */
static bool link_up(struct hda *h) {
    uint64_t f = spin_lock(&h->engine), gen = h->power_gen;
    streams_halt(h);
    spin_unlock(&h->engine, f);
    bool ok = link_down(h);
    ok = wait32(h, GCTL, GCTL_CRST, 0, RESET_NS) && ok;
    clock_spin(100 * NS_PER_US); /* the codecs see the reset for 100 us at least (5.5.1.2) */
    STEP(h, gen, ok, write32(h, GCTL, GCTL_CRST));
    ok = ok && wait32(h, GCTL, GCTL_CRST, GCTL_CRST, RESET_NS);
    clock_spin(CODEC_WAKE_NS);
    STEP(h, gen, ok, {
        uint32_t caps = read16(h, GCAP);
        h->bidirectional = !GCAP_OSS(caps);
        h->sd_index = GCAP_ISS(caps); /* the streams go inputs, outputs, bidirectional (3.3.2) */
        h->sd = h->regs + SD_FIRST + SD_STRIDE * h->sd_index;
        h->codec_present = read16(h, STATESTS) & 0x7FFF;
        write16(h, STATESTS, 0x7FFF);
        write32(h, GCTL, GCTL_CRST | GCTL_UNSOL);
        rings_place(h);
    });
    /* A controller that does not show the reset bit has done with it at once. */
    if (ok) wait16(h, CORBRP, CORBRP_RST, CORBRP_RST, STOP_NS);
    STEP(h, gen, ok, write16(h, CORBRP, 0));
    if (ok) wait16(h, CORBRP, CORBRP_RST, 0, STOP_NS);
    STEP(h, gen, ok, {
        write8(h, RIRBSTS, RIRBSTS_ALL);
        write8(h, RIRBCTL, RIRBCTL_DMA | RIRBCTL_INT);
        write8(h, CORBCTL, CORBCTL_RUN);
        write32(h, INTCTL, INT_GLOBAL | (1u << h->sd_index));
        h->silent = 0;
        h->responses = false;
        h->up = true;
    });
    f = spin_lock(&h->engine);
    ok = h->up;
    spin_unlock(&h->engine, f);
    return ok;
}

/* engine held. */
static void link_stop(struct hda *h) {
    h->up = false;
    h->power_gen++;
    streams_halt(h);
}

/* engine held: what the RIRB holds. Whoever reads it, the waiting verb or the thread that looks for
   what pins announced: a verb's answer is kept for the verb, and what pins announced is noted. */
static void rirb_take(struct hda *h) {
    unsigned wp = read16(h, RIRBWP) & (h->rirb_entries - 1);
    /* A response in the count of RINTCNT stays due until this says it was seen, with or without an
       interrupt; a controller may send no more verbs meanwhile. */
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

/* One verb (struct hda_link): written under `engine`, its answer polled for. */
static uint32_t link_verb(struct hda_link *link, unsigned codec, unsigned nid, uint32_t command) {
    struct hda *h = (struct hda *)link;
    uint32_t answer = 0;
    mutex_lock(&h->verb_lock);
    uint64_t f = spin_lock(&h->engine), gen = h->power_gen;
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
    bool announced = false;
    for (uint64_t until = clock_after(VERB_NS); sent;) {
        f = spin_lock(&h->engine);
        bool gone = gen != h->power_gen;
        if (!gone) rirb_take(h);
        bool got = !gone && h->answered;
        if (got) answer = h->answer;
        announced = announced || h->sense_due;
        spin_unlock(&h->engine, f);
        if (got || gone) break;
        if (clock_now_ns() >= until) {
            f = spin_lock(&h->engine);
            h->silent |= (uint16_t)(1u << codec);
            spin_unlock(&h->engine, f);
            kprintf("audio: HD Audio codec %u at %04x:%02x:%02x.%x does not answer verb %x of node %u\n", codec, PCI_ADDRESS_ARGS(h->pci->address), command, nid);
            break;
        }
        pause();
    }
    mutex_unlock(&h->verb_lock);
    if (announced) signal(h);
    return answer;
}

/* ---- interrupts ---- */

static void tell_open(struct hda *h) {
    struct hda_sink *o = __atomic_load_n(&h->open, __ATOMIC_ACQUIRE);
    if (o) audio_output_period(&o->output);
}

/* A block played or the stream has an error; a response came in, which is a pin's when no verb waits. */
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

/* state held: its interrupt on, as the bus gives it (hal_irq_enable: a message where the function may send
   one, a guest in a claim's domain may not). Without one, tick and the thread look instead. */
static void irq_on(struct hda *h) {
    struct irq_spec spec = {.name = "HD Audio", .handler = hda_irq, .context = h, .node = h->pci->node};
    h->quiet = !hal_irq_enable(h->hal, &spec, 1, NULL);
}

/* state held; its source is off. */
static void irq_off(struct hda *h) {
    if (!h->quiet) hal_irq_disable(h->hal);
    h->quiet = true;
}

/* ---- the stream ---- */

static uint32_t block_bytes(const struct hda *h) { return h->block_frames * h->frame_bytes; }
static uint32_t ring_frames(const struct hda *h) { return h->block_frames * h->blocks; }

/* The count of frames starts again. */
static void stream_idle(struct hda *h) {
    h->pos = 0;
    h->last_lpib = 0;
    h->running = false;
}

/* A stream that was running stops (RUN to 0 takes up to 40 us). */
static void stream_stop(struct hda *h) {
    if (!h->running) return;
    uint64_t f = spin_lock(&h->engine);
    if (h->up) sd_write8(h, SD_CTL, 0);
    spin_unlock(&h->engine, f);
    for (uint64_t until = clock_after(STOP_NS); (sd_read8(h, SD_CTL) & CTL_RUN) && clock_now_ns() < until;) pause();
    if (h->quiet) alarm_cancel(&h->tick);
    stream_idle(h);
}

/* The descriptors and the stream's registers for this format (4.5.3); it runs from the first frame.
   False after a power change, and it does not run. */
static bool stream_start(struct hda *h, struct hda_sink *o, uint64_t gen) {
    uint32_t bytes = block_bytes(h);
    for (unsigned i = 0; i < h->blocks; i++)
        h->bdl[i] = (struct bdl_entry){.address = h->ring_bus + i * bytes, .length = bytes, .flags = 1};
    iommu_cache_flush(h->bdl, h->blocks * sizeof *h->bdl);
    uint64_t bdl = h->dma_bus + RINGS_PAGES * PAGE_SIZE;
    sd_write8(h, SD_CTL, CTL_SRST);
    for (uint64_t until = clock_after(STOP_NS); !(sd_read8(h, SD_CTL) & CTL_SRST) && clock_now_ns() < until;) pause();
    sd_write8(h, SD_CTL, 0);
    for (uint64_t until = clock_after(STOP_NS); (sd_read8(h, SD_CTL) & CTL_SRST) && clock_now_ns() < until;) pause();
    mmio_write32(h->sd + SD_CBL, ring_frames(h) * h->frame_bytes);
    mmio_write16(h->sd + SD_LVI, (uint16_t)(h->blocks - 1));
    mmio_write16(h->sd + SD_FMT, hda_output_format(o->out));
    mmio_write32(h->sd + SD_BDPL, (uint32_t)bdl);
    mmio_write32(h->sd + SD_BDPU, (uint32_t)(bdl >> 32));
    sd_write8(h, SD_CTL2, CTL2_TAG(STREAM_TAG) | (h->bidirectional ? CTL2_DIR : 0));
    sd_write8(h, SD_STS, STS_BCIS | STS_FIFOE | STS_DESE);
    /* the FIFO's content is what the controller has fetched and the link has not played (3.3.41) */
    h->guard = mmio_read16(h->sd + SD_FIFOS) / h->frame_bytes + 1;
    uint64_t f = spin_lock(&h->engine);
    bool ok = h->up && gen == h->power_gen;
    if (ok) sd_write8(h, SD_CTL, CTL_RUN | CTL_IOCE | CTL_FEIE | CTL_DEIE);
    spin_unlock(&h->engine, f);
    h->last_at = h->started_at = clock_now_ns();
    h->running = ok;
    return ok;
}

/* Where the link is, on the count of frames. LPIB goes round the ring; the clock tells how many times
   when this comes more than half a turn late. */
static void position_update(struct hda *h) {
    /* the position and the clock as one pair: nothing comes between the two reads */
    uint64_t f = spin_lock(&h->engine);
    uint32_t lpib = mmio_read32(h->sd + SD_LPIB) / h->frame_bytes;
    uint64_t now = clock_now_ns();
    spin_unlock(&h->engine, f);
    uint32_t frames = ring_frames(h);
    if (lpib >= frames) return; /* no valid position to read */
    uint64_t delta = (lpib + frames - h->last_lpib) % frames;
    uint64_t elapsed = (now - h->last_at) * h->rate / NS_PER_S;
    if (elapsed > delta + frames / 2) delta += (elapsed - delta + frames / 2) / frames * frames;
    h->pos += delta;
    h->last_lpib = lpib;
    h->last_at = now;
}

/* Whether the link is up, *gen its generation, and whatever a power change undid is taken up again:
   the ring's content went with the reset, and the stream stands at its start. */
static bool stream_sync(struct hda *h, struct hda_sink *o, uint64_t *gen) {
    uint64_t f = spin_lock(&h->engine);
    bool up = h->up;
    *gen = h->power_gen;
    spin_unlock(&h->engine, f);
    if (*gen != h->seen_gen) {
        h->seen_gen = *gen;
        h->generation++;
        stream_idle(h);
        h->routed = false;
    }
    if (up && !h->routed) {
        hda_output_enable(o->out, STREAM_TAG);
        h->routed = true;
    }
    return up;
}

static int hda_open(struct audio_output *output, const struct n_audio_device_format *format, struct audio_ring *ring) {
    struct hda_sink *o = output->priv;
    struct hda *h = o->h;
    if (h->open) return -N_ERR_BUSY;
    uint16_t word = hda_format_word(format->rate, format->bits, format->channels);
    if (!word) return -N_ERR_INVALID;
    unsigned frame_bytes = (unsigned)format->channels * format->container;
    /* each buffer starts on the boundary, and holds whole frames */
    unsigned step = 1;
    while ((step * frame_bytes) % BLOCK_ALIGN) step *= 2;
    uint64_t frames = ALIGN_UP((uint64_t)ring->block_frames, step);
    uint32_t blocks = MAX(MIN(ring->blocks, BDL_HIGH), BDL_LOW);
    uint64_t bytes = frames * frame_bytes * blocks;
    if (bytes > UINT32_MAX) return -N_ERR_TOO_LARGE; /* of the cyclic buffer's length (3.3.37) */
    size_t pages = ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE;
    /* below 4 GiB, which every controller can reach (64OK is optional, 3.3.2) */
    uint64_t phys;
    struct object *memory = shared_kernel_contig(pages, 32, &phys);
    if (!memory) return -N_ERR_NO_MEMORY;
    if (h->guest_dom && !iommu_map(h->guest_dom, NULL, phys, pages)) {
        object_unref(memory);
        return -N_ERR_IO;
    }
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
    uint64_t f = spin_lock(&h->engine);
    bool up = h->up;
    h->seen_gen = h->power_gen;
    spin_unlock(&h->engine, f);
    if (up) hda_output_enable(o->out, STREAM_TAG);
    h->routed = up;
    __atomic_store_n(&h->open, o, __ATOMIC_RELEASE);
    *ring = (struct audio_ring){.memory = memory, .block_frames = (uint32_t)frames, .blocks = blocks,
                                .flush_bytes = cpu_features.clflush_bytes};
    return 0;
}

static void hda_close(struct audio_output *output) {
    struct hda_sink *o = output->priv;
    struct hda *h = o->h;
    stream_stop(h);
    __atomic_store_n(&h->open, NULL, __ATOMIC_RELEASE);
    hda_output_disable(o->out);
    h->routed = false;
    if (h->guest_dom) iommu_unmap(h->guest_dom, NULL, h->ring_phys, h->ring_pages);
    object_unref(h->ring_object);
    h->ring_object = NULL;
    h->ring = NULL;
}

static int hda_run(struct audio_output *output, bool run) {
    struct hda_sink *o = output->priv;
    struct hda *h = o->h;
    if (!run) {
        stream_stop(h);
        return 0;
    }
    uint64_t gen;
    if (!stream_sync(h, o, &gen)) return -N_ERR_NOT_READY;
    if (h->running) return 0;
    if (!stream_start(h, o, gen)) return -N_ERR_NOT_READY;
    if (h->quiet) alarm_set(&h->tick, clock_after(block_bytes(h) / h->frame_bytes * NS_PER_S / h->rate));
    return 0;
}

static void hda_status(struct audio_output *output, struct n_audio_device_status *status) {
    struct hda_sink *o = output->priv;
    struct hda *h = o->h;
    uint64_t gen;
    stream_sync(h, o, &gen);
    if (h->running) {
        position_update(h);
        unsigned delay = o->out->delay;
        status->position = h->pos > delay ? h->pos - delay : 0;
        status->at_ns = h->last_at;
        status->guard_frames = h->guard + delay;
        if (h->quiet) alarm_set(&h->tick, clock_after(block_bytes(h) / h->frame_bytes * NS_PER_S / h->rate));
    } else status->at_ns = clock_now_ns();
    status->generation = h->generation;
    status->running = h->running;
    status->errors = __atomic_load_n(&h->fifo_errors, __ATOMIC_RELAXED);
}

static const struct audio_output_ops hda_ops = {.open = hda_open, .close = hda_close, .run = hda_run, .status = hda_status};

/* ---- the outputs ---- */

/* The output's name, numbered when another of the machine's has it. */
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
    for (uint32_t i = 0; i < o->output.size_count; i++) /* the smallest container it fits (4.5.1) */
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

/* state held: what each codec has. A power change here leaves it to be done again. */
static void probe(struct hda *h) {
    uint64_t f = spin_lock(&h->engine), gen = h->power_gen;
    uint16_t present = h->codec_present;
    spin_unlock(&h->engine, f);
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
    f = spin_lock(&h->engine);
    bool same = gen == h->power_gen;
    spin_unlock(&h->engine, f);
    if (!same) return;
    for (unsigned at = 0; at < 15; at++) {
        if (!h->codecs[at]) continue;
        for (struct hda_output *o = h->outputs[at]; o; o = o->next)
            if (sink_add(h, h->codecs[at], o)) found++;
    }
    h->probed = true;
    kprintf("audio: HD Audio at %04x:%02x:%02x.%x (%04x:%04x): codecs %x, %u outputs, %s\n", PCI_ADDRESS_ARGS(h->pci->address),
            h->pci->vendor, h->pci->device, present, found, h->quiet ? "timed refill" : "interrupts");
}

/* state held: the outputs of the codecs told, and with look those that do not announce themselves. */
static void sense(struct hda *h, uint16_t codecs, bool look) {
    for (struct hda_sink *o = h->sinks; o; o = o->next) {
        bool due = codecs & (1u << o->codec->address) || (look && o->out->senses && (h->quiet || !o->out->announces));
        if (!due || !hda_output_sense(o->out)) continue;
        sink_label(o);
        audio_output_state(&o->output, jack_of(o->out), o->label);
    }
}

/* Reads the codecs, then answers what they announce, and tells them all again after the link was reset.
   Its own thread: reading a codec takes thousands of verbs. */
static void hda_thread(void *arg) {
    struct hda *h = arg;
    for (;;) {
        uint64_t seen = __atomic_load_n(&h->signals, __ATOMIC_SEQ_CST), wake = CLOCK_NEVER;
        mutex_lock(&h->state);
        if (!h->stopped && !h->probed) probe(h);
        if (!h->stopped && h->probed) {
            uint16_t due = 0;
            if (__atomic_exchange_n(&h->restore, false, __ATOMIC_SEQ_CST)) {
                for (unsigned at = 0; at < 15; at++)
                    if (h->codecs[at]) hda_codec_power(h->codecs[at], h->outputs[at], 1);
                due = UINT16_MAX;
            }
            uint64_t f = spin_lock(&h->engine);
            if (h->up) {
                rirb_take(h);
                if (!h->responses) { /* from now on they are few, and each may be a pin's */
                    write32(h, INTCTL, read32(h, INTCTL) | INT_CONTROLLER);
                    h->responses = true;
                }
            }
            due |= h->sense_due;
            h->sense_due = 0;
            spin_unlock(&h->engine, f);
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

/* ---- power, and a claim of its group ---- */

static bool hda_quiesce(struct hal_device *d) {
    struct hda *h = d->driver_data;
    uint64_t f = spin_lock(&h->engine);
    bool stopped = h->stopped;
    if (!stopped) link_stop(h);
    spin_unlock(&h->engine, f);
    if (stopped) return true;
    if (h->quiet) alarm_cancel(&h->tick);
    return link_down(h);
}

static bool hda_resume(struct hal_device *d) {
    struct hda *h = d->driver_data;
    if (__atomic_load_n(&h->stopped, __ATOMIC_RELAXED)) return true;
    bool ok = link_up(h);
    __atomic_store_n(&h->restore, true, __ATOMIC_SEQ_CST);
    signal(h);
    tell_open(h);
    return ok;
}

/* The group is about to change hands: its outputs go, then its link and everything it sends. */
static void hda_guest_stop(void *ctx) {
    struct hda *h = ctx;
    mutex_lock(&h->state);
    if (!h->stopped) {
        for (struct hda_sink *o = h->sinks; o; o = o->next) audio_output_available(&o->output, false);
        h->guest_dom = NULL;
        uint64_t f = spin_lock(&h->engine);
        link_stop(h);
        h->stopped = true;
        spin_unlock(&h->engine, f);
        link_down(h);
        if (h->quiet) alarm_cancel(&h->tick);
        irq_off(h);
        pci_disable_busmaster(h->pci, NULL);
    }
    mutex_unlock(&h->state);
}

/* dom: the claim's domain, in which its memory now has to be mapped; NULL: the group stays as it was. */
static void hda_guest_start(struct iommu_domain *dom, void *ctx) {
    struct hda *h = ctx;
    mutex_lock(&h->state);
    if (h->stopped) {
        if (dom && !iommu_map(dom, NULL, h->dma_phys, DMA_PAGES)) {
            kprintf("audio: HD Audio at %04x:%02x:%02x.%x: its memory cannot be mapped in its group's domain\n", PCI_ADDRESS_ARGS(h->pci->address));
        } else {
            h->guest_dom = dom;
            hal_enable(h->hal, HAL_ENABLE_MEMORY | HAL_ENABLE_DMA);
            uint64_t f = spin_lock(&h->engine);
            h->stopped = false;
            spin_unlock(&h->engine, f);
            if (link_up(h)) {
                irq_on(h);
                __atomic_store_n(&h->restore, true, __ATOMIC_SEQ_CST);
                for (struct hda_sink *o = h->sinks; o; o = o->next) audio_output_available(&o->output, true);
            } else kprintf("audio: HD Audio at %04x:%02x:%02x.%x does not come out of reset\n", PCI_ADDRESS_ARGS(h->pci->address));
            signal(h);
        }
    }
    mutex_unlock(&h->state);
}

/* ---- finding them ---- */

static int hda_attach(struct hal_device *hal) {
    struct pci_device *d = pci_device_of(hal);
    if (!hal_resource(hal, N_HAL_MEMORY, 0)) return -N_ERR_NOT_FOUND;
    struct hda *h = kzalloc(sizeof *h);
    if (!h) {
        kprintf("audio: HD Audio at %04x:%02x:%02x.%x found, but no memory for it\n", PCI_ADDRESS_ARGS(d->address));
        return -N_ERR_NO_MEMORY;
    }
    volatile uint8_t *regs = hal_map(hal, 0);
    /* below 4 GiB, which every controller can reach (64OK is optional, 3.3.2) */
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
    h->frame_bytes = 4; /* until a format is open */
    alarm_init(&h->tick, hda_tick);
    hal_enable(hal, HAL_ENABLE_MEMORY | HAL_ENABLE_DMA);
    bool up = link_up(h);
    uint32_t caps = up ? read16(h, GCAP) : 0;
    if (!up || (!GCAP_OSS(caps) && !GCAP_BSS(caps))) {
        /* no driver runs it: an app may */
        kprintf("audio: HD Audio at %04x:%02x:%02x.%x %s\n", PCI_ADDRESS_ARGS(d->address), up ? "has no stream to play on" : "does not come out of reset");
        uint64_t f = spin_lock(&h->engine);
        link_stop(h);
        spin_unlock(&h->engine, f);
        link_down(h);
        hal->driver_data = NULL;
        kfree(h);
        return up ? HAL_DECLINE : -N_ERR_IO;
    }
    irq_on(h);
    h->guest = (struct iommu_guest){.stop = hda_guest_stop, .start = hda_guest_start, .ctx = h};
    d->guest = &h->guest;
    h->thread = kthread_create("hda", hda_thread, h);
    if (!h->thread) {
        kprintf("audio: HD Audio at %04x:%02x:%02x.%x: no memory for its thread\n", PCI_ADDRESS_ARGS(d->address));
        d->guest = NULL;
        irq_off(h);
        uint64_t f = spin_lock(&h->engine);
        link_stop(h);
        spin_unlock(&h->engine, f);
        link_down(h);
        hal->driver_data = NULL;
        kfree(h);
        return -N_ERR_NO_MEMORY;
    }
    return 0;
}

/* Every PCI function of the audio class: a chipset's, a graphics card's. */
static const struct n_hal_rule hda_rules[] = {N_PCI_CLASS(0x04, 0x03)};
static const struct hal_driver hda_driver = {
    .name = "hda",
    .rules = hda_rules,
    .rule_count = ARRAY_SIZE(hda_rules),
    .attach = hda_attach,
    .quiesce = hda_quiesce,
    .resume = hda_resume,
};
HAL_DRIVER(hda_driver);
