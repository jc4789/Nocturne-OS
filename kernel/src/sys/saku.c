/* 朔Ⅱ：持続会計・並列返済表・打切り付きバースト分布の整数指数。
   設計はjc4789の朔Ⅱ設計ノート（2026-10-18）。全関数はBSPがIRQを閉じて呼ぶ。
   user threadのCPU許可集合は全APで同じなので、参照の部分単射は「別々の
   主体をAP数以下」で常に合法になり、返済必須集合の配置は任意の割当てでよい
   （設計の配置DPはCPU許可集合が主体ごとに違う場合だけ必要）。 */
#include "kernel.h"
#include "mm/heap.h"
#include "sys/sched.h"
#include "sys/saku.h"

#define SAKU_PERIOD_MS (SAKU_PERIOD_US / 1000)
#define SAKU_LEAD 1             /* 期限の一期間前から必須にする（BSP経由の配置遅れ） */
#define SAKU_QUOTA_US 500       /* Q：BSP経由の起動・停止の遅れを残すためP未満 */
/* D：返済までの期間数。Cityで8と比べ、外されたworkerが早くAPへ戻る2を選んだ。 */
#define SAKU_DELAY 2
#define SAKU_DECAY_MS 1000      /* バースト標本の重みを半分にする間隔 */

struct ticket {
    uint64_t due;
    uint32_t remaining;
    struct task *owner;
    int32_t prev, next;
};

static bool active;
static unsigned cpus;
static uint64_t frame, drain_until, ref_floor;
static bool issued;
static struct ticket *cells;    /* (SAKU_DELAY+1)*cpus、返済期間の剰余で区切る */
static struct task *accounts;
static struct saku_stats st;

bool saku_active(void) { return active; }

void saku_start(unsigned count, uint64_t now_ms) {
    if (active || !count) return;
    cells = kzalloc((size_t)(SAKU_DELAY + 1) * count * sizeof *cells);
    if (!cells) { kprintf("sched: saku ledger allocation failed; reference order only\n"); return; }
    cpus = count;
    frame = now_ms / SAKU_PERIOD_MS;
    issued = false;
    active = true;
    kprintf("sched: saku II P=%ums Q=%uus D=%u, %u AP runners, ledger %lu bytes\n",
            SAKU_PERIOD_MS, SAKU_QUOTA_US, SAKU_DELAY, count,
            (unsigned long)((SAKU_DELAY + 1) * count * sizeof *cells));
}

void saku_track(struct task *t) {
    struct saku_account *a = &t->saku;
    if (a->listed) return;
    a->head = a->tail = -1;
    a->next = accounts;
    accounts = t;
    a->listed = true;
}

static void unlink_ticket(int32_t idx) {
    struct ticket *k = &cells[idx];
    struct saku_account *a = &k->owner->saku;
    if (k->prev >= 0) cells[k->prev].next = k->next; else a->head = k->next;
    if (k->next >= 0) cells[k->next].prev = k->prev; else a->tail = k->prev;
    k->prev = k->next = -1;
    k->remaining = 0;
    k->owner = NULL;
}

/* 未払い残量だけを取り消す。取消しは実行サービスではない。 */
static void cancel_all(struct task *t, uint64_t *counter) {
    struct saku_account *a = &t->saku;
    if (!a->listed) return;
    while (a->head >= 0) {
        *counter += cells[a->head].remaining;
        unlink_ticket(a->head);
    }
}

void saku_forget(struct task *t) {
    struct saku_account *a = &t->saku;
    if (!a->listed) return;
    cancel_all(t, &st.sleep_cancel_us);
    st.discarded_us += a->advance; /* 終了主体の精算待ち記録を捨てる量（隠さず数える） */
    for (struct task **pp = &accounts; *pp; pp = &(*pp)->saku.next)
        if (*pp == t) { *pp = a->next; break; }
    a->listed = false;
    a->ready = false;
}

void saku_ready(struct task *t) {
    struct saku_account *a = &t->saku;
    if (!a->listed) saku_track(t);
    if (a->ready) return;
    a->ready = true;
    /* 眠っていた間の参照配分を後から一度に請求させない。前払いは持ち越す。 */
    if (a->vref < ref_floor) a->vref = ref_floor;
}

