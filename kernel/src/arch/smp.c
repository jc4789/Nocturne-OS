/* AP1はuser runner、残りAPは純計算worker。共有kernelの実行/変更はBSP所有。 */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/fpu.h"
#include "arch/smp.h"
#include "limine.h"
#include "mm/vmm.h"
#include "abi.h"
#include "sys/sched.h"

#define AP_STACK_SIZE (32 * 1024)
#define START_TIMEOUT_MS 2000
#define JOB_TIMEOUT_MS 5000

struct cpu_worker {
    uint32_t apic_id;
    unsigned index;
    bool enabled;
    unsigned ready, failed, runner_ready;
    uint64_t epoch, done;
    cpu_job_fn fn;
    void *context;
    size_t begin, end;
    uint64_t items;
    uint64_t fault_vector, fault_rip, fault_addr, fault_error;
    uint8_t stack[AP_STACK_SIZE] __attribute__((aligned(16)));
} __attribute__((aligned(64)));

static struct cpu_worker workers[CPU_MAX_COUNT];
static unsigned slots = 1, online = 1;
static unsigned detected = 1;
static uint32_t bsp_apic_id;
static bool started, busy;
static uint64_t next_epoch, jobs;
static uint64_t max_job_ticks;
static uint64_t bsp_pat;
static bool have_pat;
static unsigned runner;

extern NORETURN void smp_switch_stack(uint64_t top, void (*entry)(void *), void *argument);

unsigned cpu_online_count(void) { return online; }
uint64_t cpu_parallel_jobs(void) { return jobs; }
bool cpu_jobs_active(void) { return busy; }
uint64_t cpu_max_job_us(void) { return tsc_hz >= 1000000 ? max_job_ticks / (tsc_hz / 1000000) : 0; }
void cpu_require_bsp(void) {
    /* AP の誤使用は共有ログ等を触る panic ではなく worker fault 経路で止める。 */
    if (cpu_current_index() != 0) __asm__ volatile("ud2" : : : "memory");
}
uint64_t cpu_worker_items(unsigned i) {
    return i < slots ? __atomic_load_n(&workers[i].items, __ATOMIC_ACQUIRE) : 0;
}

void smp_get_info(struct n_cpuinfo *info) {
    memset(info, 0, sizeof *info);
    info->version = 1;
    info->detected_cpus = detected;
    info->online_cpus = online;
    info->worker_cpus = cpu_worker_count();
    info->scheduler_cpus = cpu_runner_ready() ? 2 : 1;
    info->flags = N_CPU_SSE2 | (info->worker_cpus ? N_CPU_AP_WORKERS : 0) | (fpu_has_avx() ? N_CPU_AVX : 0);
    info->parallel_jobs = jobs;
    for (unsigned i = 1; i < slots; i++) info->worker_chunks += cpu_worker_items(i);
}

unsigned cpu_runner_index(void) { return __atomic_load_n(&runner, __ATOMIC_ACQUIRE); }
bool cpu_is_runner(void) { unsigned i = cpu_runner_index(); return i && cpu_current_index() == i; }
bool cpu_is_worker(void) { return cpu_current_index() != 0 && !cpu_is_runner(); }
unsigned cpu_worker_count(void) {
    unsigned count = 0;
    for (unsigned i = 1; i < slots; i++) if (workers[i].enabled) count++;
    return count;
}
bool cpu_runner_ready(void) {
    unsigned i = cpu_runner_index();
    return i && __atomic_load_n(&workers[i].runner_ready, __ATOMIC_ACQUIRE);
}
void cpu_runner_started(void) {
    __atomic_store_n(&workers[cpu_current_index()].runner_ready, 1, __ATOMIC_RELEASE);
}
void cpu_runner_wake(void) {
    unsigned i = cpu_runner_index();
    if (i && !lapic_send_ipi(workers[i].apic_id, VEC_USER_STOP))
        panic("smp: user runner IPI failed; context retained");
}
void cpu_runner_check(void) {
    unsigned i = cpu_runner_index();
    if (i && __atomic_load_n(&workers[i].failed, __ATOMIC_ACQUIRE)) {
        struct cpu_worker *w = &workers[i];
        panic("smp: user runner fault %lu rip=%p addr=%p err=%lx; context retained",
              w->fault_vector, (void *)w->fault_rip, (void *)w->fault_addr, w->fault_error);
    }
}

