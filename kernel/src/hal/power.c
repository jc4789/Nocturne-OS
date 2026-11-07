/* 電源の walk（hal/hal.h）。眠り、休止、電源断の装置の段を持つ。機械の段取り（_TTS、_PTS、CPU、写し）は dev/pm.c と
   dev/hibernate.c の持ち物。

   段。hal_power_begin は門を閉じた後、動いている装置（カーネルの実装か claim を持つ装置。機械の装置は除く）を鎖
   （power_next）につなぎ、段を数える。段は木の親と依存先（hal_depends）を辿り、途中の動いている装置ごとに一つ深く
   なる。同じ段の動いている装置は互いの上にも依存先にも無いので、同時に止めて戻してよい。依存の輪は power_seen で
   切る（輪の中の段は決まらないが、walk は終わる）。再帰の深さは親と依存の連なりの長さである。
   門を閉じている間、動いている装置の集まりは変わらない（attach、claim の始まりと終わり、hal_remove は門を通る）。
   鎖は錠なしで辿る。木に新しく載る装置は動いていないので、鎖に入らない。

   段の中の装置は仕事の列で、walk の thread と、装置の数だけ起こす助けの thread が列から一つずつ取る。thread を作れ
   なければ並びが減るだけで、全部の装置は walk の thread が取り終える。walk の thread は段の全部が済んでから次の段へ
   進む（NVMe の shutdown のような秒の単位の手順を、同じ段で足し算にしない）。
   止める：実装の suspend か claim の SUSPEND。HAL_POWER_SLEEP なら、続けてバスの D3hot、firmware の D3（電源が切れた
   なら、バスへ D3cold を知らせる）。
   戻す：D3 にした装置は、機械が眠ったならバスへ D3cold を知らせ、firmware の D0、バスの D0（hal_power_up）。続けて実装の
   resume か claim の RESUME。 */
#include "kernel.h"
#include "hal/internal.h"
#include "sys/thread.h"
#include "sys/wait.h"

static const struct hal_firmware_ops *firmware;
static struct hal_device *running_first;
static uint32_t level_top;

/* 段の仕事の列と、その結果。walk の thread と助けの thread の間。葉 */
static struct spinlock walk_lock;
static struct wait_queue walk_wq;
static struct hal_device *queue_next; /* 鎖の中の、まだ見ていない最初 */
static uint32_t queue_level;
static bool queue_resume;
static unsigned helpers;          /* 走っている助けの thread */
static const char *walk_failed;   /* 最初に止まらなかったか、戻らなかった装置の名 */
static enum hal_power_target walk_target;
static bool walk_slept;

void hal_firmware_set(const struct hal_firmware_ops *ops) { firmware = ops; }

static bool running(const struct hal_device *d) { return !d->machine && (d->driver || d->claim); }

static uint32_t level_of(struct hal_device *d);
static uint32_t level_below(struct hal_device *above) { return level_of(above) + (running(above) ? 1 : 0); }

static uint32_t level_of(struct hal_device *d) {
    if (d->power_seen) return d->power_level;
    d->power_seen = true;
    d->power_level = 0;
    uint32_t level = d->parent ? level_below(d->parent) : 0;
    for (struct hal_depend_entry *e = d->depends; e; e = e->next) level = MAX(level, level_below(e->on));
    d->power_level = level;
    return level;
}

void hal_power_begin(void) {
    hal_gate_close();
    uint64_t f = spin_lock(&hal_lock);
    for (struct hal_device *d = &hal_root; d; d = hal_walk_next(d)) {
        d->power_seen = d->power_suspended = d->power_lowered = false;
        d->power_next = NULL;
    }
    running_first = NULL;
    level_top = 0;
    struct hal_device **tail = &running_first;
    for (struct hal_device *d = &hal_root; d; d = hal_walk_next(d)) {
        uint32_t level = level_of(d);
        if (!running(d)) continue;
        *tail = d;
        tail = &d->power_next;
        level_top = MAX(level_top, level);
    }
    spin_unlock(&hal_lock, f);
}

void hal_power_end(void) { hal_gate_open(); }

static bool cannot(const struct hal_device *d, bool hibernate) {
    if (!d->driver || d->machine) return false;
    switch (d->driver->after_sleep) {
    case HAL_AFTER_SLEEP_LOST: return true;
    case HAL_AFTER_SLEEP_BOOT: return !hibernate;
    case HAL_AFTER_SLEEP_RESUME: return false;
    }
    return true;
}

const char *hal_power_refused(bool hibernate, enum hal_refusal *why) {
    *why = HAL_REFUSAL_NONE;
    for (struct hal_device *d = running_first; d; d = d->power_next) {
        if (d->claim) {
            if (hal_claim_sleeps(d->claim)) continue;
            *why = HAL_REFUSAL_PROCESS;
            return d->driver_name;
        }
        if (cannot(d, hibernate)) {
            *why = HAL_REFUSAL_CANNOT;
            return d->driver_name;
        }
        const char *busy = d->driver->refuse ? d->driver->refuse(d) : NULL;
        if (!busy) continue;
        kprintf("pm: %s at %s: %s\n", d->driver_name, d->location, busy);
        *why = HAL_REFUSAL_BUSY;
        return d->driver_name;
    }
    return NULL;
}

