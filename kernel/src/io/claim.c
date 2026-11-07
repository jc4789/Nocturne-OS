/* プロセスの実装（io/io.h）：アプリの中の装置ドライバーが台帳の装置を持つ口。docs/nocturne-native-abi.md §9。

   claim は N_KIND_HAL_CLAIM の object で、最後の handle が閉じると終わる。BAR と DMA の object は claim の記録を
   参照で持ち、claim の終わりは記録を消さない：終わった後の写像と DMA は N_ERR_CLOSED と、全部 1 が読める頁になる。
   ここはバスを知らない。バスの持ち分は struct io_claim_ops（begin、end、call）にある。

   錠の順（先 → 後）：門（io_busy_begin）→ end_mutex と装置の bind、end_mutex → claims_mutex、end_mutex → バスの錠と mm/shared.c の錠、offers_mutex → io_lock、
   irq_lock → 待ち行列と事象の錠、power_lock → 待ち行列の錠。ends_lock は葉。
   終わりは claim ごとに一度だけ走る（end_mutex）。最後の handle の close と io_remove が同時に来ても、後の者は
   終わりが済むまで待つ。呼出しは io_claim_enter で記録を hold するので、io_remove が戻った後に claim が装置に触ることは
   無い。 */
#include "kernel.h"
#include "abi.h"
#include "arch/irq.h"
#include "arch/spin.h"
#include "io/internal.h"
#include "mm/heap.h"
#include "mm/shared.h"
#include "sys/clock.h"
#include "sys/object.h"
#include "sys/process.h"
#include "sys/syscall.h"
#include "sys/thread.h"
#include "sys/wait.h"

#define VECTORS_MOST 2048 /* claim が立てる割り込みの最大。MSI-X の表の最大（PCIe 6.0 7.7.2.2） */
/* 眠る claim が SUSPEND に答えるまでの時間。装置を止める作業のための時間で、これを過ぎたら眠りを取りやめる。 */
#define SLEEP_ANSWER_NS (10 * NS_PER_S)

/* claim の割り込みを、n_hal_interrupts が一度に立てた数だけ：それぞれの irq の文脈と、届いた印（message n は
   語 n / 64）。handler が立て、take が消す。数を増やすと新しい塊を作る。古い塊は claim が持ち続ける：割り込みの
   再割り当ての表が持つかもしれない irq は文脈を持ち続けるからである（interrupts_off）。
   level の線（ACPI の装置の GSI）は handler から take まで塞ぐ（IRQ_HOLD）：アプリが装置を静めるまで、線は CPU を
   呼び続けない。take が開ける。message では塞ぐものが無い。claim の GSI は誰とも線を分けない（alone）：claim の
   handler はどの割り込みも自分のものとして受けるので、分けた線では他の装置の割り込みを奪う。 */
struct claim_message {
    struct io_claim *claim;
    unsigned n;
    struct irq *irq; /* irq_lock の下。立てていなければ NULL */
};
struct claim_vectors {
    struct claim_vectors *older;
    unsigned room;
    uint64_t *pending; /* [(room + 63) / 64]。messages の後ろ */
    struct claim_message messages[];
};

struct claim_offer {
    struct claim_offer *next;
    uint32_t kind, instance;
};

struct io_claim {
    struct io_device *d;
    const struct io_claim_ops *ops;
    void *bus; /* バスが begin で置いた記録 */
    struct object object;
    struct shared_claim shared; /* BAR の object */
    unsigned refs;              /* object と、それを通して作った BAR と DMA の object。atomic */
    bool released;              /* 終わりが始まった。BAR の写像はもう持ち主のものでない。atomic */
    bool dma_unsafe;            /* 終わりが装置の DMA の停止を確かめられなかった：DMA の頁を返さない。atomic */
    struct mutex end_mutex;     /* 終わりを一度にする */
    bool ended;                 /* end_mutex の下 */
    struct spinlock ends_lock;  /* ends と end_started。葉 */
    struct io_claim_end *ends;
    bool end_started;
    bool listed;                /* claims の列にある。claims_mutex の下 */
    struct io_claim *next, *prev;

