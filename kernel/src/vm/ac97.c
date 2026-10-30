/* Intel AC'97 audio (ICH and compatibles; QEMU's -device AC97, VirtualBox's default).
   Output only, at 48 kHz stereo. The PCM-out DMA engine walks a ring of 32 buffer descriptors;
   instead of taking its interrupts we top the ring up from the timer tick, keeping a few
   10 ms buffers queued ahead of the one playing (CIV) up to the last valid one (LVI).
   An empty software queue does not extend LVI with silence: the issued PCM drains, then
   the engine is paused until the next PCM. Idle service performs no port I/O. */
#include "dev/audio.h"
#include "vm/devices.h"
#include "arch/cpu.h"
#include "dev/pci.h"
#include "dev/pm.h"
#include "mm/pmm.h"

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
#define SR_HALTED  0x01
#define SR_CELV    0x02 /* final descriptor has been processed, not merely CIV == LVI */
#define SR_CLEAR   0x1C /* write 1 to clear: last valid buffer, completion, FIFO error */

#define NBUF       32
#define BUF_FRAMES 480 /* 10 ms */
#define AHEAD      6   /* buffers queued past the playing one */
#define LOW        2   /* fewer queued past the playing one: the engine soon runs dry, mix what there is */
#define TAKE_MARGIN 192 /* PICB samples (2 ms): the playing buffer may end before a take-back is done */

struct bd {
    uint32_t addr;
    uint16_t samples; /* 16-bit samples, not frames */
    uint16_t flags;
} PACKED;

static uint16_t nam, nabm;
static int16_t *bufs;
static int lvi;
static bool dma_live; /* RUN set; issued descriptors may still belong to the device */
static volatile bool hw_ready; /* the codec and the PCM-out box are set up (not during sleep) */
static uint32_t bdl_phys;
static uint64_t submitted_frames;

static void spin_ms(uint64_t ms) { /* early in boot: no sleeping yet */
    uint64_t until = uptime_ms() + ms + 1;
    while (uptime_ms() < until) pause();
}

static void ac97_fill(void) {
    bool remote = audio_remote();
    bool pending = !remote && audio_pending();
    if (!dma_live && !pending) return; /* no DMA owner, no wake: zero VM exits */
    if (!hw_ready) return;             /* asleep, or the codec is being set up again */

    /* ICH7 PRM 2.2.2-4 permits this aligned read; QEMU nabm_readl also returns
       CIV | LVI<<8 | SR<<16. One snapshot replaces separate CIV/SR reads. */
    uint32_t state = dma_live ? inl(nabm + PO_CIV) : 0;
    uint16_t before = (uint16_t)(state >> 16);
    int civ = (int)(state & (NBUF - 1));
    if(dma_live){
        /* PICB and CIV are separate registers. Confirm the current descriptor
           once after PICB; a boundary crossing is conservatively deferred to
           the next tick rather than declaring unplayed samples consumed. */
        uint16_t remaining=inw(nabm+PO_PICB);
        uint32_t confirmed=inl(nabm+PO_CIV);
        if((confirmed&(NBUF-1))!=(uint32_t)civ){
            /* One descriptor crossing is normal at a tick boundary. Retry
               against the new CIV once; never refill indefinitely while
               refusing to retire the corresponding physical records. */
            civ=(int)(confirmed&(NBUF-1));before=(uint16_t)(confirmed>>16);
            remaining=inw(nabm+PO_PICB);confirmed=inl(nabm+PO_CIV);
        }
        if((confirmed&(NBUF-1))==(uint32_t)civ&&remaining<=BUF_FRAMES*2){
            uint64_t pending_frames;
            if((before&(SR_HALTED|SR_CELV))==(SR_HALTED|SR_CELV))pending_frames=0;
            else pending_frames=(uint64_t)((lvi-civ)&(NBUF-1))*BUF_FRAMES+((unsigned)remaining+1)/2;
            audio_card_played(submitted_frames>=pending_frames?submitted_frames-pending_frames:0);
        }else return; /* no refill against an unverified CIV/PICB owner */
    }
    unsigned filled = 0;
    bool restarted = false;
    if (dma_live && !pending && civ == lvi &&
        (before & (SR_HALTED | SR_CELV)) == (SR_HALTED | SR_CELV)) {
        /* Never pause a partly consumed descriptor. QEMU then drains its
           already mixed host queue (pending_disable); no DMA buffer is erased. */
        outb(nabm + PO_CR, 0);
        dma_live = false;
    }
    if (before & SR_CLEAR) outw(nabm + PO_SR, before & SR_CLEAR);

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
            outb(nabm + PO_CR, CR_RUN);
            dma_live = true;
            restarted = true;
        } else restarted = (before & SR_HALTED) != 0; /* LVI wakes a drained RUN engine */
    }
    /* The extra raw after-snapshot is opt-in only, not normal sink traffic. */
    if (audio_trace_active) audio_trace_ac97(before, inw(nabm + PO_SR), filled, restarted);
}

