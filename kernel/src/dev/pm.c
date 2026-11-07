/* 電源の段取り（dev/pm.h）。一つの thread が頼まれた電源の変化を全部受け、決まった順で行う。何が起きたかを
   言い、それが何を意味するかはサービス machine が決める。 */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/apic.h"
#include "arch/spin.h"
#include "dev/pm.h"
#include "io/io.h"
#include "sys/syscall.h"
#include "sys/watch.h"
#include "dev/acpi.h"
#include "dev/blk.h"
#include "dev/fb.h"
#include "dev/input.h"
#include "fs/store.h"
#include "dev/hibernate.h"
#include "dev/iommu.h"
#include "sys/process.h"
#include "sys/thread.h"
#include "sys/wait.h"
#include "arch/ioapic.h"
#include "arch/smp.h"
#include "arch/wake.h"
#include "dev/pci.h"
#include "sys/alarm.h"
#include "sys/clock.h"
#include "dev/serial.h"
#include "sys/log.h"
#include "vm/vm.h"

void power_off(void);
void power_reboot(void);

#define STOP_NS (3 * NS_PER_S) /* 止めたアプリが終わるまでの時間 */

static struct wait_queue pm_wq;
/* 頼みと電源の thread の進み（requested、busy、stopping、wanted）、言った状態、起きたこと。どの CPU の task からも、
   電源ボタンの handler からも取る。単独で取り、下では待ち行列の錠だけを取る。 */
static struct spinlock pm_lock;
static bool requested, busy, stopping, napping;
static enum pm_action wanted;

void pm_request(enum pm_action action) {
    uint64_t f = spin_lock(&pm_lock);
    /* 眠りは、もう待っている電源断を置き換えない */
    bool nap = action == PM_SLEEP || action == PM_HIBERNATE;
    if (!stopping && !(nap && (busy || requested))) {
        wanted = action;
        requested = true;
        wq_wake_all(&pm_wq);
    }
    spin_unlock(&pm_lock, f);
}

/* ---- 起きたこと ----
   pm_lock の下で数える。サービス machine が読んで決める。 */
static struct n_power_events happened;
static struct watched happened_watched;

void pm_button_pressed(void) {
    uint64_t f = spin_lock(&pm_lock);
    /* 機械を起こした押下は、それ以上を言わない */
    bool counts = !napping;
    if (counts) happened.button_presses++;
    spin_unlock(&pm_lock, f);
    if (counts) watched_changed(&happened_watched);
}

void pm_lid_is(bool closed) {
    uint64_t f = spin_lock(&pm_lock);
    bool changed = happened.lid_present && (happened.lid_closed != 0) != closed;
    happened.lid_present = 1;
    happened.lid_closed = closed;
    if (changed) happened.lid_changes++;
    spin_unlock(&pm_lock, f);
    if (changed) watched_changed(&happened_watched);
}

void pm_host_asks(enum pm_action action) {
    uint64_t f = spin_lock(&pm_lock);
    happened.host_asks++;
    happened.host_action = action;
    spin_unlock(&pm_lock, f);
    watched_changed(&happened_watched);
}

static void woke(void) {
    uint64_t f = spin_lock(&pm_lock);
    happened.wakes++;
    happened.last_wake_ns = clock_now_ns();
    spin_unlock(&pm_lock, f);
    watched_changed(&happened_watched);
}

int64_t pm_events_watch_syscall(void) { return watched_open(&happened_watched); }

int64_t pm_events_syscall(void *out, size_t size) {
    if (size != sizeof happened) return -N_ERR_INVALID;
    struct n_power_events now;
    uint64_t f = spin_lock(&pm_lock);
    now = happened;
    spin_unlock(&pm_lock, f);
    now.last_input_ns = input_last_ns();
    now.can_sleep = pm_can_sleep();
    now.can_hibernate = pm_can_hibernate();
    return copy_to_user(out, &now, sizeof now);
}

/* ---- 言う状態 ----
   watch が知らせる状態。pm_lock の下。 */
static struct n_power_state told;
static struct watched told_watched;

static void say(enum n_power_stage stage, enum pm_action action, enum n_power_reason reason, const char *detail,
                bool forceable) {
    uint64_t f = spin_lock(&pm_lock);
    told.serial++;
    told.stage = stage;
    told.action = action;
    told.reason = reason;
    told.forceable = forceable;
    strlcpy(told.detail, detail ? detail : "", sizeof told.detail);
    spin_unlock(&pm_lock, f);
    watched_changed(&told_watched);
}

