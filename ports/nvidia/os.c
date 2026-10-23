/* NVIDIA RM on Nocturne: the general OS services RM asks for.
 *
 * Memory, strings, time and delays, locks, semaphores, wait queues, the work queue, RC and
 * nano timers, logging, the registry table, GSP firmware loading, and the answers to the
 * "what kind of OS is this" questions. PCI, BARs, DMA memory and interrupts are in os_pci.c;
 * the callbacks Nocturne has no equivalent for are in os_stubs.c.
 *
 * Only Nocturne's native ABI and ISO C are used: malloc, <threads.h>, timespec_get,
 * wait_on_address (through C11 threads), n_document_load, n_system_log_open.
 */
#define NV_FIRMWARE_FOR_NAME(name) name
#include "nvrm_os.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <os-interface.h>
#include <nv-firmware.h>

/* RM entry points the OS layer calls back into. */
void rm_execute_work_item(nvidia_stack_t *sp, void *pNvWorkItem);
NV_STATUS rm_run_rc_callback(nvidia_stack_t *sp, nv_state_t *nv);
NV_STATUS rm_run_nano_timer_callback(nvidia_stack_t *sp, nv_state_t *nv, void *pTmrEvent);

/* ------------------------------------------------------------------------------------------
 * Registry keys (RM's NVreg_* options) and the table rm_parse_option_string searches
 * ---------------------------------------------------------------------------------------- */
#define NV_DEFINE_REGISTRY_KEY_TABLE
#include <nv-reg.h>

/* ------------------------------------------------------------------------------------------
 * Constants and variables RM reads
 * ---------------------------------------------------------------------------------------- */
NvU64 os_page_size = 4096;
NvU64 os_max_page_size = 4096;
NvU64 os_page_mask = ~(NvU64)4095;
NvU8 os_page_shift = 12;
NvBool os_cc_enabled = NV_FALSE;
NvBool os_cc_sev_snp_enabled = NV_FALSE;
NvBool os_cc_sme_enabled = NV_FALSE;
NvBool os_cc_snp_vtom_enabled = NV_FALSE;
NvBool os_cc_tdx_enabled = NV_FALSE;
NvBool os_dma_buf_enabled = NV_FALSE;
NvBool os_imex_channel_is_supported = NV_FALSE;
nv_cap_t *nvidia_caps_root = NULL;

thread_local bool nvrm_in_isr = false;
thread_local bool nvrm_thread_admin = true;
thread_local uint32_t nvrm_thread_process;

/* ------------------------------------------------------------------------------------------
 * Logging: the system log, and nvrm.log in nvrm's data folder (C:\Users\<name>\AppData\NVIDIA),
 * which stays from one start to the next
 * ---------------------------------------------------------------------------------------- */
#define NVRM_LOG_CHUNK 900          /* bytes per system log message */
#define NVRM_LOG_FILE_BUFFER 8192

static mtx_t log_lock;
static once_flag log_once = ONCE_FLAG_INIT;
static n_handle log_handle = -1;
static char log_path[1024];
static bool log_file_enabled;
static char log_file_buffer[NVRM_LOG_FILE_BUFFER];
static size_t log_file_fill;
static uint64_t log_file_flushed_ms;

static void log_file_flush_locked(void) {
    if (!log_file_enabled || !log_file_fill) return;
    n_document_append(log_path, log_file_buffer, log_file_fill);
    log_file_fill = 0;
    log_file_flushed_ms = n_clock_uptime_ms();
}

static void log_setup(void) {
    mtx_init(&log_lock, mtx_plain);
    log_handle = n_system_log_open();
    char folder[sizeof log_path];
    log_file_enabled = n_app_data_folder("NVIDIA", folder, sizeof folder) >= 0 &&
                       n_path_join(folder, "nvrm.log", log_path, sizeof log_path) >= 0;
    log_file_flushed_ms = n_clock_uptime_ms();
}

void nvrm_log_init(void) { call_once(&log_once, log_setup); }

static void log_emit(const char *text, size_t length, bool urgent) {
    call_once(&log_once, log_setup);
    while (length && (text[length - 1] == '\n' || text[length - 1] == '\r')) length--;
    mtx_lock(&log_lock);
    if (log_handle >= 0) {
        /* The system log keeps what it is sent as is: each line ends with its own newline. */
        char piece[NVRM_LOG_CHUNK + 1];
        size_t at = 0;
        do {
            size_t part = length - at < NVRM_LOG_CHUNK ? length - at : NVRM_LOG_CHUNK;
            memcpy(piece, text + at, part);
            at += part;
            if (at == length) piece[part++] = '\n';
            n_channel_send(log_handle, piece, part, -1);
        } while (at < length);
    }
    if (log_file_enabled) {
        size_t need = length + 1;
        if (log_file_fill + need > sizeof log_file_buffer) log_file_flush_locked();
        if (need <= sizeof log_file_buffer) {
            memcpy(log_file_buffer + log_file_fill, text, length);
            log_file_buffer[log_file_fill + length] = '\n';
            log_file_fill += need;
        } else {
            n_document_append(log_path, text, length);
        }
        if (urgent || n_clock_uptime_ms() - log_file_flushed_ms >= 200) log_file_flush_locked();
    }
    mtx_unlock(&log_lock);
}

void nvrm_log_raw(const char *text, size_t length) { log_emit(text, length, false); }

void nvrm_log_flush(void) {
    call_once(&log_once, log_setup);
    mtx_lock(&log_lock);
    log_file_flush_locked();
    mtx_unlock(&log_lock);
}