    /* 割り込み。handler は irq_lock の下で印を立て、待ち行列と object を起こす。vectors が 0 になれば、どの
       handler も claim に触れない。 */
    struct wait_queue wq;
    struct spinlock irq_lock;
    unsigned vectors;           /* 立てた数。irq_lock の下で、一度に一つの n_hal_interrupts が変える */
    unsigned pending;           /* 印の立った message の数。irq_lock の下。claim_ready は錠なしで読む */
    struct claim_vectors *vec;  /* 最新の塊。irq_lock と vectors_mutex の下 */
    struct mutex vectors_mutex; /* n_hal_interrupts を一度に一つにする */

    struct mutex offers_mutex;
    struct claim_offer *offers;

    /* 眠り。power_event は渡した知らせで、n_hal_power_wait が取る。power_awaiting は SUSPEND への答えを待っている。
       power_answer：0 まだ、1 できた、2 断った。power_told は SUSPEND を渡した（取りやめでも RESUME を渡す）。 */
    struct spinlock power_lock;
    struct wait_queue power_wq;
    bool sleeps; /* atomic */
    uint32_t power_event, power_answer;
    bool power_awaiting, power_told;
};

static struct mutex claims_mutex;
static struct io_claim *claims;

const struct io_claim_ops io_claim_plain = {0};

struct io_device *io_claim_device(const struct io_claim *c) { return c->d; }
void *io_claim_bus(const struct io_claim *c) { return c->bus; }
struct shared_claim *io_claim_shared(struct io_claim *c) { return &c->shared; }
bool io_claim_revoked(const struct io_claim *c) { return c && __atomic_load_n(&c->released, __ATOMIC_SEQ_CST); }
bool io_claim_dma_reusable(const struct io_claim *c) { return !c || !__atomic_load_n(&c->dma_unsafe, __ATOMIC_ACQUIRE); }
bool io_claim_sleeps(const struct io_claim *c) { return __atomic_load_n(&c->sleeps, __ATOMIC_ACQUIRE); }

/* 終わりが始まった claim は入れない。released は io_lock の下で立つので、入った者の間 c->d は生きている。 */
struct io_device *io_claim_enter(struct io_claim *c) {
    uint64_t f = spin_lock(&io_lock);
    bool in = !__atomic_load_n(&c->released, __ATOMIC_SEQ_CST);
    if (in) c->d->holds++;
    spin_unlock(&io_lock, f);
    return in ? c->d : NULL;
}

static uint32_t claim_ready(struct handle *h);
static void claim_release(struct object *o);
/* アプリの終わりでは、claim の handle は他の handle と写像より先に閉じる：装置が止まり、DMA 済みの確かめが
   つく前に DMA の object がメモリーを返すことは無い。 */
static const struct object_ops claim_object_ops = {
    .kind = N_KIND_HAL_CLAIM, .ready = claim_ready, .release = claim_release, .closes_first = true};

struct io_claim *io_claim_of(struct handle *h) { return handle_is(h, &claim_object_ops) ? h->object->priv : NULL; }

struct io_claim *io_claim_hold(struct handle *h) {
    struct io_claim *c = io_claim_of(h);
    if (c) __atomic_fetch_add(&c->refs, 1, __ATOMIC_RELAXED); /* h が一つ持っている */
    return c;
}

static void claim_drop_to_zero(struct io_claim *c) {
    if (c->bus && c->ops->release) c->ops->release(c);
    for (struct claim_vectors *v = c->vec, *older; v; v = older) {
        older = v->older;
        kfree(v);
    }
    kfree(c);
}

void io_claim_drop(struct io_claim *c) {
    if (!c) return;
    unsigned left = __atomic_sub_fetch(&c->refs, 1, __ATOMIC_ACQ_REL);
    ASSERT(left != UINT32_MAX);
    if (!left) claim_drop_to_zero(c);
}

/* DMA は、バスが dma_bits を持つ claim だけが作れる。 */
unsigned io_claim_dma_bits(const struct io_claim *c) { return c->ops->dma_bits ? c->ops->dma_bits((struct io_claim *)c) : 0; }
bool io_claim_dma_map(struct io_claim *c, const uint64_t *frames, uint64_t base, size_t count) {
    return !c->ops->dma_map || c->ops->dma_map(c, frames, base, count);
}
bool io_claim_dma_unmap(struct io_claim *c, const uint64_t *frames, uint64_t base, size_t count) {
    return !c->ops->dma_unmap || c->ops->dma_unmap(c, frames, base, count);
}

