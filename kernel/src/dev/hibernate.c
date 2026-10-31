/* Hibernation (dev/hibernate.h).

   The image, a raw document of the store (fs/store.h):
     page 0     the header (written last; until then the document is no image)
     page 1...  the maps: frames saved, frames of zeros (1 bit per frame each)
     data_off   the blocks: up to BLOCK_PAGES saved frames each, in frame order, compressed
                (lib/lz.c) or as they were when that does not make them smaller

   The snapshot cannot write to disk (the controllers are stopped), so it compresses into a pool of
   free frames set aside before it. Every frame set aside for hibernation is "nosave": the snapshot
   leaves it out and the restored kernel frees it. The nosave map itself is ordinary memory, fixed
   before the snapshot, so the restored kernel finds in it what to free. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/lapic_timer.h"
#include "arch/ioapic.h"
#include "arch/smp.h"
#include "arch/wake.h"
#include "dev/blk.h"
#include "dev/fb.h"
#include "dev/hibernate.h"
#include "dev/iommu.h"
#include "dev/pci.h"
#include "dev/pm.h"
#include "sys/clock.h"
#include "fs/store.h"
#include "vm/vm.h"
#include "lib/lz.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

void power_reboot(void); /* dev/power.c */

#define IMAGE_PATH  "C:\\Nocturne\\Hibernate"
#define VERSION     3 /* 3: the wake page's layout (arch/wake.asm) and the last copy changed */
#define VALID       1
#define CONSUMED    2
#define BLOCK_PAGES 16
#define RESERVE     16384 /* frames (64 MiB) left free for the time between the snapshot and power off */
#define STAGE_BYTES (256 * 1024)
#define HASH_START  0xcbf29ce484222325ULL

struct header {
    char magic[8];
    uint32_t version, state;
    uint64_t total_pages, map_sig, pci_sig;
    uint64_t fb_phys;
    uint32_t fb_width, fb_height, fb_pitch, fb_bpp;
    uint64_t wake_page;
    uint64_t saved, zero, blocks;  /* frames in the blocks, frames of zeros, blocks */
    uint64_t map_bytes;            /* each of the saved and zero maps */
    uint64_t maps_check;
    uint64_t data_off, data_bytes;
    uint64_t check;                /* of the header with this field 0 */
};
_Static_assert(sizeof(struct header) <= PAGE_SIZE && sizeof(struct header) % 8 == 0, "hibernate header");

struct block {
    uint32_t pages, bytes; /* bytes == pages * PAGE_SIZE: stored as they were */
    uint64_t check;        /* of the pages */
};

/* the last copy's list (hib_copy_start): physical addresses, src 0 for a page of zeros */
#define PAIRS ((PAGE_SIZE - 16) / 16)
struct pair_page {
    uint64_t next, count;
    struct { uint64_t dst, src; } pair[PAIRS];
};
_Static_assert(sizeof(struct pair_page) == PAGE_SIZE, "hibernate pair page");

static struct {
    struct store_raw image;  /* where the image's blocks are */
    uint64_t image_bytes, data_off;
    uint64_t capacity;       /* stream bytes that fit in the pool and in the image */
    uint64_t *nosave;        /* 1 bit per frame */
    uint64_t **pool;         /* pages of 512 frame addresses */
    uint64_t pool_pages;
    uint64_t *maps;          /* saved, zero */
    uint64_t map_bytes;
    uint8_t *raw, *packed;   /* a block before and after compression */
    uint32_t *table;
    uint8_t *stage;          /* disk transfers */
    /* what the snapshot made */
    uint64_t stream, saved, zero, blocks;
    bool overflow;
} h;

static uint64_t hash_words(uint64_t x, const void *p, size_t bytes) {
    const uint64_t *w = p;
    for (size_t i = 0; i < bytes / 8; i++) {
        x = (x ^ w[i]) * 0x9E3779B97F4A7C15ULL;
        x ^= x >> 32;
    }
    return x;
}

