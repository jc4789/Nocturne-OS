/* block の層（dev/blk.h）：装置の一覧、電源の門、書込みと残しの数、実装に渡す要求の整え（割り、緩衝、装置へ渡す
   番地）、一度に一つのことをする装置の thread。 */
#include "kernel.h"
#include "dev/blk.h"
#include "hal/hal.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sys/clock.h"
#include "sys/thread.h"
#include "sys/wait.h"

#define BOUNCE_BYTES (128u * 1024) /* 緩衝の大きさ（block がもっと大きければ block） */

/* 装置は実装が見つけた順（USB のディスクはいつでも）に並び、消えず、数に上限は無い。devs_lock（追加は確保するので
   mutex）が追加を一つずつにする。表は二倍の新しい表へ育ち、ndevs が新しい項を数える前に出す。読む者は ndevs を
   acquire で読んでから表を読み、ndevs より下の項は全部ある。読む者がまだ中に居るかもしれないので古い表は残す：
   全部を合わせても最後の表の大きさまで。 */
static struct mutex devs_lock;
static struct blkdev **devs;
static int ndevs, room;

void blk_register(struct hal_device *hal, struct blkdev *d) {
    ASSERT(d->ops && d->sectors);
    if (!d->block) d->block = 512;
    if (!d->segment_align) d->segment_align = 512;
    d->node = hal->node;
    mutex_lock(&devs_lock);
    int n = ndevs;
    if (n == room) {
        int grown = room ? 2 * room : 8;
        struct blkdev **table = kmalloc((size_t)grown * sizeof *table);
        if (!table) {
            mutex_unlock(&devs_lock);
            kprintf("blk: %s: no memory to list it\n", d->name);
            return;
        }
        if (n) memcpy(table, devs, (size_t)n * sizeof *table);
        __atomic_store_n(&devs, table, __ATOMIC_RELEASE);
        room = grown;
    }
    d->index = n;
    devs[n] = d;
    __atomic_store_n(&ndevs, n + 1, __ATOMIC_RELEASE);
    mutex_unlock(&devs_lock);
    kprintf("blk: %s: %s, %lu MiB, %u-byte blocks%s\n", d->name, d->model, d->sectors / 2048, d->block,
            d->has_cache ? "" : ", no write cache");
    hal_offer(hal, N_OFFER_DISK, (uint32_t)d->index, d->name);
}

int blk_count(void) { return __atomic_load_n(&ndevs, __ATOMIC_ACQUIRE); }

struct blkdev *blk_get(int i) {
    return i >= 0 && i < blk_count() ? __atomic_load_n(&devs, __ATOMIC_ACQUIRE)[i] : NULL;
}

/* 電源の門：holder（立っている間ディスクに届くただ一つの thread）と inflight（動いている要求）。gate_lock の下。
   要求は holder を、blk_hold は inflight を gate_wq で待つ：錠の下で見て、錠を返す前に列に入る。錠は gate_wq の錠
   より先。 */
static struct spinlock gate_lock;
static struct wait_queue gate_wq;
static struct thread *holder;
static unsigned inflight;

static void enter(void) {
    uint64_t f = spin_lock(&gate_lock);
    while (holder && holder != thread_current()) {
        wq_wait_locked(&gate_wq, &gate_lock);
        spin_take(&gate_lock);
    }
    inflight++;
    spin_unlock(&gate_lock, f);
}

static void leave(void) {
    uint64_t f = spin_lock(&gate_lock);
    bool wake = !--inflight && holder;
    spin_unlock(&gate_lock, f);
    if (wake) wq_wake_all(&gate_wq);
}

void blk_hold(void) {
    uint64_t f = spin_lock(&gate_lock);
    holder = thread_current();
    while (inflight) {
        wq_wait_locked(&gate_wq, &gate_lock);
        spin_take(&gate_lock);
    }
    spin_unlock(&gate_lock, f);
}

void blk_release(void) {
    uint64_t f = spin_lock(&gate_lock);
    holder = NULL;
    spin_unlock(&gate_lock, f);
    wq_wake_all(&gate_wq);
}

struct thread *blk_holder(void) {
    uint64_t f = spin_lock(&gate_lock);
    struct thread *t = holder;
    spin_unlock(&gate_lock, f);
    return t;
}

