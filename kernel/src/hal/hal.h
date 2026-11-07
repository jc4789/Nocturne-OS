/* HAL：機械の全部の装置の木と、それを動かす実装の結びつき（docs/hal.md）。

   HAL が持つのは制御の道だけである。実装は attach で番地、割り込み、DMA の番地を受け取り、その後のデータの道は
   HAL を通らない。HAL は装置の種類もバスも知らない。装置の身元は性質で、規則で実装に結ぶ。

   どのバスも同じ形で装置を出す：
       hal_device_init(&x->hal, N_BUS_..., &x_bus_ops);
       hal_prop_number(&x->hal, N_PROP_..., value);
       hal_resource_add(&x->hal, N_HAL_MEMORY, slot, flags, base, size);
       hal_location(&x->hal, "...");
       x->hal.firmware = ...;  （firmware が記述する装置なら）
       hal_publish(parent, &x->hal);
   実装を足すのは、struct hal_driver を一つ書き、HAL_DRIVER(名前) を置くことである。

   機械の装置。核の部品が firmware の表から自分で見つけて起動の早い段で使う装置（基準の時計、割り込みの線、PCI の
   ホスト、電源のボタン、console のシリアル、IOMMU）は、その部品が struct hal_machine で宣言する。HAL は宣言に合う
   装置に実装を結ばず、claim させず、電源の段で呼ばない。台帳には持ち主の部品が動かしているものとして載る。

   電源。眠り、休止、電源断の装置の段は HAL の walk が持つ（hal_power_suspend）。動いている装置（カーネルの実装か
   claim を持つ装置）を、子と依存する側から止める。眠り（S3）では、止めた装置をバスの方法（hal_bus_ops.power）と
   firmware の方法（struct hal_firmware_ops）で D3 にし、戻りは D0 にしてから実装に戻させる。実装に渡る前（attach、
   claim）にも装置を D0 にする。

   firmware の節。struct firmware_node は、機械の firmware が装置を記述する節（ACPI の名前空間の Device）である。
   中身は firmware の持ち主（dev/acpi.c）だけが知り、HAL とバスは指を運ぶだけである。

   文脈。attach と detach は装置ごとの thread、suspend と resume は電源の walk の thread で走る。どれも割り込みは
   開いていて、全部の CPU が動いている。眠ってよく、秒の単位で待ってよく、自分の装置の割り込みを待ってよい。別の
   装置を待たせない。 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hal_buses.h"
#include "sys/wait.h"

struct hal_device;
struct hal_claim;
struct hal_claim_ops;
struct hal_offer_entry;
struct hal_depend_entry;
struct hal_dma_block;
struct firmware_node;
struct irq;
struct irq_spec;

#define HAL_ENABLE_MEMORY 1u /* メモリーの資源に答える */
#define HAL_ENABLE_PORTS 2u  /* ポートの資源に答える */
#define HAL_ENABLE_DMA 4u    /* 装置がメモリーへ読み書きする */

/* 装置の電源の状態（PCIe 6.0 5.3.1、ACPI 6.5 2.3 と 7.3）。D1 と D2 は使わない。D3hot の装置には電源があり、バスの
   構成の空間に答える。D3cold の装置には電源が無く、電源が戻ると初めから始まる。 */
enum hal_power_state { HAL_D0, HAL_D3HOT, HAL_D3COLD };

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
    /* 装置の電源の状態をバスの方法で変える（PCI は PMCSR）。眠ってよい。NULL：バスに電源の状態が無い。
       HAL_D3HOT：実装が止めた後に来る。バスの構成を保存してから移す。
       HAL_D3COLD：移す操作でなく知らせ。装置が電源を失ったか、失っていた電源が戻った（firmware の方法、機械の眠り）。
       次の HAL_D0 は電源の入りから数えて待つ。
       HAL_D0：D3 の後と、実装に渡る前に来る。装置が答えるまで待ち、D0 に移し、D3 から来たなら保存した構成を戻す。
       false：移らなかった（D3HOT なら構成を保存できない、D0 なら装置が答えない）。 */
    bool (*power)(struct hal_device *d, enum hal_power_state to);
};

