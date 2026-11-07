/* hal/ の中だけの共有。錠は葉で、下で何も取らない。実装の呼出しは錠の外で行う。 */
#pragma once
#include "arch/spin.h"
#include "hal/hal.h"

extern struct spinlock hal_lock;
extern struct hal_device hal_root;

/* 装置が台帳に見せる提供と、止める順の依存（hal_depends）。どちらも装置の鎖で、hal_lock の下で変わる。 */
struct hal_offer_entry {
    struct hal_offer_entry *next;
    struct n_hal_offer offer;
};
struct hal_depend_entry {
    struct hal_depend_entry *next;
    struct hal_device *on;
};

/* 根から順に全部を辿る次の一件。錠の下で。 */
struct hal_device *hal_walk_next(struct hal_device *d);
void hal_ledger_changed(void);

void hal_bind_request(struct hal_device *d); /* hal/bind.c：結ぶ先を探す */
void pending_forget(struct hal_device *d);   /* hal/bind.c：結ぶのを待つ列から外す。hal_lock の下 */
void hal_release(struct hal_device *d);      /* hal/resource.c：実装が手放した後の片づけ */
/* hal/bind.c の門：closing の間は新しい attach を起こさず、進行中のものの終わりを待つ。open で開き、待つ列を流す */
void hal_gate_close(void);
void hal_gate_open(void);
/* hal/power.c：装置を D0 にする（firmware の D0、バスの D0）。実装か claim に渡る前と、眠りの戻りで。false：装置が
   D0 で答えない */
bool hal_power_up(struct hal_device *d);
/* hal/claim.c：電源の walk の claim の知らせ。suspend は SUSPEND を渡して答えを待ち、止まったと答えたら true。眠りに
   加わらない claim と、答えずに終わった claim は false。resume は SUSPEND を渡した claim に RESUME を渡す */
bool hal_claim_suspend(struct hal_claim *claim);
void hal_claim_resume(struct hal_claim *claim);
