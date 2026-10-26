#ifndef KEI_SCHED_H
#define KEI_SCHED_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* 継: 継続費用を考慮した局所公平スケジューラ（kei_sched、MIT License）。
 * GNU C11 / x86-64。動的確保・時計・割込み・文脈保存は呼出し側が所有する。
 * 各操作は対象キューの排他下。二キュー操作はCPU番号昇順で排他する。
 * CPU番号の上限やCPU数は、この実装には埋め込まない。
 */
#define KEI_WEIGHT_ONE UINT32_C(1024)
#define KEI_NONE SIZE_MAX
#define KEI_NO_TIMER UINT64_MAX

typedef __int128 kei_vtime;
typedef __int128 kei_key;

struct kei_mask { const uint64_t *words; size_t nbits; };
struct kei_rq;
enum kei_state { KEI_NEW, KEI_READY, KEI_RUNNING, KEI_SLEEP, KEI_DEAD };
enum kei_error { KEI_OK, KEI_BAD, KEI_FULL, KEI_BUSY, KEI_RANGE, KEI_AFFINITY };

struct kei_task {
    uint64_t id;
    uint32_t weight, remainder;
    kei_vtime v, service_ns;
    uint64_t bonus_v;
    size_t pos;
    struct kei_rq *home;
    const struct kei_mask *allowed; /* NULLなら全CPU。寿命・変更は外側で管理。 */
    enum kei_state state;
    bool context_safe; /* 保存・スタック離脱の確認が済んだ時だけtrue。 */
};

struct kei_rq {
    size_t cpu, count, capacity;
    struct kei_task **heap; /* 有効順位の索引付き最小ヒープ */
    struct kei_task *current;
    kei_vtime clock_v;
    uint64_t clock_remainder, total_weight;
    uint64_t retention_v, bonus_cap_v;
    bool online;
};

struct kei_decision {
    struct kei_task *next;
    uint64_t service_until_recheck_ns;
    bool switch_needed;
};

struct kei_cpu_candidate {
    size_t cpu;
    uint32_t capacity_q10;
    uint64_t queue_delay_ns, move_cost_ns;
    bool online, allowed;
};

size_t kei_mask_words(size_t nbits);
bool kei_mask_test(const struct kei_mask *mask, size_t cpu);
enum kei_error kei_rq_init(struct kei_rq *q, size_t cpu,
    struct kei_task **heap, size_t capacity,
    uint64_t retention_v, uint64_t bonus_cap_v);
enum kei_error kei_task_init(struct kei_task *t, uint64_t id, uint32_t weight,
    const struct kei_mask *allowed);
/* 再確保はしない。呼出し側が排他下で新しい配列を渡す。 */
enum kei_error kei_rq_resize(struct kei_rq *q,
    struct kei_task **heap, size_t capacity);
enum kei_error kei_set_online(struct kei_rq *q, bool online);

/* 前回精算後の実サービス差分だけを渡す。無関係なIRQ時間は含めない。 */
enum kei_error kei_charge(struct kei_rq *q, uint64_t service_ns);
/* NEW/SLEEPの主体がキューの外で受けたサービス。本人の仮想実行量だけを進める。 */
enum kei_error kei_charge_detached(struct kei_task *t, uint64_t service_ns);
/* NEW/SLEEPを起こす。移動起床は古いhomeと宛先の両方を排他する。 */
enum kei_error kei_wake(struct kei_rq *q, struct kei_task *t);
enum kei_error kei_sleep(struct kei_rq *q, struct kei_task *t);
enum kei_error kei_destroy(struct kei_rq *q, struct kei_task *t);
enum kei_error kei_migrate(struct kei_rq *src, struct kei_rq *dst, struct kei_task *t);
enum kei_error kei_set_bonus(struct kei_rq *q, struct kei_task *t, uint64_t bonus_v);
/* 現実行が順番を譲る：待つ主体があれば、その先頭より継続幅を超えて後ろへ仮想実行量を
   進め、次のdecideで替わるようにする。待つ主体が無ければ何もしない。 */
enum kei_error kei_yield(struct kei_rq *q);

/* decideは文脈を変更しない。commitまで同一排他区間、途中イベント時は再判断。 */
struct kei_decision kei_decide(const struct kei_rq *q);
/* 実際の文脈保存とスタック離脱の後だけ呼べる。ソフトウェアによる証明ではない。 */
enum kei_error kei_context_quiesced(struct kei_rq *q, struct kei_task *t);
/* 保存確認なしに現実行を差し替えない。新しい文脈はunsafeで公開する。 */
enum kei_error kei_commit(struct kei_rq *q, struct kei_task *next);

/* 低頻度の費用制御。値は参照重みの仮想ns。ppmは1..999999。 */
uint64_t kei_retention_from_cost(uint64_t cost_ns, uint32_t overhead_ppm,
    uint64_t lower_v, uint64_t upper_v);
/* 配置候補の作成は外側。比較は候補数に線形で、全CPUへの常時走査は不要。 */
size_t kei_choose_cpu(const struct kei_cpu_candidate *c, size_t count,
    size_t previous_cpu, uint64_t estimated_work_ns, uint64_t hysteresis_ns);
/* 呼出しごとの探索予算と継続位置を受け取る。READYの安全な文脈だけを返す。 */
struct kei_task *kei_find_stealable(const struct kei_rq *src, size_t dst_cpu,
    size_t budget, size_t *cursor);
bool kei_validate(const struct kei_rq *q);

#endif