/* Where the PCM the engine has not begun to fetch starts (audio.h, audio_set_card). CIV plays
   and the ICH fetches the descriptor at PIV, CIV + 1, ahead of it; the first that can still be
   written is CIV + 2, or CIV + 3 when CIV is about to end and the engine may move on meanwhile.
   With take, LVI goes back to the descriptor before it: the engine stops there unless fill()
   issues more. Not while halted: nothing is queued then. */
static uint64_t ac97_unissue(bool take) {
    if (!dma_live || !hw_ready) return submitted_frames;
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
   (DMA controller halted). */
static bool ac97_quiesce(void *arg) {
    (void)arg;
    hw_ready = false;
    outb(nabm + PO_CR, 0);
    dma_live = false;
    for (int i = 0; i < 1000; i++) {
        if (inw(nabm + PO_SR) & SR_HALTED) return true;
        io_wait();
    }
    return false;
}

/* The codec out of cold reset at 0 dB and 48 kHz, the PCM-out box reset, then the ring (once it
   exists): at boot and after a sleep, which powered the codec off. */
static bool hw_init(void) {
    outl(nabm + GLOB_CNT, 0x2); /* out of cold reset, interrupts off */
    for (int i = 0; i < 100 && !(inl(nabm + GLOB_STA) & 0x100); i++) spin_ms(1); /* codec ready */
    bool codec = inl(nabm + GLOB_STA) & 0x100;
    outw(nam + NAM_RESET, 1);
    outw(nam + NAM_MASTER, 0x0000);  /* 0 dB, not muted */
    outw(nam + NAM_PCM_OUT, 0x0808); /* 0 dB */
    if (inw(nam + NAM_EXT_ID) & 1) { /* variable rate: make sure it is 48 kHz */
        outw(nam + NAM_EXT_CTRL, inw(nam + NAM_EXT_CTRL) | 1);
        outw(nam + NAM_FRONT_RATE, AUDIO_RATE);
    }

    outb(nabm + PO_CR, CR_RESET);
    for (int i = 0; i < 100 && (inb(nabm + PO_CR) & CR_RESET); i++) spin_ms(1);
    bool reset = !(inb(nabm + PO_CR) & CR_RESET);
    if (!bdl_phys) return codec && reset;
    outl(nabm + PO_BDBAR, bdl_phys);
    lvi = NBUF - 1; /* no descriptor has been issued; reset PIV will start at 0 */
    dma_live = false; /* do not start a perpetual silence DMA stream */
    hw_ready = true;
    return codec && reset;
}

static bool ac97_resume(void *arg) {
    (void)arg;
    return hw_init();
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
        bdl_ring[i].flags = 0;
    }
    bdl_phys = (uint32_t)bdl;
    outl(nabm + PO_BDBAR, bdl_phys);
    lvi = NBUF - 1; /* no descriptor has been issued; reset PIV will start at 0 */
    dma_live = false; /* do not start a perpetual silence DMA stream at boot */
    hw_ready = true;
    audio_set_card("AC'97", ac97_fill, ac97_unissue);
    pm_register(&ac97_pm);
    kprintf("audio: AC'97 (%04x) at ports %#x/%#x, 48 kHz stereo\n", d->device, nam, nabm);
}