/* firmware が記述する装置の電源（ACPI なら dev/acpi_power.c）。持ち主が hal_firmware_set で一度置く。
   power は節を HAL_D0 か HAL_D3COLD へ移す。HAL_D3COLD なら節を D3 にしてから、要らなくなった共有の資源を切る。HAL_D0
   なら要る資源を入れてから節を D0 にする（ACPI 6.5 7.2、7.3.8、7.3.11）。資源の順は節ごとの要る資源の中で守る。もう
   to にある節には何もしない。*cold：この呼出しで装置の電源が切れたか、切れていた電源が戻ったかもしれない（装置は
   電源の入りから始まる）。眠ってよい。電源の walk の装置ごとの thread と結ぶ前の thread から並んで来るので、持ち主が
   自分の錠で一つずつにする（ACPI 6.5 7.3.15 の _IRC も一つずつを求める）。false：移らなかった（持ち主がログに書く）。 */
struct hal_firmware_ops {
    bool (*power)(struct firmware_node *node, enum hal_power_state to, bool *cold);
};
void hal_firmware_set(const struct hal_firmware_ops *ops);

/* 眠りの目標（hal_power_suspend）。 */
enum hal_power_target {
    HAL_POWER_SLEEP, /* 機械が S3 で眠る。止めた後、装置は D3 になり電源を失う。resume が続く */
    HAL_POWER_IMAGE, /* 休止の写しを取る。電源は残り、resume が続く（写しから戻った後の resume も同じ） */
    HAL_POWER_OFF,   /* 電源が切れるか、機械が休止の写しのカーネルへ渡る。resume は来ない */
};

/* 眠りの後に実装が装置を戻せるか。宣言しない実装（0）は戻せない。 */
enum hal_after_sleep {
    HAL_AFTER_SLEEP_LOST,   /* 戻せない：結んでいる間、機械は眠らず、休止もしない */
    HAL_AFTER_SLEEP_BOOT,   /* 戻せないが、休止の後は次の起動の firmware が元どおりにする：休止だけを許す */
    HAL_AFTER_SLEEP_RESUME, /* suspend と resume で眠りと休止に加わる */
};

/* カーネルの実装。規則に合う装置ごとに attach が呼ばれる。
   attach：0 は持った。HAL_DECLINE は持たず次の実装へ回す。負は持てなかった理由で、N_HAL_FAILED になる。0 以外なら、
   HAL が割り込みを切り、DMA を止め、DMA のメモリーを返す。
   detach：装置が去る、または手放す。自分が hal_publish した子を hal_remove してから返る。戻った後、割り込みと DMA は
   HAL が片づける。NULL の実装は装置が去らないバスの上だけで動く。
   suspend：装置の仕事を止める。DMA と割り込みを静め、戻すのに要る状態を持ち、自分のデータの道（送信、要求、描画）
   を自分の錠で閉じる（ほかの thread は動いている）。true は止まった。false は止められなかった：眠りと休止は取りやめ
   になり、resume が呼ばれる。HAL_POWER_OFF では false でも電源は切れる。NULL は止めるものが無い。
   resume：suspend の後に戻す。HAL は先に装置を D0 にし、バスの構成（PCI なら BAR、命令、MSI と MSI-X）を戻してある。
   装置そのものは電源を失っていてもよく（眠りと休止の後）、そのままでもよい（取りやめと写しの後）。どちらでも元どおり
   にする。false は戻らなかった（実装は持ち続け、hal_failed で言う）。NULL はすることが無い。
   refuse：今は眠れない理由（ログのため）か NULL。眠りと休止のたびに聞く。
   after_sleep：enum hal_after_sleep。 */
#define HAL_DECLINE 1
struct hal_driver {
    const char *name;
    const struct n_hal_rule *rules;
    uint32_t rule_count;
    int (*attach)(struct hal_device *d);
    void (*detach)(struct hal_device *d);
    bool (*suspend)(struct hal_device *d, enum hal_power_target target);
    bool (*resume)(struct hal_device *d);
    const char *(*refuse)(struct hal_device *d);
    enum hal_after_sleep after_sleep;
};
#define HAL_DRIVER(driver) \
    static const struct hal_driver *const driver##_entry __attribute__((used, section(".hal_drivers"))) = &driver

/* 機械の装置の宣言。規則に合い、mine が true を返す装置を owner が持つ。mine が NULL なら、規則に合う装置は全部。
   mine は結ぶ時（hal_start の後）に呼ばれ、眠らない。持ち主はそれまでに自分の装置を見つけておく。 */
