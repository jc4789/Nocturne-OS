/* Intel High Definition Audio controllers (High Definition Audio Specification 1.0a; section numbers
   are its): every PCI function of the class says it is one, whoever made it, a chipset's or a
   graphics card's. Each gets its link up, its codecs read (dev/hda_codec.c), and each output of its
   codecs becomes an output of the sound layer (dev/audio.h).

   Verbs go through the CORB and come back through the RIRB one at a time, polled (one thread at a
   time waits for its answer); the RIRB's interrupt only tells the controller's thread that a pin has
   announced something (4.4.1, 7.3.3.14).

   A controller plays one output at a time, as the layer does, on one output stream (or a bidirectional
   one where it has no other), through a ring of equal blocks that the stream walks without end. Each
   block asks for an interrupt, which wakes the audio thread. Its fill() reports where the link is,
   silences what the link has passed (the stream does not stop by itself, and comes round to it) and
   mixes the next blocks in. Each frame of the ring has a place on a count of frames from the stream's
   start that never wraps: `pos` is where the link is, `written` where the mix has got to, and what lies
   between is queued. A stream that runs out of PCM plays the silence, and stops only after a while of
   it: starting takes time, and clicks.

   What was played is what LPIB says the link has sent, and the pin's delay behind it, read from the
   device at each report, with the clock read right after it under `engine`: audio_played gets the
   pair. Nothing is added up from time or from the amounts given, so no error can build up between the
   report and the device: a turn of the ring missed or counted too many only changes which frames lie
   where on the count, not which frame of the ring the link is at. The clock only tells how many turns a
   report that comes very late has missed. The device's crystal runs at its own rate: whoever plays to
   a clock follows these pairs (the browser's media clock does, in user/libc/web/avmedia.c).

   A function's memory comes from the kernel and it passes the IOMMU untranslated, as every function
   the kernel runs. A graphics card's sound function may share an isolation group with the card's
   display function, which an app drives (dev/pciuser.c): it then is that claim's guest (iommu_guest):
   stopped while the group changes hands, and on again with its memory mapped in the claim's domain.

   Locks, in order: the claim's and IOMMU's mutexes, `state` (what the controller is doing as a whole:
   probing, stopped for a claim, sensing), the sound layer's mutex and lock, verb_lock, `engine`.
   `engine` is a leaf: nothing is called under it but registers. A power change (quiesce and resume, on
   a CPU that may not wait for any of the mutexes) comes between any two steps of the threads: it
   changes up and power_gen and stops the hardware under `engine`, and each of them writes what
   starts something only under `engine`, having seen the generation it began with there. */
#include "dev/audio.h"
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/spin.h"
#include "dev/acpi.h"
#include "dev/hda_codec.h"
#include "dev/iommu.h"
#include "dev/pci.h"
#include "dev/pm.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sys/alarm.h"
#include "sys/clock.h"
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

#define BLOCK       256u               /* frames: a block ends every 5.33 ms */
#define BLOCK_NS    (BLOCK * NS_PER_S / AUDIO_RATE)
#define BLOCKS      32u                /* the stream's descriptors */
#define RING_FRAMES (BLOCK * BLOCKS)
#define WIDEST      8u                 /* bytes of a frame: two channels in containers of 4 */
#define AHEAD_FIRST 4u                 /* blocks kept queued at first... */
#define AHEAD_MOST  16u                /* ... and after the link has overtaken the mix again and again */
#define GUARD_FRAMES 96u               /* how far past the FIFO the link may get while a block is written */
#define IDLE_NS     NS_PER_S           /* of silence before the stream stops */
#define POLL_NS     NS_PER_S           /* between looks at a jack that does not announce itself */
#define VERB_NS     (10 * NS_PER_MS)   /* a codec silent this long is gone (until the link's next reset) */
#define RESET_NS    (100 * NS_PER_MS)  /* the longest a controller may be given to leave or enter a reset */
#define STOP_NS     NS_PER_MS          /* a stream's or ring's run bit reads 0 by then (40 us, 3.3.35) */
#define CODEC_WAKE_NS (521 * NS_PER_US) /* after CRST# for codecs to ask for an address (4.3) */
#define STREAM_TAG  1u                 /* the stream's number on the link: there is one per controller */