static inline bool bit(const uint64_t *m, uint64_t i) { return m[i / 64] >> (i % 64) & 1; }
static inline void set_bit(uint64_t *m, uint64_t i) { m[i / 64] |= 1ULL << (i % 64); }

/* the next set bit at *at or after it, below n */
static bool next_bit(const uint64_t *m, uint64_t n, uint64_t *at) {
    for (uint64_t i = *at; i < n;) {
        uint64_t w = m[i / 64] >> (i % 64);
        if (w) {
            i += (uint64_t)__builtin_ctzll(w);
            if (i >= n) return false;
            *at = i;
            return true;
        }
        i = (i / 64 + 1) * 64;
    }
    return false;
}

static uint64_t pci_signature(void) {
    uint64_t s = HASH_START;
    for (int i = 0; i < pci_count(); i++) {
        struct pci_device *d = pci_at(i);
        uint64_t v[2] = {(uint64_t)d->bus << 16 | (uint64_t)d->dev << 8 | d->func,
                         (uint64_t)d->vendor << 32 | (uint64_t)d->device << 16 | (uint64_t)d->cls << 8 | d->subcls};
        s = hash_words(s, v, sizeof v);
    }
    return s;
}

/* what an image must agree on with the machine that restores it */
static void describe(struct header *hd) {
    memset(hd, 0, sizeof *hd);
    memcpy(hd->magic, "NOCTHIB1", 8);
    hd->version = VERSION;
    hd->total_pages = pmm_page_count();
    hd->map_sig = pmm_map_signature();
    hd->pci_sig = pci_signature();
    hd->fb_phys = fb.phys;
    hd->fb_width = fb.width;
    hd->fb_height = fb.height;
    hd->fb_pitch = fb.pitch;
    hd->fb_bpp = fb.bpp;
    hd->wake_page = wake_page();
    hd->map_bytes = ALIGN_UP((hd->total_pages + 7) / 8, 8);
    hd->data_off = ALIGN_UP(PAGE_SIZE + 2 * hd->map_bytes, PAGE_SIZE);
}

const char *hibernate_blocker(void) {
    if (!wake_available() || !wake_spare_page()) return "no room below 1 MiB";
    const char *vm = vm_hibernate_blocker();
    if (vm) return vm;
    if (!store_raw_possible()) return "C: is not on a disk";
    return NULL;
}

/* ---- the image's blocks ---- */
/* off and len in whole sectors, within the image */
static int disk_at(uint64_t off, void *buf, uint64_t len, bool write) {
    uint64_t sector = off / 512, left = len / 512;
    uint8_t *p = buf;
    for (size_t i = 0; i < h.image.n && left; i++) {
        struct store_raw_extent *e = &h.image.extents[i];
        if (sector >= e->sectors) {
            sector -= e->sectors;
            continue;
        }
        uint64_t n = MIN(left, e->sectors - sector);
        while (n) {
            uint32_t k = n > STAGE_BYTES / 512 ? STAGE_BYTES / 512 : (uint32_t)n;
            int r = write ? blk_write(h.image.disk, e->lba + sector, k, p) : blk_read(h.image.disk, e->lba + sector, k, p);
            if (r < 0) return r;
            p += (uint64_t)k * 512;
            sector += k;
            n -= k;
            left -= k;
        }
        sector = 0;
    }
    return left ? -N_ERR_IO : 0;
}

static bool disk_flush(void) { return blk_persist(h.image.disk) >= 0; }

/* ---- memory set aside ---- */
static void *nosave_pages(uint64_t n) {
    uint64_t p = n == 1 ? pmm_alloc() : pmm_alloc_contig(n, 64);
    if (!p) return NULL;
    for (uint64_t i = 0; i < n; i++) set_bit(h.nosave, p / PAGE_SIZE + i);
    return phys_to_virt(p);
}

static void free_nosave(void) {
    if (!h.nosave) return;
    uint64_t words = (pmm_page_count() + 63) / 64;
    for (uint64_t w = 0; w < words; w++)
        for (uint64_t m = h.nosave[w]; m; m &= m - 1) pmm_free((w * 64 + (uint64_t)__builtin_ctzll(m)) * PAGE_SIZE);
    kfree(h.nosave);
    h.nosave = NULL;
}

