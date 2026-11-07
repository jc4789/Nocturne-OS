/* Intel AC'97 audio (ICH and compatibles; QEMU's -device AC97, VirtualBox's default): one output of
   the sound layer (dev/audio.h), at 48 kHz stereo. The PCM-out DMA engine walks a ring of its 32 buffer
   descriptors, of the length the open asks for, from the one playing (CIV) up to the last valid one (LVI). The ring is
   shared with the service that opened the output, which writes it. To walk it without end, LVI stays
   one behind CIV: every descriptor asks for an interrupt once the engine has played it (IOC), which
   moves LVI on and tells the service to look; a card whose interrupt line is not known takes an
   alarm at the end of each buffer instead. */
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

/* mixer registers (NAM, BAR0) */
#define NAM_RESET      0x00
#define NAM_MASTER     0x02
#define NAM_PCM_OUT    0x18
#define NAM_EXT_ID     0x28
#define NAM_EXT_CTRL   0x2A
#define NAM_FRONT_RATE 0x2C
/* bus master registers (NABM, BAR1): PCM out box, global control */
#define PO_BDBAR  0x10
#define PO_CIV    0x14
#define PO_LVI    0x15
#define PO_SR     0x16
#define PO_PICB   0x18 /* remaining 16-bit samples in current descriptor */
#define PO_CR     0x1B
#define GLOB_CNT  0x2C
#define GLOB_STA  0x30

#define CR_RUN     0x01
#define CR_RESET   0x02
#define CR_LVBIE   0x04 /* interrupt when the last valid buffer has been played */
#define CR_IOCE    0x10 /* interrupt on the completion of a descriptor that asks for it */
#define SR_HALTED  0x01
#define SR_CELV    0x02 /* final descriptor has been processed, not merely CIV == LVI */
#define SR_CLEAR   0x1C /* write 1 to clear: last valid buffer, completion, FIFO error */
#define BD_IOC     0x8000 /* a descriptor's flags: interrupt on its completion */

#define NBUF       32    /* the descriptors of its list */
#define RATE       48000
#define FRAME      4     /* bytes: two 16-bit samples */
#define BUF_MOST   32766 /* frames of a descriptor: its length is 16 bits of samples, of which the last is even */

struct bd {
    uint32_t addr;
    uint16_t samples; /* 16-bit samples, not frames */
    uint16_t flags;
} PACKED;

/* One codec. The fields below engine are the sound layer's calls', which come one at a time
   (audio_output_ops). A power change comes between any two steps of them, on a CPU that may not
   wait for them: it changes hw_ready and power_gen and stops the engine under `engine`, and a call
   starts the engine only under it, having seen hw_ready and its generation there. */
struct ac97 {
    struct audio_output output; /* first: the sound layer hands it back */
    struct spinlock engine;
    bool hw_ready; /* the codec and the PCM-out box are set up (not during sleep) */
    uint64_t power_gen, seen_gen;
    uint32_t generation;
    uint16_t nam, nabm;
    struct bd *bdl_view;
    uint32_t bdl_bus;
    bool running;
    bool has_irq;        /* its interrupt is routed: without one an alarm refills at the end of each buffer */
    struct alarm refill; /* the end of the playing buffer */
    struct hal_device *hal;
    uint32_t buf_frames; /* of what is open */
    uint64_t buf_ns;
    uint64_t played; /* the frames of the buffers finished since the engine started */
    int last_civ;
    uint64_t last_at;
    struct object *ring_object;
};

static struct ac97 *ac97_of(struct audio_output *output) { return (struct ac97 *)output; }

/* engine held: LVI one behind what plays, so the engine never comes to it. */
static void ring_keep(struct ac97 *c) {
    outb(c->nabm + PO_LVI, (uint8_t)((inb(c->nabm + PO_CIV) + NBUF - 1) & (NBUF - 1)));
}

/* The engine's interrupt: a buffer played. Its status bits are answered here, so what the engine does
   after this interrupts again. The line may be another device's too: its interrupt finds none of this
   engine's status bits. */
static enum irq_result ac97_irq(void *context) {
    struct ac97 *c = context;
    uint16_t status = inw(c->nabm + PO_SR) & SR_CLEAR;
    if (!status) return IRQ_NONE;
    outw(c->nabm + PO_SR, status);
    uint64_t f = spin_lock(&c->engine);
    if (c->hw_ready && c->running) ring_keep(c);
    spin_unlock(&c->engine, f);
    audio_output_period(&c->output);
    return IRQ_HANDLED;
}

