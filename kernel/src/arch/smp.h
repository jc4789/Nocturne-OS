#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CPU_MAX_COUNT 16
#define VEC_SMP_WAKE 0x32

struct limine_mp_response;
struct regs;
struct n_cpuinfo;
typedef void (*cpu_job_fn)(size_t index, void *context);

/* AP は独立した計算 worker。通常の task/IRQ/allocator は BSP 専用のまま。
   context と参照先は共有された高位 kernel mapping で、join まで有効であること。
   callback は非重複領域だけを更新し、割当・I/O・syscall・待機・schedule・再入は禁止。
   SSE2 は使用可能。BSP の呼出し元 FPU 状態は保存復元される。AVX は未対応。
   同期 join が終わるまで戻らない。起動失敗/単CPU/nosmp は逐次実行する。
   投入後の AP 故障は context を早期解放せず panic する。 */
void smp_init(struct limine_mp_response *response);
void cpu_parallel_for(size_t count, cpu_job_fn fn, void *context);
unsigned cpu_online_count(void);
unsigned cpu_current_index(void);
uint64_t cpu_parallel_jobs(void);
uint64_t cpu_worker_items(unsigned index);
uint64_t cpu_max_job_us(void);
void smp_get_info(struct n_cpuinfo *info);
bool cpu_is_worker(void);
bool cpu_jobs_active(void);
void cpu_require_bsp(void);
void cpu_worker_fault(struct regs *regs) __attribute__((noreturn));