bool hibernate_prepare(const char **why) {
    memset(&h, 0, sizeof h);
    if ((*why = hibernate_blocker())) return false;
    struct header hd;
    describe(&hd);
    h.map_bytes = hd.map_bytes;
    h.data_off = hd.data_off;

    /* the most the snapshot can need: the frames in use now and a margin for what comes until then */
    const struct pmm_range *r;
    unsigned nr = pmm_ram_ranges(&r);
    uint64_t ram = 0;
    for (unsigned i = 0; i < nr; i++) ram += (r[i].end - r[i].base) / PAGE_SIZE;
    uint64_t most = ram - MIN(ram, pmm_free_pages()) + 8192;
    uint64_t stream = most * PAGE_SIZE + (most / BLOCK_PAGES + 1) * sizeof(struct block);

    /* the image: that large, or as large as C: has room for (images compress well) */
    int err = -N_ERR_NO_SPACE;
    for (uint64_t want = h.data_off + stream; err == -N_ERR_NO_SPACE && want >= h.data_off + (16ULL << 20);
         want = want / 4 * 3) {
        err = store_raw_place(IMAGE_PATH, want, &h.image);
        if (!err) h.image_bytes = want;
    }
    if (err) {
        *why = err == -N_ERR_NO_SPACE ? "not enough free space on C:" : "C:\\Nocturne\\Hibernate cannot be written";
        goto fail;
    }

    *why = "not enough memory";
    h.stage = kmalloc(STAGE_BYTES);
    h.nosave = kzalloc(h.map_bytes);
    if (!h.stage || !h.nosave) goto fail;
    h.maps = nosave_pages(ALIGN_UP(2 * h.map_bytes, PAGE_SIZE) / PAGE_SIZE);
    h.raw = nosave_pages(BLOCK_PAGES);
    h.packed = nosave_pages(BLOCK_PAGES);
    h.table = nosave_pages(LZ_TABLE_BYTES / PAGE_SIZE);
    if (!h.maps || !h.raw || !h.packed || !h.table) goto fail;

    /* the pool: what the stream may need, less what the machine needs until the power goes off */
    uint64_t free = pmm_free_pages();
    if (free < RESERVE + 2048) goto fail;
    uint64_t want = MIN(ALIGN_UP(stream, PAGE_SIZE) / PAGE_SIZE, free - RESERVE);
    want -= want / 512 + 1; /* the pages listing them */
    h.pool = kzalloc((want / 512 + 1) * sizeof *h.pool);
    if (!h.pool) goto fail;
    for (uint64_t i = 0; i < want; i++) {
        if (i % 512 == 0 && !(h.pool[i / 512] = nosave_pages(1))) break;
        uint64_t p = pmm_alloc();
        if (!p) break;
        set_bit(h.nosave, p / PAGE_SIZE);
        h.pool[i / 512][i % 512] = p;
        h.pool_pages++;
    }
    h.capacity = MIN(h.pool_pages * PAGE_SIZE, h.image_bytes - h.data_off);
    kprintf("hibernate: %lu MiB on C: and %lu MiB of memory for the image\n",
            (unsigned long)(h.image_bytes >> 20), (unsigned long)(h.pool_pages * PAGE_SIZE >> 20));
    *why = NULL;
    return true;
fail:
    hibernate_cleanup(false);
    return false;
}

/* ---- the snapshot ---- */
static void pool_put(const void *src, size_t n) {
    const uint8_t *s = src;
    if (h.stream + n > h.capacity) {
        h.overflow = true;
        return;
    }
    while (n) {
        uint64_t page = h.stream / PAGE_SIZE, at = h.stream % PAGE_SIZE;
        size_t k = MIN(n, PAGE_SIZE - at);
        memcpy((uint8_t *)phys_to_virt(h.pool[page / 512][page % 512]) + at, s, k);
        s += k;
        n -= k;
        h.stream += k;
    }
}

