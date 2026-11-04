/* HAL：機械の全部の装置の木と、それを動かす実装の結びつき（docs/hal.md）。

   HAL が持つのは制御の道だけである。実装は attach で番地、割り込み、DMA の番地を受け取り、その後のデータの道は
   HAL を通らない。HAL は装置の種類もバスも知らない。装置の身元は性質で、規則で実装に結ぶ。

   どのバスも同じ形で装置を出す：
       hal_device_init(&x->hal, N_BUS_..., &x_bus_ops);
       hal_prop_number(&x->hal, N_PROP_..., value);
       hal_resource_add(&x->hal, N_HAL_MEMORY, slot, flags, base, size);
       hal_location(&x->hal, "...");
       hal_publish(parent, &x->hal);
   実装を足すのは、struct hal_driver を一つ書き、HAL_DRIVER(名前) を置くことである。

   文脈。attach と detach は装置ごとの thread で走り、眠ってよく、秒の単位で待ってよい。別の装置を待たせない。
   quiesce と resume は dev/pm.h の文脈で走る（割り込みを切った CPU の上で、眠らない）。 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hal_buses.h"
#include "sys/wait.h"

struct hal_device;
struct hal_claim;
struct irq;
struct irq_spec;

#define HAL_ENABLE_MEMORY 1u /* メモリーの資源に答える */
#define HAL_ENABLE_PORTS 2u  /* ポートの資源に答える */
#define HAL_ENABLE_DMA 4u    /* 装置がメモリーへ読み書きする */

/* バスが自分の子のために持つ操作。無いものは NULL。 */
struct hal_bus_ops {
    /* count 本までの割り込みを立て、立てた数を返す。specs[i] が i 番目。irqs は NULL でよい。バスが一番良い形を選ぶ
       （PCI は MSI-X、MSI、INTx の順）。眠る。 */
    unsigned (*irq_enable)(struct hal_device *d, const struct irq_spec *specs, unsigned count, struct irq **irqs);
    bool (*irq_disable)(struct hal_device *d);
    unsigned (*irq_max)(struct hal_device *d);
    void (*enable)(struct hal_device *d, unsigned what);
    /* 装置の DMA と割り込みを止める。止まったと確かめられなければ false で、HAL はその DMA のメモリーを返さない。 */
    bool (*stop)(struct hal_device *d);
    /* プロセスの実装の口（hal/claim.c）。claim が NULL のバスはプロセスの実装を持てない。 */
    const struct hal_claim_ops *claim;
};

/* カーネルの実装。規則に合う装置ごとに attach が呼ばれる。
   attach：0 は持った。HAL_DECLINE は持たず次の実装へ回す。負は持てなかった理由で、N_HAL_FAILED になる。0 以外なら、
   HAL が割り込みを切り、DMA を止め、DMA のメモリーを返す。
   detach：装置が去る、または手放す。自分が hal_publish した子を hal_remove してから返る。戻った後、割り込みと DMA は
   HAL が片づける。NULL の実装は装置が去らないバスの上だけで動く。
   quiesce：DMA と割り込みを止める。false は止められなかった（眠りを取りやめる）。NULL は止めるものが無い。
   resume：眠りから戻す。false は戻らなかった。NULL は戻れない（その間、機械は眠らない）。
   refuse：今は眠れない理由（ログのため）か NULL。眠りのたびに聞く。
   cold_ok：resume が無くても、次の起動の後に元どおりになる（休止は妨げない）。 */
#define HAL_DECLINE 1
struct hal_driver {
    const char *name;
    const struct n_hal_rule *rules;
    uint32_t rule_count;
    int (*attach)(struct hal_device *d);
    void (*detach)(struct hal_device *d);
    bool (*quiesce)(struct hal_device *d);
    bool (*resume)(struct hal_device *d);
    const char *(*refuse)(struct hal_device *d);
    bool cold_ok;
};
#define HAL_DRIVER(driver) \
    static const struct hal_driver *const driver##_entry __attribute__((used, section(".hal_drivers"))) = &driver