/* one page of rings: the CORB, then the RIRB; a page of descriptors; the blocks */
#define RINGS_PAGES 1
#define BDL_PAGES   1
#define RING_PAGES  (ALIGN_UP(RING_FRAMES * WIDEST, PAGE_SIZE) / PAGE_SIZE)
#define DMA_PAGES   (RINGS_PAGES + BDL_PAGES + RING_PAGES)

struct bdl_entry { /* 3.6.3 */
    uint64_t address;
    uint32_t length;
    uint32_t flags; /* bit 0: interrupt on completion */
} PACKED;

struct hda;
struct hda_sink {
    struct audio_sink sink;
    struct hda *h;
    struct hda_output *out;
    struct hda_codec *codec;
    struct hda_sink *next;  /* of its controller */
    struct hda_sink *every; /* of the machine (labels) */
    char label[32];         /* the output's name, told from its namesakes */
};

struct hda {
    struct hda_link link; /* first: the verbs' way back to it */
    struct pci_device *pci;
    volatile uint8_t *regs;
    volatile uint8_t *sd;   /* the output stream descriptor's registers */
    unsigned sd_index;      /* and its bit in INTCTL and INTSTS */
    bool bidirectional;
    uint64_t dma_phys;
    uint32_t *corb;
    volatile uint64_t *rirb;
    struct bdl_entry *bdl;
    uint8_t *ring;
    struct iommu_guest guest;
    struct pm_device pm;

    struct mutex state;
    bool stopped;      /* a claim's guest out of use; written with `engine` too */
    bool probed;
    struct irq *line;  /* its INTx, where its messages cannot be used */
    bool messages;
    bool quiet;        /* neither: tick wakes the audio thread, and the thread looks every POLL_NS */
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

    /* the sound layer's calls', one at a time (audio_sink_ops), with fill() and open() only ever after
       a power change seen in seen_gen */
    uint64_t seen_gen;
    bool routed, running;
    unsigned frame_bytes;
    uint64_t pos, written, cleared; /* frames from the stream's start */
    uint64_t given;                 /* the ordinal after the last frame given to the ring */
    uint64_t mixed;                 /* frames given to it since the stream began */
    uint32_t last_lpib;
    uint64_t last_at;               /* when last_lpib was read */
    uint64_t started_at;
    uint64_t idle_since;            /* when it began to play nothing, 0 while it plays */
    uint32_t guard;
    unsigned ahead;
    bool fed;
    unsigned late;
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
    uint64_t rings = h->dma_phys, rirb = rings + PAGE_SIZE / 2;
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

/* engine held: what the RIRB holds. The answer to the verb being waited for in *answer, and true when
   it is there; what pins announced is noted. */
static bool rirb_take(struct hda *h, uint32_t *answer) {
    bool got = false;
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
        else if (answer) {
            *answer = (uint32_t)entry;
            got = true;
        }
    }
    return got;
}

/* One verb (struct hda_link): written under `engine`, its answer polled for. */
static uint32_t link_verb(struct hda_link *link, unsigned codec, unsigned nid, uint32_t command) {
    struct hda *h = (struct hda *)link;
    uint32_t answer = 0;
    mutex_lock(&h->verb_lock);
    uint64_t f = spin_lock(&h->engine), gen = h->power_gen;
    bool sent = h->up && !(h->silent & (1u << codec));
    if (sent) {
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
        bool got = !gone && rirb_take(h, &answer);
        announced = announced || h->sense_due;
        spin_unlock(&h->engine, f);
        if (got || gone) break;
        if (clock_now_ns() >= until) {
            f = spin_lock(&h->engine);
            h->silent |= (uint16_t)(1u << codec);
            spin_unlock(&h->engine, f);
            kprintf("audio: HD Audio codec %u at %02x:%02x.%u does not answer verb %x of node %u\n", codec, h->pci->bus,
                    h->pci->dev, h->pci->func, command, nid);
            break;
        }
        pause();
    }
    mutex_unlock(&h->verb_lock);
    if (announced) signal(h);
    return answer;
}

/* ---- interrupts ---- */