static void ac97_refill(struct alarm *a, struct regs *r) {
    (void)r;
    struct ac97 *c = (struct ac97 *)((char *)a - __builtin_offsetof(struct ac97, refill));
    uint64_t f = spin_lock(&c->engine);
    bool on = c->hw_ready && c->running;
    if (on) ring_keep(c);
    spin_unlock(&c->engine, f);
    if (on) alarm_set(&c->refill, clock_after(c->buf_ns));
    audio_output_period(&c->output);
}

/* A new generation: what the engine held is gone with the reset. */
static void engine_sync(struct ac97 *c) {
    uint64_t f = spin_lock(&c->engine);
    uint64_t gen = c->power_gen;
    spin_unlock(&c->engine, f);
    if (gen == c->seen_gen) return;
    c->seen_gen = gen;
    c->generation++;
    c->running = false;
}

static void ac97_status(struct audio_output *output, struct n_audio_device_status *status) {
    struct ac97 *c = ac97_of(output);
    engine_sync(c);
    status->generation = c->generation;
    status->running = c->running;
    status->at_ns = clock_now_ns();
    if (!c->running) return;
    uint64_t f = spin_lock(&c->engine);
    bool ready = c->hw_ready;
    int civ = c->last_civ;
    unsigned remaining = c->buf_frames * 2;
    uint64_t at = status->at_ns;
    if (ready) {
        /* PICB and CIV are separate registers: PICB is CIV's when CIV reads the same after it. One
           descriptor crossing between the reads is normal: once more against the new CIV. */
        int confirmed = inb(c->nabm + PO_CIV) & (NBUF - 1);
        for (int tries = 0; tries < 2; tries++) {
            civ = confirmed;
            remaining = inw(c->nabm + PO_PICB);
            at = clock_now_ns();
            confirmed = inb(c->nabm + PO_CIV) & (NBUF - 1);
            if (confirmed == civ) break;
        }
    }
    spin_unlock(&c->engine, f);
    uint64_t moved = (uint64_t)((civ + NBUF - c->last_civ) & (NBUF - 1));
    uint64_t late = (at - c->last_at) / c->buf_ns;
    if (late > moved + NBUF / 2) moved += (late - moved + NBUF / 2) / NBUF * NBUF; /* whole turns while no one looked */
    c->played += moved * c->buf_frames;
    c->last_civ = civ;
    c->last_at = at;
    if (remaining > c->buf_frames * 2) remaining = c->buf_frames * 2;
    status->position = c->played + c->buf_frames - remaining / 2;
    status->at_ns = at;
    status->guard_frames = remaining / 2 + c->buf_frames; /* the engine has fetched the descriptor after it */
}

/* The PCM-out box reset, polling the clock, and its descriptors given again. */
static bool box_reset(struct ac97 *c) {
    outb(c->nabm + PO_CR, CR_RESET);
    uint64_t until = clock_after(100 * NS_PER_MS);
    while ((inb(c->nabm + PO_CR) & CR_RESET) && clock_now_ns() < until) pause();
    bool reset = !(inb(c->nabm + PO_CR) & CR_RESET);
    outl(c->nabm + PO_BDBAR, c->bdl_bus);
    return reset;
}

static int ac97_open(struct audio_output *output, const struct n_audio_device_format *format, struct audio_ring *ring) {
    struct ac97 *c = ac97_of(output);
    (void)format; /* the one it declares */
    uint32_t frames = MAX(MIN(ring->block_frames, BUF_MOST), 2u);
    uint64_t phys;
    size_t bytes = (size_t)frames * FRAME * NBUF;
    struct object *memory = shared_kernel_contig(ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE, 32, &phys);
    if (!memory) return -N_ERR_NO_MEMORY;
    uint64_t bus = hal_dma_address(c->hal, phys);
    for (uint32_t i = 0; i < NBUF; i++) {
        c->bdl_view[i].addr = (uint32_t)(bus + (uint64_t)i * frames * FRAME);
        c->bdl_view[i].samples = (uint16_t)(frames * 2);
        c->bdl_view[i].flags = BD_IOC;
    }
    c->buf_frames = frames;
    c->buf_ns = frames * NS_PER_S / RATE;
    c->ring_object = memory;
    *ring = (struct audio_ring){.memory = memory, .block_frames = frames, .blocks = NBUF};
    return 0;
}