void saku_block(struct task *t) {
    struct saku_account *a = &t->saku;
    if (!a->listed) return;
    cancel_all(t, &st.sleep_cancel_us);
    a->ready = false;
    if (!a->burst_us) return;
    uint64_t d = a->burst_us;
    a->sample_us[a->sample_next] = d > INT32_MAX ? INT32_MAX : (uint32_t)d;
    a->sample_ms[a->sample_next] = (uint32_t)uptime_ms();
    a->sample_next = (uint8_t)((a->sample_next + 1) % SAKU_SAMPLES);
    if (a->samples < SAKU_SAMPLES) a->samples++;
    a->version++;
    a->burst_us = 0;
    st.bursts++;
}

void saku_suspend(struct task *t) {
    struct saku_account *a = &t->saku;
    if (!a->listed) return;
    cancel_all(t, &st.suspend_cancel_us);
    a->ready = false; /* バースト年齢は続く：syscallが返れば同じバースト */
}

void saku_exit(struct task *t) {
    struct saku_account *a = &t->saku;
    if (!a->listed) return;
    cancel_all(t, &st.sleep_cancel_us);
    a->ready = false; /* 打切り：完了標本にしない。前払いはfree_taskまで照合に使う */
}

void saku_charge(struct task *t, uint64_t us, bool service) {
    struct saku_account *a = &t->saku;
    a->burst_us += us;
    if (!service || !a->listed || !active) return;
    while (us && a->head >= 0) {
        struct ticket *k = &cells[a->head];
        uint32_t pay = us < k->remaining ? (uint32_t)us : k->remaining;
        k->remaining -= pay;
        us -= pay;
        st.paid_us += pay;
        if (!k->remaining) unlink_ticket(a->head);
    }
    a->advance += us;
    st.advance_us += us;
}

/* 期間gの終わり：返済期限がgの票が残っていれば供給失敗。期限を延ばさず数え、
   新規発行を止めて古い請求を排出してから次の保証エポックを始める。 */
static void close_period(uint64_t g) {
    struct ticket *row = &cells[(g % (SAKU_DELAY + 1)) * cpus];
    for (unsigned c = 0; c < cpus; c++) {
        if (!row[c].remaining || row[c].due != g) continue;
        st.violations++;
        if (row[c].owner->owner_cpu) st.violations_ap++;
        else if (row[c].owner->on_rq) st.violations_queued++;
        st.violated_us += row[c].remaining;
        int32_t idx = (int32_t)(&row[c] - cells);
        unlink_ticket(idx);
        if (drain_until <= g) st.drains++;
        drain_until = g + SAKU_DELAY + 2;
    }
}

static void issue(uint64_t f) {
    struct task *pick[CPU_MAX_COUNT];
    unsigned n = 0;
    bool floor_set = false;
    for (struct task *t = accounts; t; t = t->saku.next) {
        struct saku_account *a = &t->saku;
        if (a->ready && (!floor_set || a->vref < ref_floor)) { ref_floor = a->vref; floor_set = true; }
        if (!a->ready && !a->advance) continue; /* 眠っていて前払いもない主体へは配分しない */
        unsigned i = n < cpus ? n++ : cpus;
        if (i == cpus) {
            if (a->vref >= pick[cpus - 1]->saku.vref) continue;
            i = cpus - 1;
        }
        while (i && pick[i - 1]->saku.vref > a->vref) { pick[i] = pick[i - 1]; i--; }
        pick[i] = t;
    }
    if (f < drain_until) return;
    uint64_t due = f + SAKU_DELAY;
    struct ticket *row = &cells[(due % (SAKU_DELAY + 1)) * cpus];
    for (unsigned c = 0; c < n; c++) {
        struct saku_account *a = &pick[c]->saku;
        if (row[c].remaining) close_period(row[c].due); /* 先の期間を閉じ損ねた票 */
        uint64_t amount = a->ready || a->advance >= SAKU_QUOTA_US ? SAKU_QUOTA_US : a->advance;
        uint64_t prepaid = a->advance < amount ? a->advance : amount;
        a->advance -= prepaid;
        a->vref += amount;
        st.issued_us += amount;
        st.matched_us += prepaid;
        if (!a->ready || amount == prepaid) continue;
        int32_t idx = (int32_t)(&row[c] - cells);
        row[c] = (struct ticket){ .due = due, .remaining = (uint32_t)(amount - prepaid),
                                  .owner = pick[c], .prev = a->tail, .next = -1 };
        if (a->tail >= 0) cells[a->tail].next = idx; else a->head = idx;
        a->tail = idx;
    }
}

