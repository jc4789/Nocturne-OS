#pragma once
/* 朔Ⅱ：並列返済表と持続会計（BSPだけが書く。IRQを閉じて呼ぶ）。
   参照方針は等重みの仮想時間で、期間ごとにAP数以下の別々のuser threadへ
   Qの請求を発行し、D期間後の返済期限を付ける。前払いがあれば先に照合する。
   期限の期間に未払いが残る票の所有者は必ずAPへ置く（保護器）。残りの枠は
   CPUバースト長の経験分布から作る整数ギティンズ指数で選ぶ。
   サービスとして払うのはAP上のuser実行時間だけ。BSP上の時間はバースト
   年齢にだけ数える。BSP専用のkernel継続へ入ると「資格休止」で未払いを
   取り消し、実際のブロックとは別に数える。 */
#include <stdbool.h>
#include <stdint.h>

struct task;

#define SAKU_SAMPLES 32

struct saku_account {
    struct task *next;          /* 会計主体の一覧（終了してもfree_taskまで残る） */
    bool listed, ready;         /* ready: いまring3を実行できる */
    int32_t head, tail;         /* この主体の未払い票、古い順 */
    uint64_t vref;              /* 参照側が配分した量（µs）。等重みの仮想時間 */
    uint64_t advance;           /* 前払い（µs） */
    uint64_t progress_us;       /* 今回のAP起動で会計済みの経過時間 */
    uint32_t claim;             /* 再配置の一回の走査で選んだ印 */
    /* バースト予測：起床からブロックまでのCPU時間 */
    uint64_t burst_us;
    uint32_t sample_us[SAKU_SAMPLES], sample_ms[SAKU_SAMPLES];
    uint8_t samples, sample_next;
    uint32_t version;
    uint32_t cache_version, cache_attained, cache_cost, cache_epoch;
    uint64_t cache_n, cache_d;
};

struct saku_stats {
    uint64_t periods, skipped, issued_us, matched_us, paid_us, advance_us;
    uint64_t sleep_cancel_us, suspend_cancel_us, discarded_us;
    uint64_t violations, violated_us, drains;
    uint64_t violations_ap, violations_queued; /* 違反時の所有者：AP上、待ち行列（残りはBSP上） */
    uint64_t forced_stops, index_stops, bursts;
    uint64_t tick_us_total, tick_us_max, ticks;
};

#define SAKU_SWITCH_US 50       /* CPU移動の切替費用（指数の分母） */
#define SAKU_PERIOD_US 1000     /* P：BSPタイマーと同じ1ms */

bool saku_active(void);
void saku_start(unsigned cpus, uint64_t now_ms); /* AP runner数が分かった後に一度 */
void saku_track(struct task *t);                 /* user threadの会計を作る（冪等） */
void saku_forget(struct task *t);                /* free_task: 一覧から外す */
void saku_ready(struct task *t);                 /* user境界へ来た */
void saku_block(struct task *t);                 /* 実際のブロック：バースト終端 */
void saku_suspend(struct task *t);               /* BSP継続：資格休止、バーストは続く */
void saku_exit(struct task *t);                  /* 終了：取消し、前払いは精算待ちに残す */
void saku_charge(struct task *t, uint64_t us, bool service);
/* 期間境界：now_msまでの終了期間の期限を調べ、今の期間の参照を発行する。 */
void saku_period(uint64_t now_ms);
bool saku_due(const struct task *t);             /* 返済期限が今か次の期間の票がある */
/* aをcost_a、bをcost_bで実行する場合、aを優先すべきなら正。 */
int saku_order(struct task *a, uint32_t cost_a, struct task *b, uint32_t cost_b, uint64_t now_ms);
/* 指数が正（有限の観測から完了を見込める）で、bより高いならtrue。 */
bool saku_index_beats(struct task *a, uint32_t cost_a, struct task *b, uint32_t cost_b, uint64_t now_ms);
void saku_get_stats(struct saku_stats *out);
void saku_note_stop(bool forced);
void saku_note_tick(uint64_t us);