/* A block played or the stream has an error; a response came in, which is a pin's when no verb waits. */
static enum irq_result hda_irq(void *context) {
    struct hda *h = context;
    uint32_t status = read32(h, INTSTS);
    if (!status || status == UINT32_MAX) return IRQ_NONE;
    if (status & (1u << h->sd_index)) {
        uint8_t stream = sd_read8(h, SD_STS);
        sd_write8(h, SD_STS, stream & (STS_BCIS | STS_FIFOE | STS_DESE));
        if (stream & (STS_FIFOE | STS_DESE)) __atomic_add_fetch(&h->fifo_errors, 1, __ATOMIC_RELAXED);
        audio_sink_wake();
    }
    if (status & INT_CONTROLLER) {
        write8(h, RIRBSTS, read8(h, RIRBSTS) & RIRBSTS_ALL);
        signal(h);
    }
    return IRQ_HANDLED;
}

static void hda_tick(struct alarm *a, struct regs *r) {
    (void)a;
    (void)r;
    audio_sink_wake();
}

/* state held: its interrupt on. A message when it may send one; a guest in a claim's domain does not:
   the domain's interrupt table is the claimed function's. Else its INTx, else nothing. */
static void irq_on(struct hda *h, bool messages) {
    struct pci_device *d = h->pci;
    struct irq_spec spec = {.name = "HD Audio", .handler = hda_irq, .context = h, .node = d->node};
    h->messages = messages && pci_irq_enable(d, &spec, 1, NULL);
    h->quiet = false;
    if (h->messages) return;
    uint32_t gsi;
    if (acpi_pci_gsi(d, &gsi)) {
        pci_write16(d->bus, d->dev, d->func, PCI_COMMAND,
                    pci_read16(d->bus, d->dev, d->func, PCI_COMMAND) & ~PCI_COMMAND_INTX_DISABLE);
        h->line = irq_request_gsi(&spec, gsi);
    }
    h->quiet = !h->line;
}

/* state held; its source is off. */
static void irq_off(struct hda *h) {
    struct pci_device *d = h->pci;
    if (h->messages) pci_irq_disable(d);
    if (h->line) {
        pci_write16(d->bus, d->dev, d->func, PCI_COMMAND,
                    pci_read16(d->bus, d->dev, d->func, PCI_COMMAND) | PCI_COMMAND_INTX_DISABLE);
        irq_free(h->line);
    }
    h->messages = false;
    h->line = NULL;
}

/* ---- the stream ---- */

static uint8_t *ring_at(const struct hda *h, uint64_t frame) { return h->ring + frame % RING_FRAMES * h->frame_bytes; }

static uint32_t block_bytes(const struct hda *h) { return BLOCK * h->frame_bytes; }

static void block_silence(const struct hda *h, uint64_t frame) {
    uint8_t *at = ring_at(h, frame);
    memset(at, 0, block_bytes(h));
    iommu_cache_flush(at, block_bytes(h));
}

/* The count of frames starts again, on a silent ring. */
static void ring_reset(struct hda *h) {
    memset(h->ring, 0, RING_FRAMES * WIDEST);
    iommu_cache_flush(h->ring, RING_FRAMES * WIDEST);
    h->pos = h->written = h->cleared = 0;
    h->last_lpib = 0;
    h->idle_since = 0;
    h->ahead = AHEAD_FIRST;
    h->fed = false;
    h->late = 0;
    h->mixed = 0;
    h->running = false;
}

/* A stream that was running stops (RUN to 0 takes up to 40 us), its ring silent. */
static void stream_stop(struct hda *h, const char *name) {
    if (!h->running) return;
    uint64_t f = spin_lock(&h->engine);
    if (h->up) sd_write8(h, SD_CTL, 0);
    spin_unlock(&h->engine, f);
    for (uint64_t until = clock_after(STOP_NS); (sd_read8(h, SD_CTL) & CTL_RUN) && clock_now_ns() < until;) pause();
    if (h->quiet) alarm_cancel(&h->tick);
    uint64_t ms = (h->last_at - h->started_at) / NS_PER_MS;
    kprintf("audio: output \"%s\" stopped: played %lu frames, link at %lu Hz, %u late, %u stream errors\n", name, h->mixed,
            ms ? h->pos * 1000 / ms : 0, h->late,
            __atomic_load_n(&h->fifo_errors, __ATOMIC_RELAXED));
    ring_reset(h);
}