/* 装置の記録。バスの物に埋め、寿命はその物と同じ。hal_remove が戻るまで生きている。 */
struct hal_device {
    /* バスが hal_publish の前に書く。後は読むだけ。錠なしで読める */
    uint32_t bus;
    const struct hal_bus_ops *ops;
    int node;            /* 近い NUMA node。-1：知らない */
    uint64_t dma_offset; /* バスから見た番地 - 物理番地 */
    struct n_hal_prop *props;
    uint8_t *prop_bytes;
    uint32_t prop_count, prop_bytes_used;
    struct n_hal_resource *resources;
    uint32_t resource_count;
    bool incomplete; /* hal_prop_* か hal_resource_add が記憶を得られなかった */
    char location[48];

    void *driver_data; /* 実装の物 */

    /* ここから下は hal/ だけが書く。読むのも hal/ だけ */
    uint32_t id;
    uint32_t state; /* enum n_hal_state */
    int32_t reason;
    char name[64], driver_name[48];
    const struct hal_driver *driver;
    struct hal_claim *claim;
    struct hal_device *parent, *first_child, *last_child, *prev, *next;
    struct hal_device *pending_next;
    struct hal_device *power_next, *power_prev;
    struct hal_offer_entry *offers;
    struct hal_depend_entry *depends;
    struct hal_dma_block *dma;
    struct mutex bind; /* attach、detach、claim、hal_remove を一度に一つにする */
    uint32_t offer_count, depend_count;
    bool queued, gone, listed, power_seen, power_down, irqs_on;
};

/* ---- バスの側 ----
   hal_publish は親の下に載せ、規則に合う実装があれば結ぶ。parent が NULL なら根の子。眠ってよい thread から。
   hal_remove は眠り、実装の detach と片づけが終わってから戻る。戻った後、バスは記録を消してよい。 */
void hal_device_init(struct hal_device *d, uint32_t bus, const struct hal_bus_ops *ops);
void hal_prop_number(struct hal_device *d, uint32_t key, uint64_t value);
void hal_prop_bytes(struct hal_device *d, uint32_t key, const void *bytes, uint32_t size);
void hal_prop_text(struct hal_device *d, uint32_t key, const char *text);
void hal_resource_add(struct hal_device *d, enum n_hal_resource_kind kind, uint32_t slot, uint32_t flags, uint64_t base,
                      uint64_t size);
void hal_location(struct hal_device *d, const char *text);
void hal_publish(struct hal_device *parent, struct hal_device *d);
void hal_remove(struct hal_device *d);
/* d はこの後、on より先に止まり、on より後に戻る（ACPI の _DEP）。眠ってよい thread から。 */
void hal_depends(struct hal_device *d, struct hal_device *on);
static inline uint32_t hal_id(const struct hal_device *d) { return d->id; }

/* ---- 実装の側 ---- */
/* 資源。slot の番号はバスが決める。無ければ NULL。 */
const struct n_hal_resource *hal_resource(const struct hal_device *d, enum n_hal_resource_kind kind, uint32_t slot);
/* メモリーの資源を丸ごと uncached で写す。無い、写せないなら NULL。返す番地を実装が直接読み書きする。 */
void *hal_map(struct hal_device *d, uint32_t slot);
/* ポートの資源の最初の番地。無ければ 0。 */
uint16_t hal_port(const struct hal_device *d, uint32_t slot);
void hal_enable(struct hal_device *d, unsigned what); /* HAL_ENABLE_* */
unsigned hal_irq_enable(struct hal_device *d, const struct irq_spec *specs, unsigned count, struct irq **irqs);
void hal_irq_disable(struct hal_device *d);
unsigned hal_irq_max(struct hal_device *d);
/* DMA のメモリー：装置に近い node から、ゼロで埋めて、連続で。bits は装置が読める番地の幅（0：制限なし）。*bus は
   装置へ渡す番地。実装が手放さなければ、detach の後に HAL が返す。 */
void *hal_dma_alloc(struct hal_device *d, size_t bytes, unsigned bits, uint64_t *bus);
void hal_dma_free(struct hal_device *d, void *memory);
/* 実装が別の道で得た物理番地を、装置へ渡す番地にする。データの道で呼んでよい。 */
static inline uint64_t hal_dma_address(const struct hal_device *d, uint64_t phys) { return phys + d->dma_offset; }