/* ---- 割り込み ---- */

/* claim の message n。irq が向かう CPU で走る。 */
static enum irq_result claim_isr(void *context) {
    struct claim_message *m = context;
    struct io_claim *c = m->claim;
    spin_take(&c->irq_lock);
    if (m->n < c->vectors) {
        uint64_t *word = &c->vec->pending[m->n / 64], bit = 1ull << (m->n % 64);
        if (!(*word & bit)) {
            *word |= bit;
            __atomic_store_n(&c->pending, c->pending + 1, __ATOMIC_RELEASE);
        }
        wq_wake_all(&c->wq);
        object_notify(&c->object);
    }
    spin_give(&c->irq_lock);
    return IRQ_HOLD;
}

static void interrupts_off(struct io_claim *c) {
    if (!c->vectors) return;
    uint64_t irq = spin_lock(&c->irq_lock);
    c->vectors = 0;
    for (unsigned i = 0; i < c->vec->room; i++) c->vec->messages[i].irq = NULL;
    memset(c->vec->pending, 0, (c->vec->room + 63) / 64 * sizeof *c->vec->pending);
    __atomic_store_n(&c->pending, 0, __ATOMIC_RELAXED);
    spin_unlock(&c->irq_lock, irq);
    /* 止まったと確かめられなかった irq は残り、その handler は claim を指し続ける：claim も残る */
    io_irq_disable(c->d);
    if (c->d->irqs_on) __atomic_add_fetch(&c->refs, 1, __ATOMIC_ACQ_REL);
    /* 待っている者は、最後の割り込みを切られても眠り続けず NOT_READY を見る */
    wq_wake_all(&c->wq);
    object_notify(&c->object);
}

static uint32_t claim_ready(struct handle *h) {
    struct io_claim *c = h->object->priv;
    return __atomic_load_n(&c->pending, __ATOMIC_ACQUIRE) ? N_EVENT_RECEIVABLE : 0;
}

/* vectors_mutex を持っている。 */
static int64_t interrupts_on(struct io_claim *c, uint32_t count) {
    struct io_device *d = c->d;
    interrupts_off(c);
    if (!count) return 0;
    unsigned most = c->ops->irq_max ? c->ops->irq_max(c) : io_irq_max(d);
    if (!most) return -N_ERR_UNSUPPORTED;
    count = MIN(MIN(count, most), (uint32_t)VECTORS_MOST);
    struct claim_vectors *v = c->vec;
    if (!v || v->room < count) {
        size_t words = (count + 63) / 64;
        v = kzalloc(sizeof *v + count * sizeof *v->messages + words * sizeof *v->pending);
        if (!v) return -N_ERR_NO_MEMORY;
        v->room = count;
        v->pending = (uint64_t *)&v->messages[count];
        for (unsigned i = 0; i < count; i++) v->messages[i] = (struct claim_message){.claim = c, .n = i};
        v->older = c->vec;
    }
    struct irq_spec *specs = kmalloc(count * sizeof *specs);
    struct irq **irqs = kzalloc(count * sizeof *irqs);
    if (!specs || !irqs) {
        kfree(specs);
        kfree(irqs);
        if (v != c->vec) kfree(v);
        return -N_ERR_NO_MEMORY;
    }
    for (unsigned i = 0; i < count; i++)
        specs[i] = (struct irq_spec){.name = "app driver", .handler = claim_isr, .context = &v->messages[i], .alone = true};
    /* message が来るより先に on にする：入口を書いた瞬間に来ることがある */
    uint64_t irq = spin_lock(&c->irq_lock);
    c->vec = v;
    c->vectors = count;
    spin_unlock(&c->irq_lock, irq);
    unsigned n = io_irq_enable(d, specs, count, irqs);
    kfree(specs);
    irq = spin_lock(&c->irq_lock);
    c->vectors = n;
    for (unsigned i = 0; i < n; i++) v->messages[i].irq = irqs[i];
    spin_unlock(&c->irq_lock, irq);
    kfree(irqs);
    if (!n) { /* 名指しできる CPU に空きの vector が無い、または message を作れなかった */
        wq_wake_all(&c->wq);
        object_notify(&c->object);
        return -N_ERR_BUSY;
    }
    return n;
}