/* The descriptors and the stream's registers for this format (4.5.3); it runs from the first frame.
   False after a power change, and it does not run. */
static bool stream_start(struct hda *h, struct hda_sink *o, uint64_t gen) {
    uint32_t bytes = block_bytes(h);
    for (unsigned i = 0; i < BLOCKS; i++)
        h->bdl[i] = (struct bdl_entry){.address = h->dma_phys + (RINGS_PAGES + BDL_PAGES) * PAGE_SIZE + i * bytes,
                                       .length = bytes, .flags = 1};
    iommu_cache_flush(h->bdl, BLOCKS * sizeof *h->bdl);
    uint64_t bdl = h->dma_phys + RINGS_PAGES * PAGE_SIZE;
    sd_write8(h, SD_CTL, CTL_SRST);
    for (uint64_t until = clock_after(STOP_NS); !(sd_read8(h, SD_CTL) & CTL_SRST) && clock_now_ns() < until;) pause();
    sd_write8(h, SD_CTL, 0);
    for (uint64_t until = clock_after(STOP_NS); (sd_read8(h, SD_CTL) & CTL_SRST) && clock_now_ns() < until;) pause();
    mmio_write32(h->sd + SD_CBL, RING_FRAMES * h->frame_bytes);
    mmio_write16(h->sd + SD_LVI, BLOCKS - 1);
    mmio_write16(h->sd + SD_FMT, hda_output_format(o->out));
    mmio_write32(h->sd + SD_BDPL, (uint32_t)bdl);
    mmio_write32(h->sd + SD_BDPU, (uint32_t)(bdl >> 32));
    sd_write8(h, SD_CTL2, CTL2_TAG(STREAM_TAG) | (h->bidirectional ? CTL2_DIR : 0));
    sd_write8(h, SD_STS, STS_BCIS | STS_FIFOE | STS_DESE);
    /* the FIFO's content is what the controller has fetched and the link has not played (3.3.41) */
    h->guard = mmio_read16(h->sd + SD_FIFOS) / h->frame_bytes + GUARD_FRAMES;
    h->ahead = MIN(MAX(h->ahead, h->guard / BLOCK + 2), AHEAD_MOST);
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
    if (lpib >= RING_FRAMES) return; /* no valid position to read */
    uint64_t delta = (lpib + RING_FRAMES - h->last_lpib) % RING_FRAMES;
    uint64_t elapsed = (now - h->last_at) * AUDIO_RATE / NS_PER_S;
    if (elapsed > delta + RING_FRAMES / 2) delta += (elapsed - delta + RING_FRAMES / 2) / RING_FRAMES * RING_FRAMES;
    h->pos += delta;
    h->last_lpib = lpib;
    h->last_at = now;
}

/* The ordinal of the frame at `frame`, one mixed and not yet overwritten. */
static uint64_t ordinal_at(const struct hda *h, uint64_t frame) {
    if (frame >= h->written) return h->given;
    uint64_t back = h->written - frame;
    return h->given >= back ? h->given - back : 0;
}

/* Whether the link is up, *gen its generation, and whatever a power change undid is taken up again. */
static bool fill_begin(struct hda *h, struct hda_sink *o, uint64_t *gen) {
    uint64_t f = spin_lock(&h->engine);
    bool up = h->up;
    *gen = h->power_gen;
    spin_unlock(&h->engine, f);
    if (*gen != h->seen_gen) { /* what it held is gone with the reset, and counts as played */
        h->seen_gen = *gen;
        audio_played(h->given, clock_now_ns());
        ring_reset(h);
        h->routed = false;
    }
    if (up && !h->routed) {
        hda_output_enable(o->out, STREAM_TAG);
        h->routed = true;
    }
    return up;
}

