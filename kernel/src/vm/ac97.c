/* Intel AC'97 audio (ICH and compatibles; QEMU's -device AC97, VirtualBox's default): one output of
   the sound layer (dev/audio.h), at 48 kHz stereo. The PCM-out DMA engine walks a ring of 32 buffer
   descriptors of 10 ms each, from the one playing (CIV) up to the last valid one (LVI). Every
   descriptor asks for an interrupt once the engine has played it (IOC), and the engine interrupts
   once more when it has played the last valid one and halts (LVBCI). Each wakes the audio thread,
   whose fill() reports what the engine has played, tops the ring up to a few buffers past the playing
   one, or stops the engine when it has played everything and nothing waits. Its thresholds count
   buffers, so a completion is the moment each one is crossed; a card whose interrupt line is not known
   takes an alarm at the end of the playing buffer instead, from the engine's position in it and the
   rate.
   An empty software queue does not extend LVI with silence: the issued PCM drains, then the
   engine is stopped until the next PCM (which starts it, audio.c), and nothing runs meanwhile. */
#include "dev/audio.h"
#include "vm/devices.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/spin.h"
#include "dev/acpi.h"
#include "dev/pci.h"
#include "dev/pm.h"
#include "mm/pmm.h"
#include "sys/alarm.h"
#include "sys/clock.h"

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

#define NBUF       32
#define BUF_FRAMES 480 /* 10 ms */
#define BUF_NS     (BUF_FRAMES * NS_PER_S / AUDIO_RATE)
#define AHEAD      6   /* buffers queued past the playing one */
#define LOW        2   /* fewer queued past the playing one: the engine soon runs dry, mix what there is */
#define TAKE_MARGIN 192 /* PICB samples (2 ms): the playing buffer may end before a take-back is done */

struct bd {
    uint32_t addr;
    uint16_t samples; /* 16-bit samples, not frames */
    uint16_t flags;
} PACKED;

/* The fields below are the sound layer's calls', which come one at a time (audio_sink_ops). A power
   change comes between any two steps of them, on a CPU that may not wait for them: it changes
   hw_ready and power_gen and stops the engine under `engine`, and a call starts the engine or moves
   LVI only under it, having seen hw_ready and its generation there. A call that finds a new
   generation begins again: what the engine held is gone. */
static struct spinlock engine;
static bool hw_ready; /* the codec and the PCM-out box are set up (not during sleep) */
static uint64_t power_gen, seen_gen;
static uint16_t nam, nabm;
static int16_t *bufs;
static int lvi;
static bool dma_live; /* RUN set; issued descriptors may still belong to the device */
static uint32_t bdl_phys;
static uint64_t given;      /* the ordinal after the last frame issued */
static struct irq *line;    /* its INTx's IOAPIC line (acpi_pci_gsi), NULL when it has none known */
static struct alarm refill; /* without the line: the end of the playing buffer */

/* LVI is written, and the engine started when it is not running; false after a power change. */
static bool ring_publish(bool start, int to, uint64_t gen) {
    uint64_t f = spin_lock(&engine);
    bool ok = hw_ready && gen == power_gen;
    if (ok) {
        outb(nabm + PO_LVI, (uint8_t)to);
        if (start) outb(nabm + PO_CR, line ? CR_RUN | CR_IOCE | CR_LVBIE : CR_RUN);
    }
    spin_unlock(&engine, f);
    return ok;
}

/* After a power change: what the engine held is gone with the reset, and counts as played. */
static void ring_forget(void) {
    audio_played(given, clock_now_ns());
    lvi = NBUF - 1; /* no descriptor has been issued; reset PIV will start at 0 */
    dma_live = false;
}

