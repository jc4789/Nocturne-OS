/* 実装の一覧と、装置への結びつけ（io/io.h）。

   機械の装置の宣言に合う装置は、その持ち主のものとして台帳に載せ、実装を探さない。ほかは、規則に合う実装のうち、
   一番具体的な規則（項が多い）の実装から順に試す。同じなら足された順。合う実装の無い装置には何も起きないので、費用は
   結ぶ物の数だけである。結ぶ物ごとに thread を一つ起こすので、遅い装置は他を待たせない。 */
#include "kernel.h"
#include "io/internal.h"
#include "mm/heap.h"
#include "sys/thread.h"

struct driver_entry {
    const struct io_driver *driver;
    struct driver_entry *next;
    uint32_t order;
};

extern const struct io_driver *const __hal_drivers_start[], *const __hal_drivers_end[];
extern const struct io_machine *const __hal_machine_start[], *const __hal_machine_end[];

static struct driver_entry *drivers_first, **drivers_tail = &drivers_first;
static uint32_t drivers_added;
static struct io_device *pending_first, *pending_last;
static bool started;
/* 門。closing の間は新しい attach を起こさないが、進行中の attach や実装の thread の中の io_busy_begin（io_remove も
   呼ぶ）は通す。入れ子で待つと、門が自分の終わりを待って止まるからである。busy が 0 になったら shut で、全部が待つ。 */
static bool gate_closing, gate_shut;
static uint64_t busy;     /* 進行中の attach と、実装の io_busy_begin の数。io_lock の下 */
static uint64_t looking;  /* io_looking_begin の数。io_lock の下 */
static uint64_t queued;   /* 結ぶのを待つ数。io_lock の下 */
static struct wait_queue gate_wq;

static uint32_t driver_score(const struct io_driver *driver, const struct io_device *d) {
    uint32_t best = 0;
    for (uint32_t i = 0; i < driver->rule_count; i++) {
        uint32_t s = n_hal_rule_score(&driver->rules[i], d->props, d->prop_count, d->prop_bytes);
        if (s > best) best = s;
    }
    return best;
}

/* (score, order) の順で after の次の実装。score は大きい順、order は小さい順。 */
static const struct driver_entry *candidate(const struct io_device *d, uint32_t after_score, uint32_t after_order,
                                            uint32_t *out_score) {
    const struct driver_entry *best = NULL;
    uint32_t best_score = 0;
    for (const struct driver_entry *e = __atomic_load_n(&drivers_first, __ATOMIC_ACQUIRE); e;
         e = __atomic_load_n(&e->next, __ATOMIC_ACQUIRE)) {
        uint32_t s = driver_score(e->driver, d);
        if (!s) continue;
        if (after_score && (s > after_score || (s == after_score && e->order <= after_order))) continue;
        if (s > best_score || (s == best_score && best && e->order < best->order)) {
            best = e;
            best_score = s;
        }
    }
    *out_score = best_score;
    return best;
}

bool io_driver_exists(const struct n_hal_prop *props, uint32_t count, const uint8_t *bytes) {
    for (const struct driver_entry *e = __atomic_load_n(&drivers_first, __ATOMIC_ACQUIRE); e;
         e = __atomic_load_n(&e->next, __ATOMIC_ACQUIRE))
        for (uint32_t i = 0; i < e->driver->rule_count; i++)
            if (n_hal_rule_score(&e->driver->rules[i], props, count, bytes)) return true;
    return false;
}

static void busy_done(void) {
    uint64_t f = spin_lock(&io_lock);
    busy--;
    spin_unlock(&io_lock, f);
    wq_wake_all(&gate_wq);
}

static void set_attaching(struct io_device *d, const struct io_driver *driver) {
    uint64_t f = spin_lock(&io_lock);
    d->driver = driver;
    d->state = N_HAL_ATTACHING;
    d->reason = 0;
    strlcpy(d->driver_name, driver->name, sizeof d->driver_name);
    spin_unlock(&io_lock, f);
    io_ledger_changed();
}