int64_t io_claim_interrupts(struct handle *h, uint32_t count) {
    struct io_claim *c = io_claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (!io_claim_enter(c)) return -N_ERR_CLOSED;
    struct io_device *d = c->d;
    mutex_lock(&c->vectors_mutex);
    int64_t r = io_claim_revoked(c) ? -N_ERR_CLOSED : interrupts_on(c, count);
    mutex_unlock(&c->vectors_mutex);
    io_put(d);
    return r;
}

/* 取った message の線を開ける（claim_isr の IRQ_HOLD）。vectors_mutex の下で：irq は interrupts_off（同じ mutex の
   下）が外すまで生きている。外した後なら何もしない。irq_lock は持たない：irq の錠は handler の後の保持で claim の
   錠と並ばずに取られる。 */
static void taken_unhold(struct io_claim *c, const uint64_t *got, unsigned words) {
    mutex_lock(&c->vectors_mutex);
    for (unsigned w = 0; w < words; w++)
        for (uint64_t bits = got[w]; bits; bits &= bits - 1) {
            unsigned n = w * 64 + (unsigned)__builtin_ctzll(bits);
            struct irq *line = n < c->vectors ? c->vec->messages[n].irq : NULL;
            if (line) irq_unhold(line);
        }
    mutex_unlock(&c->vectors_mutex);
}

/* irq_lock を持っている。最初の words 語の印を got へ取り、そこで消す。取った語の数（claim の語数まで）。 */
static unsigned pending_take(struct io_claim *c, uint64_t *got, unsigned words) {
    unsigned n = MIN(words, (c->vectors + 63) / 64), taken = 0;
    for (unsigned i = 0; i < n; i++) {
        got[i] = c->vec->pending[i];
        c->vec->pending[i] = 0;
        taken += (unsigned)__builtin_popcountll(got[i]);
    }
    __atomic_store_n(&c->pending, c->pending - taken, __ATOMIC_RELEASE);
    return n;
}

int64_t io_claim_interrupt_take(struct handle *h, uint64_t deadline, uint64_t *ubits, uint32_t words) {
    struct io_claim *c = io_claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (!words) return -N_ERR_INVALID;
    uint64_t got[VECTORS_MOST / 64];
    words = MIN(words, (uint32_t)(VECTORS_MOST / 64));
    uint64_t irq = spin_lock(&c->irq_lock);
    for (;;) {
        /* 最初の words 語の中に、届いた message がある */
        bool any = false;
        for (unsigned i = 0; c->pending && !any && i < MIN(words, (c->vectors + 63) / 64); i++) any = c->vec->pending[i];
        if (any) {
            unsigned n = pending_take(c, got, words);
            spin_unlock(&c->irq_lock, irq);
            taken_unhold(c, got, n);
            if (!copy_to_user(ubits, got, n * sizeof *got)) return n;
            /* 渡せなかった：次の take のために印を戻す */
            irq = spin_lock(&c->irq_lock);
            for (unsigned i = 0; i < n && c->vectors; i++)
                for (uint64_t m = got[i] & ~c->vec->pending[i]; m; m &= m - 1) {
                    c->vec->pending[i] |= m & -m;
                    c->pending++;
                }
            spin_unlock(&c->irq_lock, irq);
            return -N_ERR_BAD_ADDRESS;
        }
        int64_t r = 0;
        if (thread_current()->killed) r = -N_ERR_STOPPED;
        else if (io_claim_revoked(c)) r = -N_ERR_CLOSED;
        else if (!c->vectors) r = -N_ERR_NOT_READY;
        else if (clock_now_ns() >= deadline) r = -N_ERR_TIMEOUT;
        if (r) {
            spin_unlock(&c->irq_lock, irq);
            return r;
        }
        /* handler と interrupts_off は irq_lock を取ってから c->wq を起こす */
        wq_wait_locked_until(&c->wq, &c->irq_lock, deadline);
        spin_take(&c->irq_lock); /* 期限の時に来た message も取る */
    }
}

/* ---- 資源とメモリー ---- */