static void emit(unsigned pages) {
    size_t raw = (size_t)pages * PAGE_SIZE;
    struct block b = {.pages = pages, .bytes = (uint32_t)raw, .check = hash_words(HASH_START, h.raw, raw)};
    size_t c = lz_compress(h.raw, raw, h.packed, raw - 64, h.table);
    if (c) b.bytes = (uint32_t)c;
    pool_put(&b, sizeof b);
    pool_put(c ? h.packed : h.raw, ALIGN_UP(b.bytes, 8));
    h.blocks++;
}

static bool zero_page(const uint64_t *p) {
    uint64_t any = 0;
    for (unsigned i = 0; i < PAGE_SIZE / 8; i++) any |= p[i];
    return !any;
}

void hibernate_snapshot(void) {
    h.overflow = false;
    h.stream = h.saved = h.zero = h.blocks = 0;
    uint64_t *saved = h.maps, *zero = saved + h.map_bytes / 8;
    memset(h.maps, 0, 2 * h.map_bytes);
    uint64_t spare = wake_spare_page() / PAGE_SIZE;
    const struct pmm_range *r;
    unsigned nr = pmm_ram_ranges(&r), fill = 0;
    for (unsigned k = 0; k < nr; k++)
        for (uint64_t pfn = r[k].base / PAGE_SIZE; pfn < r[k].end / PAGE_SIZE; pfn++) {
            if (pfn == spare || !pmm_page_used(pfn) || bit(h.nosave, pfn)) continue;
            const uint64_t *src = phys_to_virt(pfn * PAGE_SIZE);
            if (zero_page(src)) {
                set_bit(zero, pfn);
                h.zero++;
                continue;
            }
            set_bit(saved, pfn);
            memcpy(h.raw + (size_t)fill * PAGE_SIZE, src, PAGE_SIZE);
            h.saved++;
            if (++fill == BLOCK_PAGES) {
                emit(fill);
                fill = 0;
                if (h.overflow) return;
            }
        }
    if (fill) emit(fill);
}

/* ---- to disk ---- */
struct writer {
    uint64_t off;
    size_t fill;
    int err;
};

static void writer_flush(struct writer *w) {
    if (!w->fill || w->err) return;
    size_t len = ALIGN_UP(w->fill, 512);
    memset(h.stage + w->fill, 0, len - w->fill);
    w->err = disk_at(w->off, h.stage, len, true);
    w->off += len;
    w->fill = 0;
}

static void put(struct writer *w, const void *src, size_t n) {
    const uint8_t *s = src;
    while (n && !w->err) {
        size_t k = MIN(n, STAGE_BYTES - w->fill);
        if (s) memcpy(h.stage + w->fill, s, k);
        else memset(h.stage + w->fill, 0, k);
        if (s) s += k;
        w->fill += k;
        n -= k;
        if (w->fill == STAGE_BYTES) writer_flush(w);
    }
}

const char *hibernate_write(void) {
    if (h.overflow) return "The image did not fit in memory or on C:";
    uint64_t t0 = clock_now_ns(), maps = 2 * h.map_bytes;
    struct header hd;
    describe(&hd);
    hd.state = VALID;
    hd.saved = h.saved;
    hd.zero = h.zero;
    hd.blocks = h.blocks;
    hd.maps_check = hash_words(HASH_START, h.maps, maps);
    hd.data_bytes = h.stream;
    struct writer w = {.off = PAGE_SIZE};
    put(&w, h.maps, maps);
    put(&w, NULL, h.data_off - PAGE_SIZE - maps);
    for (uint64_t at = 0; at < h.stream && !w.err; at += PAGE_SIZE) {
        uint64_t page = at / PAGE_SIZE;
        put(&w, phys_to_virt(h.pool[page / 512][page % 512]), MIN(PAGE_SIZE, h.stream - at));
    }
    writer_flush(&w);
    if (w.err || !disk_flush()) return "C: could not be written";
    /* the header last: until it is on the disk, the file is no image */
    hd.check = hash_words(HASH_START, &hd, sizeof hd);
    memset(h.stage, 0, PAGE_SIZE);
    memcpy(h.stage, &hd, sizeof hd);
    if (disk_at(0, h.stage, PAGE_SIZE, true) < 0 || !disk_flush()) return "C: could not be written";
    kprintf("hibernate: %lu frames saved, %lu of zeros left out, %lu MiB written in %lu ms\n", h.saved, h.zero,
            (unsigned long)((h.data_off + h.stream) >> 20), (clock_now_ns() - t0) / NS_PER_MS);
    return NULL;
}

