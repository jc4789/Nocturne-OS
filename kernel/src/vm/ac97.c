/* Intel AC'97 audio (ICH and compatibles; QEMU's -device AC97, VirtualBox's default): one output of
   the sound layer (dev/audio.h), at 48 kHz stereo. The PCM-out DMA engine walks a ring of its 32 buffer
   descriptors, of the length the open asks for, from the one playing (CIV) up to the last valid one (LVI). The ring is
   shared with the service that opened the output, which writes it. To walk it without end, LVI stays
   one behind CIV: every descriptor asks for an interrupt once the engine has played it (IOC), which
   moves LVI on and tells the service to look; a card whose interrupt line is not known takes an
   alarm at the end of each buffer instead. */
#include "dev/audio.h"
#include "vm/devices.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/spin.h"
#include "dev/acpi.h"
#include "dev/pci.h"
#include "dev/pm.h"
#include "mm/pmm.h"
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

/* The fields below are the sound layer's calls', which come one at a time (audio_output_ops). A power
   change comes between any two steps of them, on a CPU that may not wait for them: it changes
   hw_ready and power_gen and stops the engine under `engine`, and a call starts the engine only under
   it, having seen hw_ready and its generation there. */
static struct spinlock engine;
static bool hw_ready; /* the codec and the PCM-out box are set up (not during sleep) */
static uint64_t power_gen, seen_gen;
static uint32_t generation;
static uint16_t nam, nabm;
static struct bd *bdl_view;
static bool running;
static uint32_t bdl_phys;
static uint32_t buf_frames;  /* of what is open */
static uint64_t buf_ns;
static uint64_t played;     /* the frames of the buffers finished since the engine started */
static int last_civ;
static uint64_t last_at;
static struct irq *line;    /* its INTx's IOAPIC line (acpi_pci_gsi), NULL when it has none known */
static struct alarm refill; /* without the line: the end of the playing buffer */
static struct object *ring_object;
static struct audio_output ac97_output;

/* engine held: LVI one behind what plays, so the engine never comes to it. */
static void ring_keep(void) { outb(nabm + PO_LVI, (uint8_t)((inb(nabm + PO_CIV) + NBUF - 1) & (NBUF - 1))); }

/* The engine's interrupt: a buffer played. Its status bits are answered here, so what the engine does
   after this interrupts again. The line may be another device's too: its interrupt finds none of this
   engine's status bits. */
static enum irq_result ac97_irq(void *context) {
    (void)context;
    uint16_t status = inw(nabm + PO_SR) & SR_CLEAR;
    if (!status) return IRQ_NONE;
    outw(nabm + PO_SR, status);
    uint64_t f = spin_lock(&engine);
    if (hw_ready && running) ring_keep();
    spin_unlock(&engine, f);
    audio_output_period(&ac97_output);
    return IRQ_HANDLED;
}

static void ac97_refill(struct alarm *a, struct regs *r) {
    (void)a;
    (void)r;
    uint64_t f = spin_lock(&engine);
    bool on = hw_ready && running;
    if (on) ring_keep();
    spin_unlock(&engine, f);
    if (on) alarm_set(&refill, clock_after(buf_ns));
    audio_output_period(&ac97_output);
}

/* A new generation: what the engine held is gone with the reset. */
static void engine_sync(void) {
    uint64_t f = spin_lock(&engine);
    uint64_t gen = power_gen;
    spin_unlock(&engine, f);
    if (gen == seen_gen) return;
    seen_gen = gen;
    generation++;
    running = false;
}

