/* 持たれた装置の ACPI の記述（SYS_HAL_ACPI_EVAL、SYS_HAL_ACPI_NOTIFY_TAKE、docs/nocturne-native-abi.md §9）。
   どのバスの claim でも同じ：届くのは装置の firmware の節（hal_device の firmware）、その節の子、木で上の装置の節
   （hal_parent_firmware）だけ。名前は節の直下で引き（uacpi_eval は上へ探さない）、一区画の名前は節を出られない
   （"^" と "\" は名前の字ではない）。
   Notify の記録（struct acpi_claim）は最初の n_hal_acpi_notify_take で作り、claim の終わりの hook が片づける。
   錠の順：claim の終わりの mutex → claims_mutex。notes_lock は葉で、ACPI の thread の Notify もこれだけを取る。 */
#include "kernel.h"
#include "abi.h"
#include "arch/spin.h"
#include "dev/acpi.h"
#include "hal/hal.h"
#include "mm/heap.h"
#include "sys/clock.h"
#include "sys/thread.h"
#include "sys/wait.h"

/* システムが装置と機械を動かすのに使う predefined の method。カーネルが自分の config の bit を自分で動かすように、
   これらはシステムが動かす：電源とリセット、取り外しとドッキング、資源、firmware とシステムのどちらが何を制御する
   かの交渉、起こし、初期化と handler（operation region、GPIO の事象）、システムの眠り。ドライバーが評価すると、
   カーネルが追っている状態（dev/pm.c、dev/pci.c、hal/power.c）が裏で変わり、上の装置ならその下の全部の装置が変わる。
   ドライバーと firmware の間の約束（_DSM、_ROM、_DOD、_DDC、_ADR、firmware 自身の名前）はここに無い。 */
static const char system_methods[][5] = {
    "_PS0", "_PS1", "_PS2", "_PS3", "_ON_", "_OFF", "_RST", /* 電源、リセット（ACPI 6.5 7.2、7.3） */
    "_EJ0", "_EJ1", "_EJ2", "_EJ3", "_EJ4", "_LCK", "_DCK", /* 取り外し、ドッキング（6.3.3、6.3.4、6.5.2） */
    "_DIS", "_SRS",                                         /* 資源（6.2.3、6.2.16） */
    "_OSC", "_OST",                                         /* 交渉（6.2.11、6.3.5） */
    "_PSW", "_DSW",                                         /* 起こし（7.3.1、7.3.14） */
    "_INI", "_REG", "_EVT",                                 /* 初期化と handler（6.5.1、6.5.4、5.6.5.3） */
    "_PTS", "_WAK", "_TTS", "_GTS", "_BFS",                 /* システムの眠り（7.4。_GTS と _BFS は古い版の） */
};

/* name を which の節で評価してよいか：上の method のどれでもなく、上の装置の節では predefined の名前だけ（上の
   装置の method は、その下の全部の装置と機械のもの。上の装置はこの claim のものではない）。節を引く前に決めるので、
   節のある装置も無い装置も同じ答えを受ける。 */
static bool name_allowed(uint32_t which, const char *name) {
    if (which == N_ACPI_PARENT && name[0] != '_') return false;
    for (size_t i = 0; i < ARRAY_SIZE(system_methods); i++)
        if (!memcmp(name, system_methods[i], 4)) return false;
    return true;
}

int64_t acpi_claim_eval(struct handle *h, uint32_t which, const char *name, const struct n_acpi_value *args,
                        uint32_t nargs, const uint8_t *data, size_t data_size, uint8_t *out, size_t out_size) {
    struct hal_claim *c = hal_claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (!acpi_name_segment(name)) return -N_ERR_INVALID;
    if (!name_allowed(which, name)) return -N_ERR_DENIED;
    if (!acpi_aml_ready()) return -N_ERR_UNSUPPORTED;
    struct hal_device *d = hal_claim_enter(c);
    if (!d) return -N_ERR_CLOSED;
    int64_t r = 0;
    struct firmware_node *node;
    if (which == N_ACPI_SELF) node = d->firmware;
    else if (which == N_ACPI_PARENT) node = hal_parent_firmware(d);
    else if (!d->firmware || !(node = acpi_node_child(d->firmware, which))) r = -N_ERR_INVALID; /* 最後の子より先 */
    if (!r) r = node ? acpi_node_eval(node, name, args, nargs, data, data_size, out, out_size) : -N_ERR_NOT_FOUND;
    hal_put(d);
    return r;
}

/* ---- 装置の節への Notify ---- */

#define CLAIM_NOTES 32 /* 取られていない Notify の値。溢れたら古いものが譲る */