void hibernate_cleanup(bool restored) {
    free_nosave();
    kfree(h.pool);
    store_raw_free(&h.image);
    kfree(h.stage);
    store_entry_remove(IMAGE_PATH, false);
    if (restored) kprintf("hibernate: back, the image is removed\n");
    memset(&h, 0, sizeof h);
}

/* ---- restore at boot ---- */
struct restore {
    uint64_t *saved, *zero; /* the image's maps */
    uint64_t *direct;       /* frames written in place: they were free here */
    struct pair_page *last;
    uint64_t first;         /* the first page of the copy list, physical */
    uint64_t pairs;
};

/* A frame that holds something until the last copy. Threads still run and free frames while the
   image is read; one the image fills is kept (written in place from now on) and another taken. */
static uint64_t hold_page(struct restore *r) {
    for (;;) {
        uint64_t p = pmm_alloc();
        if (!p) return 0;
        uint64_t pfn = p / PAGE_SIZE;
        if (!bit(r->saved, pfn) && !bit(r->zero, pfn)) return p;
        set_bit(r->direct, pfn);
    }
}

static uint64_t hold_zeroed(struct restore *r) {
    uint64_t p = hold_page(r);
    if (p) memset(phys_to_virt(p), 0, PAGE_SIZE);
    return p;
}

static bool add_pair(struct restore *r, uint64_t dst, uint64_t src) {
    if (!r->last || r->last->count == PAIRS) {
        uint64_t p = hold_page(r);
        if (!p) return false;
        struct pair_page *pp = phys_to_virt(p);
        pp->next = pp->count = 0;
        if (r->last) r->last->next = p;
        else r->first = p;
        r->last = pp;
    }
    r->last->pair[r->last->count].dst = dst;
    r->last->pair[r->last->count].src = src;
    r->last->count++;
    r->pairs++;
    return true;
}

/* An image page: in place when its frame was free here, else in a free frame (never one the image
   uses: those were all taken first) until the last copy. src NULL: zeros. */
static bool place(struct restore *r, uint64_t pfn, const void *src) {
    void *at = phys_to_virt(pfn * PAGE_SIZE);
    if (bit(r->direct, pfn)) {
        if (src) memcpy(at, src, PAGE_SIZE);
        else memset(at, 0, PAGE_SIZE);
        return true;
    }
    if (!src) return add_pair(r, pfn * PAGE_SIZE, 0);
    uint64_t t = hold_page(r);
    if (!t) return false;
    memcpy(phys_to_virt(t), src, PAGE_SIZE);
    return add_pair(r, pfn * PAGE_SIZE, t);
}

static void restore_free(struct restore *r, uint64_t total) {
    for (uint64_t p = r->first; p;) {
        struct pair_page *pp = phys_to_virt(p);
        uint64_t next = pp->next;
        for (uint64_t i = 0; i < pp->count; i++)
            if (pp->pair[i].src) pmm_free(pp->pair[i].src);
        pmm_free(p);
        p = next;
    }
    if (r->direct)
        for (uint64_t pfn = 0; next_bit(r->direct, total, &pfn); pfn++) pmm_free(pfn * PAGE_SIZE);
    kfree(r->saved);
    kfree(r->direct);
}

struct reader {
    uint64_t off, end;
    size_t have, at;
};

