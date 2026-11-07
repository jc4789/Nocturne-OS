/* 装置の木を止める順に辿る（hal/hal.h）。眠りの段取りは dev/pm.c の持ち物で、ここは装置の順だけを持つ。

   列は記録の中の鎖で作るので記憶を確保しない。門を閉じている間、木は変わらないので、鎖は割り込みを切っても他の CPU を
   止めても錠なしで辿れる。依存の輪は power_seen で切る。再帰の深さは親と依存の連なりの長さである。 */
#include "kernel.h"
#include "hal/internal.h"

static struct hal_device *order_first, *order_last;

static void order_clear(struct hal_device *d) {
    for (; d; d = hal_walk_next(d)) {
        d->power_seen = false;
        d->power_next = d->power_prev = NULL;
    }
}

static void order_append(struct hal_device *d) {
    d->power_prev = order_last;
    *(order_last ? &order_last->power_next : &order_first) = d;
    order_last = d;
}

/* 列は戻す順：d の前に親と依存先が並ぶ。止めるのは逆から。 */
static void order_add(struct hal_device *d) {
    if (d->power_seen) return;
    d->power_seen = true;
    if (d->parent) order_add(d->parent);
    for (struct hal_depend_entry *e = d->depends; e; e = e->next) order_add(e->on);
    order_append(d);
}

void hal_power_begin(void) {
    hal_gate_close();
    uint64_t f = spin_lock(&hal_lock);
    order_first = order_last = NULL;
    order_clear(&hal_root);
    for (struct hal_device *d = &hal_root; d; d = hal_walk_next(d)) order_add(d);
    spin_unlock(&hal_lock, f);
}

void hal_power_end(void) { hal_gate_open(); }

const char *hal_power_refused(bool hibernate, enum hal_refusal *why) {
    *why = HAL_REFUSAL_NONE;
    for (struct hal_device *d = order_first; d; d = d->power_next) {
        if (d->claim && !hal_claim_sleeps(d->claim)) {
            *why = HAL_REFUSAL_PROCESS;
            return d->driver_name;
        }
        const char *busy = d->driver && d->driver->refuse ? d->driver->refuse(d) : NULL;
        if (!busy) continue;
        kprintf("pm: %s at %s: %s\n", d->driver->name, d->location, busy);
        *why = HAL_REFUSAL_BUSY;
        return d->driver->name;
    }
    return NULL;
}