struct acpi_claim {
    struct acpi_claim *next;
    struct hal_claim *claim;
    struct hal_claim_end end;
    struct acpi_watch *watch;
    unsigned users; /* claims_mutex：待っている呼出しの数 */
    bool unlinked;  /* claims_mutex：終わりの hook が一覧から外した */
    struct spinlock notes_lock;
    uint32_t notes[CLAIM_NOTES]; /* notes_lock */
    unsigned first, count;       /* notes_lock */
    bool ended;                  /* notes_lock：Notify はもう来ない */
    struct wait_queue wq;
};

static struct mutex claims_mutex;
static struct acpi_claim *claims;

#define ACPI_CLAIM_OF_END(e) ((struct acpi_claim *)((char *)(e) - offsetof(struct acpi_claim, end)))

/* Notify(装置の節, value)。ACPI の thread で、どの CPU でも。 */
static void noted(void *context, uint64_t value) {
    struct acpi_claim *a = context;
    uint64_t irq = spin_lock(&a->notes_lock);
    if (a->count == CLAIM_NOTES) {
        a->first = (a->first + 1) % CLAIM_NOTES;
        a->count--;
    }
    a->notes[(a->first + a->count++) % CLAIM_NOTES] = (uint32_t)value;
    wq_wake_all(&a->wq);
    spin_unlock(&a->notes_lock, irq);
}

/* claim の終わり：Notify を止め、待つ者を起こし、誰も使っていなければ記録を返す。 */
static void claim_ended(struct hal_claim_end *e, struct hal_claim *c) {
    (void)c;
    struct acpi_claim *a = ACPI_CLAIM_OF_END(e);
    acpi_node_unwatch(a->watch); /* 戻った後、noted はもう走らない */
    uint64_t irq = spin_lock(&a->notes_lock);
    a->ended = true;
    wq_wake_all(&a->wq);
    spin_unlock(&a->notes_lock, irq);
    mutex_lock(&claims_mutex);
    for (struct acpi_claim **at = &claims; *at; at = &(*at)->next)
        if (*at == a) {
            *at = a->next;
            break;
        }
    a->unlinked = true;
    bool unused = !a->users;
    mutex_unlock(&claims_mutex);
    if (unused) kfree(a);
}

/* c の記録。無ければ node を見張って作る。claims_mutex の下。NULL なら *error。 */
static struct acpi_claim *claim_record(struct hal_claim *c, struct firmware_node *node, int64_t *error) {
    for (struct acpi_claim *a = claims; a; a = a->next)
        if (a->claim == c) return a;
    struct acpi_claim *a = kzalloc(sizeof *a);
    if (!a) {
        *error = -N_ERR_NO_MEMORY;
        return NULL;
    }
    a->claim = c;
    a->end.ended = claim_ended;
    a->watch = acpi_node_watch(node, noted, a);
    if (!a->watch) {
        kfree(a);
        *error = -N_ERR_NO_MEMORY;
        return NULL;
    }
    if (!hal_claim_on_end(c, &a->end)) {
        acpi_node_unwatch(a->watch);
        kfree(a);
        *error = -N_ERR_CLOSED;
        return NULL;
    }
    a->next = claims;
    claims = a;
    return a;
}

static int64_t note_take(struct acpi_claim *a, uint64_t deadline) {
    uint64_t irq = spin_lock(&a->notes_lock);
    for (;;) {
        if (a->count) {
            uint32_t value = a->notes[a->first];
            a->first = (a->first + 1) % CLAIM_NOTES;
            a->count--;
            spin_unlock(&a->notes_lock, irq);
            return value;
        }
        int64_t r = 0;
        if (thread_current()->killed) r = -N_ERR_STOPPED;
        else if (a->ended) r = -N_ERR_CLOSED;
        else if (clock_now_ns() >= deadline) r = -N_ERR_TIMEOUT;
        if (r) {
            spin_unlock(&a->notes_lock, irq);
            return r;
        }
        /* noted と claim_ended は notes_lock を取って変えてから wq を起こす */
        wq_wait_locked_until(&a->wq, &a->notes_lock, deadline);
        spin_take(&a->notes_lock); /* 期限の時に来た値も取る */
    }
}

int64_t acpi_claim_notify_take(struct handle *h, uint64_t deadline) {
    struct hal_claim *c = hal_claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (!acpi_aml_ready()) return -N_ERR_UNSUPPORTED;
    struct hal_device *d = hal_claim_enter(c);
    if (!d) return -N_ERR_CLOSED;
    struct firmware_node *node = d->firmware;
    int64_t r = node ? 0 : -N_ERR_NOT_FOUND;
    struct acpi_claim *a = NULL;
    if (node) {
        mutex_lock(&claims_mutex);
        a = claim_record(c, node, &r);
        if (a) a->users++;
        mutex_unlock(&claims_mutex);
    }
    hal_put(d);
    if (!a) return r;
    r = note_take(a, deadline);
    mutex_lock(&claims_mutex);
    bool last = !--a->users && a->unlinked;
    mutex_unlock(&claims_mutex);
    if (last) kfree(a);
    return r;
}