static bool get(struct reader *rd, void *dst, size_t n) {
    uint8_t *d = dst;
    while (n) {
        if (rd->at == rd->have) {
            if (rd->off >= rd->end) return false;
            size_t len = (size_t)MIN((uint64_t)STAGE_BYTES, rd->end - rd->off);
            if (disk_at(rd->off, h.stage, ALIGN_UP(len, 512), false) < 0) return false;
            rd->off += len;
            rd->have = len;
            rd->at = 0;
        }
        size_t k = MIN(n, rd->have - rd->at);
        memcpy(d, h.stage + rd->at, k);
        rd->at += k;
        d += k;
        n -= k;
    }
    return true;
}

/* all of RAM one to one in 2 MiB pages, for the last copy (its kernel half comes in restore_jump) */
static uint64_t identity_tables(struct restore *r) {
    uint64_t gib = (pmm_page_count() * PAGE_SIZE + (1ULL << 30) - 1) >> 30;
    if (gib > 256 * 512) return 0;
    uint64_t pml4 = hold_zeroed(r);
    if (!pml4) return 0;
    uint64_t *l4 = phys_to_virt(pml4);
    for (uint64_t g = 0; g < gib; g++) {
        if (!l4[g / 512]) {
            uint64_t p = hold_zeroed(r);
            if (!p) return 0;
            l4[g / 512] = p | PTE_P | PTE_W;
        }
        uint64_t *l3 = phys_to_virt(l4[g / 512] & PTE_ADDR), pd = hold_page(r);
        if (!pd) return 0;
        uint64_t *l2 = phys_to_virt(pd);
        for (uint64_t j = 0; j < 512; j++) l2[j] = (g << 30 | j << 21) | PTE_P | PTE_W | PTE_PS;
        l3[g % 512] = pd | PTE_P | PTE_W;
    }
    return pml4;
}

/* The last copy, which runs from the spare page below 1 MiB (arch/wake.h) that neither this kernel
   nor the image uses, on identity_tables' page table, interrupts off:
     rdi  the first page of the copy list (struct pair_page), physical
     rsi  the image's wake page, which the image's wake_sleep prepared before its snapshot
     rdx  where a CPU in long mode enters that page's code (wake_long_entry)
   Every pair is copied, then the image's own wake page table (the first 2 MiB one to one and the
   image's kernel half) is loaded, and the image comes back through its wake page as from S3, with
   rbx = the page. Its jumps are relative, so it runs wherever it is copied. */
extern const uint8_t hib_copy_start[], hib_copy_end[];
__asm__(".pushsection .text\n"
        ".globl hib_copy_start, hib_copy_end\n"
        "hib_copy_start:\n"
        "    movq %rdi, %r12\n"
        "    movq %rsi, %r15\n"
        "    movq %rdx, %r11\n"
        "    cld\n"
        "1:  testq %r12, %r12\n" /* the next page of pairs */
        "    jz 5f\n"
        "    movq 8(%r12), %r13\n"
        "    leaq 16(%r12), %r14\n"
        "2:  testq %r13, %r13\n" /* the next pair */
        "    jz 4f\n"
        "    movq (%r14), %rdi\n"
        "    movq 8(%r14), %rsi\n"
        "    movl $512, %ecx\n"
        "    testq %rsi, %rsi\n"
        "    jz 3f\n"
        "    rep movsq\n"
        "    jmp 6f\n"
        "3:  xorl %eax, %eax\n"
        "    rep stosq\n"
        "6:  addq $16, %r14\n"
        "    decq %r13\n"
        "    jmp 2b\n"
        "4:  movq (%r12), %r12\n"
        "    jmp 1b\n"
        "5:  leaq 0x1000(%r15), %rax\n"
        "    movq %rax, %cr3\n"
        "    movl %r15d, %ebx\n"
        "    jmp *%r11\n"
        "hib_copy_end:\n"
        ".popsection\n");

