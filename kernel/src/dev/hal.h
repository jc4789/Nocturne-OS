/* 台帳：機械で見つかった物の一覧（docs/hal.md）。記録はバスの物に埋め、寿命はその物と同じ。指す側は
   番号を持つ。台帳は実装を呼ばず、順序を持たない。hal_add から hal_remove までは眠らず、どの文脈からでも呼べる。 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "abi.h"

struct hal_device {
    struct n_hal_device info; /* at はバスが hal_add の前に埋める。ほかは hal.c だけが書く */
    struct hal_device *next, *prev;
    bool listed;
};

void hal_init(void);
void hal_add(struct hal_device *d, uint32_t parent, enum n_hal_bus bus);
void hal_name(struct hal_device *d, const char *name);
/* 実装が持った。一つの物を複数の実装が持つ（USB の複合装置）なら名前を並べる。 */
void hal_bound(struct hal_device *d, const char *driver);
void hal_unbound(struct hal_device *d);
void hal_failed(struct hal_device *d, const char *driver, int reason);
/* 動いていた実装を立ち上げ直した結果：だめなら N_HAL_FAILED、戻ったなら失敗の記録を消す。 */
void hal_restarted(struct hal_device *d, const char *driver, bool up);
void hal_remove(struct hal_device *d); /* 載っていなければ何もしない */

int64_t hal_syscall_list(struct n_hal_device *user_out, uint64_t max);
int64_t hal_syscall_watch(void);
