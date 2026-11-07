/* hal/ の中だけの共有。錠は葉で、下で何も取らない。実装の呼出しは錠の外で行う。 */
#pragma once
#include "arch/spin.h"
#include "io/io.h"

extern struct spinlock io_lock;
extern struct io_device io_root;

/* 装置が台帳に見せる提供と、止める順の依存（io_depends）。どちらも装置の鎖で、io_lock の下で変わる。 */
struct io_offer_entry {
    struct io_offer_entry *next;
    struct n_hal_offer offer;
};
struct io_depend_entry {
    struct io_depend_entry *next;
    struct io_device *on;
};

/* 根から順に全部を辿る次の一件。錠の下で。 */
struct io_device *io_walk_next(struct io_device *d);
void io_ledger_changed(void);

void io_bind_request(struct io_device *d); /* io/bind.c：結ぶ先を探す */
void pending_forget(struct io_device *d);   /* io/bind.c：結ぶのを待つ列から外す。io_lock の下 */
void io_release(struct io_device *d);      /* io/resource.c：実装が手放した後の片づけ */
/* io/dma.c：io_release の中で、装置の管理者を通した DMA のメモリーを外し、stopped（装置が止まったと確かめた）なら取った頁を
   返す。幅を 32 に戻す */
void io_dma_release(struct io_device *d, bool stopped);
/* io/bind.c の門：closing の間は新しい attach を起こさず、進行中のものの終わりを待つ。open で開き、待つ列を流す */
void io_gate_close(void);
void io_gate_open(void);
/* io/power.c：装置を D0 にする（firmware の D0、バスの D0）。実装か claim に渡る前と、眠りの戻りで。false：装置が
   D0 で答えない */
bool io_power_up(struct io_device *d);
/* io/claim.c：電源の walk の claim の知らせ。suspend は SUSPEND を渡して答えを待ち、止まったと答えたら true。眠りに
   加わらない claim と、答えずに終わった claim は false。resume は SUSPEND を渡した claim に RESUME を渡す */
bool io_claim_suspend(struct io_claim *claim);
void io_claim_resume(struct io_claim *claim);