void nvrm_log(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    log_emit(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1, true);
}

int NV_API_CALL nv_vprintf(NvU32 debuglevel, const char *fmt, va_list ap) {
    char line[1024];
    int n = vsnprintf(line, sizeof line, fmt, ap);
    if (n < 0) return n;
    size_t length = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
    log_emit(line, length, debuglevel >= NV_DBG_WARNINGS && debuglevel != 0xFFFFFFFFu);
    return n;
}

int NV_API_CALL nv_printf(NvU32 debuglevel, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = nv_vprintf(debuglevel, fmt, ap);
    va_end(ap);
    return n;
}

int NV_API_CALL nv_dev_vprintf(struct nv_state_t *nv, NV_LOG_LEVEL level, const char *fmt, va_list ap) {
    (void)nv;
    return nv_vprintf(level >= NV_LOG_LEVEL_WARNING ? NV_DBG_ERRORS : NV_DBG_INFO, fmt, ap);
}

void NV_API_CALL out_string(const char *str) { log_emit(str, strlen(str), false); }

void NV_API_CALL os_log_error(const char *fmt, va_list ap) { nv_vprintf(NV_DBG_ERRORS, fmt, ap); }

NV_STATUS NV_API_CALL nv_log_error(nv_state_t *nv, NvU32 error_number, const char *fmt, va_list ap) {
    (void)nv;
    char line[1024];
    int n = vsnprintf(line, sizeof line, fmt, ap);
    if (n < 0) return NV_ERR_GENERIC;
    nvrm_log("NVRM: Xid %u: %s", (unsigned)error_number, line);
    return NV_OK;
}

/* nv_printf logs everything it is given: RM filters NV_PRINTF itself (nvDbg_PrintMsg), so the
   level the ResmanDebugLevel registry key passes here is not used. */
void NV_API_CALL os_dbg_init(void) {}
void NV_API_CALL os_dbg_set_level(NvU32 level) { (void)level; }
void NV_API_CALL os_dbg_breakpoint(void) { nvrm_log("NVRM: os_dbg_breakpoint (ignored)"); }
/* RM asks for the stack when it reports a failed assertion or a breakpoint. nvrm is built with
   frame pointers (nvrm.mk) and linked at a fixed address, so the saved frame pointers chain
   through this thread's stack and each return address is one of the nvrm binary in the build
   (addr2line -e nvrm). The walk stays inside the stack and only moves toward its base, so a
   damaged chain ends it instead of faulting or looping. */
void NV_API_CALL os_dump_stack(void) {
    uintptr_t low = 0, high = 0;
    if (thread_stack_bounds(&low, &high) < 0) {
        nvrm_log("NVRM: stack: this thread's stack is unknown");
        return;
    }
    char line[256];
    int used = snprintf(line, sizeof line, "NVRM: stack:");
    const uintptr_t *frame = __builtin_frame_address(0);
    for (;;) {
        uintptr_t at = (uintptr_t)frame;
        /* at > high first: high - at is unsigned and would wrap past the top instead of failing */
        if (at % sizeof(uintptr_t) || at < low || at > high || high - at < 2 * sizeof(uintptr_t)) break;
        uintptr_t return_address = frame[1];
        if (!return_address) break;
        if (used > (int)sizeof line - 24) {
            nvrm_log("%s", line);
            used = snprintf(line, sizeof line, "NVRM: stack (more):");
        }
        used += snprintf(line + used, sizeof line - (size_t)used, " %#llx", (unsigned long long)return_address);
        const uintptr_t *caller = (const uintptr_t *)frame[0];
        if ((uintptr_t)caller <= at) break;
        frame = caller;
    }
    nvrm_log("%s", line);
}
void NV_API_CALL os_bug_check(NvU32 code, const char *message) {
    nvrm_log("NVRM: bug check 0x%x: %s", (unsigned)code, message ? message : "");
    nvrm_log_flush();
    abort();
}

/* ------------------------------------------------------------------------------------------
 * Memory
 * ---------------------------------------------------------------------------------------- */
NV_STATUS NV_API_CALL os_alloc_mem(void **address, NvU64 size) {
    if (!address) return NV_ERR_INVALID_ARGUMENT;
    void *p = malloc(size ? (size_t)size : 1);
    *address = p;
    return p ? NV_OK : NV_ERR_NO_MEMORY;
}

void NV_API_CALL os_free_mem(void *address) { free(address); }

void *NV_API_CALL os_mem_copy(void *dst, const void *src, NvU32 length) { return memcpy(dst, src, length); }
void *NV_API_CALL os_mem_set(void *dst, NvU8 value, NvU32 length) { return memset(dst, value, length); }
NvS32 NV_API_CALL os_mem_cmp(const NvU8 *a, const NvU8 *b, NvU32 length) { return memcmp(a, b, length); }

/* Client pointers in a request point into the arena the client shared with nvrm; the request
   server (nvrm_serve.c) translates them and refuses any address outside that arena. */

NV_STATUS NV_API_CALL os_memcpy_from_user(void *to, const void *from, NvU32 n) {
    if (!n) return NV_OK;
    const void *src = nvrm_user_translate(from, n);
    if (!src || !to) return NV_ERR_INVALID_ADDRESS;
    memcpy(to, src, n);
    return NV_OK;
}

