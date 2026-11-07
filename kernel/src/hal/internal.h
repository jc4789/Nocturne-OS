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