unsigned cpu_current_index(void) {
    if (!started) return 0;
    uint32_t id = lapic_current_id();
    for (unsigned i = 0; i < slots; i++) if (workers[i].apic_id == id) return i;
    return 0;
}

NORETURN void cpu_worker_fault(struct regs *r) {
    cli();
    struct cpu_worker *w = &workers[cpu_current_index()];
    w->fault_vector = r->vector;
    w->fault_rip = r->rip;
    w->fault_addr = read_cr2();
    w->fault_error = r->error;
    __atomic_store_n(&w->failed, 1, __ATOMIC_RELEASE);
    /* AP は共有ログ/GUI/current_task に触れない。BSP が故障を報告する。 */
    for (;;) hlt();
}

static bool deadline(uint64_t since, unsigned ms, uint64_t spins) {
    if (tsc_hz >= 1000000) return rdtsc() - since > (tsc_hz / 1000) * ms;
    return spins > 100000000;
}

static NORETURN void worker_main(void *argument) {
    struct cpu_worker *w = argument;
    cli();
    gdt_init_cpu(w->index);
    idt_load();
    if (!fpu_init_ap()) {
        __atomic_store_n(&w->failed, 1, __ATOMIC_RELEASE);
        for (;;) hlt();
    }
    if (pte_nx) wrmsr(0xC0000080, rdmsr(0xC0000080) | (1ULL << 11));
    write_cr0(read_cr0() | (1ULL << 16)); /* kernel write protection */
    if (have_pat && rdmsr(0x277) != bsp_pat) {
        __atomic_store_n(&w->failed, 1, __ATOMIC_RELEASE);
        for (;;) hlt();
    }
    write_cr3(kernel_pml4);
    if (!lapic_worker_init()) {
        __atomic_store_n(&w->failed, 1, __ATOMIC_RELEASE);
        for (;;) hlt();
    }
    __atomic_store_n(&w->ready, 1, __ATOMIC_RELEASE);
    uint64_t seen = 0;
    for (;;) {
        cli();
        if (w->index == cpu_runner_index()) {
            if (!lapic_runner_timer_start()) {
                __atomic_store_n(&w->failed, 1, __ATOMIC_RELEASE);
                for (;;) hlt();
            }
            sched_ap_loop();
        }
        uint64_t epoch = __atomic_load_n(&w->epoch, __ATOMIC_ACQUIRE);
        if (epoch == seen) {
            /* IF=0 の検査から sti;hlt までに IPI が来ても lost wake にならない。 */
            __asm__ volatile("sti; hlt" : : : "memory");
            continue;
        }
        /* BSP が増設した共有 page table と新しい vmalloc の stale TLB を除く。
           この callback 中は mapping の変更/解放も BSP の schedule も禁止。 */
        write_cr3(kernel_pml4);
        vmm_flush_all();
        fpu_reset();
        for (size_t i = w->begin; i < w->end; i++) w->fn(i, w->context);
        __asm__ volatile("sfence" : : : "memory"); /* framebuffer WC の書込完了 */
        __atomic_fetch_add(&w->items, w->end - w->begin, __ATOMIC_RELAXED);
        seen = epoch;
        __atomic_store_n(&w->done, epoch, __ATOMIC_RELEASE);
    }
}

static NORETURN void ap_entry(struct limine_mp_info *info) {
    struct cpu_worker *w = (void *)info->extra_argument;
    smp_switch_stack((uint64_t)w->stack + sizeof w->stack, worker_main, w);
}

static void range(size_t count, unsigned rank, unsigned participants, size_t *begin, size_t *end) {
    size_t each = count / participants, extra = count % participants;
    *begin = each * rank + MIN((size_t)rank, extra);
    *end = *begin + each + (rank < extra);
}

