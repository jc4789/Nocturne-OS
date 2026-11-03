/* 台帳（dev/hal.h）。錠は葉で、下で何も取らない。 */
#include "kernel.h"
#include "arch/spin.h"
#include "dev/hal.h"
#include "mm/heap.h"
#include "sys/syscall.h"
#include "sys/watch.h"

static struct spinlock hal_lock;
static struct hal_device *hal_first, *hal_last;
static uint32_t hal_ids;
static size_t hal_count;
static struct watched hal_changed;

void hal_init(void) { watched_init(&hal_changed, NULL); }

void hal_add(struct hal_device *d, uint32_t parent, enum n_hal_bus bus) {
    uint64_t f = spin_lock(&hal_lock);
    d->info.id = ++hal_ids;
    d->info.parent = parent;
    d->info.bus = bus;
    d->info.state = N_HAL_UNBOUND;
    d->info.name[0] = 0;
    d->info.driver[0] = 0;
    d->next = NULL;
    d->prev = hal_last;
    *(hal_last ? &hal_last->next : &hal_first) = d;
    hal_last = d;
    d->listed = true;
    hal_count++;
    spin_unlock(&hal_lock, f);
    watched_changed(&hal_changed);
}

/* 入るだけ写す。UTF-8 の字の途中では切らない。 */
void hal_name(struct hal_device *d, const char *name) {
    uint64_t f = spin_lock(&hal_lock);
    size_t cap = sizeof d->info.name;
    if (strlcpy(d->info.name, name, cap) >= cap) {
        size_t end = cap - 1;
        while (end && ((uint8_t)name[end] & 0xC0) == 0x80) end--;
        d->info.name[end] = 0;
    }
    spin_unlock(&hal_lock, f);
    watched_changed(&hal_changed);
}

void hal_bound(struct hal_device *d, const char *driver) {
    uint64_t f = spin_lock(&hal_lock);
    if (d->info.state == N_HAL_RUNNING && d->info.driver[0]) {
        strlcat(d->info.driver, ", ", sizeof d->info.driver);
        strlcat(d->info.driver, driver, sizeof d->info.driver);
    } else {
        strlcpy(d->info.driver, driver, sizeof d->info.driver);
    }
    d->info.state = N_HAL_RUNNING;
    d->info.reason = 0;
    spin_unlock(&hal_lock, f);
    watched_changed(&hal_changed);
}

void hal_unbound(struct hal_device *d) {
    uint64_t f = spin_lock(&hal_lock);
    d->info.state = N_HAL_UNBOUND;
    d->info.reason = 0;
    d->info.driver[0] = 0;
    spin_unlock(&hal_lock, f);
    watched_changed(&hal_changed);
}

void hal_failed(struct hal_device *d, const char *driver, int reason) {
    uint64_t f = spin_lock(&hal_lock);
    d->info.state = N_HAL_FAILED;
    d->info.reason = reason;
    strlcpy(d->info.driver, driver, sizeof d->info.driver);
    spin_unlock(&hal_lock, f);
    watched_changed(&hal_changed);
}

void hal_restarted(struct hal_device *d, const char *driver, bool up) {
    uint64_t f = spin_lock(&hal_lock);
    bool was_failed = d->info.state == N_HAL_FAILED;
    if (was_failed == up) {
        d->info.state = up ? N_HAL_RUNNING : N_HAL_FAILED;
        d->info.reason = up ? 0 : -N_ERR_IO;
        strlcpy(d->info.driver, driver, sizeof d->info.driver);
    }
    spin_unlock(&hal_lock, f);
    if (was_failed == up) watched_changed(&hal_changed);
}

void hal_remove(struct hal_device *d) {
    uint64_t f = spin_lock(&hal_lock);
    bool was = d->listed;
    if (was) {
        *(d->prev ? &d->prev->next : &hal_first) = d->next;
        *(d->next ? &d->next->prev : &hal_last) = d->prev;
        d->listed = false;
        hal_count--;
    }
    spin_unlock(&hal_lock, f);
    if (was) watched_changed(&hal_changed);
}

/* プログラムへの写しは眠るので、錠の下で核の記憶へ写す。錠を放して確保する間に台帳が増えたら、取り直す。だから
   書いた数はいつも min(max, 戻り) である。 */
int64_t hal_syscall_list(struct n_hal_device *user_out, uint64_t max) {
    struct n_hal_device *copy = NULL;
    size_t room = 0, want, total;
    for (;;) {
        uint64_t f = spin_lock(&hal_lock);
        total = hal_count;
        want = (size_t)MIN(max, (uint64_t)total);
        if (want <= room) {
            size_t n = 0;
            for (struct hal_device *d = hal_first; n < want; d = d->next) copy[n++] = d->info;
            spin_unlock(&hal_lock, f);
            break;
        }
        spin_unlock(&hal_lock, f);
        kfree(copy);
        room = want;
        copy = kzalloc(room * sizeof *copy);
        if (!copy) return -N_ERR_NO_MEMORY;
    }
    int64_t r = want && copy_to_user(user_out, copy, want * sizeof *copy) ? -N_ERR_BAD_ADDRESS : (int64_t)total;
    kfree(copy);
    return r;
}

int64_t hal_syscall_watch(void) { return watched_open(&hal_changed); }
