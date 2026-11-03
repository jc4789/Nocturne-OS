/* 台帳（dev/hal.h）。錠は葉で、下で何も取らない。 */
#include "kernel.h"
#include "arch/spin.h"
#include "dev/hal.h"
#include "mm/heap.h"
#include "sys/syscall.h"
#include "sys/watch.h"

static struct spinlock hal_lock;
static struct hal_device *first, *last;
static uint32_t ids;
static size_t listed;
static struct watched changed;

void hal_init(void) { watched_init(&changed, NULL); }

void hal_add(struct hal_device *d, uint32_t parent, enum n_hal_bus bus, int node, const char *name) {
    uint64_t f = spin_lock(&hal_lock);
    d->info.id = ++ids;
    d->info.parent = parent;
    d->info.bus = bus;
    d->info.state = N_HAL_UNBOUND;
    d->info.node = node;
    strlcpy(d->info.name, name ? name : "", sizeof d->info.name);
    d->next = NULL;
    d->prev = last;
    *(last ? &last->next : &first) = d;
    last = d;
    d->listed = true;
    listed++;
    spin_unlock(&hal_lock, f);
    watched_changed(&changed);
}

void hal_name(struct hal_device *d, const char *name) {
    uint64_t f = spin_lock(&hal_lock);
    strlcpy(d->info.name, name, sizeof d->info.name);
    spin_unlock(&hal_lock, f);
    watched_changed(&changed);
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
    watched_changed(&changed);
}

void hal_unbound(struct hal_device *d) {
    uint64_t f = spin_lock(&hal_lock);
    d->info.state = N_HAL_UNBOUND;
    d->info.reason = 0;
    d->info.driver[0] = 0;
    spin_unlock(&hal_lock, f);
    watched_changed(&changed);
}

void hal_failed(struct hal_device *d, const char *driver, int reason) {
    uint64_t f = spin_lock(&hal_lock);
    d->info.state = N_HAL_FAILED;
    d->info.reason = reason;
    strlcpy(d->info.driver, driver, sizeof d->info.driver);
    spin_unlock(&hal_lock, f);
    watched_changed(&changed);
}

void hal_remove(struct hal_device *d) {
    uint64_t f = spin_lock(&hal_lock);
    bool was = d->listed;
    if (was) {
        *(d->prev ? &d->prev->next : &first) = d->next;
        *(d->next ? &d->next->prev : &last) = d->prev;
        d->listed = false;
        listed--;
    }
    spin_unlock(&hal_lock, f);
    if (was) watched_changed(&changed);
}

int64_t hal_syscall_list(struct n_hal_device *user_out, uint64_t max) {
    size_t room = (size_t)MIN(max, (uint64_t)__atomic_load_n(&listed, __ATOMIC_RELAXED));
    struct n_hal_device *copy = room ? kmalloc(room * sizeof *copy) : NULL;
    if (room && !copy) return -N_ERR_NO_MEMORY;
    size_t n = 0;
    uint64_t f = spin_lock(&hal_lock);
    size_t total = listed;
    for (struct hal_device *d = first; d && n < room; d = d->next) copy[n++] = d->info;
    spin_unlock(&hal_lock, f);
    int64_t r = n && copy_to_user(user_out, copy, n * sizeof *copy) ? -N_ERR_BAD_ADDRESS : (int64_t)total;
    kfree(copy);
    return r;
}

int64_t hal_syscall_watch(void) { return watched_open(&changed); }