static int ac97_run(struct audio_output *output, bool run) {
    struct ac97 *c = ac97_of(output);
    engine_sync(c);
    if (run == c->running) return 0;
    if (!c->has_irq) alarm_cancel(&c->refill);
    uint64_t f = spin_lock(&c->engine);
    bool ready = c->hw_ready;
    uint64_t gen = c->power_gen;
    spin_unlock(&c->engine, f);
    if (!ready) return run ? -N_ERR_NOT_READY : 0;
    bool reset = box_reset(c);
    if (!run) {
        c->running = false;
        return reset ? 0 : -N_ERR_IO;
    }
    /* the engine starts at the first descriptor, and the last one is valid until it moves on */
    c->played = 0;
    c->last_civ = 0;
    c->last_at = clock_now_ns();
    f = spin_lock(&c->engine);
    ready = c->hw_ready && gen == c->power_gen;
    if (ready) {
        outb(c->nabm + PO_LVI, NBUF - 1);
        outb(c->nabm + PO_CR, c->has_irq ? CR_RUN | CR_IOCE : CR_RUN);
    }
    spin_unlock(&c->engine, f);
    if (!ready) return -N_ERR_NOT_READY;
    c->running = true;
    if (!c->has_irq) alarm_set(&c->refill, clock_after(c->buf_ns));
    return 0;
}

/* Another output plays: the engine stops where it is. */
static void ac97_close(struct audio_output *output) {
    struct ac97 *c = ac97_of(output);
    ac97_run(output, false);
    object_unref(c->ring_object);
    c->ring_object = NULL;
}

/* The codec out of cold reset at 0 dB and 48 kHz, the PCM-out box reset: at boot and after a sleep,
   which powered the codec off. It polls the clock: nothing may sleep here. */
static bool hw_init(struct ac97 *c) {
    outl(c->nabm + GLOB_CNT, 0x2); /* out of cold reset, interrupts off */
    uint64_t until = clock_after(100 * NS_PER_MS);
    while (!(inl(c->nabm + GLOB_STA) & 0x100) && clock_now_ns() < until) pause(); /* codec ready */
    bool codec = inl(c->nabm + GLOB_STA) & 0x100;
    outw(c->nam + NAM_RESET, 1);
    outw(c->nam + NAM_MASTER, 0x0000);  /* 0 dB, not muted */
    outw(c->nam + NAM_PCM_OUT, 0x0808); /* 0 dB */
    if (inw(c->nam + NAM_EXT_ID) & 1) { /* variable rate: make sure it is 48 kHz */
        outw(c->nam + NAM_EXT_CTRL, inw(c->nam + NAM_EXT_CTRL) | 1);
        outw(c->nam + NAM_FRONT_RATE, RATE);
    }
    return box_reset(c) && codec;
}

static const struct audio_output_ops ac97_ops = {.open = ac97_open, .close = ac97_close, .run = ac97_run, .status = ac97_status};

/* The mixer registers are the port BAR 0, the bus master's the port BAR 1. */
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

    /* the descriptors, below 4 GiB for the 32-bit addresses */
    uint64_t bus;
    c->bdl_view = hal_dma_alloc(d, PAGE_SIZE, 32, &bus);
    if (!c->bdl_view) {
        kprintf("audio: AC'97 found, but no memory below 4 GiB for its descriptors\n");
        d->driver_data = NULL;
        kfree(c);
        return -N_ERR_NO_MEMORY;
    }
    c->bdl_bus = (uint32_t)bus;
    hw_init(c);
    c->hw_ready = true;
    alarm_init(&c->refill, ac97_refill);
    c->has_irq = hal_irq_enable(d, &(struct irq_spec){.name = "AC'97", .handler = ac97_irq, .context = c}, 1, NULL) == 1;
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
}

static const struct n_hal_rule ac97_rules[] = {N_PCI_ID(0x8086, 0x2415), N_PCI_ID(0x8086, 0x2425), N_PCI_ID(0x8086, 0x2445),
                                               N_PCI_ID(0x8086, 0x2485), N_PCI_ID(0x8086, 0x24C5), N_PCI_ID(0x8086, 0x24D5),
                                               N_PCI_ID(0x8086, 0x266E), N_PCI_ID(0x8086, 0x27DE), N_PCI_ID(0x8086, 0x7195)};
const struct hal_driver ac97_driver = {
    .name = "ac97",
    .rules = ac97_rules,
    .rule_count = ARRAY_SIZE(ac97_rules),
    .attach = ac97_attach,
};