void cpu_parallel_for(size_t count, cpu_job_fn fn, void *context) {
    if (!count || !fn) return;
    cpu_require_bsp();
    uint64_t irq_flags = irq_save(), since_job = rdtsc();
    bool nested = busy;
    busy = true;
    struct fpu_state saved_fpu;
    fpu_save(&saved_fpu);
    fpu_reset();
    unsigned participants = cpu_worker_count() + 1;
    if (participants == 1 || nested || count < participants ||
        (uint64_t)fn < 0xFFFF800000000000ULL ||
        (context && (uint64_t)context < 0xFFFF800000000000ULL)) {
        for (size_t i = 0; i < count; i++) fn(i, context);
        __asm__ volatile("sfence" : : : "memory");
        fpu_restore(&saved_fpu);
        busy = nested;
        max_job_ticks = MAX(max_job_ticks, rdtsc() - since_job);
        irq_restore(irq_flags);
        return;
    }

    uint64_t epoch = ++next_epoch;
    unsigned rank = 1;
    /* BSP のみが descriptor を変更し、前 job 全員の join 後にだけ再利用する。 */
    for (unsigned i = 1; i < slots; i++) {
        struct cpu_worker *w = &workers[i];
        if (!w->enabled) continue;
        w->fn = fn;
        w->context = context;
        range(count, rank++, participants, &w->begin, &w->end);
        __atomic_store_n(&w->epoch, epoch, __ATOMIC_RELEASE);
        if (!lapic_send_ipi(w->apic_id, VEC_SMP_WAKE))
            panic("smp: worker %u IPI failed after job publication", i);
    }
    size_t begin, end;
    range(count, 0, participants, &begin, &end);
    for (size_t i = begin; i < end; i++) fn(i, context);
    __asm__ volatile("sfence" : : : "memory");
    __atomic_fetch_add(&workers[0].items, end - begin, __ATOMIC_RELAXED);
    uint64_t since = rdtsc(), spins = 0;
    for (unsigned i = 1; i < slots; i++) {
        struct cpu_worker *w = &workers[i];
        if (!w->enabled) continue;
        while (__atomic_load_n(&w->done, __ATOMIC_ACQUIRE) != epoch) {
            if (__atomic_load_n(&w->failed, __ATOMIC_ACQUIRE))
                panic("smp: worker %u fault %lu rip=%p addr=%p err=%lx; context retained", i,
                      w->fault_vector, (void *)w->fault_rip, (void *)w->fault_addr, w->fault_error);
            if (deadline(since, JOB_TIMEOUT_MS, ++spins))
                panic("smp: worker %u job timeout; context retained", i);
            pause();
        }
    }
    jobs++;
    busy = false;
    fpu_restore(&saved_fpu);
    max_job_ticks = MAX(max_job_ticks, rdtsc() - since_job);
    irq_restore(irq_flags);
}

struct probe {
    uint64_t value[257];
    uint64_t sse[257][2];
    uint8_t avx_ok[257];
    unsigned cpu[257];
};

static uint64_t probe_value(size_t i) {
    uint64_t v = i + 1;
    for (unsigned j = 0; j < 2048; j++) v = v * 6364136223846793005ULL + 1442695040888963407ULL;
    return v;
}

static void probe_job(size_t i, void *arg) {
    struct probe *p = arg;
    p->value[i] = probe_value(i);
    uint64_t words[2] = {i + 1, i + 17};
    /* compiler は general-regs-only。明示 SSE2 callback の実行も検査する。 */
    __asm__ volatile("movdqu %1, %%xmm0; paddq %%xmm0, %%xmm0; movdqu %%xmm0, %0"
                     : "=m"(p->sse[i]) : "m"(words) : "memory");
    if (fpu_has_avx()) {
        uint64_t in[4] = {i + 1, i + 17, i + 33, i + 49}, out[4];
        /* AVX1だけを使用（256bit整数算術/AVX2は要求しない）。復帰検査用に全YMM0を破壊。 */
        __asm__ volatile("vmovdqu %1, %%ymm0; vmovdqu %%ymm0, %0; vxorps %%ymm0, %%ymm0, %%ymm0"
                         : "=m"(out) : "m"(in) : "memory");
        p->avx_ok[i] = !memcmp(in, out, sizeof in);
    }
    p->cpu[i] = cpu_current_index();
}

