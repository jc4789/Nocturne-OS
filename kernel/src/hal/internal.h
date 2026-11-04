/* hal/ の中だけの共有。錠は葉で、下で何も取らない。実装の呼出しは錠の外で行う。 */
#pragma once
#include "arch/spin.h"
#include "hal/hal.h"

extern struct spinlock hal_lock;
extern struct hal_device hal_root;

/* 根から順に全部を辿る次の一件。錠の下で。 */
struct hal_device *hal_walk_next(struct hal_device *d);
void hal_ledger_changed(void);

void hal_bind_request(struct hal_device *d); /* hal/bind.c：結ぶ先を探す */
void pending_forget(struct hal_device *d);   /* hal/bind.c：結ぶのを待つ列から外す。hal_lock の下 */
void hal_release(struct hal_device *d);      /* hal/resource.c：実装が手放した後の片づけ */
