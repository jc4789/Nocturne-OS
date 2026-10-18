/* 継の選択核。提供された kei_sched（MIT License）をカーネルへ取り込んだもの。
   接続（APごとのキュー、排他、文脈保存、配置）は sys/sched.c が持つ。 */
#include "sys/kei.h"

/* 最大値を明示し、符号付き比較と中間積に十分な余地を残す。 */
static const kei_vtime vlimit = ((kei_vtime)1 << 120) - 1;
static kei_key key_of(const struct kei_task *t) {
    return (kei_key)t->v - (kei_key)t->bonus_v;
}
static bool less(const struct kei_task *a, const struct kei_task *b) {
    kei_key ak = key_of(a), bk = key_of(b);
    return ak != bk ? ak < bk : a->id < b->id;
}
static void hswap(struct kei_rq *q, size_t a, size_t b) {
    struct kei_task *t = q->heap[a];
    q->heap[a] = q->heap[b]; q->heap[b] = t;
    q->heap[a]->pos = a; t->pos = b;
}
static void up(struct kei_rq *q, size_t p) {
    while (p) {
        size_t parent = (p - 1) / 2;
        if (!less(q->heap[p], q->heap[parent])) break;
        hswap(q, p, parent); p = parent;
    }
}
static void down(struct kei_rq *q, size_t p, size_t count) {
    while (p < count / 2) {
        size_t child = 2 * p + 1;
        if (child + 1 < count && less(q->heap[child + 1], q->heap[child])) ++child;
        if (!less(q->heap[child], q->heap[p])) break;
        hswap(q, p, child); p = child;
    }
}
static void fix(struct kei_rq *q, size_t p, size_t count) {
    if (p && less(q->heap[p], q->heap[(p - 1) / 2])) up(q, p);
    else down(q, p, count);
}
static void insert(struct kei_rq *q, struct kei_task *t) {
    q->heap[q->count] = t; t->pos = q->count; up(q, q->count);
    ++q->count; t->state = KEI_READY;
}
static void remove_ready(struct kei_rq *q, struct kei_task *t) {
    size_t count = q->count - 1, p = t->pos;
    if (p != count) {
        q->heap[p] = q->heap[count]; q->heap[p]->pos = p; fix(q, p, count);
    }
    q->heap[count] = NULL; t->pos = KEI_NONE; q->count = count;
}
/* 分母変更時には1仮想ns未満の参照時計端数を切り捨てる。
 * タスクの実行会計端数は捨てない。無実行の参加/移動では時計を進めない。 */
static void weight_add(struct kei_rq *q, uint32_t weight) {
    q->total_weight += weight; q->clock_remainder = 0;
}
static void weight_sub(struct kei_rq *q, uint32_t weight) {
    q->total_weight -= weight; q->clock_remainder = 0;
}
static kei_vtime weighted_delta(uint64_t ns, uint64_t weight, uint64_t old_remainder,
                                uint64_t *new_remainder) {
    uint64_t fraction = (ns % weight) * (uint64_t)KEI_WEIGHT_ONE + old_remainder;
    *new_remainder = fraction % weight;
    return (kei_vtime)(ns / weight) * KEI_WEIGHT_ONE + fraction / weight;
}
static const uint64_t weight_limit = UINT64_MAX / (KEI_WEIGHT_ONE + UINT64_C(1));
static size_t resident_count(const struct kei_rq *q) {
    return q->count + (q->current != NULL);
}