void saku_period(uint64_t now_ms) {
    if (!active) return;
    uint64_t f = now_ms / SAKU_PERIOD_MS;
    if (issued && f == frame) return;
    if (issued) {
        close_period(frame);
        /* BSPが止まっていた期間。剰余の行はD+1個なので、それ以上は見ない。 */
        for (uint64_t g = frame + 1; g < f && g <= frame + SAKU_DELAY + 1; g++) close_period(g);
        if (f > frame + 1) st.skipped += f - frame - 1;
    }
    frame = f;
    issue(f);
    issued = true;
    st.periods++;
}

bool saku_due(const struct task *t) {
    const struct saku_account *a = &t->saku;
    return active && a->listed && a->head >= 0 && cells[a->head].due <= frame + SAKU_LEAD;
}

/* I(a,c) = max_h  Σ_{0<r_j≤h} w_j / (c·Σ_{r_j>0} w_j + Σ w_j·min(r_j,h))。
   標本は全て完了済みなので打切りの尾はない（自分の進行中バーストは年齢aとして
   条件付けに消える）。有限支持点に将来の完了がなければ0：参照順位へ戻す。 */
static void index_of(struct task *t, uint32_t cost, uint64_t now_ms, uint64_t *pn, uint64_t *pd) {
    struct saku_account *a = &t->saku;
    uint32_t attained = a->burst_us > UINT32_MAX ? UINT32_MAX : (uint32_t)a->burst_us;
    uint32_t epoch = (uint32_t)(now_ms / SAKU_DECAY_MS);
    if (a->cache_version == a->version + 1 && a->cache_cost == cost && a->cache_epoch == epoch &&
        attained >= a->cache_attained && attained - a->cache_attained < 64) {
        *pn = a->cache_n; *pd = a->cache_d;
        return;
    }
    uint32_t x[SAKU_SAMPLES];
    uint64_t w[SAKU_SAMPLES], alive = 0;
    unsigned k = 0;
    for (unsigned i = 0; i < a->samples; i++) {
        uint32_t age = (uint32_t)now_ms - a->sample_ms[i];
        uint32_t shift = age / SAKU_DECAY_MS;
        if (shift > 20 || a->sample_us[i] <= attained) continue;
        uint64_t weight = (1u << 20) >> shift;
        uint32_t r = a->sample_us[i] - attained;
        unsigned j = 0;
        while (j < k && x[j] != r) j++;
        if (j == k) { x[k] = r; w[k++] = 0; }
        w[j] += weight;
        alive += weight;
    }
    uint64_t best_n = 0, best_d = 1;
    for (unsigned h = 0; h < k; h++) {
        uint64_t n = 0, d = (uint64_t)cost * alive;
        for (unsigned j = 0; j < k; j++) {
            if (x[j] <= x[h]) n += w[j];
            d += w[j] * (x[j] < x[h] ? x[j] : x[h]);
        }
        if (d && (unsigned __int128)n * best_d > (unsigned __int128)best_n * d) { best_n = n; best_d = d; }
    }
    a->cache_version = a->version + 1;
    a->cache_cost = cost;
    a->cache_epoch = epoch;
    a->cache_attained = attained;
    a->cache_n = best_n;
    a->cache_d = best_d;
    *pn = best_n; *pd = best_d;
}

int saku_order(struct task *a, uint32_t cost_a, struct task *b, uint32_t cost_b, uint64_t now_ms) {
    bool ma = saku_due(a), mb = saku_due(b);
    if (ma != mb) return ma ? 1 : -1;
    uint64_t na, da, nb, db;
    index_of(a, cost_a, now_ms, &na, &da);
    index_of(b, cost_b, now_ms, &nb, &db);
    unsigned __int128 l = (unsigned __int128)na * db, r = (unsigned __int128)nb * da;
    if (l != r) return l > r ? 1 : -1;
    if (a->saku.vref != b->saku.vref) return a->saku.vref < b->saku.vref ? 1 : -1;
    return 0;
}

bool saku_index_beats(struct task *a, uint32_t cost_a, struct task *b, uint32_t cost_b, uint64_t now_ms) {
    uint64_t na, da, nb, db;
    index_of(a, cost_a, now_ms, &na, &da);
    if (!na) return false;
    index_of(b, cost_b, now_ms, &nb, &db);
    return (unsigned __int128)na * db > (unsigned __int128)nb * da;
}

void saku_note_stop(bool forced) { if (forced) st.forced_stops++; else st.index_stops++; }
void saku_note_tick(uint64_t us) {
    st.ticks++;
    st.tick_us_total += us;
    if (us > st.tick_us_max) st.tick_us_max = us;
}
void saku_get_stats(struct saku_stats *out) {
    *out = st;
    st.tick_us_max = 0;
}