int64_t io_claim_resources(struct handle *h, struct n_hal_resource *uout, size_t max) {
    struct io_claim *c = io_claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (!io_claim_enter(c)) return -N_ERR_CLOSED;
    struct io_device *d = c->d;
    uint32_t total = d->resource_count;
    size_t n = MIN(max, (size_t)total);
    int64_t r = n && copy_to_user(uout, d->resources, n * sizeof *uout) ? -N_ERR_BAD_ADDRESS : (int64_t)total;
    io_put(d);
    return r;
}

int64_t io_claim_memory(struct io_claim *c, uint32_t slot, uint64_t *address, uint64_t *size, bool *prefetchable) {
    if (!io_claim_enter(c)) return -N_ERR_CLOSED;
    struct io_device *d = c->d;
    const struct n_hal_resource *r = io_resource(d, N_HAL_MEMORY, slot);
    int64_t result = 0;
    if (!r || !r->base || !r->size) {
        result = io_resource(d, N_HAL_PORT, slot) ? -N_ERR_UNSUPPORTED : -N_ERR_NOT_FOUND;
    } else if ((r->base | r->size) & (PAGE_SIZE - 1)) { /* 頁より小さい資源は、隣にあるものと頁を分け合う */
        result = -N_ERR_UNSUPPORTED;
    } else {
        *address = r->base;
        *size = r->size;
        *prefetchable = (r->flags & N_HAL_PREFETCHABLE) != 0;
    }
    io_put(d);
    return result;
}

/* ---- バスだけの操作 ---- */

int64_t io_claim_bus_call(struct handle *h, uint32_t op, const void *in, size_t in_size, void *out, size_t out_size,
                           size_t *used) {
    struct io_claim *c = io_claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (!c->ops->call) return -N_ERR_UNSUPPORTED;
    if (!io_claim_enter(c)) return -N_ERR_CLOSED;
    struct io_device *d = c->d;
    *used = 0;
    int64_t r = c->ops->call(c, op, in, in_size, out, out_size, used);
    ASSERT(*used <= out_size);
    io_put(d);
    return r;
}

/* ---- 台帳への提供 ---- */

int64_t io_claim_offer(struct handle *h, uint32_t kind, uint32_t instance, const char *label) {
    struct io_claim *c = io_claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    struct claim_offer *o = kzalloc(sizeof *o);
    if (!o) return -N_ERR_NO_MEMORY;
    o->kind = kind;
    o->instance = instance;
    if (!io_claim_enter(c)) {
        kfree(o);
        return -N_ERR_CLOSED;
    }
    struct io_device *d = c->d;
    int64_t r = 0;
    mutex_lock(&c->offers_mutex);
    for (struct claim_offer *e = c->offers; e; e = e->next)
        if (e->kind == kind && e->instance == instance) r = -N_ERR_EXISTS;
    if (!r && io_claim_revoked(c)) r = -N_ERR_CLOSED; /* offers_end はこの錠の後に走る */
    if (!r) {
        io_offer(d, kind, instance, label);
        o->next = c->offers;
        c->offers = o;
    }
    mutex_unlock(&c->offers_mutex);
    io_put(d);
    if (r) kfree(o);
    return r;
}

int64_t io_claim_offer_withdraw(struct handle *h, uint32_t kind, uint32_t instance) {
    struct io_claim *c = io_claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (!io_claim_enter(c)) return -N_ERR_CLOSED;
    struct io_device *d = c->d;
    struct claim_offer *found = NULL;
    mutex_lock(&c->offers_mutex);
    for (struct claim_offer **p = &c->offers; *p; p = &(*p)->next)
        if ((*p)->kind == kind && (*p)->instance == instance) {
            found = *p;
            *p = found->next;
            break;
        }
    if (found) io_offer_withdraw(d, kind, instance);
    mutex_unlock(&c->offers_mutex);
    io_put(d);
    if (!found) return -N_ERR_NOT_FOUND;
    kfree(found);
    return 0;
}

/* 終わりの中で：出した提供を全部引っ込める。 */
static void offers_end(struct io_claim *c) {
    mutex_lock(&c->offers_mutex);
    for (struct claim_offer *o = c->offers, *next; o; o = next) {
        next = o->next;
        io_offer_withdraw(c->d, o->kind, o->instance);
        kfree(o);
    }
    c->offers = NULL;
    mutex_unlock(&c->offers_mutex);
}