const char *hal_power_cannot(bool hibernate) {
    const char *name = NULL;
    uint64_t f = spin_lock(&hal_lock);
    for (struct hal_device *d = &hal_root; d && !name; d = hal_walk_next(d))
        if (cannot(d, hibernate)) name = d->driver->name;
    spin_unlock(&hal_lock, f);
    return name;
}

bool hal_power_up(struct hal_device *d) {
    bool cold = false;
    if (d->firmware && firmware) firmware->power(d->firmware, HAL_D0, &cold);
    if (!d->ops || !d->ops->power) return true;
    if (cold) d->ops->power(d, HAL_D3COLD);
    if (d->ops->power(d, HAL_D0)) return true;
    kprintf("hal: %s does not answer in D0\n", d->location);
    return false;
}

/* 止めて、眠りなら D3 にする。止まったら true（D3 にならなくても、firmware の失敗は持ち主が書いて進む）。 */
static bool device_suspend(struct hal_device *d, enum hal_power_target target) {
    /* 止められなかった実装も道を半ば閉じている：suspend を呼んだ装置は全部 resume で戻す */
    d->power_suspended = true;
    bool stopped = d->claim ? hal_claim_suspend(d->claim) : !d->driver->suspend || d->driver->suspend(d, target);
    if (!stopped) {
        kprintf("pm: %s at %s did not stop\n", d->driver_name, d->location);
        return false;
    }
    if (target != HAL_POWER_SLEEP) return true;
    bool bus = d->ops && d->ops->power, cold = false;
    if (bus && !d->ops->power(d, HAL_D3HOT)) {
        kprintf("pm: %s at %s did not go to D3\n", d->driver_name, d->location);
        return false;
    }
    if (d->firmware && firmware) firmware->power(d->firmware, HAL_D3COLD, &cold);
    if (bus && cold) d->ops->power(d, HAL_D3COLD);
    d->power_lowered = bus || cold;
    return true;
}

/* D3 にしたなら D0 にし、実装に戻させる。装置が D0 で答えなくても実装の resume は呼ぶ：実装が自分で失敗を言う。 */
static bool device_resume(struct hal_device *d) {
    bool up = true;
    if (d->power_lowered) {
        d->power_lowered = false;
        if (walk_slept && d->ops && d->ops->power) d->ops->power(d, HAL_D3COLD);
        up = hal_power_up(d);
    }
    d->power_suspended = false;
    bool back = up;
    if (d->claim) hal_claim_resume(d->claim);
    else if (d->driver->resume && !d->driver->resume(d)) back = false;
    if (!back) kprintf("pm: %s at %s did not come back\n", d->driver_name, d->location);
    return back;
}

/* walk_lock の下。 */
static struct hal_device *job_take(void) {
    for (struct hal_device *d = queue_next; d; d = d->power_next) {
        if (d->power_level != queue_level || (queue_resume && !d->power_suspended)) continue;
        queue_next = d->power_next;
        return d;
    }
    queue_next = NULL;
    return NULL;
}

static void jobs_run(void) {
    for (;;) {
        uint64_t f = spin_lock(&walk_lock);
        struct hal_device *d = job_take();
        spin_unlock(&walk_lock, f);
        if (!d) return;
        bool ok = queue_resume ? device_resume(d) : device_suspend(d, walk_target);
        if (ok) continue;
        f = spin_lock(&walk_lock);
        if (!walk_failed) walk_failed = d->driver_name;
        spin_unlock(&walk_lock, f);
    }
}

static void helper_thread(void *arg) {
    (void)arg;
    jobs_run();
    uint64_t f = spin_lock(&walk_lock);
    helpers--;
    spin_unlock(&walk_lock, f);
    wq_wake_all(&walk_wq);
}

static void level_run(uint32_t level, bool resume) {
    unsigned count = 0;
    for (struct hal_device *d = running_first; d; d = d->power_next)
        if (d->power_level == level && (!resume || d->power_suspended)) count++;
    if (!count) return;
    uint64_t f = spin_lock(&walk_lock);
    queue_next = running_first;
    queue_level = level;
    queue_resume = resume;
    spin_unlock(&walk_lock, f);
    for (unsigned i = 1; i < count; i++) {
        f = spin_lock(&walk_lock);
        helpers++;
        spin_unlock(&walk_lock, f);
        if (kthread_create("power", helper_thread, NULL)) continue;
        f = spin_lock(&walk_lock);
        helpers--;
        spin_unlock(&walk_lock, f);
        break;
    }
    jobs_run();
    f = spin_lock(&walk_lock);
    while (helpers) {
        wq_wait_locked(&walk_wq, &walk_lock);
        spin_take(&walk_lock);
    }
    spin_unlock(&walk_lock, f);
}

const char *hal_power_suspend(enum hal_power_target target) {
    walk_target = target;
    walk_failed = NULL;
    for (uint32_t level = level_top + 1; level-- > 0;) {
        level_run(level, false);
        if (walk_failed && target != HAL_POWER_OFF) break;
    }
    return walk_failed;
}

const char *hal_power_resume(bool slept) {
    walk_slept = slept;
    walk_failed = NULL;
    for (uint32_t level = 0; level <= level_top; level++) level_run(level, true);
    return walk_failed;
}