/* 実装の thread が attach の戻りより先に io_failed を書くことがあるので、その記録は上書きしない。 */
static void set_attached(struct io_device *d) {
    uint64_t f = spin_lock(&io_lock);
    if (d->state == N_HAL_ATTACHING) d->state = N_HAL_RUNNING;
    spin_unlock(&io_lock, f);
    io_ledger_changed();
}

static void set_detached(struct io_device *d, int reason) {
    uint64_t f = spin_lock(&io_lock);
    const struct io_driver *driver = d->driver;
    d->driver = NULL;
    d->state = reason ? N_HAL_FAILED : N_HAL_UNBOUND;
    d->reason = reason;
    if (!reason) d->driver_name[0] = 0;
    else if (driver) strlcpy(d->driver_name, driver->name, sizeof d->driver_name);
    spin_unlock(&io_lock, f);
    io_ledger_changed();
}

static void bind_thread(void *arg) {
    struct io_device *d = arg;
    mutex_lock(&d->bind);
    uint32_t score = 0, order = 0;
    for (;;) {
        if (io_gone(d) || d->driver || d->claim) break;
        uint32_t next_score;
        const struct driver_entry *e = candidate(d, score, order, &next_score);
        if (!e) break;
        score = next_score;
        order = e->order;
        set_attaching(d, e->driver);
        int r = io_power_up(d) ? e->driver->attach(d) : -N_ERR_IO;
        if (!r) {
            set_attached(d);
            break;
        }
        const struct io_driver *failed = d->driver;
        io_release(d);
        if (r < 0) {
            kprintf("hal: %s at %s: %d\n", failed->name, d->location, r);
            set_detached(d, r);
            break;
        }
        set_detached(d, 0);
    }
    mutex_unlock(&d->bind);
    io_put(d);
    busy_done();
}

/* d が機械の装置なら、持ち主のものとして台帳に載せて true。 */
static bool machine_take(struct io_device *d) {
    for (const struct io_machine *const *p = __hal_machine_start; p < __hal_machine_end; p++) {
        const struct io_machine *m = *p;
        bool matches = false;
        for (uint32_t i = 0; i < m->rule_count && !matches; i++)
            matches = n_hal_rule_score(&m->rules[i], d->props, d->prop_count, d->prop_bytes) != 0;
        if (!matches || (m->mine && !m->mine(d))) continue;
        uint64_t f = spin_lock(&io_lock);
        d->machine = true;
        d->state = N_HAL_RUNNING;
        d->reason = 0;
        strlcpy(d->driver_name, m->owner, sizeof d->driver_name);
        spin_unlock(&io_lock, f);
        io_ledger_changed();
        return true;
    }
    return false;
}

void pending_forget(struct io_device *d) {
    if (!d->queued) return;
    struct io_device *before = NULL;
    for (struct io_device *o = pending_first; o != d; o = o->pending_next) before = o;
    *(before ? &before->pending_next : &pending_first) = d->pending_next;
    if (pending_last == d) pending_last = before;
    d->queued = false;
    queued--;
}

/* 機械の装置でなく、合う実装があれば thread を起こす。 */
static void bind_start(struct io_device *d) {
    uint32_t score;
    if (d->machine || machine_take(d) || !candidate(d, 0, 0, &score)) return;
    uint64_t f = spin_lock(&io_lock);
    if (gate_closing) {
        if (!d->queued) {
            d->queued = true;
            d->pending_next = NULL;
            *(pending_last ? &pending_last->pending_next : &pending_first) = d;
            pending_last = d;
            queued++;
        }
        spin_unlock(&io_lock, f);
        return;
    }
    if (d->gone) {
        spin_unlock(&io_lock, f);
        return;
    }
    busy++;
    d->holds++;
    spin_unlock(&io_lock, f);
    if (!kthread_create("attach", bind_thread, d)) {
        kprintf("hal: no thread to attach %s\n", d->location);
        io_failed(d, -N_ERR_NO_MEMORY);
        io_put(d);
        busy_done();
    }
}

void io_bind_request(struct io_device *d) {
    uint64_t f = spin_lock(&io_lock);
    if (!started) {
        if (!d->queued) {
            d->queued = true;
            d->pending_next = NULL;
            *(pending_last ? &pending_last->pending_next : &pending_first) = d;
            pending_last = d;
            queued++;
        }
        spin_unlock(&io_lock, f);
        return;
    }
    spin_unlock(&io_lock, f);
    bind_start(d);
}