static void hda_fill(struct audio_sink *sink) {
    struct hda_sink *o = sink->priv;
    struct hda *h = o->h;
    uint64_t gen;
    if (!fill_begin(h, o, &gen)) return;
    if (h->running) {
        position_update(h);
        unsigned delay = o->out->delay;
        audio_played(ordinal_at(h, h->pos > delay ? h->pos - delay : 0), h->last_at);
        /* the blocks passed go silent before the stream comes round to them */
        if (h->pos - h->cleared > RING_FRAMES) h->cleared = ALIGN_DOWN(h->pos, BLOCK) - RING_FRAMES;
        for (; h->cleared + BLOCK <= h->pos; h->cleared += BLOCK) block_silence(h, h->cleared);
    }
    bool remote = audio_remote(), pending = !remote && audio_pending();
    if (!h->running && !pending) return; /* nothing to start: no device I/O */
    if (h->running && pending && h->fed && h->pos >= h->written) { /* it heard silence with PCM waiting */
        h->late++;
        h->ahead = MIN(h->ahead + 1, AHEAD_MOST);
    }
    bool played_all = h->pos >= h->written;
    if (h->running && played_all && !pending) {
        if (!h->idle_since) h->idle_since = h->last_at;
        if (h->last_at - h->idle_since >= IDLE_NS) {
            stream_stop(h, sink->name);
            return;
        }
    } else h->idle_since = 0;

    uint64_t began = h->written;
    while (!remote) {
        uint64_t queued = h->written > h->pos ? h->written - h->pos : 0;
        if (queued >= h->ahead * (uint64_t)BLOCK) break;
        /* whole blocks while the device has enough: a stream fed a little at a time (audio_ready) */
        if (!audio_ready(BLOCK, queued < 2 * (uint64_t)BLOCK)) break;
        /* past what the controller may have fetched */
        uint64_t at = h->written;
        if (h->running && at < h->pos + h->guard) at = ALIGN_UP(h->pos + h->guard, BLOCK);
        uint64_t end;
        if (!audio_sink_mix(sink, ring_at(h, at), BLOCK, &end)) break;
        iommu_cache_flush(ring_at(h, at), block_bytes(h));
        h->written = at + BLOCK;
        h->given = end;
        h->mixed += BLOCK;
    }
    h->fed = h->written > h->pos;
    if (!h->running && h->written > began && !stream_start(h, o, gen)) return; /* the next fill() sees the new generation */
    if (h->running && h->quiet) alarm_set(&h->tick, clock_after(BLOCK_NS));
}

/* Where the PCM the controller has not fetched begins, at a block's start: the rest of the block the
   link is in has its frames in the FIFO's reach, and take stops the ring short of it. The mixer's lock
   is held: no fill() is under way. */
static uint64_t hda_unissue(struct audio_sink *sink, bool take) {
    struct hda_sink *o = sink->priv;
    struct hda *h = o->h;
    uint64_t f = spin_lock(&h->engine);
    bool up = h->up && h->seen_gen == h->power_gen;
    spin_unlock(&h->engine, f);
    if (!up || !h->running) return h->given;
    position_update(h);
    uint64_t first = ALIGN_UP(h->pos + h->guard, BLOCK);
    if (first >= h->written) return h->given;
    uint64_t at = ordinal_at(h, first);
    if (take) {
        for (uint64_t frame = first; frame < h->written; frame += BLOCK) block_silence(h, frame);
        h->mixed -= MIN(h->mixed, h->written - first);
        h->given = at;
        h->written = first;
    }
    return at;
}

static bool hda_open(struct audio_sink *sink) {
    struct hda_sink *o = sink->priv;
    struct hda *h = o->h;
    h->frame_bytes = (unsigned)sink->format.channels * sink->format.container;
    ring_reset(h);
    uint64_t f = spin_lock(&h->engine);
    bool up = h->up;
    h->seen_gen = h->power_gen;
    spin_unlock(&h->engine, f);
    if (up) hda_output_enable(o->out, STREAM_TAG);
    h->routed = up;
    return true;
}

static void hda_close(struct audio_sink *sink) {
    struct hda_sink *o = sink->priv;
    struct hda *h = o->h;
    stream_stop(h, sink->name);
    hda_output_disable(o->out);
    h->routed = false;
    audio_played(h->given, clock_now_ns());
}

static const struct audio_sink_ops hda_ops = {.open = hda_open, .close = hda_close, .fill = hda_fill, .unissue = hda_unissue};

/* ---- the outputs ---- */