static void rejected_ap_job(size_t i, void *arg) {
    (void)i;
    (void)arg;
    if (cpu_is_worker()) cpu_require_bsp();
}

static void selftest(void) {
    struct probe p;
    uint64_t reference[257];
    memset(&p, 0, sizeof p);
    uint64_t since = rdtsc();
    for (size_t i = 0; i < ARRAY_SIZE(reference); i++) reference[i] = probe_value(i);
    uint64_t serial_ticks = rdtsc() - since;
    struct fpu_state original_fpu, checked_fpu;
    uint64_t sentinel[4] = {0x123456789ABCDEF0ULL, 0xFEDCBA9876543210ULL,
                          0x0F0E0D0C0B0A0908ULL, 0x8877665544332211ULL};
    fpu_save(&original_fpu);
    if (fpu_has_avx()) __asm__ volatile("vmovdqu %0, %%ymm0" : : "m"(sentinel) : "memory");
    else __asm__ volatile("movdqu %0, %%xmm0" : : "m"(sentinel) : "memory");
    since = rdtsc();
    cpu_parallel_for(ARRAY_SIZE(p.value), probe_job, &p);
    uint64_t ticks = rdtsc() - since, mask = 0;
    fpu_save(&checked_fpu);
    if (memcmp(fpu_state_data(&checked_fpu) + 160, sentinel, 16)) panic("smp: BSP SSE state not restored");
    if (fpu_has_avx() && memcmp(fpu_state_data(&checked_fpu) + 576, sentinel + 2, 16))
        panic("smp: BSP YMM state not restored");
    fpu_restore(&original_fpu);
    for (size_t i = 0; i < ARRAY_SIZE(p.value); i++) {
        if (p.value[i] != reference[i] || p.sse[i][0] != (i + 1) * 2 || p.sse[i][1] != (i + 17) * 2 ||
            (fpu_has_avx() && !p.avx_ok[i]))
            panic("smp: selftest mismatch at %lu", i);
        mask |= 1ULL << p.cpu[i];
    }
    unsigned active = __builtin_popcountll(mask);
    if (active != online) panic("smp: selftest used %u of %u CPUs", active, online);
    kprintf("smp: selftest PASS items=257 active=%u mask=%lx serial_ticks=%lu parallel_ticks=%lu jobs=%lu SSE2=PASS\n",
            active, mask, serial_ticks, ticks, jobs);
    kprintf("smp: XSTATE restore PASS AVX=%s\n", fpu_has_avx() ? "PASS" : "disabled");
    for (unsigned i = 0; i < slots; i++) if (i == 0 || workers[i].enabled)
        kprintf("smp: cpu%u apic=%u items=%lu\n", i, workers[i].apic_id, cpu_worker_items(i));
    if (cmdline_has("smp-test")) {
        static const size_t counts[] = {0, 1, 3, 7, 16, 256, 257};
        for (unsigned round = 0; round < 32; round++) {
            for (size_t c = 0; c < ARRAY_SIZE(counts); c++) {
                memset(&p, 0, sizeof p);
                cpu_parallel_for(counts[c], probe_job, &p);
                for (size_t i = 0; i < counts[c]; i++)
                    if (p.value[i] != reference[i] || p.sse[i][0] != (i + 1) * 2 || p.sse[i][1] != (i + 17) * 2 ||
                        (fpu_has_avx() && !p.avx_ok[i]))
                        panic("smp: stress mismatch round=%u count=%lu index=%lu", round, counts[c], i);
                for (size_t i = counts[c]; i < ARRAY_SIZE(p.value); i++)
                    if (p.value[i] || p.sse[i][0] || p.sse[i][1] || p.avx_ok[i])
                        panic("smp: job wrote outside count=%lu", counts[c]);
            }
        }
        kprintf("smp: stress PASS rounds=32 boundary-counts=7 jobs=%lu max_job_us=%lu\n", jobs, cpu_max_job_us());
    }
    if (cmdline_has("smp-test-ap-fault") && online > 1) {
        kprintf("smp: negative test: rejecting BSP-only operation from AP\n");
        cpu_parallel_for(257, rejected_ap_job, &p);
        panic("smp: negative test failed to reject AP operation");
    }
}