static NORETURN void restore_jump(uint64_t pml4, uint64_t list, uint64_t image_wake_page) {
    uint64_t spare = wake_spare_page(), entry = wake_long_entry(image_wake_page);
    ASSERT((size_t)(hib_copy_end - hib_copy_start) <= PAGE_SIZE);
    memcpy(phys_to_virt(spare), hib_copy_start, (size_t)(hib_copy_end - hib_copy_start));
    /* the kernel half stays mapped until the jump into the spare page */
    uint64_t *p = phys_to_virt(pml4), *k = phys_to_virt(kernel_pml4 & PTE_ADDR);
    for (unsigned i = 256; i < 512; i++) p[i] = k[i];
    cli();
    /* every TLB entry goes, global ones too, and none is made global again before the image's
       own cpu_setup (Intel SDM vol. 3A 5.10.4.1) */
    write_cr4(read_cr4() & ~CR4_PGE);
    write_cr3(pml4);
    ((void (*)(uint64_t, uint64_t, uint64_t))spare)(list, image_wake_page, entry);
    for (;;) hlt();
}

static const char *check(const struct header *hd) {
    struct header me, c = *hd;
    describe(&me);
    if (memcmp(hd->magic, me.magic, 8) || hd->version != VERSION) return "not an image of this version";
    c.check = 0;
    if (hash_words(HASH_START, &c, sizeof c) != hd->check) return "the header is damaged";
    if (hd->state != VALID) return "it was used already";
    if (hd->total_pages != me.total_pages || hd->map_sig != me.map_sig) return "the memory is not the same";
    if (hd->pci_sig != me.pci_sig) return "the devices are not the same";
    if (hd->fb_phys != me.fb_phys || hd->fb_width != me.fb_width || hd->fb_height != me.fb_height ||
        hd->fb_pitch != me.fb_pitch || hd->fb_bpp != me.fb_bpp)
        return "the display mode is not the same";
    if (hd->wake_page != me.wake_page || !wake_spare_page()) return "the low memory is not the same";
    if (hd->map_bytes != me.map_bytes || hd->data_off != me.data_off) return "the header is damaged";
    return NULL;
}