static void ac97_fill(struct audio_sink *sink) {
    uint64_t f = spin_lock(&engine);
    bool ready = hw_ready;
    uint64_t gen = power_gen;
    spin_unlock(&engine, f);
    if (gen != seen_gen) {
        seen_gen = gen;
        ring_forget();
    }
    if (!ready) return; /* asleep, or the codec is being set up again */
    bool remote = audio_remote();
    uint16_t sr = 0, remaining = 0;
    int civ = 0;
    uint64_t at = 0; /* when PICB was read */
    if (dma_live) {
        /* ICH7 PRM 2.2.2-4 permits this aligned read; QEMU nabm_readl also returns
           CIV | LVI<<8 | SR<<16. */
        uint32_t confirmed = inl(nabm + PO_CIV);
        /* PICB and CIV are separate registers: PICB is CIV's when CIV reads the same after it. One
           descriptor crossing between the reads is normal: once more against the new CIV, and
           never a report or a refill against an unconfirmed one. */
        for (int tries = 0; tries < 2; tries++) {
            civ = (int)(confirmed & (NBUF - 1));
            remaining = inw(nabm + PO_PICB);
            at = clock_now_ns();
            confirmed = inl(nabm + PO_CIV);
            if ((int)(confirmed & (NBUF - 1)) == civ) break;
        }
        sr = (uint16_t)(confirmed >> 16);
        if ((int)(confirmed & (NBUF - 1)) != civ || remaining > BUF_FRAMES * 2) {
            if (!line) alarm_set(&refill, at + BUF_NS); /* a buffer has just begun */
            return;
        }
        uint64_t pending_frames;
        if ((sr & (SR_HALTED | SR_CELV)) == (SR_HALTED | SR_CELV)) pending_frames = 0;
        else pending_frames = (uint64_t)((lvi - civ) & (NBUF - 1)) * BUF_FRAMES + ((unsigned)remaining + 1) / 2;
        /* Before the queues are looked at: a stream that gets PCM after that look finds the output
           has played everything and wakes this thread again. */
        audio_played(given >= pending_frames ? given - pending_frames : 0, at);
    }
    bool pending = !remote && audio_pending();
    if (!dma_live && !pending) return; /* no DMA owner, no wake: zero VM exits */
    if (dma_live && !pending && civ == lvi && (sr & (SR_HALTED | SR_CELV)) == (SR_HALTED | SR_CELV)) {
        /* Never pause a partly consumed descriptor. QEMU then drains its already mixed host queue
           (pending_disable); no DMA buffer is erased. */
        f = spin_lock(&engine);
        if (hw_ready && gen == power_gen) outb(nabm + PO_CR, 0);
        spin_unlock(&engine, f);
        dma_live = false;
    }

    /* On first start/reset PIV is 0 and lvi is 31. After a complete drain PIV
       is the successor of lvi. Only these unpublished successors are writable.
       While live, a stale CIV can only under-estimate room as DMA advances. */
    int queued = dma_live ? (lvi - civ) & (NBUF - 1) : 0, room = AHEAD - queued, last = lvi;
    uint64_t end = given;
    unsigned filled = 0;
    /* Whole buffers while the engine has enough queued: a stream fed a little at a time (Web
       Audio's quanta) is not cut into sound and silence (audio.h, audio_ready). */
    while (!remote && room-- > 0 && audio_ready(BUF_FRAMES, queued < LOW)) {
        int next = (last + 1) & (NBUF - 1);
        if (!audio_sink_mix(sink, bufs + next * BUF_FRAMES * 2, BUF_FRAMES, &end)) break;
        last = next;
        queued++;
        filled++;
    }
    if (filled) {
        __sync_synchronize(); /* publish all PCM before the single LVI doorbell */
        bool start = !dma_live, restarted = start || (sr & SR_HALTED);
        /* A power change here forgets these too: the next fill() reports them played. */
        given = end;
        lvi = last;
        if (ring_publish(start, last, gen)) dma_live = true;
        if (restarted) { /* its first buffer plays from now; LVI wakes a drained RUN engine */
            remaining = BUF_FRAMES * 2;
            at = clock_now_ns();
        }
    }
    if (dma_live && !line) alarm_set(&refill, at + (uint64_t)(remaining / 2) * NS_PER_S / AUDIO_RATE);
}

/* The engine's interrupt: a buffer played, or the last valid one and the engine halted. Its status
   bits are answered here, so what the engine does after this interrupts again.
   The line may be another device's too: its interrupt finds none of this engine's status bits. */
static enum irq_result ac97_irq(void *context) {
    (void)context;
    uint16_t status = inw(nabm + PO_SR) & SR_CLEAR;
    if (!status) return IRQ_NONE;
    outw(nabm + PO_SR, status);
    audio_sink_wake();
    return IRQ_HANDLED;
}

static void ac97_refill(struct alarm *a, struct regs *r) {
    (void)a;
    (void)r;
    audio_sink_wake();
}

/* Where the PCM the engine has not begun to fetch starts (audio_sink_ops). CIV plays and the ICH
   fetches the descriptor at PIV, CIV + 1, ahead of it; the first that can still be written is
   CIV + 2, or CIV + 3 when CIV is about to end and the engine may move on meanwhile. With take, LVI
   goes back to the descriptor before it: the engine stops there unless fill() issues more. Not
   while halted: nothing is queued then. */