size_t kei_mask_words(size_t nbits) { return nbits / 64 + (nbits % 64 != 0); }
bool kei_mask_test(const struct kei_mask *mask, size_t cpu) {
    if (!mask) return true;
    return cpu < mask->nbits && mask->words && ((mask->words[cpu / 64] >> (cpu % 64)) & 1u);
}
enum kei_error kei_rq_init(struct kei_rq *q, size_t cpu,
    struct kei_task **heap, size_t capacity,
    uint64_t retention_v, uint64_t bonus_cap_v) {
    if (!q || !heap || !capacity || cpu == KEI_NONE || !retention_v) return KEI_BAD;
    if (capacity > SIZE_MAX / sizeof(*heap)) return KEI_RANGE;
    *q = (struct kei_rq){ .cpu = cpu, .capacity = capacity, .heap = heap,
        .retention_v = retention_v, .bonus_cap_v = bonus_cap_v, .online = true };
    return KEI_OK;
}
enum kei_error kei_task_init(struct kei_task *t, uint64_t id, uint32_t weight,
    const struct kei_mask *allowed) {
    if (!t || !weight) return KEI_BAD;
    *t = (struct kei_task){ .id = id, .weight = weight, .pos = KEI_NONE,
        .allowed = allowed, .state = KEI_NEW, .context_safe = true };
    return KEI_OK;
}
enum kei_error kei_rq_resize(struct kei_rq *q,
    struct kei_task **heap, size_t capacity) {
    if (!q || !heap) return KEI_BAD;
    if (capacity < resident_count(q)) return KEI_FULL;
    if (!capacity || capacity > SIZE_MAX / sizeof(*heap)) return KEI_RANGE;
    /* 異なる新規配列、または現配列そのもの。部分重複は契約違反。 */
    for (size_t i = 0; i < q->count; ++i) heap[i] = q->heap[i];
    q->heap = heap; q->capacity = capacity; return KEI_OK;
}
enum kei_error kei_set_online(struct kei_rq *q, bool online) {
    if (!q) return KEI_BAD;
    if (!online && resident_count(q)) return KEI_BUSY;
    q->online = online; return KEI_OK;
}
enum kei_error kei_charge(struct kei_rq *q, uint64_t service_ns) {
    if (!q || !q->current || !q->total_weight) return KEI_BAD;
    struct kei_task *t = q->current;
    uint64_t tr, qr;
    kei_vtime inc = weighted_delta(service_ns, t->weight, t->remainder, &tr);
    kei_vtime clock_inc = weighted_delta(service_ns, q->total_weight, q->clock_remainder, &qr);
    if (inc > vlimit - t->v || clock_inc > vlimit - q->clock_v ||
        (kei_vtime)service_ns > vlimit - t->service_ns) return KEI_RANGE;
    t->v += inc; t->service_ns += service_ns; t->remainder = (uint32_t)tr;
    q->clock_v += clock_inc; q->clock_remainder = qr;
    return KEI_OK;
}
/* キューの外（BSPで借りて走らせた間）の実サービスを、本人の仮想実行量だけに
   加える。参照時計はそのCPUの供給ではないので進めない。起床時に先行分として残る。 */