int64_t pm_watch_syscall(void) { return watched_open(&told_watched); }

int64_t pm_state_syscall(void *out, size_t size) {
    if (size != sizeof told) return -N_ERR_INVALID;
    struct n_power_state now;
    uint64_t f = spin_lock(&pm_lock);
    now = told;
    spin_unlock(&pm_lock, f);
    now.can_sleep = pm_can_sleep();
    now.can_hibernate = pm_can_hibernate();
    return copy_to_user(out, &now, sizeof now);
}

/* SYS_POWER_SAY：サービス machine が率いる段。利用者に言う者のため。 */
int64_t pm_say_syscall(const void *state, size_t size) {
    struct n_power_state s;
    if (size != sizeof s) return -N_ERR_INVALID;
    if (copy_from_user(&s, state, sizeof s)) return -N_ERR_BAD_ADDRESS;
    if ((s.stage != N_POWER_CLOSING && s.stage != N_POWER_CANCELLED && s.stage != N_POWER_STOPPING) || s.action > POWER_HIBERNATE)
        return -N_ERR_INVALID;
    s.detail[sizeof s.detail - 1] = 0;
    say(s.stage, (enum pm_action)s.action, N_POWER_REASON_NONE, s.detail, s.forceable);
    return 0;
}

static void stop_apps(void) {
    uint64_t end = clock_after(STOP_NS);
    int left;
    while ((left = process_stop_all()) && clock_now_ns() < end) sleep_until(clock_after(20 * NS_PER_MS));
    if (left) kprintf("pm: %d app tasks did not end\n", left);
}

static bool flush_disks(void) {
    bool ok = true;
    for (int i = 0; i < blk_count(); i++) {
        struct blkdev *d = blk_get(i);
        if (__atomic_load_n(&d->gone, __ATOMIC_ACQUIRE)) continue; /* 持っていたものは一緒に去った */
        /* blk がログに書く。眠りの前にディスクの cache が失ったもの（blk_cache_lost）は、眠りが悪くしない */
        if (blk_persist(d) < 0 && !__atomic_load_n(&d->cache_lost, __ATOMIC_ACQUIRE)) ok = false;
    }
    return ok;
}

/* 電源断と再起動（dev/pm.h）。ボリュームの保存は先にディスクにある（pm_thread、休止の写しの書き終わり）。装置は
   IO_POWER_OFF の walk で止まり（止まらない装置があっても進む）、その後に電源が切れる。 */
static NORETURN void finish(enum pm_action action) {
    io_power_begin();
    if (action == PM_POWER_OFF) {
        acpi_sleep_transition(5); /* _TTS(5)：装置に知らせる前（ACPI 6.5 7.4.4） */
        acpi_sleep_prepare(5);    /* _PTS(5)：先に（16.1.7 の 1） */
    }
    blk_hold();
    flush_disks(); /* 失敗したディスクはログに書かれる：電源は同じく切れる */
    kprintf("pm: stopping the devices\n");
    const char *stuck = io_power_suspend(IO_POWER_OFF);
    if (stuck) kprintf("pm: %s did not stop; the power goes off all the same\n", stuck);
    cli();
    serial_flush();
    if (action == PM_REBOOT) power_reboot();
    power_off();
    for (;;) hlt();
}

/* ---- 眠り（S3）と休止 ---- */
/* ログのため。 */
static const char *reason_text(enum n_power_reason reason) {
    switch (reason) {
    case N_POWER_REASON_NONE: return "none";
    case N_POWER_REASON_IOMMU: return "the IOMMU's state is unconfirmed, or it did not come back";
    case N_POWER_REASON_NO_S3: return "the firmware has no S3";
    case N_POWER_REASON_NO_WAKE_PAGE: return "no wake page below 1 MiB";
    case N_POWER_REASON_DEVICE: return "a device cannot come back";
    case N_POWER_REASON_DEVICE_BUSY: return "a device cannot sleep now";
    case N_POWER_REASON_DEVICE_STUCK: return "a device did not stop";
    case N_POWER_REASON_APP_DRIVER: return "an app drives a device and does not take part in sleep";
    case N_POWER_REASON_DISK_FLUSH: return "a disk did not write its cache back";
    case N_POWER_REASON_CPU_STOP: return "a processor did not stop";
    case N_POWER_REASON_FIRMWARE: return "the firmware did not sleep";
    case N_POWER_REASON_LOW_ROOM: return "no room below 1 MiB";
    case N_POWER_REASON_NOT_ON_DISK: return "C: is not on a disk";
    case N_POWER_REASON_NO_MEMORY: return "not enough memory";
    case N_POWER_REASON_NO_SPACE: return "not enough free space on C:";
    case N_POWER_REASON_IMAGE_TOO_LARGE: return "the image did not fit in memory or on C:";
    case N_POWER_REASON_STORE_WRITE: return "C: could not be written";
    case N_POWER_REASON_COUNT: break;
    }
    return "unknown";
}