static uint64_t ac97_unissue(struct audio_sink *sink, bool take) {
    (void)sink;
    uint64_t f = spin_lock(&engine), at = given;
    if (hw_ready && dma_live && seen_gen == power_gen) {
        uint32_t state = inl(nabm + PO_CIV);
        uint16_t remaining = inw(nabm + PO_PICB);
        uint32_t confirmed = inl(nabm + PO_CIV);
        int civ = (int)(state & (NBUF - 1));
        if ((confirmed & (NBUF - 1)) == (uint32_t)civ && !((state >> 16) & SR_HALTED)) {
            int first = remaining < TAKE_MARGIN ? 3 : 2, queued = (lvi - civ) & (NBUF - 1);
            if (queued >= first) {
                at = given - (uint64_t)(queued - first + 1) * BUF_FRAMES;
                if (take) {
                    lvi = (civ + first - 1) & (NBUF - 1);
                    given = at;
                    outb(nabm + PO_LVI, (uint8_t)lvi);
                }
            }
        }
    }
    spin_unlock(&engine, f);
    return at;
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

static bool ac97_open(struct audio_sink *sink) {
    (void)sink;
    return true;
}

/* Another output plays: the engine stops where it is, and what it held counts as played. */
static void ac97_close(struct audio_sink *sink) {
    (void)sink;
    if (!line) alarm_cancel(&refill);
    box_reset();
    ring_forget();
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
        outw(nam + NAM_FRONT_RATE, AUDIO_RATE);
    }
    return box_reset() && codec;
}

/* What waits in the queues starts the engine again, in the audio thread. */
static bool ac97_resume(void *arg) {
    (void)arg;
    bool ok = hw_init();
    uint64_t f = spin_lock(&engine);
    hw_ready = true;
    spin_unlock(&engine, f);
    audio_sink_wake();
    return ok;
}

static struct pm_device ac97_pm = {.name = "ac97", .stage = PM_STAGE_IO, .quiesce = ac97_quiesce,
                                   .resume = ac97_resume};

static const struct audio_sink_ops ac97_ops = {.open = ac97_open, .close = ac97_close, .fill = ac97_fill,
                                               .unissue = ac97_unissue};
static struct audio_sink ac97_sink = {.ops = &ac97_ops, .name = "AC'97", .kind = AUDIO_KIND_LINE, .jack = AUDIO_JACK_NONE,
                                      .format = {.rate = AUDIO_RATE, .channels = 2, .bits = 16, .container = 2},
                                      .period_ns = BUF_NS, .blocks = NBUF};

void ac97_init(void) {
    static const uint16_t ids[] = {0x2415, 0x2425, 0x2445, 0x2485, 0x24C5, 0x24D5, 0x266E, 0x27DE, 0x7195};
    struct pci_device *d = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(ids) && !d; i++) d = pci_find(0x8086, ids[i]);
    if (!d || !(d->bar[0] & 1) || !(d->bar[1] & 1)) return;
    nam = (uint16_t)(d->bar[0] & ~3u);
    nabm = (uint16_t)(d->bar[1] & ~3u);
    pci_enable_busmaster(d);
    pci_enable_io(d);

    /* descriptors and buffers, below 4 GiB for the 32-bit addresses */
    uint64_t bdl = pmm_alloc_contig(1, 32);
    uint64_t pages = ALIGN_UP(NBUF * BUF_FRAMES * AUDIO_FRAME, PAGE_SIZE) / PAGE_SIZE;
    uint64_t buf_phys = pmm_alloc_contig(pages, 32);
    if (!bdl || !buf_phys) {
        if (bdl) pmm_free(bdl);
        if (buf_phys) pmm_free_contig(buf_phys, pages);
        kprintf("audio: AC'97 found, but no memory below 4 GiB for its buffers\n");
        return;
    }
    memset(phys_to_virt(bdl), 0, PAGE_SIZE);
    volatile struct bd *bdl_ring = phys_to_virt(bdl);
    bufs = phys_to_virt(buf_phys);
    memset(bufs, 0, pages * PAGE_SIZE);
    for (int i = 0; i < NBUF; i++) {
        bdl_ring[i].addr = (uint32_t)(buf_phys + (uint64_t)i * BUF_FRAMES * AUDIO_FRAME);
        bdl_ring[i].samples = BUF_FRAMES * 2;
        bdl_ring[i].flags = BD_IOC;
    }
    bdl_phys = (uint32_t)bdl;
    hw_init();
    lvi = NBUF - 1; /* no descriptor has been issued; reset PIV will start at 0 */
    hw_ready = true;
    alarm_init(&refill, ac97_refill);
    /* its INTx where ACPI routes it; without, the refill alarm instead */
    uint32_t gsi;
    if (acpi_pci_gsi(d, &gsi))
        line = irq_request_gsi(&(struct irq_spec){.name = "AC'97", .handler = ac97_irq, .node = -1}, gsi);
    audio_sink_add(&ac97_sink);
    pm_register(&ac97_pm);
    kprintf("audio: AC'97 (%04x) at ports %#x/%#x, %s, 48 kHz stereo\n", d->device, nam, nabm,
            line ? "interrupt" : "no interrupt (timed refill)");
}