struct hal_machine {
    const char *owner;
    const struct n_hal_rule *rules;
    uint32_t rule_count;
    bool (*mine)(struct hal_device *d);
};
#define HAL_MACHINE(machine) \
    static const struct hal_machine *const machine##_entry __attribute__((used, section(".hal_machine"))) = &machine

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
    struct firmware_node *firmware; /* firmware が装置を記述する節。NULL：記述しない */

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
    struct hal_device *power_next; /* 電源の walk の動いている装置の鎖（hal/power.c） */
    uint32_t power_level;          /* その段 */
    struct hal_offer_entry *offers;
    struct hal_depend_entry *depends;
    struct hal_dma_block *dma;
    struct mutex bind; /* attach、detach、claim の始まり、hal_remove を一度に一つにする */
    uint32_t holds;    /* 記録を読んでいる attach の thread、claim、呼出しの数。hal_remove は 0 を待つ。hal_lock の下 */
    uint32_t offer_count, depend_count;
    bool queued, gone, listed, irqs_on;
    bool power_seen, power_suspended, power_lowered; /* walk が辿った、止めた、D3 にした */
    bool machine; /* 機械の装置（struct hal_machine） */
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
/* hal_device_init の後、hal_publish の前に出さないと決めた記録の性質と資源を返す。この後、バスは記録を消してよい。 */
void hal_device_discard(struct hal_device *d);
/* firmware の節 f が記述する装置。見つかれば記録を hold する（hal_put で放す）。去っている装置は見つからない。眠らない。 */
struct hal_device *hal_find_firmware(const struct firmware_node *f);
/* 木で d の上の装置の firmware の節。根の子か、上の装置を firmware が記述しなければ NULL。 */
struct firmware_node *hal_parent_firmware(const struct hal_device *d);
/* d はこの後、on より先に止まり、on より後に戻る（ACPI の _DEP）。眠ってよい thread から。 */
void hal_depends(struct hal_device *d, struct hal_device *on);
static inline uint32_t hal_id(const struct hal_device *d) { return d->id; }

/* ---- 実装の側 ---- */
/* 資源。slot の番号はバスが決める。無ければ NULL。 */
const struct n_hal_resource *hal_resource(const struct hal_device *d, enum n_hal_resource_kind kind, uint32_t slot);
/* kind の資源のどれかが address（メモリーかポートの番地、GSI の番号）を含むか。 */
bool hal_resource_holds(const struct hal_device *d, enum n_hal_resource_kind kind, uint64_t address);
/* メモリーの資源を丸ごと uncached で写す。無い、写せないなら NULL。返す番地を実装が直接読み書きする。 */
void *hal_map(struct hal_device *d, uint32_t slot);
/* ポートの資源の最初の番地。無ければ 0。 */
uint16_t hal_port(const struct hal_device *d, uint32_t slot);
void hal_enable(struct hal_device *d, unsigned what); /* HAL_ENABLE_* */
/* 割り込みを count まで、specs[i] が i 番目。バスが選ぶ形（PCI は MSI-X、MSI、INTx の順、ACPI は _CRS の GSI）で、
   向かう CPU は装置の node の中から選ぶ（spec の node は装置の node に置き換わる）。立てた数を返し、irqs があれば
   それぞれの irq を入れる。 */
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

/* ---- 電源（dev/pm.c と dev/hibernate.c が呼ぶ） ----
   hal_power_begin は新しい attach と変更を止め、進行中のものを待ち、装置の段を数える。hal_power_end で開く。
   段は、動いている装置だけを数えた、木の親と依存先（hal_depends）を辿る深さである。
   hal_power_suspend は動いている装置を深い段から止める。同じ段の装置は装置ごとの thread で同時に止まる。
   HAL_POWER_SLEEP では、止まった装置をバスと firmware の方法で D3 にする。止まらない装置があれば、その段の後で
   止め、その名を返す（HAL_POWER_OFF は全部を回り、最初の名を返す）。止まった装置はそのまま残る。
   hal_power_resume は止まった装置を浅い段から D0 にして戻す。取りやめでも眠りの後でも呼ぶ。slept：機械が眠った
   （D3 にした装置は電源を失った）。戻らない装置があれば最初の名を返す。どちらも hal_power_begin の間に、割り込みを
   開け、全部の CPU が動く中で呼ぶ。 */