/* ---- 眠り ---- */

int64_t io_claim_power_wait(struct handle *h, uint64_t deadline) {
    struct io_claim *c = io_claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    __atomic_store_n(&c->sleeps, true, __ATOMIC_RELEASE);
    uint64_t irq = spin_lock(&c->power_lock);
    for (;;) {
        if (c->power_event) {
            int64_t event = c->power_event;
            c->power_event = 0;
            spin_unlock(&c->power_lock, irq);
            return event;
        }
        int64_t r = 0;
        if (thread_current()->killed) r = -N_ERR_STOPPED;
        else if (io_claim_revoked(c)) r = -N_ERR_CLOSED;
        else if (clock_now_ns() >= deadline) r = -N_ERR_TIMEOUT;
        if (r) {
            spin_unlock(&c->power_lock, irq);
            return r;
        }
        /* 知らせを置く者と終わりは power_lock を取ってから power_wq を起こす */
        wq_wait_locked_until(&c->power_wq, &c->power_lock, deadline);
        spin_take(&c->power_lock);
    }
}

int64_t io_claim_power_done(struct handle *h, bool ok) {
    struct io_claim *c = io_claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    uint64_t irq = spin_lock(&c->power_lock);
    bool awaiting = c->power_awaiting;
    if (awaiting) {
        c->power_awaiting = false;
        c->power_answer = ok ? 1 : 2;
        wq_wake_all(&c->power_wq);
    }
    spin_unlock(&c->power_lock, irq);
    return awaiting ? 0 : -N_ERR_INVALID;
}

bool io_claim_suspend(struct io_claim *c) {
    if (!io_claim_sleeps(c)) return false;
    uint64_t deadline = clock_after(SLEEP_ANSWER_NS);
    uint64_t irq = spin_lock(&c->power_lock);
    c->power_event = N_HAL_POWER_SUSPEND;
    c->power_told = true;
    c->power_awaiting = true;
    c->power_answer = 0;
    wq_wake_all(&c->power_wq);
    while (!c->power_answer && !io_claim_revoked(c) && clock_now_ns() < deadline) {
        wq_wait_locked_until(&c->power_wq, &c->power_lock, deadline);
        spin_take(&c->power_lock);
    }
    bool ok = c->power_answer == 1;
    spin_unlock(&c->power_lock, irq);
    return ok;
}

void io_claim_resume(struct io_claim *c) {
    uint64_t irq = spin_lock(&c->power_lock);
    if (c->power_told) {
        c->power_told = false;
        /* SUSPEND をまだ取っていないなら、見ずに済む */
        c->power_event = c->power_event == N_HAL_POWER_SUSPEND ? 0 : N_HAL_POWER_RESUME;
        c->power_awaiting = false;
        wq_wake_all(&c->power_wq);
    }
    spin_unlock(&c->power_lock, irq);
}

/* ---- 終わり ---- */

bool io_claim_on_end(struct io_claim *c, struct io_claim_end *e) {
    uint64_t irq = spin_lock(&c->ends_lock);
    bool ok = !c->end_started;
    if (ok) {
        e->next = c->ends;
        c->ends = e;
    }
    spin_unlock(&c->ends_lock, irq);
    return ok;
}

static void claim_wake_power(struct io_claim *c) {
    uint64_t irq = spin_lock(&c->power_lock);
    wq_wake_all(&c->power_wq);
    spin_unlock(&c->power_lock, irq);
}

/* 一度だけ走る。写像を外し、割り込みを止め、バスが装置を止め、出した提供を引っ込め、claim の hook を走らせ、装置を
   実装の無い状態へ戻す。 */