/* The output's name, numbered when another of the machine's has it. */
static void sink_label(struct hda_sink *o) {
    mutex_lock(&label_mutex);
    for (unsigned n = 1;; n++) {
        if (n == 1) strlcpy(o->label, o->out->name, sizeof o->label);
        else ksnprintf(o->label, sizeof o->label, "%.28s %u", o->out->name, n);
        bool taken = false;
        for (struct hda_sink *s = every_sink; s && !taken; s = s->every) taken = s != o && !strcmp(s->label, o->label);
        if (!taken) break;
    }
    mutex_unlock(&label_mutex);
}

static enum audio_jack jack_of(const struct hda_output *out) {
    return !out->senses ? AUDIO_JACK_NONE : out->plugged ? AUDIO_JACK_PLUGGED : AUDIO_JACK_EMPTY;
}

static bool sink_add(struct hda *h, struct hda_codec *codec, struct hda_output *out) {
    static const enum audio_kind kinds[] = {[HDA_OUTPUT_SPEAKER] = AUDIO_KIND_SPEAKER, [HDA_OUTPUT_HEADPHONE] = AUDIO_KIND_HEADPHONE,
                                            [HDA_OUTPUT_LINE] = AUDIO_KIND_LINE, [HDA_OUTPUT_DISPLAY] = AUDIO_KIND_DISPLAY,
                                            [HDA_OUTPUT_DIGITAL] = AUDIO_KIND_DIGITAL, [HDA_OUTPUT_OTHER] = AUDIO_KIND_OTHER};
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
    o->sink.ops = &hda_ops;
    o->sink.priv = o;
    strlcpy(o->sink.name, o->label, sizeof o->sink.name);
    o->sink.kind = kinds[out->kind];
    o->sink.jack = jack_of(out);
    unsigned container = out->bits == 16 ? 2 : 4;
    o->sink.format = (struct audio_format){.rate = AUDIO_RATE, .channels = out->channels, .bits = out->bits, .container = (uint8_t)container};
    o->sink.period_ns = BLOCK_NS;
    o->sink.blocks = BLOCKS;
    struct hda_sink **tail = &h->sinks;
    while (*tail) tail = &(*tail)->next;
    *tail = o;
    kprintf("audio: HD Audio codec %u pin %u: \"%s\" (%u bits, %u channels, %s)\n", codec->address, out->pin->nid, o->label,
            out->bits, out->channels, !out->senses ? "no jack sense" : out->plugged ? "plugged in" : "nothing plugged in");
    audio_sink_add(&o->sink);
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
            kprintf("audio: HD Audio codec %u at %02x:%02x.%u has no audio function that can be read\n", at, h->pci->bus,
                    h->pci->dev, h->pci->func);
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
    kprintf("audio: HD Audio at %02x:%02x.%u (%04x:%04x): codecs %x, %u outputs, %s\n", h->pci->bus, h->pci->dev, h->pci->func,
            h->pci->vendor, h->pci->device, present, found, h->messages ? "message interrupts" : h->line ? "INTx" : "timed refill");
}