/* 機械の部分：IOMMU、S3 か休止の置き場、起こしの頁。 */
static enum n_power_reason machine_blocker(bool hibernate, const char **detail) {
    *detail = NULL;
    if (!iommu_power_ready()) return N_POWER_REASON_IOMMU;
    enum n_power_reason why = hibernate ? hibernate_blocker(detail) : !acpi_sleep_supported(3) ? N_POWER_REASON_NO_S3 : N_POWER_REASON_NONE;
    if (why) return why;
    if (!wake_available()) return N_POWER_REASON_NO_WAKE_PAGE;
    return N_POWER_REASON_NONE;
}

/* 装置の部分：io_power_begin の間に。 */
static enum n_power_reason devices_blocker(bool hibernate, const char **detail) {
    enum io_refusal refusal;
    const char *who = io_power_refused(hibernate, &refusal);
    if (who) *detail = who;
    switch (refusal) {
    case IO_REFUSAL_NONE: return N_POWER_REASON_NONE;
    case IO_REFUSAL_CANNOT: return N_POWER_REASON_DEVICE;
    case IO_REFUSAL_BUSY: return N_POWER_REASON_DEVICE_BUSY;
    case IO_REFUSAL_PROCESS: return N_POWER_REASON_APP_DRIVER;
    }
    return N_POWER_REASON_DEVICE;
}

/* 機械の部分は pm_sleep_check が起動で一度決める（開始メニューは描くたびに聞く）。装置の部分は結んでいる実装の
   after_sleep で、実装が来て去るたびに変わるので、聞かれるたびに見る（io_power_cannot）。 */
static bool can_sleep, can_hibernate;

bool pm_can_sleep(void) { return can_sleep && !io_power_cannot(false); }
bool pm_can_hibernate(void) { return can_hibernate && !io_power_cannot(true); }

static void blocker_log(const char *what, enum n_power_reason why, const char *detail) {
    if (why) kprintf("pm: %s unavailable (%s%s%s)\n", what, reason_text(why), detail ? ": " : "", detail ? detail : "");
}

void pm_sleep_check(void) {
    const char *detail;
    enum n_power_reason why = machine_blocker(false, &detail);
    can_sleep = !why;
    blocker_log("sleep", why, detail);
    if (!why) kprintf("pm: sleep (S3) available\n");
    why = machine_blocker(true, &detail);
    can_hibernate = !why;
    blocker_log("hibernation", why, detail);
    if (!why) kprintf("pm: hibernation available\n");
    watched_changed(&told_watched);
}

static void enter_s3(void) { acpi_sleep_enter(3); }

/* 割り込みの制御器と CPU を止め、enter() を呼び、全部を戻す（ACPI 6.5 7.5 の 5 から 8）。装置はもう装置の管理者の walk で
   止まっている。機械が眠った（S3 から起きた、休止の写しが戻った）なら true。enter() が戻ったか、PCI の構成を取れ
   なかったなら false（後者は *why と *detail が言い、enter() は呼ばない）。CPU 0 で、ほかの CPU を止めてから
   （smp_suspend）CPU を戻す（smp_resume）まで、ほかの thread は走らない。保存して戻す割り込みは CPU 0 のもの。ほかの
   CPU は割り込みの制御器が戻った後に動き出す：装置の割り込みはどの CPU にも来て、INIT の中の CPU へのものは失われる。 */
