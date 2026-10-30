#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CPU_MAX_COUNT 16
#define VEC_RESCHED 0x32       /* look at this CPU's run queue (sys/sched.c), or stop for a sleep */
#define VEC_TLB_SHOOTDOWN 0x35 /* mm/vmm.c */

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
struct n_cpuinfo;
typedef void (*cpu_job_fn)(size_t index, void *context);

/* Every CPU is the same: each runs the scheduler's tasks (sys/sched.c), takes its own traps,
   faults and system calls, serves interrupts sent to it and fires its own alarms (sys/alarm.c).
   The BSP is CPU 0 only in that it starts the others and takes the devices' interrupts. */
void smp_init(struct limine_mp_response *response);
unsigned cpu_online_count(void);
unsigned cpu_current_index(void);
bool cpu_online_at(unsigned index);
/* Interrupt the scheduler on that CPU (it looks at its run queue). Never the calling CPU. */
void cpu_kick(unsigned index);
bool cpu_tlb_ipi(unsigned index); /* false when that CPU is not running */
/* Interrupt that CPU's alarms (sys/alarm.c): it fires what is due and sets its wake device again. */
void cpu_alarm(unsigned index);
void smp_get_info(struct n_cpuinfo *info);
/* panic (lib/printf.c): true on the CPU that reports, every other CPU stopped first; false on one
   that is to halt (another CPU reports, or this one faulted while reporting too often). */
bool smp_panic_enter(void);
/* An NMI: a CPU stopped for another's panic never returns from it. */
void smp_panic_nmi(void);

/* cpu_parallel_for: fn(i, context) for every i below count, on this CPU and on the CPUs that are
   idle meanwhile, before it returns. Any CPU, any task; one batch at a time (a second caller, or
   a nested one, runs its batch alone). The callbacks run with interrupts off, on the idle task's
   stack of another CPU: they update only their own part of what context points at, and never
   allocate, wait, sleep, take a lock, call the scheduler or reenter. context and what it points
   at stay valid until the call returns; both live in the kernel's half. SSE2 may be used, AVX
   when N_CPU_AVX is published; the caller's XSTATE is saved and restored around its own share. */
void cpu_parallel_for(size_t count, cpu_job_fn fn, void *context);
/* An idle CPU, interrupts off: run any share of the batch under way. True when it ran some. */
bool cpu_jobs_poll(void);
uint64_t cpu_parallel_jobs(void);
uint64_t cpu_max_job_us(void);

/* Sleep: smp_suspend (CPU 0, interrupts on, every app stopped) takes every other CPU's tasks
   back to CPU 0 and halts each of those CPUs with interrupts off, false when one did not stop;
   smp_resume starts them again with INIT-SIPI, after S3 or after a sleep that failed.
   cpu_park_check is the idle loop's side, interrupts off. */
bool smp_suspend(void);
void smp_resume(void);
void cpu_park_check(void);
void smp_hold_in_init(void); /* after smp_suspend, for a hibernation restore: every AP to INIT */
