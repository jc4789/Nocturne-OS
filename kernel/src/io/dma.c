/* 装置の DMA（io/io.h）：装置が届く番地の幅、装置の管理者を通したメモリーの一覧、訳の領域に入る・出る止めと始め。
   装置へ渡す番地は物理番地と同じ：訳の無いバスはそのまま見せ、訳の領域へはバスが同じ番地で写す（dev/iommu.h）。
   錠：装置の dma_lock（眠れる：バスの写しは眠る）が一覧と dma_translated と dma_paused を守る。順は実装の錠 →
   dma_lock → バスの写しの錠（dev/iommu.c の domain の mutex）。dma_lock の下で実装を呼ばない。 */
#include "kernel.h"
#include "arch/topology.h"
#include "io/internal.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

/* 装置の管理者を通したメモリーの一区間：取ったもの（owned）か、ほかの持ち主のものを写したものか。kept：訳の領域から外せたと
   確かめられなかった：装置がまだ届くかもしれないので頁を返さない。 */
struct io_dma_block {
    struct io_dma_block *next;
    uint64_t phys, pages;
    bool owned, kept;
};

void io_dma_bits(struct io_device *d, unsigned bits) {
    ASSERT(bits && bits <= 64 && !d->dma);
    d->dma_bits = bits;
}

static bool within(const struct io_device *d, uint64_t phys, uint64_t pages) {
    return d->dma_bits >= 64 || !((phys + pages * PAGE_SIZE - 1) >> d->dma_bits);
}

/* dma_lock の下：訳の領域にいれば写してから一覧に入れる。 */
static bool block_add(struct io_device *d, struct io_dma_block *b) {
    if (d->dma_translated && !d->ops->dma_map(d, b->phys, b->pages)) return false;
    b->next = d->dma;
    d->dma = b;
    return true;
}

/* dma_lock の下：訳の領域から外す。外れたと確かめられなければ kept。 */
static void block_unmap(struct io_device *d, struct io_dma_block *b) {
    if (!d->ops->dma_unmap(d, b->phys, b->pages)) b->kept = true;
}

/* dma_lock の下：phys から始まる区間を一覧から外し、訳の領域にいれば領域からも外す。 */
static struct io_dma_block *block_take(struct io_device *d, uint64_t phys, bool owned) {
    for (struct io_dma_block **p = &d->dma; *p; p = &(*p)->next) {
        struct io_dma_block *b = *p;
        if (b->phys != phys || b->owned != owned) continue;
        *p = b->next;
        if (d->dma_translated) block_unmap(d, b);
        return b;
    }
    return NULL;
}

/* 一覧から外した区間の終わり。give：取った頁を返してよい（装置が止まった）。 */
static void block_drop(struct io_device *d, struct io_dma_block *b, bool give) {
    if (b->kept) kprintf("hal: %s: DMA memory at %#lx may still be reached through a domain it left\n", d->location, b->phys);
    if (b->owned && give && !b->kept) pmm_free_contig(b->phys, b->pages);
    kfree(b);
}

void *io_dma_alloc(struct io_device *d, size_t bytes, uint64_t *bus) {
    uint64_t pages = ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE;
    unsigned node = d->node >= 0 ? (unsigned)d->node : topology.cpu[cpu_current_index()].node;
    struct io_dma_block *b = kmalloc(sizeof *b);
    if (!b) return NULL;
    /* 幅が全部の番地に届けば装置の node の近くから。届かなければ幅の中から */
    uint64_t phys = d->dma_bits >= 64 ? pmm_alloc_run(pages, node) : 0;
    if (!phys) phys = pmm_alloc_contig(pages, d->dma_bits);
    if (!phys) {
        kfree(b);
        return NULL;
    }
    memset(phys_to_virt(phys), 0, pages * PAGE_SIZE);
    *b = (struct io_dma_block){.phys = phys, .pages = pages, .owned = true};
    mutex_lock(&d->dma_lock);
    bool added = block_add(d, b);
    mutex_unlock(&d->dma_lock);
    if (!added) {
        pmm_free_contig(phys, pages);
        kfree(b);
        return NULL;
    }
    *bus = phys;
    return phys_to_virt(phys);
}