static bool machine_sleep(void (*enter)(void), unsigned state, enum pm_action action, enum n_power_reason *why,
                          const char **detail, bool *units_told) {
    acpi_freeze(true);
    const char *stuck = pci_suspend();
    uint64_t f = irq_save();
    irq_suspend();
    /* log の読み手は APIC が戻った後の log_resume で知らされる。時計が止まる前に：serial の最後の待ちを限る */
    log_suspend();
    alarm_suspend(); /* LAPIC が戻るまで：戻る時計の alarm は待つ */
    clock_suspend();
    /* 取りやめなら、戻る道は enter() が戻った後と同じ */
    bool slept = !stuck && wake_sleep(enter);
    /* 起きた（か眠らなかった）：CPU は戻った。次はそれが話す相手 */
    /* 起こしの firmware が素の経路を開けたかもしれない。再割り当ての切り替えの前にその元を止める。保存した経路は
       最後の戻しのために残る */
    ioapic_mask_all();
    /* 時計が先：ここからの待ち（答えない unit や link の待ち）は時計で限られ、戻るまで時計は止まっている（TSC は 0
       から数え直した） */
    int64_t secs = clock_resume();
    /* 何かが割り込みや要求を送る前に、APIC が自分の型に戻る前に（arch/apic.h。x2APIC は拡張 ID の割り込み再割り当ての
       後、SDM 13.12.7）。眠らなかった unit は claim の表で訳したまま動いているので触れない。戻らなかった unit は確かめ
       られる所で切り（iommu_resume）、IOAPIC の項と PCI の機能の message は素で作り直すか切ったままにし、APIC は
       再割り当てなしで自分の型に戻る。 */
    const char *units_lost = slept ? iommu_resume() : NULL;
    if (!apic_init_cpu()) panic("pm: this CPU's APIC did not come back in %s mode", apic_x2() ? "x2APIC" : "xAPIC");
    irq_resume(slept); /* 8259 をまた塞ぐ。IOAPIC の項は全部の CPU が戻るまで塞いだまま */
    log_resume();      /* serial を立て直す */
    vm_resume();       /* hypervisor のほかの重ね頁と割り込み。LAPIC の後 */
    alarm_resume();    /* CPU 0 の起こしの装置。LAPIC の後 */
    acpi_wake_prepare(state);
    if (slept) {
        /* INTx の配線は、どの機能の構成（割り込みの許可）より先に、この CPU が割り込みを受ける前に戻す：立て直して
           いない link を通って pin に届く level は無い。電源資源は PCI の構成より先に入れ直す */
        acpi_links_restore();
        acpi_power_restore();
    }
    pci_resume(slept, units_lost != NULL);
    irq_restore(f);
    smp_resume();
    irq_resume_lines(); /* IOAPIC の項を、動いている CPU へ */
    input_resumed();
    acpi_freeze(false);
    if (units_lost) say(N_POWER_DEVICE_LOST, action, N_POWER_REASON_IOMMU, units_lost, false);
    *units_told = units_lost != NULL;
    if (slept) kprintf("pm: awake after %ld s\n", (long)secs);
    if (stuck) {
        *why = N_POWER_REASON_DEVICE_STUCK;
        *detail = stuck;
    }
    return slept;
}

/* 眠りの順（ACPI 6.5 7.5、16.1.6）：拒みの確かめ、_TTS(3)、保存と書き出し、IO_POWER_SLEEP の walk、_PTS(3)、CPU、
   enter。戻りは machine_sleep の戻し、_WAK(3)、walk の戻し、_TTS(0)。_TTS(0) は取りやめでも、_WAK は _PTS の後の
   取りやめでも呼ぶ（7.4.1、7.4.4）。
   休止：拒みの確かめ、保存と書き出し、IO_POWER_IMAGE の walk、CPU、写し、戻し、walk の戻し、写しを書いて電源断。
   写しを書く間、門は開く（ディスクの実装の thread が動く）が、ディスクは電源断まで holder のもの（写しの後に C: へ
   届くのは写しだけ）。写しから戻った機械は、写しを取った所から machine_sleep の戻しへ進む（_WAK(4)）。 */