static void pending_drain(void) {
    for (;;) {
        uint64_t f = spin_lock(&io_lock);
        struct io_device *d = gate_closing ? NULL : pending_first;
        if (d) {
            pending_first = d->pending_next;
            if (!pending_first) pending_last = NULL;
            d->queued = false;
            queued--;
        }
        spin_unlock(&io_lock, f);
        if (!d) return;
        bind_start(d);
    }
}

void io_driver_add(const struct io_driver *driver) {
    struct driver_entry *e = kmalloc(sizeof *e);
    if (!e) {
        kprintf("hal: no memory to know the driver %s\n", driver->name);
        return;
    }
    e->driver = driver;
    e->next = NULL;
    uint64_t f = spin_lock(&io_lock);
    e->order = drivers_added++;
    __atomic_store_n(drivers_tail, e, __ATOMIC_RELEASE);
    drivers_tail = &e->next;
    /* 持つ実装の無い装置に、この実装が合うかもしれない */
    bool now = started;
    if (now)
        for (struct io_device *d = io_root.first_child; d; d = io_walk_next(d))
            if (d->state == N_HAL_UNBOUND && !d->queued && !d->claim) {
                d->queued = true;
                d->pending_next = NULL;
                *(pending_last ? &pending_last->pending_next : &pending_first) = d;
                pending_last = d;
                queued++;
            }
    spin_unlock(&io_lock, f);
    if (now) pending_drain();
}

void io_start(void) {
    for (const struct io_driver *const *p = __hal_drivers_start; p < __hal_drivers_end; p++) io_driver_add(*p);
    uint64_t f = spin_lock(&io_lock);
    started = true;
    spin_unlock(&io_lock, f);
    pending_drain();
}

bool io_settled(void) {
    uint64_t f = spin_lock(&io_lock);
    bool settled = started && !busy && !looking && !queued;
    spin_unlock(&io_lock, f);
    return settled;
}

void io_looking_begin(void) {
    uint64_t f = spin_lock(&io_lock);
    looking++;
    spin_unlock(&io_lock, f);
}

void io_looking_end(void) {
    uint64_t f = spin_lock(&io_lock);
    looking--;
    spin_unlock(&io_lock, f);
}

void io_busy_begin(void) {
    uint64_t f = spin_lock(&io_lock);
    while (gate_shut) {
        wq_wait_locked(&gate_wq, &io_lock);
        spin_take(&io_lock);
    }
    busy++;
    spin_unlock(&io_lock, f);
}

void io_busy_end(void) { busy_done(); }

/* 門を閉じ、進行中のものの終わりを待つ。 */
void io_gate_close(void) {
    uint64_t f = spin_lock(&io_lock);
    gate_closing = true;
    while (busy) {
        wq_wait_locked(&gate_wq, &io_lock);
        spin_take(&io_lock);
    }
    gate_shut = true;
    spin_unlock(&io_lock, f);
}

void io_gate_open(void) {
    uint64_t f = spin_lock(&io_lock);
    gate_closing = gate_shut = false;
    spin_unlock(&io_lock, f);
    wq_wake_all(&gate_wq);
    pending_drain();
}

int io_bind_process(struct io_device *d, struct io_claim *claim, const char *program) {
    int r = 0;
    uint64_t f = spin_lock(&io_lock);
    if (d->driver || d->claim || d->gone || d->machine) {
        r = -N_ERR_BUSY;
    } else {
        d->claim = claim;
        d->holds++;
        d->state = N_HAL_RUNNING;
        d->reason = 0;
        strlcpy(d->driver_name, program, sizeof d->driver_name);
    }
    spin_unlock(&io_lock, f);
    if (!r) io_ledger_changed();
    return r;
}

void io_unbind_process(struct io_device *d) {
    uint64_t f = spin_lock(&io_lock);
    d->claim = NULL;
    d->state = N_HAL_UNBOUND;
    d->reason = 0;
    d->driver_name[0] = 0;
    spin_unlock(&io_lock, f);
    io_ledger_changed();
    io_put(d);
}