void io_dma_free(struct io_device *d, void *memory) {
    mutex_lock(&d->dma_lock);
    struct io_dma_block *b = block_take(d, hhdm_virt_to_phys(memory), true);
    mutex_unlock(&d->dma_lock);
    ASSERT(b);
    block_drop(d, b, true);
}

bool io_dma_map(struct io_device *d, uint64_t phys, uint64_t pages, uint64_t *bus) {
    if (!within(d, phys, pages)) return false;
    struct io_dma_block *b = kmalloc(sizeof *b);
    if (!b) return false;
    *b = (struct io_dma_block){.phys = phys, .pages = pages};
    mutex_lock(&d->dma_lock);
    bool added = block_add(d, b);
    mutex_unlock(&d->dma_lock);
    if (!added) {
        kfree(b);
        return false;
    }
    *bus = phys;
    return true;
}

void io_dma_unmap(struct io_device *d, uint64_t phys, uint64_t pages) {
    mutex_lock(&d->dma_lock);
    struct io_dma_block *b = block_take(d, phys, false);
    mutex_unlock(&d->dma_lock);
    ASSERT(b && b->pages == pages);
    block_drop(d, b, false);
}

bool io_dma_pausable(const struct io_device *d) { return d->driver && d->driver->dma_pause && d->driver->dma_restart; }

void io_dma_pause(struct io_device *d) {
    mutex_lock(&d->dma_lock);
    bool already = d->dma_paused;
    d->dma_paused = true;
    mutex_unlock(&d->dma_lock);
    if (already) return;
    d->driver->dma_pause(d);
    /* 止まったと確かめられなくても領域からは外す：まだ送る装置の要求は訳の段で落ちる */
    if (d->ops->stop && !d->ops->stop(d)) kprintf("hal: %s: its DMA is not confirmed stopped\n", d->location);
    mutex_lock(&d->dma_lock);
    if (d->dma_translated) {
        for (struct io_dma_block *b = d->dma; b; b = b->next) block_unmap(d, b);
        __atomic_store_n(&d->dma_translated, false, __ATOMIC_RELEASE);
    }
    mutex_unlock(&d->dma_lock);
}

bool io_dma_restart(struct io_device *d, bool translated) {
    mutex_lock(&d->dma_lock);
    if (!d->dma_paused) {
        mutex_unlock(&d->dma_lock);
        return true;
    }
    struct io_dma_block *failed = NULL;
    if (translated) {
        ASSERT(d->ops->dma_map && d->ops->dma_unmap);
        for (struct io_dma_block *b = d->dma; b && !failed; b = b->next)
            if (!d->ops->dma_map(d, b->phys, b->pages)) failed = b;
        for (struct io_dma_block *b = d->dma; failed && b != failed; b = b->next) block_unmap(d, b);
    }
    if (!failed) {
        __atomic_store_n(&d->dma_translated, translated, __ATOMIC_RELEASE);
        d->dma_paused = false;
    }
    mutex_unlock(&d->dma_lock);
    if (failed) {
        kprintf("hal: %s: its DMA memory cannot be mapped where it now sends: it stays stopped\n", d->location);
        return false;
    }
    d->driver->dma_restart(d);
    return true;
}

void io_dma_release(struct io_device *d, bool stopped) {
    mutex_lock(&d->dma_lock);
    struct io_dma_block *blocks = d->dma;
    d->dma = NULL;
    if (d->dma_translated)
        for (struct io_dma_block *b = blocks; b; b = b->next) block_unmap(d, b);
    __atomic_store_n(&d->dma_translated, false, __ATOMIC_RELEASE);
    d->dma_paused = false;
    d->dma_bits = 32;
    mutex_unlock(&d->dma_lock);
    /* 止まったと確かめられない装置が書くかもしれないメモリーは、誰にも渡さない */
    if (!stopped && blocks) kprintf("hal: %s did not stop: its DMA memory is kept\n", d->location);
    while (blocks) {
        struct io_dma_block *next = blocks->next;
        block_drop(d, blocks, stopped);
        blocks = next;
    }
}
