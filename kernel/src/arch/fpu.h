#pragma once
#include <stdbool.h>
#include <stdint.h>

/* 標準XSAVEのx87/SSE/AVXだけ。AVX-512/AMX/compacted/supervisor stateは許可しない。
   heapは16B境界なので、保存域自身の内側で64B整列する。raw copyは不可。 */
#define FPU_STATE_BYTES 1024
struct fpu_state { uint8_t storage[FPU_STATE_BYTES + 63]; };

void fpu_init(void);             /* BSPがtask/AP起動前に不変policyを選択 */
bool fpu_init_ap(void);          /* 同じmask/標準layout/容量のAPだけを受け入れる */
bool fpu_has_avx(void);
void fpu_state_init(struct fpu_state *state);
void fpu_save(struct fpu_state *state);
void fpu_restore(const struct fpu_state *state);
void fpu_reset(void);
const uint8_t *fpu_state_data(const struct fpu_state *state);

/* 呼出し中のscheduleは禁止。通常kernel/ISRはgeneral-regs-onlyを維持する。
   SIMDを使うkernel callbackはcpu_parallel_forの同期所有区間だけで行う。 */