void hal_power_begin(void);
void hal_power_end(void);
const char *hal_power_suspend(enum hal_power_target target);
const char *hal_power_resume(bool slept);
/* 眠りを妨げるものの名か NULL。hal_power_begin の間に呼ぶ。 */
enum hal_refusal {
    HAL_REFUSAL_NONE,
    HAL_REFUSAL_CANNOT,  /* 実装が眠りの後に装置を戻せない（after_sleep） */
    HAL_REFUSAL_BUSY,    /* 実装の refuse が理由を言った */
    HAL_REFUSAL_PROCESS, /* 眠りに加わらないプロセスの実装が持っている */
};
const char *hal_power_refused(bool hibernate, enum hal_refusal *why);
/* 結んでいる実装の after_sleep だけを見て、眠り（休止）の後に戻せない実装の名か NULL。錠を取る。いつでも呼べる
   （開始メニューは描くたびに聞く）。 */
const char *hal_power_cannot(bool hibernate);

/* ---- プログラムへ ---- */
int64_t hal_syscall_list(void *user_out, uint64_t bytes);
int64_t hal_syscall_watch(void);

/* ---- プロセスの実装（hal/claim.c） ----
   プロセスは n_hal_claim で装置を持ち、実装になる。カーネルの実装と同じ装置の記録に結び（hal_bind_process）、
   資源の写し、割り込み、DMA、台帳への提供、眠りの知らせは、バスを問わず claim が持つ。バスだけの操作は、バスが
   struct hal_bus_ops の claim に置く struct hal_claim_ops が受け持つ。claim が NULL のバスの装置は持てない。
   claim は N_KIND_HAL_CLAIM の object で、最後の handle が閉じると終わる。終わりは hal_remove からも起こる
   （hal_claim_end_device）：どちらも一度だけ走る。hal_remove は走っている呼出しの終わりを待ち、後の呼出しは
   N_ERR_CLOSED を返す。バスの call が長く眠るなら、hal_claim_revoked か end で起きること。 */
struct handle;
struct hal_claim_ops {
    /* バスの持ち分を始める。hal_bind_process の後に呼ぶ。眠ってよい。バスの claim ごとの記録を作って *bus に
       置き、0 を返す。負なら記録も装置への手出しも残さない。 */
    int (*begin)(struct hal_claim *c, void **bus);
    /* claim の終わり。写像は外れ、割り込みは止まっている。装置を止め、その DMA が二度とメモリーに届かないと
       確かめられたら true（DMA のメモリーを返してよい）。眠ってよい。 */
    bool (*end)(struct hal_claim *c);
    /* 記録の寿命の終わり：BAR と DMA の object も手放した。記録を返す。NULL は記録が無い。 */
    void (*release)(struct hal_claim *c);
    /* バスだけの操作（n_hal_bus_call）。in と out は核の写しで、out_size 以内の *used バイトを書いてよい。戻りは
       操作の結果（0 以上）か負の N_ERR_*。眠ってよい。NULL は操作が無い。 */
    int64_t (*call)(struct hal_claim *c, uint32_t op, const void *in, size_t in_size, void *out, size_t out_size,
                    size_t *used);
    /* 割り込みの数の上限。NULL は hal_irq_max。 */
    unsigned (*irq_max)(struct hal_claim *c);
    /* DMA：装置が届く番地の幅（64：制限なし）と、DMA の頁を装置へ見せる・外す（IOMMU）。dma_bits が NULL なら
       claim は DMA を作れない。unmap が false なら装置が頁にまだ届くかもしれない。眠ってよい。 */
    unsigned (*dma_bits)(struct hal_claim *c);
    bool (*dma_map)(struct hal_claim *c, const uint64_t *frames, uint64_t base, size_t count);
    bool (*dma_unmap)(struct hal_claim *c, const uint64_t *frames, uint64_t base, size_t count);
};
/* 資源とメモリーと割り込みだけで足りるバスの claim の操作。 */
extern const struct hal_claim_ops hal_claim_plain;

/* 呼ぶ側が持っている handle を受ける。眠ってよい thread から、spinlock を持たずに呼ぶ。 */
/* driver は台帳に見せる実装の名（claim を渡されて動くプログラムの名）。 */
int64_t hal_claim_open(uint32_t device, const char *driver, struct handle **out);
int64_t hal_claim_resources(struct handle *h, struct n_hal_resource *user_out, size_t max);
int64_t hal_claim_interrupts(struct handle *h, uint32_t count);
int64_t hal_claim_interrupt_take(struct handle *h, uint64_t deadline, uint64_t *user_bits, uint32_t words);
int64_t hal_claim_bus_call(struct handle *h, uint32_t op, const void *in, size_t in_size, void *out, size_t out_size,
                           size_t *used);