static void claim_end(struct io_claim *c) {
    uint64_t f = spin_lock(&io_lock);
    __atomic_store_n(&c->released, true, __ATOMIC_SEQ_CST);
    spin_unlock(&io_lock, f);
    wq_wake_all(&c->wq);
    claim_wake_power(c);
    /* 装置に触る終わりは電源の walk と並ばない：walk の SUSPEND の待ちは上の起こしで済み、終わりは walk の後に走る */
    io_busy_begin();
    mutex_lock(&c->end_mutex);
    if (c->ended) {
        mutex_unlock(&c->end_mutex);
        io_busy_end();
        return;
    }
    c->ended = true;
    struct io_device *d = c->d;
    shared_revoke_claim(c); /* どの process も装置の頁に触れなくなる */
    /* 走っている n_hal_interrupts が立てたものも切る */
    mutex_lock(&c->vectors_mutex);
    interrupts_off(c);
    mutex_unlock(&c->vectors_mutex);
    bool safe = c->ops->end ? c->ops->end(c) : true;
    __atomic_store_n(&c->dma_unsafe, !safe, __ATOMIC_RELEASE);
    offers_end(c);
    uint64_t irq = spin_lock(&c->ends_lock);
    c->end_started = true;
    struct io_claim_end *ends = c->ends;
    c->ends = NULL;
    spin_unlock(&c->ends_lock, irq);
    for (struct io_claim_end *e = ends, *next; e; e = next) {
        next = e->next;
        e->ended(e, c);
    }
    mutex_lock(&claims_mutex);
    if (c->listed) {
        *(c->prev ? &c->prev->next : &claims) = c->next;
        if (c->next) c->next->prev = c->prev;
        c->listed = false;
    }
    mutex_unlock(&claims_mutex);
    kprintf("hal: %s released\n", d->location);
    mutex_unlock(&c->end_mutex);
    io_unbind_process(d);
    io_busy_end();
}

/* d->claim が見えている間は、object の参照が残っている。 */
void io_claim_end_device(struct io_device *d) {
    uint64_t f = spin_lock(&io_lock);
    struct io_claim *c = d->claim;
    if (c) __atomic_add_fetch(&c->refs, 1, __ATOMIC_ACQ_REL);
    spin_unlock(&io_lock, f);
    if (!c) return;
    claim_end(c);
    io_claim_drop(c);
}

static void claim_release(struct object *o) {
    struct io_claim *c = o->priv;
    claim_end(c);
    io_claim_drop(c);
}

/* d->bind を持っている：io_remove は始まりが済むまで待つ。 */
static int64_t claim_begin(struct io_device *d, struct process *t, const char *driver, struct handle **out) {
    const struct io_claim_ops *ops = d->ops ? d->ops->claim : NULL;
    if (!ops) return -N_ERR_UNSUPPORTED;
    struct io_claim *c = kzalloc(sizeof *c);
    if (!c) return -N_ERR_NO_MEMORY;
    object_init(&c->object, &claim_object_ops, c); /* 最後の handle が閉じると終わる */
    c->d = d;
    c->ops = ops;
    c->refs = 1;
    int r = io_bind_process(d, c, driver);
    if (r < 0) {
        kfree(c);
        return r;
    }
    r = !io_power_up(d) ? -N_ERR_IO : ops->begin ? ops->begin(c, &c->bus) : 0;
    if (r < 0) {
        io_unbind_process(d);
        kfree(c);
        return r;
    }
    struct handle *h = handle_open(&c->object, HANDLE_WRITE);
    if (!h) {
        claim_end(c);
        io_claim_drop(c);
        return -N_ERR_NO_MEMORY;
    }
    mutex_lock(&claims_mutex);
    c->next = claims;
    c->prev = NULL;
    if (claims) claims->prev = c;
    claims = c;
    c->listed = true;
    mutex_unlock(&claims_mutex);
    kprintf("hal: %s (%s) claimed for %s by pid %d\n", d->location, d->name, driver, t->pid);
    *out = h;
    return 0;
}

int64_t io_claim_open(uint32_t device, const char *driver, struct handle **out) {
    struct process *t = process_current();
    if (!t) return -N_ERR_DENIED;
    struct io_device *d = io_find(device);
    if (!d) return -N_ERR_NOT_FOUND;
    io_busy_begin(); /* 電源の walk の間は始めない */
    mutex_lock(&d->bind);
    int64_t r = d == &io_root ? -N_ERR_NOT_FOUND : claim_begin(d, t, driver, out);
    mutex_unlock(&d->bind);
    io_busy_end();
    io_put(d);
    return r;
}