/* 実装の状態。どの文脈からでも呼べる。 */
void hal_name(struct hal_device *d, const char *name);
void hal_failed(struct hal_device *d, int reason); /* 動いた後に死んだ。実装は持ち続ける */
void hal_recovered(struct hal_device *d);          /* 立ち上げ直して戻った */
/* 提供するものを台帳に見せる。眠ってよい thread から。 */
void hal_offer(struct hal_device *d, uint32_t kind, uint32_t instance, const char *label);
void hal_offer_withdraw(struct hal_device *d, uint32_t kind, uint32_t instance);
/* 装置が去っている途中。長い attach と実装の thread が見て早く終わる。どの文脈からでも。 */
static inline bool hal_gone(const struct hal_device *d) { return __atomic_load_n(&d->gone, __ATOMIC_ACQUIRE); }

/* 実装が attach の後も別の thread で探している間（ポートの答えを待つ、USB の木を辿る）。hal_settled が見る。 */
void hal_looking_begin(void);
void hal_looking_end(void);
/* 実装の thread が装置に触る仕事の間。電源の変化はこれの終わりを待ち、これは電源の変化の終わりを待つ。 */
void hal_busy_begin(void);
void hal_busy_end(void);

/* ---- 全体 ---- */
void hal_init(void);
/* 実装を一つ足す。合う装置がもう居れば、その場で結ぶ。VM だけの実装は vm/vm.c がこれで足す。 */
void hal_driver_add(const struct hal_driver *driver);
void hal_start(void);    /* ここから実装を結ぶ。IOMMU の後 */
bool hal_settled(void);  /* 結ぶ途中のものも、探しているものも無い */
/* この性質の物に合うカーネルの実装があるか（USB の設定を選ぶ）。 */
bool hal_driver_exists(const struct n_hal_prop *props, uint32_t count, const uint8_t *bytes);

/* ---- 電源（dev/pm.c だけが呼ぶ） ----
   hal_power_begin は新しい attach と変更を止め、進行中のものを待ち、止める順の列を作る。hal_power_end で開く。
   間の hal_quiesce_all と hal_resume_all は列を錠なしで辿るので、割り込みを切っても他の CPU を止めても呼べる。
   順は子が先で親が後、依存する側が先。戻すのは逆。 */
void hal_power_begin(void);
void hal_power_end(void);
const char *hal_quiesce_all(bool stop_at_failure); /* NULL か、最初の止まらなかった実装の名 */
const char *hal_resume_all(void);                  /* NULL か、最初の戻らなかった実装の名 */
/* 眠りを妨げるものの名か NULL。hal_power_begin の間に呼ぶ。 */
enum hal_refusal {
    HAL_REFUSAL_NONE,
    HAL_REFUSAL_CANNOT,  /* 実装に resume が無い */
    HAL_REFUSAL_BUSY,    /* 実装の refuse が理由を言った */
    HAL_REFUSAL_PROCESS, /* 眠りに加わらないプロセスの実装が持っている */
};
const char *hal_power_refused(bool hibernate, enum hal_refusal *why);
/* 実装の resume の有無だけを見る。錠を取る。いつでも呼べる。 */
const char *hal_power_cannot(bool hibernate);

/* ---- プログラムへ ---- */
int64_t hal_syscall_list(void *user_out, uint64_t bytes);
int64_t hal_syscall_watch(void);

/* ---- プロセスの実装（hal/claim.c） ----
   hal_bind_process は装置を実装の無い状態からプロセスの持ち物にする。動いている実装があれば -N_ERR_BUSY。
   hal_unbind_process は実装の無い状態へ戻す。hal_remove はこの持ち物を hal_claim_revoke で終わらせる。 */
int hal_bind_process(struct hal_device *d, struct hal_claim *claim, const char *program);
void hal_unbind_process(struct hal_device *d);
void hal_claim_revoke(struct hal_claim *claim);
/* そのプロセスの実装が眠りの知らせを受ける（n_hal_power_wait を呼んだ）。 */
bool hal_claim_sleeps(const struct hal_claim *claim);
/* 全部のプロセスの実装へ「眠る」を知らせ、答えを期限つきで待つ（スレッドが動いている間）。答えない実装があれば
   その名を返し、眠りは取りやめる。hal_claims_resume は全部のスレッドが動いた後に「戻った」を知らせる。 */
const char *hal_claims_suspend(void);
void hal_claims_resume(void);
/* 番号で引く。眠らない。指す物の寿命は呼ぶ者が hal_remove との間で守る（hal/claim.c の順）。 */
struct hal_device *hal_find(uint32_t id);