static void ac97_status(struct audio_output *output, struct n_audio_device_status *status) {
    (void)output;
    engine_sync();
    status->generation = generation;
    status->running = running;
    status->at_ns = clock_now_ns();
    if (!running) return;
    uint64_t f = spin_lock(&engine);
    bool ready = hw_ready;
    int civ = last_civ;
    unsigned remaining = buf_frames * 2;
    uint64_t at = status->at_ns;
    if (ready) {
        /* PICB and CIV are separate registers: PICB is CIV's when CIV reads the same after it. One
           descriptor crossing between the reads is normal: once more against the new CIV. */
        int confirmed = inb(nabm + PO_CIV) & (NBUF - 1);
        for (int tries = 0; tries < 2; tries++) {
            civ = confirmed;
            remaining = inw(nabm + PO_PICB);
            at = clock_now_ns();
            confirmed = inb(nabm + PO_CIV) & (NBUF - 1);
            if (confirmed == civ) break;
        }
    }
    spin_unlock(&engine, f);
    uint64_t moved = (uint64_t)((civ + NBUF - last_civ) & (NBUF - 1));
    uint64_t late = (at - last_at) / buf_ns;
    if (late > moved + NBUF / 2) moved += (late - moved + NBUF / 2) / NBUF * NBUF; /* whole turns while no one looked */
    played += moved * buf_frames;
    last_civ = civ;
    last_at = at;
    if (remaining > buf_frames * 2) remaining = buf_frames * 2;
    status->position = played + buf_frames - remaining / 2;
    status->at_ns = at;
    status->guard_frames = remaining / 2 + buf_frames; /* the engine has fetched the descriptor after it */
}

/* The PCM-out box reset, polling the clock, and its descriptors given again. */
static bool box_reset(void) {
    outb(nabm + PO_CR, CR_RESET);
    uint64_t until = clock_after(100 * NS_PER_MS);
    while ((inb(nabm + PO_CR) & CR_RESET) && clock_now_ns() < until) pause();
    bool reset = !(inb(nabm + PO_CR) & CR_RESET);
    if (bdl_phys) outl(nabm + PO_BDBAR, bdl_phys);
    return reset;
}

static int ac97_open(struct audio_output *output, const struct n_audio_device_format *format, struct audio_ring *ring) {
    (void)output;
    (void)format; /* the one it declares */
    uint32_t frames = MAX(MIN(ring->block_frames, BUF_MOST), 2u);
    uint64_t phys;
    size_t bytes = (size_t)frames * FRAME * NBUF;
    struct object *memory = shared_kernel_contig(ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE, 32, &phys);
    if (!memory) return -N_ERR_NO_MEMORY;
    for (uint32_t i = 0; i < NBUF; i++) {
        bdl_view[i].addr = (uint32_t)(phys + (uint64_t)i * frames * FRAME);
        bdl_view[i].samples = (uint16_t)(frames * 2);
        bdl_view[i].flags = BD_IOC;
    }
    buf_frames = frames;
    buf_ns = frames * NS_PER_S / RATE;
    ring_object = memory;
    *ring = (struct audio_ring){.memory = memory, .block_frames = frames, .blocks = NBUF};
    return 0;
}

static int ac97_run(struct audio_output *output, bool run) {
    (void)output;
    engine_sync();
    if (run == running) return 0;
    if (!line) alarm_cancel(&refill);
    uint64_t f = spin_lock(&engine);
    bool ready = hw_ready;
    uint64_t gen = power_gen;
    spin_unlock(&engine, f);
    if (!ready) return run ? -N_ERR_NOT_READY : 0;
    bool reset = box_reset();
    if (!run) {
        running = false;
        return reset ? 0 : -N_ERR_IO;
    }
    /* the engine starts at the first descriptor, and the last one is valid until it moves on */
    played = 0;
    last_civ = 0;
    last_at = clock_now_ns();
    f = spin_lock(&engine);
    ready = hw_ready && gen == power_gen;
    if (ready) {
        outb(nabm + PO_LVI, NBUF - 1);
        outb(nabm + PO_CR, line ? CR_RUN | CR_IOCE : CR_RUN);
    }
    spin_unlock(&engine, f);
    if (!ready) return -N_ERR_NOT_READY;
    running = true;
    if (!line) alarm_set(&refill, clock_after(buf_ns));
    return 0;
}

/* Another output plays: the engine stops where it is. */
static void ac97_close(struct audio_output *output) {
    ac97_run(output, false);
    object_unref(ring_object);
    ring_object = NULL;
}

/* Power change (dev/pm.h): the PCM-out engine stops reading descriptors and buffers, and says so
   (DMA controller halted). Its interrupts go off with it. */
static bool ac97_quiesce(void *arg) {
    (void)arg;
    uint64_t f = spin_lock(&engine);
    hw_ready = false;
    power_gen++;
    outb(nabm + PO_CR, 0);
    outw(nabm + PO_SR, SR_CLEAR);
    spin_unlock(&engine, f);
    alarm_cancel(&refill);
    for (int i = 0; i < 1000; i++) {
        if (inw(nabm + PO_SR) & SR_HALTED) return true;
        io_wait();
    }
    return false;
}