int64_t hal_claim_offer(struct handle *h, uint32_t kind, uint32_t instance, const char *label);
int64_t hal_claim_offer_withdraw(struct handle *h, uint32_t kind, uint32_t instance);
int64_t hal_claim_power_wait(struct handle *h, uint64_t deadline);
int64_t hal_claim_power_done(struct handle *h, bool ok);

/* claim の記録（mm/shared.c の BAR と DMA の object、dev/screen_driver.c、バス）。
   hal_claim_of：h が claim の handle ならその記録、でなければ NULL。参照は増やさない。
   hal_claim_hold：同じで参照を一つ増やす（h が持つ分に足す）。BAR と DMA の object が claim の寿命の間持つ。
   hal_claim_revoked：claim は終わった（最後の handle が閉じた、または装置が去った）。写像はもう持ち主のものでない。
   hal_claim_dma_reusable：残っている DMA の頁をメモリーへ返してよい（動いている claim か、装置が届かないと確かめて終わった）。
   hal_claim_memory：メモリーの資源 slot の番地と大きさ（頁に揃っていること）。
   hal_claim_device・hal_claim_bus：装置の記録と、バスが begin で置いた記録。claim の呼出しの間、装置の記録に触るなら
   hal_claim_enter で hold する。
   hal_claim_on_end：claim の終わりにバスが装置を止めた後、hook の ended を一度呼ぶ（画面を返す、保持を戻す）。終わりが
   もう始まっていれば false（hook は呼ばれない）。ended は終わりの途中で走り、眠ってよい。 */
struct hal_claim_end {
    struct hal_claim_end *next;
    void (*ended)(struct hal_claim_end *e, struct hal_claim *c);
};
struct shared_claim;
struct hal_claim *hal_claim_of(struct handle *h);
struct hal_claim *hal_claim_hold(struct handle *h);
void hal_claim_drop(struct hal_claim *c);
struct shared_claim *hal_claim_shared(struct hal_claim *c);
bool hal_claim_revoked(const struct hal_claim *c);
bool hal_claim_dma_reusable(const struct hal_claim *c);
unsigned hal_claim_dma_bits(const struct hal_claim *c);
bool hal_claim_dma_map(struct hal_claim *c, const uint64_t *frames, uint64_t base, size_t count);
bool hal_claim_dma_unmap(struct hal_claim *c, const uint64_t *frames, uint64_t base, size_t count);
int64_t hal_claim_memory(struct hal_claim *c, uint32_t slot, uint64_t *address, uint64_t *size, bool *prefetchable);
struct hal_device *hal_claim_device(const struct hal_claim *c);
/* claim の終わりが始まっていなければ装置の記録を hold して返す（hal_put で放す）。始まっていれば NULL。 */
struct hal_device *hal_claim_enter(struct hal_claim *c);
void *hal_claim_bus(const struct hal_claim *c);
bool hal_claim_on_end(struct hal_claim *c, struct hal_claim_end *e);

/* hal_bind_process は d->bind を持つ者が呼び、装置を実装の無い状態からプロセスの持ち物にする。動いている実装があれば
   -N_ERR_BUSY。持ち物の間、記録は hold されている。hal_unbind_process は実装の無い状態へ戻し、その hold を放す：
   呼んだ後、呼ぶ側は記録に触らない。hal_remove は持ち物を hal_claim_end_device で終わらせる。 */
int hal_bind_process(struct hal_device *d, struct hal_claim *claim, const char *program);
void hal_unbind_process(struct hal_device *d);
void hal_claim_end_device(struct hal_device *d);

/* 眠り。n_hal_power_wait を一度でも呼んだ claim は眠りの知らせを受ける（hal_claim_sleeps）。呼ばない claim を
   持つ装置は眠りと休止を断る（HAL_REFUSAL_PROCESS）。知らせは電源の walk が渡す（hal/power.c）。 */
bool hal_claim_sleeps(const struct hal_claim *claim);
/* 番号で引き、見つかれば記録を hold する。去っている装置は見つからない。眠らない。hal_put で放す。 */
struct hal_device *hal_find(uint32_t id);
void hal_put(struct hal_device *d);
