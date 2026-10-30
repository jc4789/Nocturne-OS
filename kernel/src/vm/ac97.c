/* Intel AC'97 audio (ICH and compatibles; QEMU's -device AC97, VirtualBox's default).
   Output only, at 48 kHz stereo. The PCM-out DMA engine walks a ring of 32 buffer descriptors of
   10 ms each, from the one playing (CIV) up to the last valid one (LVI). Every descriptor asks for
   an interrupt once the engine has played it (IOC), and the engine interrupts once more when it has
   played the last valid one and halts (LVBCI). There fill() reports what the engine has played,
   tops the ring up to a few buffers past the playing one, or stops the engine when it has played
   everything and nothing waits. Its thresholds count buffers, so a completion is the moment each
   one is crossed; a card whose interrupt line is not known takes an alarm at the end of the playing
   buffer instead, from the engine's position in it and the rate.
   An empty software queue does not extend LVI with silence: the issued PCM drains, then the
   engine is stopped until the next PCM (which starts it, audio.c), and nothing runs meanwhile. */
#include "dev/audio.h"
#include "vm/devices.h"
#include "arch/cpu.h"
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

/* fill() and unissue() run under the card's lock (dev/audio.h) and look at hw_ready first; it
   changes under that lock too. While it is false they touch nothing else, so a power change
   (quiesce, hw_init on resume) programs the engine and the fields below without the lock. */
static uint16_t nam, nabm;
static int16_t *bufs;
static int lvi;
static bool dma_live; /* RUN set; issued descriptors may still belong to the device */
static bool hw_ready; /* the codec and the PCM-out box are set up (not during sleep) */
static uint32_t bdl_phys;
static uint64_t submitted_frames;
static int irq;             /* its 8259 line, -1 when it has none that is known */
static struct alarm refill; /* without the line: the end of the playing buffer */

/* Under the card's lock: from the engine's interrupt, the refill alarm, a stream that got PCM
   while the card had played everything (audio.c), a flush, a resume. */
static void ac97_fill(void) {
    if (!hw_ready) return;             /* asleep, or the codec is being set up again */
    bool remote = audio_remote();
    uint16_t before = 0, sr = 0, remaining = 0;
    int civ = 0;
    uint64_t at = 0; /* when PICB was read */
    if (dma_live) {
        /* ICH7 PRM 2.2.2-4 permits this aligned read; QEMU nabm_readl also returns
           CIV | LVI<<8 | SR<<16. The status bits seen are answered first, so whatever the engine
           does after this read interrupts again. */
        uint32_t state = inl(nabm + PO_CIV), confirmed = state;
        before = (uint16_t)(state >> 16);
        if (before & SR_CLEAR) outw(nabm + PO_SR, before & SR_CLEAR);
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
            if (irq < 0) alarm_set(&refill, at + BUF_NS); /* a buffer has just begun */
            return;
        }
        uint64_t pending_frames;
        if ((sr & (SR_HALTED | SR_CELV)) == (SR_HALTED | SR_CELV)) pending_frames = 0;
        else pending_frames = (uint64_t)((lvi - civ) & (NBUF - 1)) * BUF_FRAMES + ((unsigned)remaining + 1) / 2;
        /* Before the queues are looked at: a stream that gets PCM after that look finds the card
           has played everything and calls fill() again. */
        audio_played(submitted_frames >= pending_frames ? submitted_frames - pending_frames : 0, at);
    }
    bool pending = !remote && audio_pending();
    if (!dma_live && !pending) return; /* no DMA owner, no wake: zero VM exits */
    unsigned filled = 0;
    bool restarted = false;
    if (dma_live && !pending && civ == lvi && (sr & (SR_HALTED | SR_CELV)) == (SR_HALTED | SR_CELV)) {
        /* Never pause a partly consumed descriptor. QEMU then drains its already mixed host queue
           (pending_disable); no DMA buffer is erased. */
        outb(nabm + PO_CR, 0);
        dma_live = false;
    }

    /* On first start/reset PIV is 0 and lvi is 31. After a complete drain PIV
       is the successor of lvi. Only these unpublished successors are writable.
       While live, a stale CIV can only under-estimate room as DMA advances. */
    int queued = dma_live ? (lvi - civ) & (NBUF - 1) : 0, room = AHEAD - queued;
    /* Whole buffers while the engine has enough queued: a stream fed a little at a time (Web
       Audio's quanta) is not cut into sound and silence (audio.h, audio_ready). */
    while (!remote && room-- > 0 && audio_ready(BUF_FRAMES, queued < LOW)) {
        int next = (lvi + 1) & (NBUF - 1);
        if (!audio_card_mix(bufs + next * BUF_FRAMES * 2, BUF_FRAMES)) break;
        submitted_frames+=BUF_FRAMES;
        lvi = next;
        queued++;
        filled++;
    }
    if (filled) {
        __sync_synchronize(); /* publish all PCM before the single LVI doorbell */
        outb(nabm + PO_LVI, (uint8_t)lvi);
        if (!dma_live) {
            outb(nabm + PO_CR, irq >= 0 ? CR_RUN | CR_IOCE | CR_LVBIE : CR_RUN);
            dma_live = true;
            restarted = true;
        } else restarted = (sr & SR_HALTED) != 0; /* LVI wakes a drained RUN engine */
        if (restarted) { /* its first buffer plays from now */
            remaining = BUF_FRAMES * 2;
            at = clock_now_ns();
        }
    }
    if (dma_live && irq < 0) alarm_set(&refill, at + (uint64_t)(remaining / 2) * NS_PER_S / AUDIO_RATE);
    /* The extra raw after-snapshot is opt-in only, not normal sink traffic. */
    if (audio_trace_active) audio_trace_ac97(before, inw(nabm + PO_SR), filled, restarted);
}

