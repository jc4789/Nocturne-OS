#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CPU_MAX_COUNT 16
#define VEC_SMP_WAKE 0x32
#define VEC_USER_STOP 0x33
#define VEC_USER_RETURN 0x34
#define VEC_TLB_SHOOTDOWN 0x35

struct task;
/* GS is kernel-owned; user TLS remains in FS. The self pointer makes current
   task/reschedule access independent of APIC registers and CPU enumeration. */
struct cpu_local {
    struct cpu_local *self;
    unsigned index;
    struct task *task;
    volatile bool resched;
} __attribute__((aligned(64)));
static inline struct cpu_local *cpu_local_get(void) {
    struct cpu_local *local;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(local));
    return local;
}
void cpu_local_init(unsigned index); /* after loading this CPU's GDT, before IDT */

struct limine_mp_response;
struct regs;
struct n_cpuinfo;
typedef void (*cpu_job_fn)(size_t index, void *context);

/* 全APはuser実行と独立計算jobを交替で処理。共有kernel/driver/allocatorはBSP専用。
   例外はnet_ap_syscallの最大64KiB TCP ring copy。allocator/wait queue/NICには触れず、
   netのBSP gateとsocket io_ownerで寿命/単一所有を保ち、CPU別doorbellで制御を返す。
   user runnerはCPU別mailboxでring3のみ実行し、kernel処理前にBSPへ退避する。
   context と参照先は共有された高位 kernel mapping で、join まで有効であること。
   callback は非重複領域だけを更新し、割当・I/O・syscall・待機・schedule・再入は禁止。
   SSE2 は使用可能。AVX はN_CPU_AVX公開時だけ。BSPの全許可XSTATEを保存復元する。
   countに必要なAPだけを投入し、同期 join が終わるまで戻らない。
   起動失敗/単CPU/nosmp は逐次実行する。user taskを実行中のAPは止めず、空きAPだけを使う。
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
bool cpu_is_runner(void);
unsigned cpu_runner_index(void);
unsigned cpu_worker_count(void);
bool cpu_runner_ready(void);
bool cpu_runner_ready_at(unsigned index);
void cpu_runner_started(void); /* AP固定idle stack初期化後のready publication */
void cpu_runner_wake(void);
void cpu_runner_wake_at(unsigned index);
void cpu_bsp_wake(void); /* AP: a published retire ACK is ready for collection */
bool cpu_tlb_online(void);           /* BSP: any AP can hold TLB entries */
bool cpu_tlb_ipi(unsigned index);    /* BSP: false when that AP is not running */
bool cpu_worker_poll(void); /* AP idle stack only: execute a published bounded job */
bool cpu_worker_pending(void); /* AP: 公開済みで未実行のjobがある（user実行を退かせて受ける） */
void cpu_runner_check(void); /* BSP: failed APはcontextを保持してpanic */
bool cpu_jobs_active(void);
void cpu_require_bsp(void);
void cpu_worker_fault(struct regs *regs) __attribute__((noreturn));