NV_STATUS NV_API_CALL os_memcpy_to_user(void *to, const void *from, NvU32 n) {
    if (!n) return NV_OK;
    void *dst = nvrm_user_translate(to, n);
    if (!dst || !from) return NV_ERR_INVALID_ADDRESS;
    memcpy(dst, from, n);
    return NV_OK;
}

/* ------------------------------------------------------------------------------------------
 * Strings
 * ---------------------------------------------------------------------------------------- */
char *NV_API_CALL os_string_copy(char *dst, const char *src) { return strcpy(dst, src); }
NvU32 NV_API_CALL os_string_length(const char *s) { return (NvU32)strlen(s); }
NvU32 NV_API_CALL os_strtoul(const char *s, char **end, NvU32 base) { return (NvU32)strtoul(s, end, (int)base); }
NvS32 NV_API_CALL os_string_compare(const char *a, const char *b) { return strcmp(a, b); }

NvS32 NV_API_CALL os_vsnprintf(char *buf, NvU32 size, const char *fmt, va_list ap) {
    return vsnprintf(buf, size, fmt, ap);
}

NvS32 NV_API_CALL os_snprintf(char *buf, NvU32 size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* ------------------------------------------------------------------------------------------
 * Time
 *
 * timespec_get has millisecond resolution; RM measures microseconds. The system offers its
 * TSC rate and a (uptime, tsc) sample (clock_info); with them nvrm_now_ns is exact to the TSC.
 * ---------------------------------------------------------------------------------------- */
static once_flag clock_once = ONCE_FLAG_INIT;
static struct n_clockinfo clock_base;
static bool clock_tsc;
static uint64_t clock_last_ns;

static void clock_setup(void) {
    memset(&clock_base, 0, sizeof clock_base);
    if (clock_info(&clock_base) >= 0 && (clock_base.flags & N_CLOCK_USER_TSC) && clock_base.tsc_hz >= 1000000)
        clock_tsc = true;
}

static inline uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return (uint64_t)hi << 32 | lo;
}