/* The engine's interrupt (CPU 0): a buffer played, or the last valid one and the engine halted.
   The line may be another device's too: its interrupt finds none of this engine's status bits. */
static void ac97_irq(struct regs *r) {
    (void)r;
    uint64_t f = audio_card_lock();
    if (hw_ready && dma_live && (inw(nabm + PO_SR) & SR_CLEAR)) ac97_fill();
    audio_card_unlock(f);
}

static void ac97_refill(struct alarm *a, struct regs *r) {
    (void)a;
    (void)r;
    uint64_t f = audio_card_lock();
    ac97_fill();
    audio_card_unlock(f);
}

/* Where the PCM the engine has not begun to fetch starts (audio.h, audio_set_card). CIV plays
   and the ICH fetches the descriptor at PIV, CIV + 1, ahead of it; the first that can still be
   written is CIV + 2, or CIV + 3 when CIV is about to end and the engine may move on meanwhile.
   With take, LVI goes back to the descriptor before it: the engine stops there unless fill()
   issues more. Not while halted: nothing is queued then. */
static uint64_t ac97_unissue(bool take) {
    if (!hw_ready || !dma_live) return submitted_frames;
    uint32_t state = inl(nabm + PO_CIV);
    uint16_t remaining = inw(nabm + PO_PICB);
    uint32_t confirmed = inl(nabm + PO_CIV);
    int civ = (int)(state & (NBUF - 1));
    if ((confirmed & (NBUF - 1)) != (uint32_t)civ || ((state >> 16) & SR_HALTED)) return submitted_frames;
    int first = remaining < TAKE_MARGIN ? 3 : 2, queued = (lvi - civ) & (NBUF - 1);
    if (queued < first) return submitted_frames;
    uint64_t at = submitted_frames - (uint64_t)(queued - first + 1) * BUF_FRAMES;
    if (take) {
        lvi = (civ + first - 1) & (NBUF - 1);
        submitted_frames = at;
        outb(nabm + PO_LVI, (uint8_t)lvi);
    }
    return at;
}

/* Power change (dev/pm.h): the PCM-out engine stops reading descriptors and buffers, and says so
   (DMA controller halted). Its interrupts go off with it, under the card's lock, so none comes
   that fill() would no longer answer. */
static bool ac97_quiesce(void *arg) {
    (void)arg;
    uint64_t f = audio_card_lock();
    hw_ready = false;
    outb(nabm + PO_CR, 0);
    outw(nabm + PO_SR, SR_CLEAR);
    dma_live = false;
    audio_card_unlock(f);
    alarm_cancel(&refill);
    for (int i = 0; i < 1000; i++) {
        if (inw(nabm + PO_SR) & SR_HALTED) return true;
        io_wait();
    }
    return false;
}