void smp_init(struct limine_mp_response *response) {
    if (response && response->cpu_count) detected = (unsigned)MIN(response->cpu_count, 0xFFFFFFFFULL);
    if (!response || cmdline_has("nosmp") || !lapic_present() ||
        (response->flags & LIMINE_MP_RESPONSE_X86_64_X2APIC)) {
        kprintf("smp: single CPU fallback (%s)\n", cmdline_has("nosmp") ? "nosmp" : "MP/xAPIC unavailable");
        selftest();
        return;
    }
    bsp_apic_id = response->bsp_lapic_id;
    workers[0].apic_id = bsp_apic_id;
    workers[0].ready = 1;
    workers[0].enabled = true;
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    have_pat = (d & (1u << 16)) != 0;
    if (have_pat) bsp_pat = rdmsr(0x277);
    for (uint64_t i = 0; i < response->cpu_count && slots < CPU_MAX_COUNT; i++) {
        struct limine_mp_info *info = response->cpus[i];
        if (!info || info->lapic_id == bsp_apic_id || info->lapic_id > 255) continue;
        struct cpu_worker *w = &workers[slots];
        w->index = slots++;
        w->apic_id = info->lapic_id;
    }
    started = true;
    for (unsigned i = 1; i < slots; i++) {
        struct cpu_worker *w = &workers[i];
        struct limine_mp_info *info = NULL;
        for (uint64_t j = 0; j < response->cpu_count; j++)
            if (response->cpus[j] && response->cpus[j]->lapic_id == w->apic_id) info = response->cpus[j];
        info->extra_argument = (uint64_t)w;
        __atomic_store_n(&info->goto_address, ap_entry, __ATOMIC_RELEASE);
        uint64_t since = rdtsc(), spins = 0;
        while (!__atomic_load_n(&w->ready, __ATOMIC_ACQUIRE) &&
               !__atomic_load_n(&w->failed, __ATOMIC_ACQUIRE) &&
               !deadline(since, START_TIMEOUT_MS, ++spins)) pause();
        if (__atomic_load_n(&w->ready, __ATOMIC_ACQUIRE)) {
            w->enabled = true;
            online++;
        } else {
            kprintf("smp: cpu%u AP startup failed; excluded from jobs\n", i);
        }
    }
    kprintf("smp: %u/%lu CPUs online, initial AP worker selftest (kernel/IRQ BSP-owned)\n", online, response->cpu_count);
    selftest();
    /* 初期の全CPU job検査後に一つだけuser実行役へ移す。timer較正はBSPのみ。 */
    if (!cmdline_has("nousermp")) for (unsigned i = 1; i < slots; i++) {
        struct cpu_worker *w = &workers[i];
        if (!w->enabled) continue;
        w->enabled = false;
        __atomic_store_n(&runner, i, __ATOMIC_RELEASE);
        cpu_runner_wake();
        uint64_t since = rdtsc(), spins = 0;
        while (!cpu_runner_ready() && !__atomic_load_n(&w->failed, __ATOMIC_ACQUIRE) &&
               !deadline(since, START_TIMEOUT_MS, ++spins)) pause();
        if (!cpu_runner_ready()) {
            __atomic_store_n(&runner, 0, __ATOMIC_RELEASE);
            online--;
            kprintf("smp: user runner startup failed; CPU excluded\n");
        } else {
            kprintf("smp: user scheduler CPUs=2 runner=%u workers=%u; kernel/GUI BSP-owned\n", i, cpu_worker_count());
        }
        break;
    }
}