enum kei_error kei_charge_detached(struct kei_task *t, uint64_t service_ns) {
    if (!t || t->state == KEI_READY || t->state == KEI_RUNNING || t->state == KEI_DEAD) return KEI_BAD;
    uint64_t tr;
    kei_vtime inc = weighted_delta(service_ns, t->weight, t->remainder, &tr);
    if (inc > vlimit - t->v || (kei_vtime)service_ns > vlimit - t->service_ns) return KEI_RANGE;
    t->v += inc; t->service_ns += service_ns; t->remainder = (uint32_t)tr;
    return KEI_OK;
}
enum kei_error kei_wake(struct kei_rq *q, struct kei_task *t) {
    if (!q || !t || !q->online || (t->state != KEI_NEW && t->state != KEI_SLEEP) || !t->context_safe)
        return KEI_BAD;
    if (!kei_mask_test(t->allowed, q->cpu)) return KEI_AFFINITY;
    if (resident_count(q) >= q->capacity) return KEI_FULL;
    if (t->bonus_v > q->bonus_cap_v || t->weight > weight_limit - q->total_weight) return KEI_RANGE;
    kei_vtime lead = t->home && t->v > t->home->clock_v ? t->v - t->home->clock_v : 0;
    if (lead > vlimit - q->clock_v) return KEI_RANGE;
    t->v = q->clock_v + lead; t->home = q;
    weight_add(q, t->weight); insert(q, t);
    return KEI_OK;
}
enum kei_error kei_sleep(struct kei_rq *q, struct kei_task *t) {
    if (!q || !t || t->home != q || (t->state != KEI_RUNNING && t->state != KEI_READY)) return KEI_BAD;
    if (!t->context_safe) return KEI_BUSY;
    if (t == q->current) q->current = NULL; else remove_ready(q, t);
    weight_sub(q, t->weight); t->state = KEI_SLEEP; return KEI_OK;
}
enum kei_error kei_destroy(struct kei_rq *q, struct kei_task *t) {
    if (!q || !t || (t->home && t->home != q) || t->state == KEI_DEAD) return KEI_BAD;
    if (t->state == KEI_RUNNING || t->state == KEI_READY) {
        enum kei_error e = kei_sleep(q, t); if (e != KEI_OK) return e;
    }
    if (!t->context_safe) return KEI_BUSY;
    t->state = KEI_DEAD; t->home = NULL; return KEI_OK;
}
enum kei_error kei_migrate(struct kei_rq *src, struct kei_rq *dst, struct kei_task *t) {
    if (!src || !dst || !t || src == dst || t->home != src || t->state != KEI_READY) return KEI_BAD;
    if (!t->context_safe) return KEI_BUSY;
    if (!dst->online || !kei_mask_test(t->allowed, dst->cpu)) return KEI_AFFINITY;
    if (resident_count(dst) >= dst->capacity) return KEI_FULL;
    if (t->bonus_v > dst->bonus_cap_v || t->weight > weight_limit - dst->total_weight) return KEI_RANGE;
    /* 待機中の主体は正負両方の相対差を保持する。移動だけでは借りも不足も消えない。 */
    kei_vtime lag = t->v - src->clock_v;
    kei_vtime next = dst->clock_v + lag;
    if (next > vlimit || next < -vlimit) return KEI_RANGE;
    remove_ready(src, t); weight_sub(src, t->weight);
    t->v = next; t->home = dst; weight_add(dst, t->weight); insert(dst, t);
    return KEI_OK;
}
enum kei_error kei_set_bonus(struct kei_rq *q, struct kei_task *t, uint64_t bonus_v) {
    if (!q || !t || t->home != q || t->state == KEI_DEAD) return KEI_BAD;
    if (bonus_v > q->bonus_cap_v) return KEI_RANGE;
    t->bonus_v = bonus_v;
    if (t->state == KEI_READY) fix(q, t->pos, q->count);
    return KEI_OK;
}
static uint64_t next_crossing(const struct kei_task *current, kei_key target) {
    kei_key needed = target - key_of(current) + 1;
    if (needed <= 0) return 0;
    /* これ以上なら最軽量でもUINT64_MAX ns以内に交差しない。 */
    if ((kei_vtime)needed > (kei_vtime)UINT64_MAX * KEI_WEIGHT_ONE) return KEI_NO_TIMER;
    kei_vtime num = (kei_vtime)needed * current->weight;
    if (num <= current->remainder) return 0;
    num -= current->remainder;
    kei_vtime ns = (num + KEI_WEIGHT_ONE - 1) / KEI_WEIGHT_ONE;
    return ns >= UINT64_MAX ? KEI_NO_TIMER : (uint64_t)ns;
}
struct kei_decision kei_decide(const struct kei_rq *q) {
    struct kei_decision d = { .service_until_recheck_ns = KEI_NO_TIMER };
    if (!q || !q->online) return d;
    struct kei_task *waiting = q->count ? q->heap[0] : NULL;
    if (!q->current) { d.next = waiting; d.switch_needed = waiting != NULL; return d; }
    if (!waiting) { d.next = q->current; return d; }
    kei_key target = key_of(waiting) + q->retention_v;
    if (key_of(q->current) > target) {
        d.next = waiting; d.switch_needed = true; d.service_until_recheck_ns = 0;
    } else {
        d.next = q->current; d.service_until_recheck_ns = next_crossing(q->current, target);
    }
    return d;
}
enum kei_error kei_context_quiesced(struct kei_rq *q, struct kei_task *t) {
    if (!q || !t || q->current != t || t->state != KEI_RUNNING || t->home != q) return KEI_BAD;
    t->context_safe = true; return KEI_OK;
}
enum kei_error kei_commit(struct kei_rq *q, struct kei_task *next) {
    if (!q || !q->online || !next) return KEI_BAD;
    if (kei_decide(q).next != next) return KEI_BAD;
    if (q->current == next) { next->context_safe = false; return KEI_OK; }
    if (next->home != q || next->state != KEI_READY || !next->context_safe) return KEI_BAD;
    if (q->current && !q->current->context_safe) return KEI_BUSY;
    struct kei_task *old = q->current;
    remove_ready(q, next);
    q->current = NULL;
    if (old) insert(q, old);
    q->current = next; next->state = KEI_RUNNING; next->context_safe = false;
    return KEI_OK;
}