/* The codec out of cold reset at 0 dB and 48 kHz, the PCM-out box reset, then the ring (once it
   exists): at boot and after a sleep, which powered the codec off. It polls the clock: nothing
   may sleep here. */
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

    outb(nabm + PO_CR, CR_RESET);
    until = clock_after(100 * NS_PER_MS);
    while ((inb(nabm + PO_CR) & CR_RESET) && clock_now_ns() < until) pause();
    bool reset = !(inb(nabm + PO_CR) & CR_RESET);
    if (!bdl_phys) return codec && reset;
    outl(nabm + PO_BDBAR, bdl_phys);
    lvi = NBUF - 1; /* no descriptor has been issued; reset PIV will start at 0 */
    dma_live = false; /* do not start a perpetual silence DMA stream */
    uint64_t f = audio_card_lock();
    hw_ready = true;
    audio_card_unlock(f);
    return codec && reset;
}

/* What the engine held when it stopped is gone with the reset: it counts as played, and what
   waits in the queues starts the engine again. */
static bool ac97_resume(void *arg) {
    (void)arg;
    bool ok = hw_init();
    uint64_t f = audio_card_lock();
    audio_played(submitted_frames, clock_now_ns());
    ac97_fill();
    audio_card_unlock(f);
    return ok;
}

static struct pm_device ac97_pm = {.name = "ac97", .stage = PM_STAGE_IO, .quiesce = ac97_quiesce,
                                   .resume = ac97_resume};

void ac97_init(void) {
    static const uint16_t ids[] = {0x2415, 0x2425, 0x2445, 0x2485, 0x24C5, 0x24D5, 0x266E, 0x27DE, 0x7195};
    struct pci_device *d = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(ids) && !d; i++) d = pci_find(0x8086, ids[i]);
    if (!d || !(d->bar[0] & 1) || !(d->bar[1] & 1)) return;
    nam = (uint16_t)(d->bar[0] & ~3u);
    nabm = (uint16_t)(d->bar[1] & ~3u);
    pci_enable_busmaster(d);
    hw_init();

    /* descriptors and buffers, below 4 GiB for the 32-bit addresses */
    uint64_t bdl = pmm_alloc_zeroed();
    uint64_t pages = ALIGN_UP(NBUF * BUF_FRAMES * AUDIO_FRAME, PAGE_SIZE) / PAGE_SIZE;
    uint64_t buf_phys = pmm_alloc_contig(pages);
    if (!bdl || !buf_phys || buf_phys + pages * PAGE_SIZE > 0x100000000ULL || bdl >= 0x100000000ULL) {
        kprintf("audio: AC'97 found, but no memory for its buffers\n");
        return;
    }
    volatile struct bd *bdl_ring = phys_to_virt(bdl);
    bufs = phys_to_virt(buf_phys);
    memset(bufs, 0, pages * PAGE_SIZE);
    for (int i = 0; i < NBUF; i++) {
        bdl_ring[i].addr = (uint32_t)(buf_phys + (uint64_t)i * BUF_FRAMES * AUDIO_FRAME);
        bdl_ring[i].samples = BUF_FRAMES * 2;
        bdl_ring[i].flags = BD_IOC;
    }
    bdl_phys = (uint32_t)bdl;
    outl(nabm + PO_BDBAR, bdl_phys);
    lvi = NBUF - 1; /* no descriptor has been issued; reset PIV will start at 0 */
    dma_live = false; /* do not start a perpetual silence DMA stream at boot */
    hw_ready = true;
    alarm_init(&refill, ac97_refill);
    /* PCI's interrupt line register: 0 or above 15 (0xFF: unknown) leaves the 8259 nothing to take */
    irq = d->irq && d->irq < 16 ? d->irq : -1;
    if (irq >= 0) {
        irq_register(irq, ac97_irq);
        pic_unmask(irq);
    }
    audio_set_card("AC'97", ac97_fill, ac97_unissue);
    pm_register(&ac97_pm);
    kprintf("audio: AC'97 (%04x) at ports %#x/%#x, irq %d, 48 kHz stereo\n", d->device, nam, nabm, irq);
}