uint64_t nvrm_now_ns(void) {
    call_once(&clock_once, clock_setup);
    uint64_t ns;
    uint64_t tsc;
    if (clock_tsc && (tsc = read_tsc()) >= clock_base.tsc_sample) {
        uint64_t delta = tsc - clock_base.tsc_sample;
        uint64_t hz = clock_base.tsc_hz;
        ns = clock_base.uptime_ms * 1000000ull + (delta / hz) * 1000000000ull +
             (delta % hz) * 1000000000ull / hz;
    } else {
        ns = n_clock_uptime_ms() * 1000000ull;
    }
    uint64_t last = __atomic_load_n(&clock_last_ns, __ATOMIC_RELAXED);
    while (ns > last &&
           !__atomic_compare_exchange_n(&clock_last_ns, &last, ns, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    return ns < last ? last : ns;
}

NvU64 NV_API_CALL os_get_monotonic_time_ns(void) { return nvrm_now_ns(); }
NvU64 NV_API_CALL os_get_monotonic_time_ns_hr(void) { return nvrm_now_ns(); }
NvU64 NV_API_CALL os_get_monotonic_tick_resolution_ns(void) {
    call_once(&clock_once, clock_setup);
    return clock_tsc ? 1 : 1000000;
}

NV_STATUS NV_API_CALL os_get_system_time(NvU32 *seconds, NvU32 *useconds) {
    struct timespec ts;
    if (!timespec_get(&ts, TIME_UTC)) return NV_ERR_GENERIC;
    if (seconds) *seconds = (NvU32)ts.tv_sec;
    if (useconds) *useconds = (NvU32)(ts.tv_nsec / 1000);
    return NV_OK;
}

NvU64 NV_API_CALL os_get_cpu_frequency(void) {
    call_once(&clock_once, clock_setup);
    return clock_tsc ? clock_base.tsc_hz : 0;
}

/* A delay of a few microseconds is a spin on the clock (RM calls it with locks held and from
   the interrupt thread); a long one in a normal thread sleeps. */
static void delay_ns(uint64_t ns, bool may_sleep) {
    uint64_t deadline = nvrm_now_ns() + ns;
    for (;;) {
        uint64_t now = nvrm_now_ns();
        if (now >= deadline) return;
        uint64_t left = deadline - now;
        if (may_sleep && left > 2000000ull) {
            n_sleep_ms((left - 1000000ull) / 1000000ull);
        } else {
            for (int i = 0; i < 64; i++) __builtin_ia32_pause();
        }
    }
}

NV_STATUS NV_API_CALL os_delay(NvU32 ms) {
    delay_ns((uint64_t)ms * 1000000ull, !nvrm_in_isr);
    return NV_OK;
}

NV_STATUS NV_API_CALL os_delay_us(NvU32 us) {
    delay_ns((uint64_t)us * 1000ull, !nvrm_in_isr && us >= 2000);
    return NV_OK;
}

NV_STATUS NV_API_CALL os_schedule(void) {
    thrd_yield();
    return NV_OK;
}

/* ------------------------------------------------------------------------------------------
 * Process, thread and CPU identity
 * ---------------------------------------------------------------------------------------- */
NvU32 NV_API_CALL os_get_current_process(void) {
    if (nvrm_thread_process) return nvrm_thread_process;
    n_result id = n_app_self();
    return id > 0 && (uint64_t)id <= UINT32_MAX ? (NvU32)id : 0;
}

void NV_API_CALL os_get_current_process_name(char *name, NvU32 length) {
    if (!name || !length) return;
    strncpy(name, "nvrm", length);
    name[length - 1] = 0;
}

NV_STATUS NV_API_CALL os_get_current_thread(NvU64 *thread) {
    if (!thread) return NV_ERR_INVALID_ARGUMENT;
    *thread = (NvU64)thread_id();
    return NV_OK;
}

NvU32 NV_API_CALL os_get_current_process_flags(void) { return OS_CURRENT_PROCESS_FLAG_NONE; }

NvU32 NV_API_CALL os_get_cpu_count(void) {
    struct n_cpuinfo ci;
    if (cpu_info(&ci) >= 0 && ci.online_cpus) return ci.online_cpus;
    return 1;
}

NvU32 NV_API_CALL os_get_cpu_number(void) { return cpu_index(); }

/* RM's cpu.c reads CR4 to learn whether the OS saves SSE and AVX state (OSFXSR, bit 9, and
   OSXSAVE, bit 18). Its own nv_rdcr.c does that with "mov %cr4", which a program cannot run
   (nvrm.mk leaves the file out). The same two answers are visible from here: every Nocturne
   program runs SSE code, so OSFXSR is on, and CPUID.1:ECX bit 27 mirrors CR4.OSXSAVE. */
NvU32 osNv_rdcr4(void);
NvU32 osNv_rdcr4(void) {
    uint32_t a = 1, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return 1u << 9 | (c & 1u << 27 ? 1u << 18 : 0);
}

NvU64 NV_API_CALL os_get_max_user_va(void) { return (1ull << 47) - 4096; }

NvBool NV_API_CALL os_is_administrator(void) { return nvrm_thread_admin ? NV_TRUE : NV_FALSE; }
NvBool NV_API_CALL os_check_access(RsAccessRight right) {
    (void)right;
    return nvrm_thread_admin ? NV_TRUE : NV_FALSE;
}
NV_STATUS NV_API_CALL os_get_euid(NvU32 *euid) {
    if (!euid) return NV_ERR_INVALID_ARGUMENT;
    *euid = nvrm_thread_admin ? 0 : 1000;
    return NV_OK;
}

NvBool NV_API_CALL os_is_isr(void) { return nvrm_in_isr ? NV_TRUE : NV_FALSE; }
NvBool NV_API_CALL os_semaphore_may_sleep(void) { return nvrm_in_isr ? NV_FALSE : NV_TRUE; }

NV_STATUS NV_API_CALL os_get_version_info(os_version_info *info) {
    if (!info) return NV_ERR_INVALID_ARGUMENT;
    info->os_major_version = 0;
    info->os_minor_version = 1;
    info->os_build_number = 0;
    info->os_build_version_str = "Nocturne OS";
    info->os_build_date_plus_str = "";
    return NV_OK;
}

NV_STATUS NV_API_CALL os_get_is_openrm(NvBool *is_openrm) {
    if (!is_openrm) return NV_ERR_INVALID_ARGUMENT;
    *is_openrm = NV_TRUE;
    return NV_OK;
}

NV_STATUS NV_API_CALL os_get_random_bytes(NvU8 *bytes, NvU16 n) {
    if (!bytes) return NV_ERR_INVALID_ARGUMENT;
    return n_random(bytes, n) >= 0 ? NV_OK : NV_ERR_GENERIC;
}

NV_STATUS NV_API_CALL os_registry_init(void) { return NV_OK; }

/* ------------------------------------------------------------------------------------------
 * Locks
 *
 * RM's "spinlock" protects short sections that are also taken by the interrupt handler. RM
 * runs in ordinary threads here (the interrupt handler is a thread too), so a mutex is the
 * right primitive; the returned "irql" is unused.
 * ---------------------------------------------------------------------------------------- */
NV_STATUS NV_API_CALL os_alloc_spinlock(void **lock) {
    mtx_t *m = malloc(sizeof *m);
    if (!m) return NV_ERR_NO_MEMORY;
    mtx_init(m, mtx_plain);
    *lock = m;
    return NV_OK;
}
void NV_API_CALL os_free_spinlock(void *lock) {
    if (!lock) return;
    mtx_destroy(lock);
    free(lock);
}
NvU64 NV_API_CALL os_acquire_spinlock(void *lock) {
    mtx_lock(lock);
    return 0;
}
void NV_API_CALL os_release_spinlock(void *lock, NvU64 irql) {
    (void)irql;
    mtx_unlock(lock);
}

NV_STATUS NV_API_CALL os_alloc_mutex(void **mutex) {
    mtx_t *m = malloc(sizeof *m);
    if (!m) return NV_ERR_NO_MEMORY;
    mtx_init(m, mtx_plain);
    *mutex = m;
    return NV_OK;
}
void NV_API_CALL os_free_mutex(void *mutex) {
    if (!mutex) return;
    mtx_destroy(mutex);
    free(mutex);
}
NV_STATUS NV_API_CALL os_acquire_mutex(void *mutex) {
    return mtx_lock(mutex) == thrd_success ? NV_OK : NV_ERR_GENERIC;
}
NV_STATUS NV_API_CALL os_cond_acquire_mutex(void *mutex) {
    return mtx_trylock(mutex) == thrd_success ? NV_OK : NV_ERR_TIMEOUT_RETRY;
}
void NV_API_CALL os_release_mutex(void *mutex) { mtx_unlock(mutex); }

struct nvrm_semaphore {
    mtx_t lock;
    cnd_t cv;
    NvU32 count;
};

void *NV_API_CALL os_alloc_semaphore(NvU32 initial) {
    struct nvrm_semaphore *s = malloc(sizeof *s);
    if (!s) return NULL;
    mtx_init(&s->lock, mtx_plain);
    cnd_init(&s->cv);
    s->count = initial;
    return s;
}
void NV_API_CALL os_free_semaphore(void *semaphore) {
    struct nvrm_semaphore *s = semaphore;
    if (!s) return;
    cnd_destroy(&s->cv);
    mtx_destroy(&s->lock);
    free(s);
}
NV_STATUS NV_API_CALL os_acquire_semaphore(void *semaphore) {
    struct nvrm_semaphore *s = semaphore;
    mtx_lock(&s->lock);
    while (s->count == 0) cnd_wait(&s->cv, &s->lock);
    s->count--;
    mtx_unlock(&s->lock);
    return NV_OK;
}
NV_STATUS NV_API_CALL os_cond_acquire_semaphore(void *semaphore) {
    struct nvrm_semaphore *s = semaphore;
    NV_STATUS status = NV_ERR_TIMEOUT_RETRY;
    mtx_lock(&s->lock);
    if (s->count) {
        s->count--;
        status = NV_OK;
    }
    mtx_unlock(&s->lock);
    return status;
}
NV_STATUS NV_API_CALL os_release_semaphore(void *semaphore) {
    struct nvrm_semaphore *s = semaphore;
    mtx_lock(&s->lock);
    s->count++;
    mtx_unlock(&s->lock);
    cnd_signal(&s->cv);
    return NV_OK;
}

/* Readers share; a waiting writer holds back new readers so writers cannot starve. */
struct nvrm_rwlock {
    mtx_t lock;
    cnd_t cv;
    int readers;
    int writers_waiting;
    bool writer;
};

void *NV_API_CALL os_alloc_rwlock(void) {
    struct nvrm_rwlock *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    mtx_init(&l->lock, mtx_plain);
    cnd_init(&l->cv);
    return l;
}
void NV_API_CALL os_free_rwlock(void *rwlock) {
    struct nvrm_rwlock *l = rwlock;
    if (!l) return;
    cnd_destroy(&l->cv);
    mtx_destroy(&l->lock);
    free(l);
}

NV_STATUS NV_API_CALL os_acquire_rwlock_read(void *rwlock) {
    struct nvrm_rwlock *l = rwlock;
    mtx_lock(&l->lock);
    while (l->writer || l->writers_waiting) cnd_wait(&l->cv, &l->lock);
    l->readers++;
    mtx_unlock(&l->lock);
    return NV_OK;
}
NV_STATUS NV_API_CALL os_acquire_rwlock_write(void *rwlock) {
    struct nvrm_rwlock *l = rwlock;
    mtx_lock(&l->lock);
    l->writers_waiting++;
    while (l->writer || l->readers) cnd_wait(&l->cv, &l->lock);
    l->writers_waiting--;
    l->writer = true;
    mtx_unlock(&l->lock);
    return NV_OK;
}
NV_STATUS NV_API_CALL os_cond_acquire_rwlock_read(void *rwlock) {
    struct nvrm_rwlock *l = rwlock;
    NV_STATUS status = NV_ERR_TIMEOUT_RETRY;
    mtx_lock(&l->lock);
    if (!l->writer && !l->writers_waiting) {
        l->readers++;
        status = NV_OK;
    }
    mtx_unlock(&l->lock);
    return status;
}
NV_STATUS NV_API_CALL os_cond_acquire_rwlock_write(void *rwlock) {
    struct nvrm_rwlock *l = rwlock;
    NV_STATUS status = NV_ERR_TIMEOUT_RETRY;
    mtx_lock(&l->lock);
    if (!l->writer && !l->readers) {
        l->writer = true;
        status = NV_OK;
    }
    mtx_unlock(&l->lock);
    return status;
}
void NV_API_CALL os_release_rwlock_read(void *rwlock) {
    struct nvrm_rwlock *l = rwlock;
    mtx_lock(&l->lock);
    if (--l->readers == 0) cnd_broadcast(&l->cv);
    mtx_unlock(&l->lock);
}
void NV_API_CALL os_release_rwlock_write(void *rwlock) {
    struct nvrm_rwlock *l = rwlock;
    mtx_lock(&l->lock);
    l->writer = false;
    cnd_broadcast(&l->cv);
    mtx_unlock(&l->lock);
}

/* RM's queue is a one-shot completion: waking releases every present and future
   waiter. There is no reset operation in this OS interface. */
struct os_wait_queue {
    mtx_t lock;
    cnd_t cv;
    bool signaled;
};

NV_STATUS NV_API_CALL os_alloc_wait_queue(os_wait_queue **queue) {
    if (!queue) return NV_ERR_INVALID_ARGUMENT;
    os_wait_queue *q = calloc(1, sizeof *q);
    if (!q) return NV_ERR_NO_MEMORY;
    if (mtx_init(&q->lock, mtx_plain) != thrd_success) { free(q); return NV_ERR_NO_MEMORY; }
    if (cnd_init(&q->cv) != thrd_success) { mtx_destroy(&q->lock); free(q); return NV_ERR_NO_MEMORY; }
    *queue = q;
    return NV_OK;
}
void NV_API_CALL os_free_wait_queue(os_wait_queue *queue) {
    if (!queue) return;
    cnd_destroy(&queue->cv);
    mtx_destroy(&queue->lock);
    free(queue);
}
static void wait_queue_wait(os_wait_queue *q) {
    mtx_lock(&q->lock);
    while (!q->signaled) cnd_wait(&q->cv, &q->lock);
    mtx_unlock(&q->lock);
}
void NV_API_CALL os_wait_uninterruptible(os_wait_queue *queue) { wait_queue_wait(queue); }
void NV_API_CALL os_wait_interruptible(os_wait_queue *queue) { wait_queue_wait(queue); }
void NV_API_CALL os_wake_up(os_wait_queue *queue) {
    mtx_lock(&queue->lock);
    queue->signaled = true;
    cnd_broadcast(&queue->cv);
    mtx_unlock(&queue->lock);
}

/* ------------------------------------------------------------------------------------------
 * Work queue
 *
 * os_queue_work_item hands RM's work item to a worker thread that calls rm_execute_work_item.
 * A single queue serves the system queue (NULL) and the GPU's; workers are added on demand (a
 * work item may block on RM locks that another item releases) up to WORK_THREADS_MAX.
 * ---------------------------------------------------------------------------------------- */
#define WORK_THREADS_MAX 8

struct work_item {
    struct work_item *next;
    void *data;
};

struct os_work_queue {
    mtx_t lock;
    cnd_t work_cv;
    cnd_t idle_cv;
    struct work_item *head, *tail;
    int pending;      /* queued + running */
    int idle_workers;
    int workers;      /* threads started and not yet ended */
    int flushing;
    bool stopping;    /* nvrm_work_stop: no new items, workers end when the queue is empty */
};

static struct os_work_queue work_queue;
static once_flag work_once = ONCE_FLAG_INIT;

static void work_setup(void) {
    mtx_init(&work_queue.lock, mtx_plain);
    cnd_init(&work_queue.work_cv);
    cnd_init(&work_queue.idle_cv);
}
void nvrm_work_init(void) { call_once(&work_once, work_setup); }
struct os_work_queue *nvrm_work_queue(void) {
    nvrm_work_init();
    return &work_queue;
}

static int work_thread(void *arg) {
    struct os_work_queue *q = arg;
    mtx_lock(&q->lock);
    for (;;) {
        while (!q->head) {
            if (q->stopping) {
                q->workers--;
                cnd_broadcast(&q->idle_cv);
                mtx_unlock(&q->lock);
                return 0;
            }
            q->idle_workers++;
            cnd_wait(&q->work_cv, &q->lock);
            q->idle_workers--;
        }
        struct work_item *item = q->head;
        q->head = item->next;
        if (!q->head) q->tail = NULL;
        mtx_unlock(&q->lock);
        rm_execute_work_item(nvrm_dev.sp, item->data);
        free(item);
        mtx_lock(&q->lock);
        if (--q->pending == 0) cnd_broadcast(&q->idle_cv);
    }
    return 0;
}

NV_STATUS NV_API_CALL os_queue_work_item(struct os_work_queue *queue, void *data) {
    (void)queue; /* one queue for everything; see above */
    nvrm_work_init();
    struct os_work_queue *q = &work_queue;
    struct work_item *item = malloc(sizeof *item);
    if (!item) return NV_ERR_NO_MEMORY;
    item->next = NULL;
    item->data = data;
    bool spawn = false;
    mtx_lock(&q->lock);
    if (q->stopping) {
        mtx_unlock(&q->lock);
        free(item);
        return NV_ERR_INVALID_STATE;
    }
    if (q->idle_workers == 0 && q->workers < WORK_THREADS_MAX) {
        q->workers++;
        spawn = true;
    }
    mtx_unlock(&q->lock);
    /* A worker is started before the item is accepted: with none running and none startable, the
       item would wait forever, so the request fails and RM decides what to do. */
    if (spawn) {
        thrd_t t;
        if (thrd_create(&t, work_thread, q) == thrd_success) {
            thrd_detach(t);
        } else {
            mtx_lock(&q->lock);
            bool none = --q->workers == 0;
            mtx_unlock(&q->lock);
            if (none) {
                free(item);
                nvrm_log("NVRM: cannot start a work thread");
                return NV_ERR_INSUFFICIENT_RESOURCES;
            }
            /* other workers exist: one of them takes the item when it is free */
        }
    }
    mtx_lock(&q->lock);
    if (q->stopping) {
        mtx_unlock(&q->lock);
        free(item);
        return NV_ERR_INVALID_STATE;
    }
    if (q->tail) q->tail->next = item; else q->head = item;
    q->tail = item;
    q->pending++;
    mtx_unlock(&q->lock);
    cnd_signal(&q->work_cv);
    return NV_OK;
}

/* Called once nothing new can arrive (clients gone, interrupts stopped): the items already
   queued run, then every worker ends. */
void nvrm_work_stop(void) {
    nvrm_work_init();
    struct os_work_queue *q = &work_queue;
    mtx_lock(&q->lock);
    q->stopping = true;
    cnd_broadcast(&q->work_cv);
    while (q->workers) cnd_wait(&q->idle_cv, &q->lock);
    mtx_unlock(&q->lock);
}

NV_STATUS NV_API_CALL os_flush_work_queue(struct os_work_queue *queue, NvBool do_work) {
    (void)queue; (void)do_work; /* RM drops the items it marked droppable itself while flushing */
    nvrm_work_init();
    struct os_work_queue *q = &work_queue;
    mtx_lock(&q->lock);
    q->flushing++;
    while (q->pending) cnd_wait(&q->idle_cv, &q->lock);
    q->flushing--;
    mtx_unlock(&q->lock);
    return NV_OK;
}

NvBool NV_API_CALL os_is_queue_flush_ongoing(struct os_work_queue *queue) {
    (void)queue;
    return __atomic_load_n(&work_queue.flushing, __ATOMIC_RELAXED) ? NV_TRUE : NV_FALSE;
}

/* ------------------------------------------------------------------------------------------
 * RC timer (1 Hz) and nano timers
 *
 * One timer thread keeps the armed timers and runs their callbacks. A nano timer fires once,
 * a relative number of nanoseconds after nv_start_nano_timer; the RC timer repeats every
 * second while enabled.
 * ---------------------------------------------------------------------------------------- */
struct nv_nano_timer {
    nv_state_t *nv;
    void *event;
    uint64_t deadline_ns;
    bool armed;
    bool running;
    int runner;               /* thread id while the callback runs */
    struct nv_nano_timer *next;
};

static mtx_t timer_lock;
static cnd_t timer_cv;
static cnd_t timer_done_cv;
static once_flag timer_once = ONCE_FLAG_INIT;
static struct nv_nano_timer *timers;
static thrd_t timer_thread_id;
static bool timer_thread_started;   /* the thread runs and accepts timers */
static bool timer_stop;
static nv_state_t *rc_nv;
static uint64_t rc_deadline_ns;
static bool rc_running;             /* the RC callback is running */
static int rc_runner;

static int timer_thread(void *arg) {
    (void)arg;
    mtx_lock(&timer_lock);
    while (!timer_stop) {
        uint64_t now = nvrm_now_ns();
        struct nv_nano_timer *due = NULL;
        uint64_t next = UINT64_MAX;
        for (struct nv_nano_timer *t = timers; t; t = t->next) {
            if (!t->armed) continue;
            if (t->deadline_ns <= now) { due = t; break; }
            if (t->deadline_ns < next) next = t->deadline_ns;
        }
        if (due) {
            due->armed = false;
            due->running = true;
            due->runner = thread_id();
            mtx_unlock(&timer_lock);
            rm_run_nano_timer_callback(nvrm_dev.sp, due->nv, due->event);
            mtx_lock(&timer_lock);
            due->running = false;
            cnd_broadcast(&timer_done_cv);
            continue;
        }
        if (rc_nv && rc_nv->rc_timer_enabled) {
            if (rc_deadline_ns <= now) {
                nv_state_t *nv = rc_nv;
                rc_deadline_ns = now + 1000000000ull;
                rc_running = true;
                rc_runner = thread_id();
                mtx_unlock(&timer_lock);
                rm_run_rc_callback(nvrm_dev.sp, nv);
                mtx_lock(&timer_lock);
                rc_running = false;
                cnd_broadcast(&timer_done_cv);
                continue;
            }
            if (rc_deadline_ns < next) next = rc_deadline_ns;
        }
        if (next == UINT64_MAX) {
            cnd_wait(&timer_cv, &timer_lock);
        } else {
            uint64_t left_ns = next - now;
            if (left_ns < 1000000ull) {
                /* Under a millisecond: the wait API counts milliseconds, so spin. */
                mtx_unlock(&timer_lock);
                for (int i = 0; i < 32; i++) __builtin_ia32_pause();
                mtx_lock(&timer_lock);
            } else {
                /* cnd_timedwait takes a TIME_UTC deadline. Nocturne's TIME_UTC never runs
                   backwards (libc time.c), so a wall clock set forward can only end the wait
                   early, and the loop measures again on the monotonic clock. */
                struct timespec ts;
                timespec_get(&ts, TIME_UTC);
                uint64_t ms = left_ns / 1000000ull;
                ts.tv_sec += (time_t)(ms / 1000);
                ts.tv_nsec += (long)(ms % 1000) * 1000000L;
                if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                cnd_timedwait(&timer_cv, &timer_lock, &ts);
            }
        }
    }
    mtx_unlock(&timer_lock);
    return 0;
}

static void timers_setup(void) {
    mtx_init(&timer_lock, mtx_plain);
    cnd_init(&timer_cv);
    cnd_init(&timer_done_cv);
}

static bool timer_thread_ensure_locked(void) {
    if (timer_stop) return false;
    if (timer_thread_started) return true;
    if (thrd_create(&timer_thread_id, timer_thread, NULL) != thrd_success) return false;
    timer_thread_started = true;
    return true;
}

void nvrm_timers_init(void) { call_once(&timer_once, timers_setup); }

/* RM's timers all run on this thread. It is started during bring-up, so a failure there ends the
   start of the GPU instead of leaving timers that are accepted and never fire. */
bool nvrm_timers_start(void) {
    nvrm_timers_init();
    mtx_lock(&timer_lock);
    bool ok = timer_thread_ensure_locked();
    mtx_unlock(&timer_lock);
    return ok;
}

void nvrm_timers_stop(void) {
    nvrm_timers_init();
    mtx_lock(&timer_lock);
    bool join = timer_thread_started;
    timer_stop = true;
    for (struct nv_nano_timer *t = timers; t; t = t->next) t->armed = false;
    rc_nv = NULL;
    mtx_unlock(&timer_lock);
    cnd_broadcast(&timer_cv);
    if (join) thrd_join(timer_thread_id, NULL);
    mtx_lock(&timer_lock);
    timer_thread_started = false;
    mtx_unlock(&timer_lock);
}

NvS32 NV_API_CALL nv_start_rc_timer(nv_state_t *nv) {
    nvrm_timers_init();
    mtx_lock(&timer_lock);
    bool ok = timer_thread_ensure_locked();
    if (ok) {
        nv->rc_timer_enabled = 1;
        rc_nv = nv;
        rc_deadline_ns = nvrm_now_ns() + 1000000000ull;
    }
    mtx_unlock(&timer_lock);
    cnd_signal(&timer_cv);
    return ok ? 0 : -1;
}

NvS32 NV_API_CALL nv_stop_rc_timer(nv_state_t *nv) {
    nvrm_timers_init();
    mtx_lock(&timer_lock);
    nv->rc_timer_enabled = 0;
    if (rc_nv == nv) rc_nv = NULL;
    /* RC recovery uses RM's state: it must not still run once the timer is stopped (it may stop
       itself from inside the callback). */
    while (rc_running && rc_runner != thread_id()) cnd_wait(&timer_done_cv, &timer_lock);
    mtx_unlock(&timer_lock);
    return 0;
}

void NV_API_CALL nv_create_nano_timer(nv_state_t *nv, void *event, nv_nano_timer_t **timer) {
    nvrm_timers_init();
    struct nv_nano_timer *t = NULL;
    mtx_lock(&timer_lock);
    /* A timer is only handed out while the thread that runs it exists. */
    if (timer_thread_ensure_locked()) t = calloc(1, sizeof *t);
    if (t) {
        t->nv = nv;
        t->event = event;
        t->next = timers;
        timers = t;
    }
    mtx_unlock(&timer_lock);
    if (!t) nvrm_log("NVRM: cannot create a nano timer");
    *timer = t;
}

void NV_API_CALL nv_start_nano_timer(nv_state_t *nv, nv_nano_timer_t *timer, NvU64 time_ns) {
    (void)nv;
    if (!timer) return;
    mtx_lock(&timer_lock);
    if (!timer_thread_ensure_locked()) {
        /* Not armed: nothing would run it. (RM's start has no way to report this.) */
        mtx_unlock(&timer_lock);
        nvrm_log("NVRM: nano timer not started: the timer thread is not running");
        return;
    }
    timer->deadline_ns = nvrm_now_ns() + time_ns;
    timer->armed = true;
    mtx_unlock(&timer_lock);
    cnd_signal(&timer_cv);
}

void NV_API_CALL nv_cancel_nano_timer(nv_state_t *nv, nv_nano_timer_t *timer) {
    (void)nv;
    if (!timer) return;
    mtx_lock(&timer_lock);
    timer->armed = false;
    /* Linux's hrtimer_cancel waits for a running callback; so does this, except from the
       callback itself. */
    while (timer->running && timer->runner != thread_id()) cnd_wait(&timer_done_cv, &timer_lock);
    mtx_unlock(&timer_lock);
}

void NV_API_CALL nv_destroy_nano_timer(nv_state_t *nv, nv_nano_timer_t *timer) {
    (void)nv;
    if (!timer) return;
    mtx_lock(&timer_lock);
    timer->armed = false;
    while (timer->running && timer->runner != thread_id()) cnd_wait(&timer_done_cv, &timer_lock);
    for (struct nv_nano_timer **p = &timers; *p; p = &(*p)->next) {
        if (*p == timer) { *p = timer->next; break; }
    }
    mtx_unlock(&timer_lock);
    free(timer);
}

/* ------------------------------------------------------------------------------------------
 * GSP firmware
 *
 * RM asks for gsp_<family>.bin, ucodes_<family>.bin and gsp_log_<family>.bin. The files are
 * not distributed with Nocturne (docs/nvidia/LEDGER.md D9); a disk built with them (the
 * Makefile's NVIDIA_FIRMWARE) has them in C:\Nocturne\Firmware\NVIDIA\<version>.
 * ---------------------------------------------------------------------------------------- */
struct nvrm_firmware {
    void *data;
    size_t size;
};

#define FIRMWARE_FOLDER "C:\\Nocturne\\Firmware\\NVIDIA\\" NVRM_FIRMWARE_VERSION

const void *NV_API_CALL nv_get_firmware(nv_state_t *nv, nv_firmware_type_t type,
                                        nv_firmware_chip_family_t family, const void **buffer,
                                        NvU32 *size) {
    (void)nv;
    const char *base = nv_firmware_for_chip_family(type, family);
    if (!base || !*base || !buffer || !size) {
        nvrm_log("NVRM: no firmware file for type %d, chip family %s", (int)type,
                 nv_firmware_chip_family_to_string(family));
        return NULL;
    }
    char path[160];
    snprintf(path, sizeof path, FIRMWARE_FOLDER "\\%s.bin", base);
    void *data = NULL;
    size_t length = 0;
    if (n_document_load(path, &data, &length) < 0 || !data) {
        nvrm_log("NVRM: firmware %s not found", path);
        return NULL;
    }
    if (length > 0xFFFFFFFFu) {
        n_memory_release(data, length);
        nvrm_log("NVRM: firmware %s is too large", path);
        return NULL;
    }
    struct nvrm_firmware *fw = malloc(sizeof *fw);
    if (!fw) {
        n_memory_release(data, length);
        return NULL;
    }
    fw->data = data;
    fw->size = length;
    *buffer = data;
    *size = (NvU32)length;
    nvrm_log("NVRM: firmware %s (%u bytes)", path, (unsigned)length);
    return fw;
}

void NV_API_CALL nv_put_firmware(const void *handle) {
    struct nvrm_firmware *fw = (struct nvrm_firmware *)handle;
    if (!fw) return;
    n_memory_release(fw->data, fw->size);
    free(fw);
}