/* The codec out of cold reset at 0 dB and 48 kHz, the PCM-out box reset: at boot and after a sleep,
   which powered the codec off. It polls the clock: nothing may sleep here. */
static bool hw_init(void) {
    outl(nabm + GLOB_CNT, 0x2); /* out of cold reset, interrupts off */
    uint64_t until = clock_after(100 * NS_PER_MS);
    while (!(inl(nabm + GLOB_STA) & 0x100) && clock_now_ns() < until) pause(); /* codec ready */
    bool codec = inl(nabm + GLOB_STA) & 0x100;
    outw(nam + NAM_RESET, 1);
    outw(nam + NAM_MASTER, 0x0000);  /* 0 dB, not muted */
    outw(nam + NAM_PCM_OUT, 0x0808); /* 0 dB */
    if (inw(nam + NAM_EXT_ID) & 1) { /* variable rate: make sure it is 48 kHz */
        outw(nam + NAM_EXT_CTRL, inw(nam + NAM_EXT_CTRL) | 1);
        outw(nam + NAM_FRONT_RATE, RATE);
    }
    return box_reset() && codec;
}

/* The service looks, and starts the engine again. */
static bool ac97_resume(void *arg) {
    (void)arg;
    bool ok = hw_init();
    uint64_t f = spin_lock(&engine);
    hw_ready = true;
    spin_unlock(&engine, f);
    audio_output_period(&ac97_output);
    return ok;
}

static struct pm_device ac97_pm = {.name = "ac97", .stage = PM_STAGE_IO, .quiesce = ac97_quiesce,
                                   .resume = ac97_resume};

static const struct audio_output_ops ac97_ops = {.open = ac97_open, .close = ac97_close, .run = ac97_run, .status = ac97_status};

void ac97_init(void) {
    static const uint16_t ids[] = {0x2415, 0x2425, 0x2445, 0x2485, 0x24C5, 0x24D5, 0x266E, 0x27DE, 0x7195};
    struct pci_device *d = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(ids) && !d; i++) d = pci_find(0x8086, ids[i]);
    if (!d || !(d->bar[0] & 1) || !(d->bar[1] & 1)) return;
    nam = (uint16_t)(d->bar[0] & ~3u);
    nabm = (uint16_t)(d->bar[1] & ~3u);
    pci_enable_busmaster(d);
    pci_enable_io(d);

    /* the descriptors, below 4 GiB for the 32-bit addresses */
    uint64_t bdl = pmm_alloc_contig(1, 32);
    if (!bdl) {
        kprintf("audio: AC'97 found, but no memory below 4 GiB for its descriptors\n");
        return;
    }
    memset(phys_to_virt(bdl), 0, PAGE_SIZE);
    bdl_view = phys_to_virt(bdl);
    bdl_phys = (uint32_t)bdl;
    hw_init();
    hw_ready = true;
    alarm_init(&refill, ac97_refill);
    /* its INTx where ACPI routes it; without, the refill alarm instead */
    uint32_t gsi;
    if (acpi_pci_gsi(d, &gsi))
        line = irq_request_gsi(&(struct irq_spec){.name = "AC'97", .handler = ac97_irq, .node = -1}, gsi);
    ac97_output.ops = &ac97_ops;
    static const uint32_t rates[] = {RATE};
    static const struct n_audio_size sizes[] = {{16, FRAME / 2}};
    ac97_output.kind = N_AUDIO_KIND_LINE;
    ac97_output.jack = N_AUDIO_JACK_NONE;
    ac97_output.name = "AC'97";
    ac97_output.key = "ac97";
    ac97_output.rates = rates;
    ac97_output.rate_count = 1;
    ac97_output.sizes = sizes;
    ac97_output.size_count = 1;
    ac97_output.channels_low = ac97_output.channels_high = 2;
    audio_output_add(&ac97_output);
    pm_register(&ac97_pm);
    kprintf("audio: AC'97 (%04x) at ports %#x/%#x, %s, 48 kHz stereo\n", d->device, nam, nabm,
            line ? "interrupt" : "no interrupt (timed refill)");
}