static void sleep_now(bool hibernate) {
    enum pm_action action = hibernate ? PM_HIBERNATE : PM_SLEEP;
    unsigned state = hibernate ? 4 : 3;
    const char *detail;
    enum n_power_reason why = machine_blocker(hibernate, &detail);
    io_power_begin(); /* ここから io_power_end まで、実装は来も去りもせず、途中のものも無い */
    if (!why) why = devices_blocker(hibernate, &detail);
    if (!why && hibernate) why = hibernate_prepare();
    if (why) {
        kprintf("pm: not %s: %s%s%s\n", hibernate ? "hibernating" : "sleeping", reason_text(why), detail ? ": " : "",
                detail ? detail : "");
        say(N_POWER_REFUSED, action, why, detail, false);
        io_power_end();
        return;
    }
    kprintf("pm: going to %s\n", hibernate ? "hibernate" : "sleep");
    if (!hibernate) acpi_sleep_transition(3);
    store_sync(); /* 保存したものは、眠りがどうなってもボリュームにある */
    /* 制御器が状態を失って切れる転送は無く、写しの後にディスクへ届くのは写しだけ */
    blk_hold();
    bool walked = false, prepared = false, slept = false, snapshot = false, units_told = false;
    if (!flush_disks()) why = N_POWER_REASON_DISK_FLUSH; /* 眠りか写しの中で失われうる */
    if (!why) {
        walked = true;
        const char *stuck = io_power_suspend(hibernate ? IO_POWER_IMAGE : IO_POWER_SLEEP);
        if (stuck) {
            why = N_POWER_REASON_DEVICE_STUCK;
            detail = stuck;
        }
    }
    if (!why && !hibernate) {
        prepared = true;
        if (!acpi_sleep_prepare(3)) why = N_POWER_REASON_FIRMWARE;
    }
    if (!why) {
        if (!smp_suspend()) {
            smp_resume();
            why = N_POWER_REASON_CPU_STOP;
        } else {
            slept = machine_sleep(hibernate ? hibernate_snapshot : enter_s3, state, action, &why, &detail, &units_told);
            snapshot = hibernate && !slept && !why;
            if (!hibernate && !slept && !why) why = N_POWER_REASON_FIRMWARE;
        }
    }
    if (prepared || (hibernate && slept)) acpi_wake(state);
    const char *lost = walked ? io_power_resume(slept) : NULL;
    if (!hibernate) acpi_sleep_transition(0);
    io_power_end();
    if (snapshot) {
        /* 写し：ここからの機械は戻って来ない */
        say(N_POWER_SAVING, action, N_POWER_REASON_NONE, NULL, false);
        if (!(why = hibernate_write())) {
            uint64_t f = spin_lock(&pm_lock);
            stopping = true;
            spin_unlock(&pm_lock, f);
            kprintf("pm: hibernated, power off\n");
            finish(PM_POWER_OFF);
        }
    }
    blk_release();
    if (hibernate) hibernate_cleanup(slept);
    if (screen_display) screen_display->repaint();
    if (lost) say(N_POWER_DEVICE_LOST, action, N_POWER_REASON_DEVICE, lost, false);
    if (slept) {
        acpi_lid_read(); /* 誰も聞いていない間に変わったかもしれない */
        woke();
    }
    if (!slept) {
        kprintf("pm: %s failed: %s%s%s\n", hibernate ? "hibernation" : "sleep", reason_text(why), detail ? ": " : "",
                detail ? detail : "");
        say(N_POWER_FAILED, action, why, detail, false);
    } else if (!lost && !units_told) {
        say(N_POWER_AWAKE, action, N_POWER_REASON_NONE, NULL, false);
    }
}

static void pm_thread(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t f = spin_lock(&pm_lock);
        while (!requested) {
            wq_wait_locked(&pm_wq, &pm_lock);
            spin_take(&pm_lock);
        }
        requested = false;
        busy = true;
        enum pm_action action = wanted;
        spin_unlock(&pm_lock, f);
        if (action == PM_SLEEP || action == PM_HIBERNATE) {
            kprintf("pm: %s requested\n", action == PM_SLEEP ? "sleep" : "hibernation");
            f = spin_lock(&pm_lock);
            napping = true;
            spin_unlock(&pm_lock, f);
            sleep_now(action == PM_HIBERNATE); /* アプリは走り続ける：時計が進んだと知らされるだけ */
            f = spin_lock(&pm_lock);
            napping = false;
            spin_unlock(&pm_lock, f);
        } else {
            kprintf("pm: %s requested\n", action == PM_REBOOT ? "restart" : "shutdown");
            f = spin_lock(&pm_lock);
            stopping = true;
            action = wanted;
            spin_unlock(&pm_lock, f);
            say(N_POWER_STOPPING, action, N_POWER_REASON_NONE, NULL, false);
            stop_apps();
            store_sync(); /* 保存したものが先にボリュームへ */
            finish(action);
        }
        f = spin_lock(&pm_lock);
        busy = false;
        spin_unlock(&pm_lock, f);
    }
}

/* 最初の CPU で：眠り（smp_suspend、wake_sleep）は firmware が起こす CPU で戻る。 */
void pm_init(void) {
    watched_init(&told_watched, NULL);
    watched_init(&happened_watched, NULL);
    kthread_create_on("power", pm_thread, NULL, 0);
}