uint64_t kei_retention_from_cost(uint64_t cost_ns, uint32_t ppm,
    uint64_t lower_v, uint64_t upper_v) {
    if (!ppm || ppm >= 1000000 || !lower_v || lower_v > upper_v) return 0;
    uint64_t m = 1000000 - ppm;
    uint64_t whole = cost_ns / ppm;
    if (whole > upper_v / m) return upper_v;
    uint64_t a = whole * m;
    uint64_t b = ((cost_ns % ppm) * m + ppm - 1) / ppm;
    uint64_t answer = b > upper_v - a ? upper_v : a + b;
    return answer < lower_v ? lower_v : answer;
}
static uint64_t saturate_add(uint64_t a, uint64_t b) {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
static uint64_t finish_cost(const struct kei_cpu_candidate *c, uint64_t work) {
    uint64_t w = work / c->capacity_q10;
    if (w > UINT64_MAX / KEI_WEIGHT_ONE) return UINT64_MAX;
    uint64_t a = w * KEI_WEIGHT_ONE;
    uint64_t b = ((work % c->capacity_q10) * (uint64_t)KEI_WEIGHT_ONE + c->capacity_q10 - 1) / c->capacity_q10;
    return saturate_add(saturate_add(a, b), saturate_add(c->queue_delay_ns, c->move_cost_ns));
}
size_t kei_choose_cpu(const struct kei_cpu_candidate *c, size_t count,
    size_t previous_cpu, uint64_t estimated_work_ns, uint64_t hysteresis_ns) {
    if (!c) return KEI_NONE;
    size_t best = KEI_NONE, prev = KEI_NONE;
    uint64_t best_cost = UINT64_MAX, prev_cost = UINT64_MAX;
    for (size_t i = 0; i < count; ++i) {
        if (!c[i].online || !c[i].allowed || !c[i].capacity_q10 || c[i].cpu == KEI_NONE) continue;
        uint64_t cost = finish_cost(&c[i], estimated_work_ns);
        if (best == KEI_NONE || cost < best_cost || (cost == best_cost && c[i].cpu < best)) {
            best = c[i].cpu; best_cost = cost;
        }
        if (c[i].cpu == previous_cpu) { prev = previous_cpu; prev_cost = cost; }
    }
    if (prev != KEI_NONE && (best == prev || saturate_add(best_cost, hysteresis_ns) >= prev_cost)) return prev;
    return best;
}
struct kei_task *kei_find_stealable(const struct kei_rq *src, size_t dst_cpu,
    size_t budget, size_t *cursor) {
    if (!src || !cursor || !src->count || !budget || src->cpu == dst_cpu) return NULL;
    size_t probes = budget < src->count ? budget : src->count;
    size_t p = *cursor % src->count;
    while (probes--) {
        struct kei_task *t = src->heap[p];
        p = p + 1 == src->count ? 0 : p + 1; *cursor = p;
        if (t->context_safe && kei_mask_test(t->allowed, dst_cpu)) return t;
    }
    return NULL;
}
bool kei_validate(const struct kei_rq *q) {
    if (!q || !q->retention_v || resident_count(q) > q->capacity || (!q->online && resident_count(q))) return false;
    uint64_t weight = q->current ? q->current->weight : 0;
    for (size_t i = 0; i < q->count; ++i) {
        struct kei_task *t = q->heap[i];
        if (!t || t->home != q || t->state != KEI_READY || !t->context_safe || t->pos != i ||
            !kei_mask_test(t->allowed, q->cpu) || t->bonus_v > q->bonus_cap_v ||
            t->weight > weight_limit - weight || !t->weight || t->remainder >= t->weight) return false;
        weight += t->weight;
        if (i && less(t, q->heap[(i - 1) / 2])) return false;
    }
    if (q->current && (q->current->home != q || q->current->state != KEI_RUNNING ||
        q->current->pos != KEI_NONE || !kei_mask_test(q->current->allowed, q->cpu) ||
        !q->current->weight || q->current->remainder >= q->current->weight ||
        q->current->bonus_v > q->bonus_cap_v)) return false;
    return weight == q->total_weight && weight <= weight_limit &&
        (!weight || q->clock_remainder < weight);
}