/* state held: the outputs of the codecs told, and with look those that do not announce themselves. */
static void sense(struct hda *h, uint16_t codecs, bool look) {
    for (struct hda_sink *o = h->sinks; o; o = o->next) {
        bool due = codecs & (1u << o->codec->address) || (look && o->out->senses && (h->quiet || !o->out->announces));
        if (!due || !hda_output_sense(o->out)) continue;
        sink_label(o);
        audio_sink_state(&o->sink, jack_of(o->out), o->label);
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
                rirb_take(h, NULL);
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

static bool hda_quiesce(void *arg) {
    struct hda *h = arg;
    uint64_t f = spin_lock(&h->engine);
    bool stopped = h->stopped;
    if (!stopped) link_stop(h);
    spin_unlock(&h->engine, f);
    if (stopped) return true;
    if (h->quiet) alarm_cancel(&h->tick);
    return link_down(h);
}

static bool hda_resume(void *arg) {
    struct hda *h = arg;
    if (__atomic_load_n(&h->stopped, __ATOMIC_RELAXED)) return true;
    bool ok = link_up(h);
    __atomic_store_n(&h->restore, true, __ATOMIC_SEQ_CST);
    signal(h);
    audio_sink_wake();
    return ok;
}

/* The group is about to change hands: its outputs go, then its link and everything it sends. */
static void hda_guest_stop(void *ctx) {
    struct hda *h = ctx;
    mutex_lock(&h->state);
    if (!h->stopped) {
        for (struct hda_sink *o = h->sinks; o; o = o->next) audio_sink_available(&o->sink, false);
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
            kprintf("audio: HD Audio at %02x:%02x.%u: its memory cannot be mapped in its group's domain\n", h->pci->bus,
                    h->pci->dev, h->pci->func);
        } else {
            pci_enable_busmaster(h->pci);
            uint64_t f = spin_lock(&h->engine);
            h->stopped = false;
            spin_unlock(&h->engine, f);
            if (link_up(h)) {
                irq_on(h, !dom);
                __atomic_store_n(&h->restore, true, __ATOMIC_SEQ_CST);
                for (struct hda_sink *o = h->sinks; o; o = o->next) audio_sink_available(&o->sink, true);
            } else kprintf("audio: HD Audio at %02x:%02x.%u does not come out of reset\n", h->pci->bus, h->pci->dev, h->pci->func);
            signal(h);
        }
    }
    mutex_unlock(&h->state);
}

/* ---- finding them ---- */

static void controller_add(struct pci_device *d) {
    uint64_t base = pci_bar_address(d, 0), size = pci_bar_size(d, 0);
    if (!base || !size) return;
    struct hda *h = kzalloc(sizeof *h);
    /* below 4 GiB, which every controller can reach (64OK is optional, 3.3.2) */
    uint64_t dma = h ? pmm_alloc_contig(DMA_PAGES, 32) : 0;
    volatile uint8_t *regs = dma ? vmm_map_mmio(base, size) : NULL;
    if (!regs) {
        kprintf("audio: HD Audio at %02x:%02x.%u found, but no memory for it\n", d->bus, d->dev, d->func);
        if (dma) pmm_free_contig(dma, DMA_PAGES);
        if (h) kfree(h);
        return;
    }
    h->link.verb = link_verb;
    h->pci = d;
    h->regs = regs;
    h->dma_phys = dma;
    uint8_t *memory = phys_to_virt(dma);
    memset(memory, 0, DMA_PAGES * PAGE_SIZE);
    iommu_cache_flush(memory, DMA_PAGES * PAGE_SIZE);
    h->corb = (uint32_t *)memory;
    h->rirb = (volatile uint64_t *)(memory + PAGE_SIZE / 2);
    h->bdl = (struct bdl_entry *)(memory + RINGS_PAGES * PAGE_SIZE);
    h->ring = memory + (RINGS_PAGES + BDL_PAGES) * PAGE_SIZE;
    h->frame_bytes = 4;
    alarm_init(&h->tick, hda_tick);
    pci_enable_busmaster(d);
    bool up = link_up(h);
    uint32_t caps = up ? read16(h, GCAP) : 0;
    if (!up || (!GCAP_OSS(caps) && !GCAP_BSS(caps))) {
        /* no driver runs it: an app may */
        kprintf("audio: HD Audio at %02x:%02x.%u %s\n", d->bus, d->dev, d->func, up ? "has no stream to play on" : "does not come out of reset");
        uint64_t f = spin_lock(&h->engine);
        link_stop(h);
        spin_unlock(&h->engine, f);
        link_down(h);
        pci_disable_busmaster(d, NULL);
        d->kernel_owned = false;
        pmm_free_contig(dma, DMA_PAGES);
        kfree(h);
        return;
    }
    irq_on(h, true);
    h->guest = (struct iommu_guest){.stop = hda_guest_stop, .start = hda_guest_start, .ctx = h};
    d->guest = &h->guest;
    h->pm = (struct pm_device){.name = "hda", .stage = PM_STAGE_IO, .quiesce = hda_quiesce, .resume = hda_resume, .ctx = h};
    pm_register(&h->pm);
    h->thread = kthread_create("hda", hda_thread, h);
    if (!h->thread) kprintf("audio: HD Audio at %02x:%02x.%u: no memory for its thread\n", d->bus, d->dev, d->func);
}

void hda_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        struct pci_device *d = pci_at(i);
        if (d->cls == 0x04 && d->subcls == 0x03) controller_add(d);
    }
}