void blk_write_given(struct blkdev *d) { __atomic_add_fetch(&d->given, 1, __ATOMIC_SEQ_CST); }

void blk_write_ended(struct blkdev *d) { __atomic_add_fetch(&d->ended, 1, __ATOMIC_SEQ_CST); }

void blk_cache_lost(struct blkdev *d) {
    if (__atomic_load_n(&d->flushed, __ATOMIC_SEQ_CST) < __atomic_load_n(&d->given, __ATOMIC_SEQ_CST))
        __atomic_store_n(&d->cache_lost, true, __ATOMIC_SEQ_CST);
}

static void count_up_to(uint64_t *count, uint64_t upto) {
    uint64_t was = __atomic_load_n(count, __ATOMIC_SEQ_CST);
    while (was < upto &&
           !__atomic_compare_exchange_n(count, &was, upto, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {}
}

/* ---- 要求の終わり ---- */
void blk_complete(struct blk_request *r, int status) { r->done(r, status); }

/* 失敗した要求は数え、初めの幾つかをログに書く：死にかけのディスクがログを溢れさせない。実装は自分の符号で失敗し、
   呼ぶ者が見るのは N_ERR_IO、N_ERR_TIMEOUT、N_ERR_NO_MEMORY。 */
static int failed(struct blk_request *r, int status) {
    struct blkdev *d = r->device;
    if (__atomic_fetch_add(&d->errors, 1, __ATOMIC_RELAXED) < 8)
        kprintf("blk: %s: %s of %u sectors at %lu failed (%d)\n", d->name,
                r->op == BLK_READ ? "read" : r->op == BLK_WRITE ? "write" : "cache flush", r->sectors, r->lba, status);
    return status == -N_ERR_TIMEOUT || status == -N_ERR_NO_MEMORY ? status : -N_ERR_IO;
}

/* 呼ぶ者の渡した要求が、幾つの部分を持っていても終わった：書込みは装置が持ったとき（または諦めたとき。それも媒体を
   変えたかもしれない）に数える。終わった残しは始まる前に数えた書込みを覆い、その後に装置が cache を失っていれば
   失敗する。 */
static void request_over(struct blk_request *r, int status) {
    struct blkdev *d = r->device;
    if (r->op == BLK_WRITE) {
        __atomic_add_fetch(&d->written, 1, __ATOMIC_SEQ_CST);
    } else if (r->op == BLK_PERSIST && !status) {
        count_up_to(&d->persisted, r->written_at);
        count_up_to(&d->flushed, r->ended_at);
        if (__atomic_load_n(&d->cache_lost, __ATOMIC_SEQ_CST)) status = -N_ERR_IO;
    }
    if (status) status = failed(r, status);
    void (*done)(struct blk_request *, int) = r->done_to;
    leave();
    done(r, status);
}

/* ---- thread で要求を待つ ---- */
/* 語は待つ者の stack にあり、待つ者は語が動けばすぐ去ってよい：起こしはいつもある列の上で、語の番地で選ぶ。 */
#define SYNC_QUEUES 64
static struct wait_queue sync_wq[SYNC_QUEUES];

struct sync_wait {
    uint64_t seq;
    int status;
};

static struct wait_queue *sync_queue(const struct sync_wait *w) { return &sync_wq[((uintptr_t)w >> 6) % SYNC_QUEUES]; }

static void sync_done(struct blk_request *r, int status) {
    struct sync_wait *w = r->context;
    struct wait_queue *q = sync_queue(w);
    w->status = status;
    __atomic_add_fetch(&w->seq, 1, __ATOMIC_RELEASE);
    wq_wake_all(q);
}

static int sync_wait(struct sync_wait *w) {
    while (!__atomic_load_n(&w->seq, __ATOMIC_ACQUIRE)) wq_wait_unless(sync_queue(w), &w->seq, 0, CLOCK_NEVER);
    return w->status;
}

/* ---- 整った要求 ---- */
static uint32_t request_bytes(const struct blk_request *r) {
    uint64_t sum = 0;
    for (uint32_t i = 0; i < r->segment_count; i++) sum += r->segments[i].bytes;
    return sum > UINT32_MAX ? UINT32_MAX : (uint32_t)sum;
}

/* 区切りの装置が見る番地：DMA の装置には bus、ほかは phys。揃えと境はこれで見る。 */
static uint64_t seen(const struct blkdev *d, const struct blk_segment *s) { return d->dma ? s->bus : s->phys; }

/* DMA の装置へ渡す番地を区切りごとに HAL に問う。届かない区切りが一つでもあれば false：要求は緩衝を通る。dma の
   無い装置は物理番地で受ける。 */
static bool reach(const struct blkdev *d, struct blk_segment *segs, uint32_t count) {
    if (!d->dma) return true;
    for (uint32_t i = 0; i < count; i++)
        if (!hal_dma_reach(d->dma, segs[i].phys, segs[i].bytes, &segs[i].bus)) return false;
    return true;
}

/* block の丸ごとで、どの区切りも装置の望むとおりに揃う：実装がそのまま動かせる。 */
static bool shaped(const struct blkdev *d, const struct blk_request *r) {
    unsigned per = d->block / 512;
    if (r->lba % per || r->sectors % per) return false;
    for (uint32_t i = 0; i < r->segment_count; i++)
        if (seen(d, &r->segments[i]) % d->segment_align || r->segments[i].bytes % d->segment_align) return false;
    return true;
}

static bool joined(const struct blkdev *d, uint64_t end, uint64_t next) {
    return !d->segment_boundary || (!(end % d->segment_boundary) && !(next % d->segment_boundary));
}

static bool fits(const struct blkdev *d, const struct blk_request *r) {
    if (d->max_sectors && r->sectors > d->max_sectors) return false;
    if (d->max_segments && r->segment_count > d->max_segments) return false;
    for (uint32_t i = 1; i < r->segment_count; i++)
        if (!joined(d, seen(d, &r->segments[i - 1]) + r->segments[i - 1].bytes, seen(d, &r->segments[i]))) return false;
    return true;
}

/* 要求の区切りの中の位置。 */
struct cursor {
    uint32_t index;
    uint32_t used; /* segments[index] の取ったバイト */
};

/* c からの次の部分：装置の上限の中で block で終わるそのバイト。区切りの数を *count に。部分が作れない（block の中で
   上限に着く）なら 0。out があれば区切りをそこに書き、c を進める。 */
static uint32_t part_cut(const struct blkdev *d, const struct blk_request *r, struct cursor *c, uint32_t *count,
                         struct blk_segment *out) {
    uint64_t limit = d->max_sectors ? (uint64_t)d->max_sectors * 512 / d->block * d->block : UINT32_MAX;
    uint32_t got = 0, n = 0;
    struct cursor at = *c;
    uint64_t end = 0;
    while (at.index < r->segment_count && got < limit) {
        const struct blk_segment *s = &r->segments[at.index];
        uint64_t start = seen(d, s) + at.used;
        if (n && (n == d->max_segments || !joined(d, end, start))) break;
        uint32_t take = (uint32_t)MIN((uint64_t)s->bytes - at.used, limit - got);
        if (out) out[n] = (struct blk_segment){s->phys + at.used, s->bus + at.used, take};
        n++;
        got += take;
        end = start + take;
        at.used += take;
        if (at.used == s->bytes) {
            at.index++;
            at.used = 0;
        }
    }
    /* block の中で終わってよいのは要求の終わりだけ */
    if (got % d->block && at.index < r->segment_count) return 0;
    *count = n;
    if (out) *c = at;
    return got;
}

struct part {
    struct blk_request request;
    struct blk_segment segments[];
};

static void part_over(struct blk_request *p, int status) {
    struct blk_request *whole = p->parent;
    int none = 0;
    if (status) __atomic_compare_exchange_n(&whole->status, &none, status, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    kfree(p);
    if (__atomic_sub_fetch(&whole->parts, 1, __ATOMIC_ACQ_REL)) return;
    whole->done(whole, __atomic_load_n(&whole->status, __ATOMIC_ACQUIRE));
}

/* r を装置の受ける部分の鎖にして *chain に。block で割れないとき、または記憶が無いとき（*memory）は何も作らず
   false。 */
static bool split(const struct blkdev *d, struct blk_request *r, struct blk_request **chain, bool *memory) {
    struct cursor c = {0, 0};
    uint32_t parts = 0, count, bytes;
    while (c.index < r->segment_count) {
        struct cursor next = c;
        if (!(bytes = part_cut(d, r, &next, &count, NULL))) return false;
        c = next;
        parts++;
    }
    struct part **made = kzalloc(parts * sizeof *made);
    if (!made) {
        *memory = true;
        return false;
    }
    c = (struct cursor){0, 0};
    uint64_t lba = r->lba;
    for (uint32_t i = 0; i < parts; i++) {
        struct cursor next = c;
        bytes = part_cut(d, r, &next, &count, NULL);
        struct part *p = kmalloc(sizeof *p + count * sizeof(struct blk_segment));
        if (!p) {
            for (uint32_t k = 0; k < i; k++) kfree(made[k]);
            kfree(made);
            *memory = true;
            return false;
        }
        part_cut(d, r, &c, &count, p->segments);
        p->request = (struct blk_request){.op = r->op,
                                          .lba = lba,
                                          .sectors = bytes / 512,
                                          .segments = p->segments,
                                          .segment_count = count,
                                          .device = r->device,
                                          .done = part_over,
                                          .parent = r};
        lba += bytes / 512;
        made[i] = p;
    }
    r->status = 0;
    __atomic_store_n(&r->parts, parts, __ATOMIC_RELEASE);
    struct blk_request **tail = chain;
    for (uint32_t i = 0; i < parts; i++) {
        *tail = &made[i]->request;
        tail = &made[i]->request.next;
    }
    *tail = NULL;
    kfree(made);
    return true;
}

/* ---- 整わない要求：block の一部、揃わない区切り、装置の届かない区切り ---- */
/* 緩衝：装置が届くメモリー。DMA の装置には HAL が装置の幅の中に取り（訳の領域にいる装置にはそこへ写る）、ほかの
   装置には近い node の頁を。 */
struct bounce {
    uint8_t *memory;
    struct blk_segment segment; /* 緩衝の全部 */
};

static bool bounce_take(struct blkdev *d, uint32_t bytes, struct bounce *b) {
    if (d->dma) {
        uint64_t bus;
        void *memory = hal_dma_alloc(d->dma, bytes, &bus);
        if (!memory) return false;
        *b = (struct bounce){memory, {vmm_kernel_translate((uint64_t)memory), bus, bytes}};
        return true;
    }
    uint64_t phys = pmm_alloc_run(bytes / PAGE_SIZE, (unsigned)MAX(d->node, 0));
    if (!phys) return false;
    *b = (struct bounce){phys_to_virt(phys), {phys, 0, bytes}};
    return true;
}

static void bounce_give(struct blkdev *d, struct bounce *b) {
    if (d->dma) hal_dma_free(d->dma, b->memory);
    else pmm_free_contig(b->segment.phys, b->segment.bytes / PAGE_SIZE);
}

/* 要求の区切りの skip バイト目からと buf の間でバイトを写す。 */
static void copy_segments(const struct blk_request *r, uint64_t skip, uint8_t *buf, size_t n, bool into_buf) {
    for (uint32_t i = 0; i < r->segment_count && n; i++) {
        const struct blk_segment *s = &r->segments[i];
        if (skip >= s->bytes) {
            skip -= s->bytes;
            continue;
        }
        size_t take = MIN((size_t)(s->bytes - skip), n);
        uint8_t *at = (uint8_t *)phys_to_virt(s->phys + skip);
        if (into_buf) memcpy(buf, at, take);
        else memcpy(at, buf, take);
        buf += take;
        n -= take;
        skip = 0;
    }
}

/* 緩衝の初めから sectors の分を一つの整った要求として実装へ渡して待つ。門と数には加わらない：呼ぶ者の要求が
   加わっている。 */
static int run_inside(struct blkdev *d, enum blk_op op, uint64_t lba, uint32_t sectors, const struct bounce *b) {
    struct blk_segment seg = {b->segment.phys, b->segment.bus, sectors * 512};
    struct sync_wait w = {0};
    struct blk_request r = {.op = op,
                            .lba = lba,
                            .sectors = sectors,
                            .segments = &seg,
                            .segment_count = 1,
                            .device = d,
                            .done = sync_done,
                            .context = &w};
    struct blk_request *chain = &r;
    bool memory = false;
    if (!fits(d, &r) && !split(d, &r, &chain, &memory)) return memory ? -N_ERR_NO_MEMORY : -N_ERR_IO;
    d->ops->submit(d, chain);
    return sync_wait(&w);
}

/* 整わない要求の読み書き：block の続きごとに緩衝を通す。同じ block のほかの部分の書込みを元のまま書き戻さない
   （partial）。隣の block を丸ごと書く要求との重なりは、重なる二つの要求と同じく呼ぶ者が分ける。 */
static int bounce_io(struct blkdev *d, struct blk_request *r) {
    unsigned per = d->block / 512;
    struct bounce b;
    if (!bounce_take(d, MAX(BOUNCE_BYTES, d->block), &b) && !bounce_take(d, MAX((uint32_t)PAGE_SIZE, d->block), &b))
        return -N_ERR_NO_MEMORY;
    uint64_t first = r->lba / per, end = (r->lba + r->sectors + per - 1) / per;
    uint64_t want_start = r->lba * 512, want_end = (r->lba + r->sectors) * 512;
    int status = 0;
    for (uint64_t at = first; at < end && !status;) {
        uint64_t n = MIN(end - at, b.segment.bytes / d->block);
        uint64_t from = MAX(at * d->block, want_start), to = MIN((at + n) * d->block, want_end);
        bool whole = from == at * d->block && to == (at + n) * d->block;
        uint8_t *place = b.memory + (from - at * d->block);
        if (r->op == BLK_READ) {
            status = run_inside(d, BLK_READ, at * per, (uint32_t)(n * per), &b);
            if (!status) copy_segments(r, from - want_start, place, (size_t)(to - from), false);
        } else {
            if (!whole) mutex_lock(&d->partial);
            if (!whole) status = run_inside(d, BLK_READ, at * per, (uint32_t)(n * per), &b);
            if (!status) {
                copy_segments(r, from - want_start, place, (size_t)(to - from), true);
                status = run_inside(d, BLK_WRITE, at * per, (uint32_t)(n * per), &b);
            }
            if (!whole) mutex_unlock(&d->partial);
        }
        at += n;
    }
    bounce_give(d, &b);
    return status;
}

/* ---- 要求を渡す ---- */
/* 一つの要求を門の中で実装へ。reached：DMA の装置がどの区切りにもそのまま届く（reach）。整わない要求は緩衝を通し、
   その終わりまで眠る。大きい要求は実装の受ける部分に割り、一度の submit で鎖ごと渡す。 */
static void request_give(struct blkdev *d, struct blk_request *r, bool reached) {
    enter();
    r->device = d;
    r->done_to = r->done;
    r->done = request_over;
    struct blk_request *chain = r;
    bool memory = false;
    if (__atomic_load_n(&d->gone, __ATOMIC_ACQUIRE)) {
        request_over(r, -N_ERR_IO);
    } else if (r->op == BLK_PERSIST) {
        r->written_at = __atomic_load_n(&d->written, __ATOMIC_SEQ_CST);
        r->ended_at = __atomic_load_n(&d->ended, __ATOMIC_SEQ_CST);
        if (d->has_cache && __atomic_load_n(&d->persisted, __ATOMIC_SEQ_CST) < r->written_at) d->ops->submit(d, r);
        else request_over(r, d->has_cache || !__atomic_load_n(&d->cache_lost, __ATOMIC_SEQ_CST) ? 0 : -N_ERR_IO);
    } else if (r->lba + r->sectors > d->sectors || request_bytes(r) != r->sectors * 512 || !r->sectors) {
        request_over(r, -N_ERR_IO);
    } else if (!reached || !shaped(d, r)) {
        request_over(r, bounce_io(d, r));
    } else if (fits(d, r) || split(d, r, &chain, &memory)) {
        d->ops->submit(d, chain);
    } else {
        request_over(r, memory ? -N_ERR_NO_MEMORY : -N_ERR_IO);
    }
}

/* ---- 一つの thread の要求 ---- */
/* buf にある核のメモリーの物理の区切り：頁を順に、一続きの所はつないで。区切りは stack に、足りなければ heap に
   （*heap）。 */
static struct blk_segment *segments_of(const void *buf, size_t bytes, struct blk_segment *stack, unsigned room,
                                       uint32_t *count, struct blk_segment **heap) {
    uint64_t va = (uint64_t)buf;
    unsigned pages = (unsigned)(((va & (PAGE_SIZE - 1)) + bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    struct blk_segment *out = stack;
    *heap = NULL;
    if (pages > room) {
        if (!(out = *heap = kmalloc(pages * sizeof *out))) return NULL;
    }
    uint32_t n = 0;
    while (bytes) {
        uint64_t phys = vmm_kernel_translate(va);
        uint32_t take = (uint32_t)MIN((uint64_t)bytes, PAGE_SIZE - (va & (PAGE_SIZE - 1)));
        if (!phys) {
            kfree(*heap);
            *heap = NULL;
            return NULL;
        }
        if (n && out[n - 1].phys + out[n - 1].bytes == phys) out[n - 1].bytes += take;
        else out[n++] = (struct blk_segment){.phys = phys, .bytes = take};
        va += take;
        bytes -= take;
    }
    *count = n;
    return out;
}

static int run(struct blkdev *d, enum blk_op op, uint64_t lba, uint32_t n, const void *buf) {
    struct blk_segment stack[8], *heap = NULL, *segs = NULL;
    uint32_t count = 0;
    if (op != BLK_PERSIST && !(segs = segments_of(buf, (size_t)n * 512, stack, ARRAY_SIZE(stack), &count, &heap)))
        return -N_ERR_NO_MEMORY;
    bool reached = reach(d, segs, count);
    struct sync_wait w = {0};
    struct blk_request r = {.op = op, .lba = lba, .sectors = n, .segments = segs, .segment_count = count,
                            .done = sync_done, .context = &w};
    request_give(d, &r, reached);
    int status = sync_wait(&w);
    kfree(heap);
    return status;
}

int blk_read(struct blkdev *d, uint64_t lba, uint32_t n, void *buf) { return run(d, BLK_READ, lba, n, buf); }

int blk_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf) { return run(d, BLK_WRITE, lba, n, buf); }

int blk_persist(struct blkdev *d) { return run(d, BLK_PERSIST, 0, 0, NULL); }

/* ---- 一度に一つのことをする装置 ---- */
struct blk_serial {
    struct blkdev *d;
    blk_transfer_fn transfer;
    struct spinlock lock;
    struct blk_request *head, **tail;
    struct wait_queue wq;
    bool stop;
};

static void serial_submit(struct blkdev *d, struct blk_request *chain) {
    struct blk_serial *s = d->serial;
    uint64_t f = spin_lock(&s->lock);
    *s->tail = chain;
    while (*s->tail) s->tail = &(*s->tail)->next;
    spin_unlock(&s->lock, f);
    wq_wake_all(&s->wq);
}

const struct blk_ops blk_serial_ops = {.submit = serial_submit};

static void serial_thread(void *arg) {
    struct blk_serial *s = arg;
    for (;;) {
        uint64_t f = spin_lock(&s->lock);
        while (!s->head && !s->stop) {
            wq_wait_locked(&s->wq, &s->lock);
            spin_take(&s->lock);
        }
        struct blk_request *r = s->head;
        if (r && !(s->head = r->next)) s->tail = &s->head;
        bool stop = s->stop;
        spin_unlock(&s->lock, f);
        if (!r) break;
        r->next = NULL;
        blk_complete(r, stop || __atomic_load_n(&s->d->gone, __ATOMIC_ACQUIRE) ? -N_ERR_IO : s->transfer(s->d, r));
    }
    kfree(s);
}

void blk_serial_start(struct blkdev *d, blk_transfer_fn transfer) {
    struct blk_serial *s = kzalloc(sizeof *s);
    ASSERT(s);
    s->d = d;
    s->transfer = transfer;
    s->tail = &s->head;
    d->serial = s;
    d->ops = &blk_serial_ops;
    ASSERT(kthread_create(d->name[0] ? d->name : "blk", serial_thread, s));
}

void blk_serial_stop(struct blkdev *d) {
    struct blk_serial *s = d->serial;
    d->serial = NULL;
    uint64_t f = spin_lock(&s->lock);
    s->stop = true;
    spin_unlock(&s->lock, f);
    wq_wake_all(&s->wq);
}