void hibernate_restore(void) {
    memset(&h, 0, sizeof h);
    int found = store_raw_find(IMAGE_PATH, &h.image);
    if (found == -N_ERR_NOT_FOUND || found == -N_ERR_UNSUPPORTED) return;
    struct restore r = {0};
    struct header hd;
    uint64_t total = pmm_page_count(), t0 = clock_now_ns();
    uint8_t *raw = NULL, *packed = NULL;
    const char *why = "it cannot be read";
    if (cmdline_has("noresume")) {
        why = "noresume";
        goto drop;
    }
    if (found < 0 || !(h.stage = kmalloc(STAGE_BYTES)) || disk_at(0, h.stage, PAGE_SIZE, false) < 0) goto drop;
    memcpy(&hd, h.stage, sizeof hd);
    if ((why = check(&hd))) goto drop;
    why = "the image is damaged";
    if (hd.data_off + hd.data_bytes > h.image.size) goto drop;

    /* the maps */
    uint64_t maps = 2 * hd.map_bytes;
    r.saved = kmalloc(maps);
    r.direct = kzalloc(hd.map_bytes);
    struct reader rd = {.off = PAGE_SIZE, .end = hd.data_off};
    if (!r.saved || !r.direct || !get(&rd, r.saved, maps) || hash_words(HASH_START, r.saved, maps) != hd.maps_check)
        goto drop;
    r.zero = r.saved + hd.map_bytes / 8;

    /* every frame the image fills that is free here is taken now: from here on, a free frame is one
       the image does not use, safe to hold pages until the last copy */
    const struct pmm_range *ranges;
    unsigned nr = pmm_ram_ranges(&ranges);
    uint64_t busy = 0;
    for (unsigned k = 0; k < nr; k++)
        for (uint64_t pfn = ranges[k].base / PAGE_SIZE; pfn < ranges[k].end / PAGE_SIZE; pfn++) {
            if (!bit(r.saved, pfn) && !bit(r.zero, pfn)) continue;
            if (pmm_claim(pfn)) set_bit(r.direct, pfn);
            else busy++;
        }
    uint64_t need = busy + busy / PAIRS + 2 + (total >> 18) + 2048;
    if (pmm_free_pages() < need) {
        why = "not enough memory to restore it";
        goto drop;
    }

    /* the blocks */
    why = "the image is damaged";
    raw = kmalloc(BLOCK_PAGES * PAGE_SIZE);
    packed = kmalloc(BLOCK_PAGES * PAGE_SIZE);
    if (!raw || !packed) goto drop;
    rd = (struct reader){.off = hd.data_off, .end = hd.data_off + hd.data_bytes};
    uint64_t next = 0, pages = 0;
    for (uint64_t n = 0; n < hd.blocks; n++) {
        struct block b;
        if (!get(&rd, &b, sizeof b) || !b.pages || b.pages > BLOCK_PAGES || !b.bytes || b.bytes > b.pages * PAGE_SIZE)
            goto drop;
        size_t size = (size_t)b.pages * PAGE_SIZE;
        if (b.bytes == size) {
            if (!get(&rd, raw, size)) goto drop;
        } else if (!get(&rd, packed, ALIGN_UP(b.bytes, 8)) || lz_decompress(packed, b.bytes, raw, size) != (long)size) {
            goto drop;
        }
        if (hash_words(HASH_START, raw, size) != b.check) goto drop;
        for (uint32_t k = 0; k < b.pages; k++, next++, pages++) {
            if (!next_bit(r.saved, total, &next)) goto drop;
            if (!place(&r, next, raw + (size_t)k * PAGE_SIZE)) goto nomem;
        }
    }
    if (pages != hd.saved || next_bit(r.saved, total, &next)) goto drop;
    for (uint64_t pfn = 0; next_bit(r.zero, total, &pfn); pfn++)
        if (!place(&r, pfn, NULL)) goto nomem;
    uint64_t pml4 = identity_tables(&r);
    if (!pml4) goto nomem;

    /* used from here on: if the restore goes wrong, the next boot is a normal one */
    hd.state = CONSUMED;
    hd.check = 0;
    hd.check = hash_words(HASH_START, &hd, sizeof hd);
    memset(h.stage, 0, PAGE_SIZE);
    memcpy(h.stage, &hd, sizeof hd);
    why = "C: could not be written";
    if (disk_at(0, h.stage, PAGE_SIZE, true) < 0 || !disk_flush()) goto drop;
    kprintf("hibernate: restoring %lu frames (%lu copied last) after %lu ms\n", hd.saved + hd.zero, r.pairs,
            (clock_now_ns() - t0) / NS_PER_MS);

    blk_hold();
    if (!smp_suspend()) {
        /* a CPU still running would run on through the copy, and may hold a lock the devices'
           quiesce takes; the image is used up, so the next boot is a normal one */
        kprintf("hibernate: a processor did not stop, restarting instead of restoring\n");
        power_reboot();
    }
    smp_hold_in_init(); /* out of every page the copy overwrites */
    cli();
    if (pm_quiesce_devices(false)) {
        /* its DMA could write over the restored pages; the image is used up, so the next boot is
           a normal one */
        kprintf("hibernate: a device did not stop, restarting instead of restoring\n");
        power_reboot();
    }
    ioapic_mask_all(); /* the 8259 has been masked since boot (arch/irq.h) */
    if (!iommu_shutdown()) {
        kprintf("hibernate: IOMMU did not stop; restarting without overwriting live DMA tables\n");
        power_reboot();
    } /* the image's tables come with it, and its own resume turns the units on */
    lapic_wake_at(CLOCK_NEVER);
    vm_suspend(); /* the hypervisor's overlays would swallow the copy's writes to their frames */
    restore_jump(pml4, r.first, hd.wake_page);

nomem:
    why = "not enough memory to restore it";
drop:
    kprintf("hibernate: the image is not restored (%s) and is removed\n", why);
    restore_free(&r, total);
    kfree(raw);
    kfree(packed);
    store_raw_free(&h.image);
    kfree(h.stage);
    memset(&h, 0, sizeof h);
    store_entry_remove(IMAGE_PATH, false);
}
