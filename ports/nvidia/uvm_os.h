/* The OS layer NVIDIA's UVM driver (nvidia-uvm, third_party/nvidia-open/kernel-open/nvidia-uvm)
 * is compiled against in nvrm: what uvm_linux.h includes on Linux, implemented on Nocturne.
 *
 * UVM is written against the vocabulary of a kernel: lists, red-black trees, bitmaps, atomics,
 * locks, memory and page allocators, character devices with file operations, and the address
 * spaces (mm_struct, vm_area_struct) of the processes that map it. In nvrm it runs as part of a
 * user-space driver app, so each of those words here names a Nocturne thing:
 *
 *   locks, wait queues                C11 mutexes and conditions, wait_on_address
 *   kernel threads (nv_kthread_q)     C11 threads
 *   kmalloc / vmalloc / kmem_cache    malloc (vmalloc's blocks are kept in a tree, so that
 *                                     is_vmalloc_addr can be asked of any address)
 *   struct page, alloc_pages          RM's system memory for the GPU (nv_alloc_pages): DMA
 *                                     objects of nvrm's PCI claim, mapped in nvrm
 *   dma_map_page                      the bus address of such a page (the GPU's claim)
 *   struct file + file_operations     a UVM file a client of nvrm opened (uvm_serve.c)
 *   mm_struct                         a client app; vm_area_struct one of its mappings of UVM
 *                                     memory: pager-backed shared memory (docs/nocturne-native-
 *                                     abi.md §7), vm_insert_page = n_pager_supply,
 *                                     unmap_mapping_range = n_pager_revoke
 *   copy_from_user / copy_to_user     the request's argument block or the client's arena
 *
 * What a Linux kernel has and Nocturne does not is configured off the way UVM itself offers:
 * HMM (UVM_IS_CONFIG_HMM 0: no GPU access to an app's ordinary memory), MMU notifiers, ATS,
 * dma-buf import, device-coherent pages, memory cgroups and procfs (CONFIG_PROC_FS is not
 * defined). ports/nvidia/README.md ("UVM") has the details.
 *
 * Nothing here is POSIX; the implementation uses Nocturne's native ABI and ISO C.
 */
#ifndef NVRM_UVM_OS_H
#define NVRM_UVM_OS_H

#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "nocturne.h"

#include "nvtypes.h"
#include "nvstatus.h"
#include "nv.h"

#include "uvm_nvrm.h"
#include "uvm_shared.h"

/* ==========================================================================================
 * Types and compiler words
 * ======================================================================================== */

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long long u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef long long s64;
typedef unsigned long ulong;
typedef long ssize_t;
typedef long long loff_t;
typedef int pid_t;
typedef u64 phys_addr_t;
typedef u64 dma_addr_t;
typedef u64 resource_size_t;
typedef unsigned gfp_t;
typedef uint32_t dev_t;
typedef unsigned long vm_flags_t;
typedef unsigned vm_fault_t;
typedef struct { uint64_t pgprot; } pgprot_t;

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define __same_type(a, b) __builtin_types_compatible_p(__typeof__(a), __typeof__(b))
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BUILD_BUG_ON(cond) ((void)sizeof(char[1 - 2 * !!(cond)]))
#define BUILD_BUG_ON_NOT_POWER_OF_2(n) BUILD_BUG_ON((n) == 0 || (((n) & ((n) - 1)) != 0))
#define ____cacheline_aligned_in_smp __attribute__((aligned(64)))
#define __maybe_unused __attribute__((unused))
#define __read_mostly
#define __always_inline inline __attribute__((always_inline))
#define noinline __attribute__((noinline))
#define __init
#define __exit
#define __user
#define __iomem
#define __force
#define __must_check __attribute__((warn_unused_result))
#define fallthrough __attribute__((fallthrough))

#define min(a, b) ({ __typeof__(a) _min_a = (a); __typeof__(b) _min_b = (b); _min_a < _min_b ? _min_a : _min_b; })
#define max(a, b) ({ __typeof__(a) _max_a = (a); __typeof__(b) _max_b = (b); _max_a > _max_b ? _max_a : _max_b; })
#define min3(a, b, c) min(min(a, b), c)
#define max3(a, b, c) max(max(a, b), c)
#define min_t(t, a, b) min((t)(a), (t)(b))
#define max_t(t, a, b) max((t)(a), (t)(b))
#define clamp(v, lo, hi) min(max(v, lo), hi)
#define swap(a, b) do { __typeof__(a) _swap_t = (a); (a) = (b); (b) = _swap_t; } while (0)
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#define rounddown(x, y) ((x) - ((x) % (y)))
#define IS_ALIGNED(x, a) (((x) & ((__typeof__(x))(a) - 1)) == 0)
#define ALIGN(x, a) (((x) + ((__typeof__(x))(a) - 1)) & ~((__typeof__(x))(a) - 1))
#define U32_MAX UINT32_MAX
#define U64_MAX UINT64_MAX
#define S64_MAX INT64_MAX

/* ==========================================================================================
 * Errors: UVM's errno vocabulary (uvm_shared.h)
 * ======================================================================================== */

#define IS_ERR_VALUE(x) unlikely((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)
static inline void *ERR_PTR(long error) { return (void *)error; }
static inline long PTR_ERR(const void *ptr) { return (long)ptr; }
static inline bool IS_ERR(const void *ptr) { return IS_ERR_VALUE((unsigned long)ptr); }
static inline bool IS_ERR_OR_NULL(const void *ptr) { return !ptr || IS_ERR(ptr); }

/* ==========================================================================================
 * Printing (uvm_os.c): nvrm's log. KERN_* only mark a message's level in the source.
 * ======================================================================================== */

#define KERN_EMERG ""
#define KERN_ALERT ""
#define KERN_CRIT ""
#define KERN_ERR ""
#define KERN_WARNING ""
#define KERN_NOTICE ""
#define KERN_INFO ""
#define KERN_DEBUG ""
#define KERN_CONT ""
#define NVIDIA_UVM_PRETTY_PRINTING_PREFIX "nvidia-uvm: "
#define pr_fmt(fmt) NVIDIA_UVM_PRETTY_PRINTING_PREFIX fmt

int printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vprintk(const char *fmt, va_list ap);
/* Checks the format and arguments like printk, prints nothing. */
#define no_printk(fmt, ...) ({ if (0) printk(fmt, ##__VA_ARGS__); 0; })
#define pr_err(fmt, ...) printk(KERN_ERR pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn(fmt, ...) printk(KERN_WARNING pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...) printk(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)
#define pr_debug(fmt, ...) no_printk(pr_fmt(fmt), ##__VA_ARGS__)

/* At most `burst` messages per `interval_ms` from one call site; the rest are counted and the
   count is reported with the next message that gets through. */
struct uvm_os_ratelimit {
    mtx_t lock;
    uint64_t begin_ms;
    unsigned printed, missed;
};
bool uvm_os_ratelimit_pass(struct uvm_os_ratelimit *rl);
#define printk_ratelimited(fmt, ...) ({                                   \
        static struct uvm_os_ratelimit _rl;                               \
        uvm_os_ratelimit_pass(&_rl) ? printk(fmt, ##__VA_ARGS__) : 0;     \
    })
#define pr_info_ratelimited(fmt, ...) printk_ratelimited(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)

/* The return addresses of the calling thread's frames, into the log. */
void dump_stack(void);
/* Logs the message and ends nvrm: UVM found its state broken beyond repair. */
__attribute__((noreturn, format(printf, 1, 2))) void panic(const char *fmt, ...);

#define WARN_ON(cond) ({                                                                   \
        bool _warn = !!(cond);                                                             \
        if (unlikely(_warn)) {                                                             \
            printk(KERN_WARNING "nvidia-uvm: WARNING at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            dump_stack();                                                                  \
        }                                                                                  \
        unlikely(_warn);                                                                   \
    })
#define WARN_ON_ONCE(cond) ({                                                              \
        static bool _warned;                                                               \
        bool _warn = !!(cond);                                                             \
        if (unlikely(_warn) && !__atomic_exchange_n(&_warned, true, __ATOMIC_RELAXED)) {   \
            printk(KERN_WARNING "nvidia-uvm: WARNING at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            dump_stack();                                                                  \
        }                                                                                  \
        unlikely(_warn);                                                                   \
    })
#define BUG_ON(cond) do { if (unlikely(cond)) panic("BUG at %s:%d: %s", __FILE__, __LINE__, #cond); } while (0)
#define BUG() panic("BUG at %s:%d", __FILE__, __LINE__)

static inline const char *kbasename(const char *path)
{
    const char *tail = strrchr(path, '/');
    const char *back = strrchr(tail ? tail : path, '\\');
    return back ? back + 1 : tail ? tail + 1 : path;
}

/* scnprintf: like snprintf, but returns what was written (without the terminating NUL). */
int scnprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* ==========================================================================================
 * Module words: what a Linux module's loader does is done by nvrm_uvm_start/stop
 * ======================================================================================== */

struct module;
#define THIS_MODULE ((struct module *)0)
#define MODULE_LICENSE(x)
#define MODULE_INFO(tag, info)
#define MODULE_PARM_DESC(name, text)
#define MODULE_SOFTDEP(x)
#define MODULE_VERSION(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_AUTHOR(x)
#define MODULE_IMPORT_NS(ns)
#define EXPORT_SYMBOL(sym)
#define EXPORT_SYMBOL_GPL(sym)
/* Module parameters: set by name from nvrm's launch setting UVM_OPTIONS (uvm_os.c). */
struct uvm_os_param {
    const char *name;
    char type;          /* 'i' int, 'u' unsigned, 'b' bool, 'l' unsigned long, 's' char * */
    void *value;
};
#define S_IRUGO 0444
#define S_IWUSR 0200
#define _UVM_PARAM_TYPE(type) _Generic((type *)0, int *: 'i', unsigned *: 'u', bool *: 'b', \
    unsigned long *: 'l', char **: 's', default: '?')
#define module_param(name, type, perm)                                                     \
    static const struct uvm_os_param __uvm_param_##name                                    \
        __attribute__((used, retain, section("uvm_params"), aligned(8))) = {#name, _UVM_PARAM_TYPE(__typeof__(name)), &name}
#define module_param_named(alias, name, type, perm)                                        \
    static const struct uvm_os_param __uvm_param_##alias                                   \
        __attribute__((used, retain, section("uvm_params"), aligned(8))) = {#alias, _UVM_PARAM_TYPE(__typeof__(name)), &name}
/* The module's entry points: the only symbols nvidia-uvm.o exports (ports/nvidia/uvm.mk). */
#define module_init(fn) int nvrm_uvm_module_init(void) { return fn(); }
#define module_exit(fn) void nvrm_uvm_module_exit(void) { fn(); }

/* ==========================================================================================
 * Atomics and barriers (x86-64: loads and stores are ordered except store-load)
 * ======================================================================================== */

/* atomic_t, atomic64_t, atomic_long_t: uvm_shared.h */

#define mb() __asm__ volatile("mfence" ::: "memory")
#define rmb() __asm__ volatile("lfence" ::: "memory")
#define wmb() __asm__ volatile("sfence" ::: "memory")
#define smp_mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define smp_rmb() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define smp_wmb() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define smp_mb__before_atomic() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define smp_mb__after_atomic() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define barrier() __asm__ volatile("" ::: "memory")
#define cpu_relax() __builtin_ia32_pause()
#define smp_load_acquire(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define smp_store_release(p, v) __atomic_store_n(p, v, __ATOMIC_RELEASE)


#define xchg(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define cmpxchg(p, old, new_value) ({                                                         \
        __typeof__(*(p)) _cx_old = (old);                                                     \
        __atomic_compare_exchange_n((p), &_cx_old, (new_value), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
        _cx_old;                                                                              \
    })

/* ==========================================================================================
 * Bits and bitmaps (bitmaps: uvm_os.c)
 * ======================================================================================== */

#define BITS_PER_BYTE 8
#define BITS_PER_LONG 64
#define BITS_PER_LONG_LONG 64
#define BIT(n) (1UL << (n))
#define BIT_ULL(n) (1ULL << (n))
#define BIT_MASK(n) (1UL << ((n) % BITS_PER_LONG))
#define BIT_WORD(n) ((n) / BITS_PER_LONG)
#define BITS_TO_LONGS(n) DIV_ROUND_UP((n), BITS_PER_LONG)
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#define GENMASK_ULL(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))

static inline void set_bit(long nr, volatile unsigned long *addr)
{ __atomic_fetch_or(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST); }
static inline void clear_bit(long nr, volatile unsigned long *addr)
{ __atomic_fetch_and(&addr[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_SEQ_CST); }
static inline void change_bit(long nr, volatile unsigned long *addr)
{ __atomic_fetch_xor(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST); }
static inline bool test_and_set_bit(long nr, volatile unsigned long *addr)
{ return (__atomic_fetch_or(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST) & BIT_MASK(nr)) != 0; }
static inline bool test_and_clear_bit(long nr, volatile unsigned long *addr)
{ return (__atomic_fetch_and(&addr[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_SEQ_CST) & BIT_MASK(nr)) != 0; }
static inline bool test_and_set_bit_lock(long nr, volatile unsigned long *addr)
{ return (__atomic_fetch_or(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_ACQUIRE) & BIT_MASK(nr)) != 0; }
static inline void clear_bit_unlock(long nr, volatile unsigned long *addr)
{ __atomic_fetch_and(&addr[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_RELEASE); }
static inline void __set_bit(long nr, volatile unsigned long *addr) { addr[BIT_WORD(nr)] |= BIT_MASK(nr); }
static inline void __clear_bit(long nr, volatile unsigned long *addr) { addr[BIT_WORD(nr)] &= ~BIT_MASK(nr); }
static inline void __change_bit(long nr, volatile unsigned long *addr) { addr[BIT_WORD(nr)] ^= BIT_MASK(nr); }
static inline bool __test_and_set_bit(long nr, volatile unsigned long *addr)
{
    bool old = (addr[BIT_WORD(nr)] & BIT_MASK(nr)) != 0;
    addr[BIT_WORD(nr)] |= BIT_MASK(nr);
    return old;
}
static inline bool __test_and_clear_bit(long nr, volatile unsigned long *addr)
{
    bool old = (addr[BIT_WORD(nr)] & BIT_MASK(nr)) != 0;
    addr[BIT_WORD(nr)] &= ~BIT_MASK(nr);
    return old;
}
static inline bool test_bit(long nr, const volatile unsigned long *addr)
{ return (addr[BIT_WORD(nr)] & BIT_MASK(nr)) != 0; }

/* Bit positions: __ffs/__fls of a non-zero word (0-based); ffs/fls/fls64 are 1-based, 0 for 0. */
static inline unsigned long __ffs(unsigned long w) { return (unsigned long)__builtin_ctzl(w); }
static inline unsigned long __fls(unsigned long w) { return (unsigned long)(BITS_PER_LONG - 1 - __builtin_clzl(w)); }
static inline unsigned long ffz(unsigned long w) { return __ffs(~w); }
static inline int ffs(int x) { return __builtin_ffs(x); }
static inline int fls(unsigned x) { return x ? 32 - __builtin_clz(x) : 0; }
static inline int fls64(uint64_t x) { return x ? 64 - __builtin_clzll(x) : 0; }
static inline unsigned hweight8(unsigned w) { return (unsigned)__builtin_popcount(w & 0xFFu); }
static inline unsigned hweight16(unsigned w) { return (unsigned)__builtin_popcount(w & 0xFFFFu); }
static inline unsigned hweight32(unsigned w) { return (unsigned)__builtin_popcount(w); }
static inline unsigned long hweight64(uint64_t w) { return (unsigned long)__builtin_popcountll(w); }
static inline unsigned long hweight_long(unsigned long w) { return (unsigned long)__builtin_popcountl(w); }
/* Integer constant expressions for constant arguments (UVM sizes bit-fields with them). */
#define ilog2(n) ((int)(63 - __builtin_clzll((unsigned long long)(n) | 1)))
#define order_base_2(n) ((n) <= 1 ? 0 : ilog2(((n) - 1) | 1) + 1)
#define roundup_pow_of_two(n) (1UL << order_base_2(n))
#define rounddown_pow_of_two(n) (1UL << ilog2(n))
static inline bool is_power_of_2(unsigned long n) { return n != 0 && (n & (n - 1)) == 0; }

/* find_*: the first matching bit at or after `offset`, or `size` when there is none. */
unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset);
unsigned long find_next_zero_bit(const unsigned long *addr, unsigned long size, unsigned long offset);
unsigned long find_last_bit(const unsigned long *addr, unsigned long size);
#define find_first_bit(addr, size) find_next_bit((addr), (size), 0)
#define find_first_zero_bit(addr, size) find_next_zero_bit((addr), (size), 0)
#define for_each_set_bit(bit, addr, size) \
    for ((bit) = find_first_bit((addr), (size)); (bit) < (size); (bit) = find_next_bit((addr), (size), (bit) + 1))
#define for_each_set_bit_from(bit, addr, size) \
    for ((bit) = find_next_bit((addr), (size), (bit)); (bit) < (size); (bit) = find_next_bit((addr), (size), (bit) + 1))
#define for_each_clear_bit(bit, addr, size) \
    for ((bit) = find_first_zero_bit((addr), (size)); (bit) < (size); (bit) = find_next_zero_bit((addr), (size), (bit) + 1))
#define for_each_clear_bit_from(bit, addr, size) \
    for ((bit) = find_next_zero_bit((addr), (size), (bit)); (bit) < (size); (bit) = find_next_zero_bit((addr), (size), (bit) + 1))

/* Whole bitmaps of nbits bits. The operations that write work on whole words, so they may
   change the bits past nbits in the last word; those that read (and bitmap_shift_right's
   source) ignore them. */
void bitmap_zero(unsigned long *dst, unsigned nbits);
void bitmap_fill(unsigned long *dst, unsigned nbits);
void bitmap_copy(unsigned long *dst, const unsigned long *src, unsigned nbits);
bool bitmap_and(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned nbits);
void bitmap_or(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned nbits);
void bitmap_xor(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned nbits);
bool bitmap_andnot(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned nbits);
void bitmap_complement(unsigned long *dst, const unsigned long *src, unsigned nbits);
bool bitmap_equal(const unsigned long *a, const unsigned long *b, unsigned nbits);
bool bitmap_intersects(const unsigned long *a, const unsigned long *b, unsigned nbits);
bool bitmap_subset(const unsigned long *a, const unsigned long *b, unsigned nbits);
bool bitmap_empty(const unsigned long *src, unsigned nbits);
bool bitmap_full(const unsigned long *src, unsigned nbits);
unsigned bitmap_weight(const unsigned long *src, unsigned nbits);
void bitmap_set(unsigned long *map, unsigned start, unsigned len);
void bitmap_clear(unsigned long *map, unsigned start, unsigned len);
void bitmap_shift_left(unsigned long *dst, const unsigned long *src, unsigned shift, unsigned nbits);
void bitmap_shift_right(unsigned long *dst, const unsigned long *src, unsigned shift, unsigned nbits);

/* ==========================================================================================
 * Doubly linked lists
 * ======================================================================================== */

struct list_head {
    struct list_head *next, *prev;
};
struct hlist_node {
    struct hlist_node *next, **pprev;
};
struct hlist_head {
    struct hlist_node *first;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)
static inline void INIT_LIST_HEAD(struct list_head *list) { list->next = list; list->prev = list; }
static inline void __list_link(struct list_head *entry, struct list_head *prev, struct list_head *next)
{
    next->prev = entry;
    entry->next = next;
    entry->prev = prev;
    prev->next = entry;
}
static inline void list_add(struct list_head *entry, struct list_head *head) { __list_link(entry, head, head->next); }
static inline void list_add_tail(struct list_head *entry, struct list_head *head) { __list_link(entry, head->prev, head); }
static inline void __list_unlink(struct list_head *entry)
{
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
}
/* An entry taken off with list_del is no longer on any list; using it as one is a bug, so its
   links are made to fault. */
static inline void list_del(struct list_head *entry)
{
    __list_unlink(entry);
    entry->next = (struct list_head *)0x100;
    entry->prev = (struct list_head *)0x122;
}
static inline void list_del_init(struct list_head *entry) { __list_unlink(entry); INIT_LIST_HEAD(entry); }
static inline void list_replace(struct list_head *old, struct list_head *entry)
{
    entry->next = old->next;
    entry->next->prev = entry;
    entry->prev = old->prev;
    entry->prev->next = entry;
}
static inline void list_replace_init(struct list_head *old, struct list_head *entry)
{
    list_replace(old, entry);
    INIT_LIST_HEAD(old);
}
static inline void list_move(struct list_head *entry, struct list_head *head) { __list_unlink(entry); list_add(entry, head); }
static inline void list_move_tail(struct list_head *entry, struct list_head *head) { __list_unlink(entry); list_add_tail(entry, head); }
static inline bool list_empty(const struct list_head *head) { return READ_ONCE(head->next) == head; }
static inline bool list_empty_careful(const struct list_head *head) { return head->next == head && head->prev == head; }
static inline bool list_is_first(const struct list_head *entry, const struct list_head *head) { return entry->prev == head; }
static inline bool list_is_last(const struct list_head *entry, const struct list_head *head) { return entry->next == head; }
static inline bool list_is_singular(const struct list_head *head) { return !list_empty(head) && head->next == head->prev; }
/* Moves every entry of `list` between prev and next. */
static inline void __list_splice(const struct list_head *list, struct list_head *prev, struct list_head *next)
{
    struct list_head *first = list->next, *last = list->prev;
    first->prev = prev;
    prev->next = first;
    last->next = next;
    next->prev = last;
}
static inline void list_splice(const struct list_head *list, struct list_head *head)
{ if (!list_empty(list)) __list_splice(list, head, head->next); }
static inline void list_splice_tail(const struct list_head *list, struct list_head *head)
{ if (!list_empty(list)) __list_splice(list, head->prev, head); }
static inline void list_splice_init(struct list_head *list, struct list_head *head)
{ if (!list_empty(list)) { __list_splice(list, head, head->next); INIT_LIST_HEAD(list); } }
static inline void list_splice_tail_init(struct list_head *list, struct list_head *head)
{ if (!list_empty(list)) { __list_splice(list, head->prev, head); INIT_LIST_HEAD(list); } }
/* Moves the entries of head up to and including `entry` to `list` (which is replaced). */
static inline void list_cut_position(struct list_head *list, struct list_head *head, struct list_head *entry)
{
    if (list_empty(head) || (list_is_singular(head) && head->next != entry && head != entry))
        return;
    if (entry == head) {
        INIT_LIST_HEAD(list);
        return;
    }
    struct list_head *first = head->next;
    list->next = first;
    first->prev = list;
    list->prev = entry;
    head->next = entry->next;
    head->next->prev = head;
    entry->next = list;
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_first_entry(head, type, member) list_entry((head)->next, type, member)
#define list_last_entry(head, type, member) list_entry((head)->prev, type, member)
#define list_first_entry_or_null(head, type, member) \
    (!list_empty(head) ? list_first_entry(head, type, member) : NULL)
#define list_last_entry_or_null(head, type, member) \
    (!list_empty(head) ? list_last_entry(head, type, member) : NULL)
#define list_next_entry(pos, member) list_entry((pos)->member.next, __typeof__(*(pos)), member)
#define list_prev_entry(pos, member) list_entry((pos)->member.prev, __typeof__(*(pos)), member)
#define list_entry_is_head(pos, head, member) (&(pos)->member == (head))
#define list_for_each(pos, head) for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)
#define list_for_each_safe(pos, n, head) \
    for ((pos) = (head)->next, (n) = (pos)->next; (pos) != (head); (pos) = (n), (n) = (pos)->next)
#define list_for_each_entry(pos, head, member)                                   \
    for ((pos) = list_first_entry(head, __typeof__(*(pos)), member);             \
         !list_entry_is_head(pos, head, member); (pos) = list_next_entry(pos, member))
#define list_for_each_entry_reverse(pos, head, member)                           \
    for ((pos) = list_last_entry(head, __typeof__(*(pos)), member);              \
         !list_entry_is_head(pos, head, member); (pos) = list_prev_entry(pos, member))
#define list_for_each_entry_continue(pos, head, member)                          \
    for ((pos) = list_next_entry(pos, member);                                   \
         !list_entry_is_head(pos, head, member); (pos) = list_next_entry(pos, member))
#define list_for_each_entry_from(pos, head, member)                              \
    for (; !list_entry_is_head(pos, head, member); (pos) = list_next_entry(pos, member))
#define list_for_each_entry_safe(pos, n, head, member)                           \
    for ((pos) = list_first_entry(head, __typeof__(*(pos)), member),             \
         (n) = list_next_entry(pos, member);                                     \
         !list_entry_is_head(pos, head, member); (pos) = (n), (n) = list_next_entry(n, member))
#define list_for_each_entry_safe_reverse(pos, n, head, member)                   \
    for ((pos) = list_last_entry(head, __typeof__(*(pos)), member),              \
         (n) = list_prev_entry(pos, member);                                     \
         !list_entry_is_head(pos, head, member); (pos) = (n), (n) = list_prev_entry(n, member))
#define list_for_each_entry_safe_from(pos, n, head, member)                      \
    for ((n) = list_next_entry(pos, member); !list_entry_is_head(pos, head, member); \
         (pos) = (n), (n) = list_next_entry(n, member))

/* ==========================================================================================
 * Red-black trees (uvm_os.c). An empty node (RB_CLEAR_NODE) is its own parent.
 * ======================================================================================== */

struct rb_node {
    struct rb_node *rb_parent;
    struct rb_node *rb_left;
    struct rb_node *rb_right;
    bool rb_red;
};
struct rb_root {
    struct rb_node *rb_node;
};
#define RB_ROOT (struct rb_root){ NULL }
#define RB_EMPTY_ROOT(root) ((root)->rb_node == NULL)
#define RB_EMPTY_NODE(node) ((node)->rb_parent == (node))
#define RB_CLEAR_NODE(node) ((node)->rb_parent = (node))
#define rb_entry(ptr, type, member) container_of(ptr, type, member)
#define rb_entry_safe(ptr, type, member) ({ __typeof__(ptr) _rbe = (ptr); _rbe ? rb_entry(_rbe, type, member) : NULL; })
#define rb_parent(node) ((node)->rb_parent)
/* Puts node into the tree at *link (a child pointer of parent, or the root's pointer). */
static inline void rb_link_node(struct rb_node *node, struct rb_node *parent, struct rb_node **link)
{
    node->rb_parent = parent;
    node->rb_left = node->rb_right = NULL;
    node->rb_red = true;
    *link = node;
}
/* Rebalances after rb_link_node. */
void rb_insert_color(struct rb_node *node, struct rb_root *root);
void rb_erase(struct rb_node *node, struct rb_root *root);
struct rb_node *rb_first(const struct rb_root *root);
struct rb_node *rb_next(const struct rb_node *node);
struct rb_node *rb_prev(const struct rb_node *node);

/* ==========================================================================================
 * An index -> pointer map (Linux's radix tree, which only uvm_test_file.c uses): uvm_os.c keeps
 * it as a red-black tree of entries.
 * ======================================================================================== */

struct radix_tree_root {
    struct rb_root tree;
};
struct radix_tree_iter {
    unsigned long index;
    struct rb_node *node;
};
#define INIT_RADIX_TREE(root, gfp) ((root)->tree = RB_ROOT)
#define RADIX_TREE_INIT(name, gfp) { RB_ROOT }
void *radix_tree_lookup(const struct radix_tree_root *root, unsigned long index);
/* 0, -EEXIST when index is taken, or -ENOMEM. */
int radix_tree_insert(struct radix_tree_root *root, unsigned long index, void *item);
void *radix_tree_delete(struct radix_tree_root *root, unsigned long index);
/* The slot at the first index >= iter->index (from `start` the first time), or NULL. */
void **radix_tree_iter_find(const struct radix_tree_root *root, struct radix_tree_iter *iter, unsigned long start);
#define radix_tree_for_each_slot(slot, root, iter, start)                        \
    for ((slot) = radix_tree_iter_find((root), (iter), (start)); (slot);         \
         (slot) = (iter)->index == ULONG_MAX ? NULL : radix_tree_iter_find((root), (iter), (iter)->index + 1))

/* ==========================================================================================
 * Time
 * ======================================================================================== */

#define HZ 1000
#define NSEC_PER_USEC 1000ULL
#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_SEC 1000000000ULL
#define USEC_PER_MSEC 1000ULL
#define USEC_PER_SEC 1000000ULL
#define MSEC_PER_SEC 1000ULL
#define MAX_JIFFY_OFFSET ((LONG_MAX >> 1) - 1)

/* nvrm's monotonic clock (os.c, declared in uvm_nvrm.h). */
static inline NvU64 NV_GETTIME(void) { return nvrm_now_ns(); }
static inline uint64_t ktime_get_ns(void) { return nvrm_now_ns(); }
static inline uint64_t ktime_get_raw_ns(void) { return nvrm_now_ns(); }
/* jiffies are milliseconds of nvrm's clock. */
#define jiffies ((unsigned long)(nvrm_now_ns() / NSEC_PER_MSEC))
#define jiffies_64 ((uint64_t)(nvrm_now_ns() / NSEC_PER_MSEC))
static inline unsigned long msecs_to_jiffies(unsigned ms) { return ms; }
static inline unsigned long usecs_to_jiffies(unsigned long us) { return (us + 999) / 1000; }
static inline unsigned jiffies_to_msecs(unsigned long j) { return (unsigned)j; }
static inline uint64_t jiffies_to_usecs(unsigned long j) { return (uint64_t)j * 1000; }
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define time_before(a, b) time_after(b, a)
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define time_before_eq(a, b) time_after_eq(b, a)

void msleep(unsigned ms);
void usleep_range(unsigned long min_us, unsigned long max_us);
/* Busy waits (the caller cannot sleep); nvrm's clock. */
void udelay(unsigned long us);
#define mdelay(ms) udelay((unsigned long)(ms) * 1000)
/* Every thread of nvrm is preemptible, so a long loop needs no point at which to give the CPU
   away; Nocturne's scheduler takes it. */
#define cond_resched() ((void)0)
#define need_resched() false
static inline void schedule(void) { thrd_yield(); }
#define TASK_RUNNING 0
#define TASK_INTERRUPTIBLE 1
#define TASK_UNINTERRUPTIBLE 2
#define set_current_state(state) ((void)(state))
#define __set_current_state(state) ((void)(state))

/* ==========================================================================================
 * Threads ("tasks") and execution context
 * ======================================================================================== */

struct mm_struct;
#define TASK_COMM_LEN 16
#define PF_KTHREAD 0x00200000
#define THREAD_SIZE (256u * 1024u)   /* a Nocturne thread's stack (nocturne.h) */

/* One per thread of nvrm that UVM runs on. A client's request runs with the client's pid (its
   app ID) and mm (uvm_serve.c); nvrm's own threads are kernel threads (PF_KTHREAD) with pid 0
   and no mm. */
struct task_struct {
    pid_t pid;
    pid_t tgid;
    unsigned flags;
    struct mm_struct *mm;
    char comm[TASK_COMM_LEN];
    void *stack;                   /* lowest address of the thread's stack */
    bool fatal_pending;            /* the client has gone: give up long requests */
    /* kthread_* */
    thrd_t thread;
    int (*thread_fn)(void *);
    void *thread_data;
    atomic_t should_stop;
    int exit_code;
    volatile uint32_t run_state;   /* UVM_OS_KTHREAD_*: what wake_up_process / kthread_stop did */
};
enum { UVM_OS_KTHREAD_CREATED, UVM_OS_KTHREAD_RUN, UVM_OS_KTHREAD_CANCELLED };
struct task_struct *get_current(void);
#define current (get_current())
#define task_pid_nr(task) ((task)->pid)
#define task_tgid_nr(task) ((task)->tgid)
static inline bool fatal_signal_pending(struct task_struct *task) { return READ_ONCE(task->fatal_pending); }
#define signal_pending(task) fatal_signal_pending(task)
#define KSTK_EIP(task) 0UL

/* The interrupt thread (nvrm.c) is nvrm's interrupt context: in_interrupt() is true on it while it
   runs a device interrupt's top half (os.c's nvrm_in_isr, uvm_nvrm.h). Nothing runs with
   interrupts off. */
#define in_interrupt() (nvrm_in_isr)
#define in_irq() (nvrm_in_isr)
#define in_atomic() false
#define irqs_disabled() false
#define preempt_disable() barrier()
#define preempt_enable() barrier()
#define local_irq_save(flags) ((flags) = 0)
#define local_irq_restore(flags) ((void)(flags))

/* kthread_run's threads are C11 threads; kthread_stop asks one to stop (kthread_should_stop) and
   waits for its function's return value. */
struct task_struct *kthread_create(int (*fn)(void *), void *data, const char *namefmt, ...)
    __attribute__((format(printf, 3, 4)));
#define kthread_create_on_node(fn, data, node, namefmt, ...) kthread_create(fn, data, namefmt, ##__VA_ARGS__)
void wake_up_process(struct task_struct *task);
#define kthread_run(fn, data, namefmt, ...) ({                                              \
        struct task_struct *_kt = kthread_create(fn, data, namefmt, ##__VA_ARGS__);         \
        if (!IS_ERR(_kt)) wake_up_process(_kt);                                             \
        _kt;                                                                                \
    })
int kthread_stop(struct task_struct *task);
bool kthread_should_stop(void);

/* CPUs: Nocturne's (cpu_info); smp_processor_id is the CPU the thread runs on at that moment. */
#define NR_CPUS 256
unsigned uvm_os_cpu_count(void);
#define num_possible_cpus() uvm_os_cpu_count()
#define num_online_cpus() uvm_os_cpu_count()
#define nr_cpu_ids uvm_os_cpu_count()
#define smp_processor_id() ((int)cpu_index())
#define raw_smp_processor_id() ((int)cpu_index())
#define get_cpu() ((int)cpu_index())
#define put_cpu() ((void)0)
/* Per-CPU data is reached in the context that owns it (interrupt context, see in_interrupt): on
   Linux that is a CPU, in nvrm it is a thread, so a per-CPU variable is a thread-local one. */
#define DEFINE_PER_CPU(type, name) _Thread_local type name
#define get_cpu_var(var) (var)
#define put_cpu_var(var) ((void)0)
#define this_cpu_ptr(ptr) (ptr)

typedef struct cpumask { DECLARE_BITMAP(bits, NR_CPUS); } cpumask_t;
#define cpumask_bits(mask) ((mask)->bits)
static inline void cpumask_set_cpu(unsigned cpu, struct cpumask *mask) { set_bit(cpu, mask->bits); }
static inline void cpumask_clear_cpu(unsigned cpu, struct cpumask *mask) { clear_bit(cpu, mask->bits); }
static inline bool cpumask_test_cpu(int cpu, const struct cpumask *mask) { return test_bit(cpu, mask->bits); }
static inline void cpumask_clear(struct cpumask *mask) { bitmap_zero(mask->bits, NR_CPUS); }
static inline bool cpumask_empty(const struct cpumask *mask) { return bitmap_empty(mask->bits, NR_CPUS); }
static inline bool cpumask_subset(const struct cpumask *a, const struct cpumask *b) { return bitmap_subset(a->bits, b->bits, NR_CPUS); }
static inline unsigned cpumask_weight(const struct cpumask *mask) { return bitmap_weight(mask->bits, NR_CPUS); }
#define for_each_cpu(cpu, mask) for_each_set_bit(cpu, (mask)->bits, NR_CPUS)
#define for_each_possible_cpu(cpu) for ((cpu) = 0; (cpu) < (int)uvm_os_cpu_count(); (cpu)++)
#define for_each_online_cpu(cpu) for_each_possible_cpu(cpu)

/* ==========================================================================================
 * NUMA: Nocturne has one memory node, node 0, with every CPU on it.
 * ======================================================================================== */

#define NUMA_NO_NODE (-1)
#define MAX_NUMNODES 1
#define nr_node_ids 1
#define nr_online_nodes 1
typedef struct { DECLARE_BITMAP(bits, MAX_NUMNODES); } nodemask_t;
enum node_states { N_POSSIBLE, N_ONLINE, N_NORMAL_MEMORY, N_HIGH_MEMORY, N_MEMORY, N_CPU, NR_NODE_STATES };
extern nodemask_t node_possible_map;
extern nodemask_t node_online_map;
extern nodemask_t node_states[NR_NODE_STATES];
#define NODE_MASK_NONE { { 0 } }
#define node_set(node, mask) __set_bit((node), (mask).bits)
#define node_clear(node, mask) __clear_bit((node), (mask).bits)
#define node_isset(node, mask) test_bit((node), (mask).bits)
#define nodes_clear(mask) bitmap_zero((mask).bits, MAX_NUMNODES)
#define nodes_empty(mask) bitmap_empty((mask).bits, MAX_NUMNODES)
#define nodes_weight(mask) bitmap_weight((mask).bits, MAX_NUMNODES)
#define __nodes_weight(maskp, nbits) bitmap_weight((maskp)->bits, (nbits))
#define first_node(mask) ((int)find_first_bit((mask).bits, MAX_NUMNODES))
#define next_node(n, mask) ((int)find_next_bit((mask).bits, MAX_NUMNODES, (n) + 1))
#define for_each_node_mask(node, mask) for_each_set_bit(node, (mask).bits, MAX_NUMNODES)
#define for_each_node_state(node, state) for_each_node_mask(node, node_states[state])
#define for_each_online_node(node) for_each_node_mask(node, node_online_map)
#define for_each_possible_node(node) for_each_node_mask(node, node_possible_map)
#define num_possible_nodes() 1
#define num_online_nodes() 1
static inline bool node_state(int node, enum node_states state) { return node == 0 && state < NR_NODE_STATES; }
static inline bool node_online(int node) { return node == 0; }
static inline int numa_node_id(void) { return 0; }
static inline int numa_mem_id(void) { return 0; }
static inline int cpu_to_node(int cpu) { (void)cpu; return 0; }
#define LOCAL_DISTANCE 10
static inline int node_distance(int from, int to) { (void)from; (void)to; return LOCAL_DISTANCE; }
static inline NvBool nv_numa_node_has_memory(int node) { return node == 0; }
const struct cpumask *cpumask_of_node(int node);
/* The node's page frames (pfn: bus address >> PAGE_SHIFT): all of them. */
#define node_start_pfn(node) 0UL
#define node_end_pfn(node) (~0UL >> PAGE_SHIFT)

/* ==========================================================================================
 * Locks
 *
 * Every lock may be taken by any thread, including the interrupt thread (spin_lock_irqsave):
 * nvrm has no context that cannot block. The "is locked" questions are what UVM's lock
 * tracking asserts with.
 * ======================================================================================== */

typedef struct spinlock { mtx_t lock; } spinlock_t;
#define __SPIN_LOCK_UNLOCKED(name) { { 0 } }
#define DEFINE_SPINLOCK(name) spinlock_t name = __SPIN_LOCK_UNLOCKED(name)
static inline void spin_lock_init(spinlock_t *l) { mtx_init(&l->lock, mtx_plain); }
static inline void spin_lock(spinlock_t *l) { mtx_lock(&l->lock); }
static inline void spin_unlock(spinlock_t *l) { mtx_unlock(&l->lock); }
static inline bool spin_trylock(spinlock_t *l) { return mtx_trylock(&l->lock) == thrd_success; }
#define spin_lock_irqsave(l, flags) do { (flags) = 0; spin_lock(l); } while (0)
#define spin_unlock_irqrestore(l, flags) do { (void)(flags); spin_unlock(l); } while (0)
#define spin_lock_irq(l) spin_lock(l)
#define spin_unlock_irq(l) spin_unlock(l)
#define spin_lock_bh(l) spin_lock(l)
#define spin_unlock_bh(l) spin_unlock(l)
static inline bool spin_is_locked(spinlock_t *l) { return READ_ONCE(l->lock.state) != 0; }
#define assert_spin_locked(l) ((void)0)

struct mutex { mtx_t lock; };
#define DEFINE_MUTEX(name) struct mutex name = { { 0 } }
static inline void mutex_init(struct mutex *m) { mtx_init(&m->lock, mtx_plain); }
static inline void mutex_destroy(struct mutex *m) { (void)m; }
static inline void mutex_lock(struct mutex *m) { mtx_lock(&m->lock); }
static inline int mutex_lock_interruptible(struct mutex *m) { mtx_lock(&m->lock); return 0; }
#define mutex_lock_nested(m, subclass) mutex_lock(m)
static inline int mutex_trylock(struct mutex *m) { return mtx_trylock(&m->lock) == thrd_success; }
static inline void mutex_unlock(struct mutex *m) { mtx_unlock(&m->lock); }
static inline bool mutex_is_locked(struct mutex *m) { return READ_ONCE(m->lock.state) != 0; }

/* Readers-writer semaphore. Writers waiting hold off new readers, so writers do not starve. */
struct rw_semaphore {
    mtx_t lock;
    cnd_t readers_go, writers_go;
    int readers;            /* holding it for reading */
    bool writer;            /* holding it for writing */
    int writers_waiting;
};
#define __RWSEM_INITIALIZER(name) { { 0 } }
#define DECLARE_RWSEM(name) struct rw_semaphore name = __RWSEM_INITIALIZER(name)
void init_rwsem(struct rw_semaphore *sem);
void down_read(struct rw_semaphore *sem);
int down_read_trylock(struct rw_semaphore *sem);
void up_read(struct rw_semaphore *sem);
void down_write(struct rw_semaphore *sem);
int down_write_trylock(struct rw_semaphore *sem);
void up_write(struct rw_semaphore *sem);
void downgrade_write(struct rw_semaphore *sem);
#define down_read_nested(sem, subclass) down_read(sem)
#define down_write_nested(sem, subclass) down_write(sem)
#define down_read_non_owner(sem) down_read(sem)
#define up_read_non_owner(sem) up_read(sem)
static inline int rwsem_is_locked(struct rw_semaphore *sem)
{ return READ_ONCE(sem->readers) != 0 || READ_ONCE(sem->writer); }

/* Readers-writer spin lock: the same, for code that holds it only briefly. */
typedef struct rw_semaphore rwlock_t;
#define rwlock_init(l) init_rwsem(l)
#define read_lock(l) down_read(l)
#define read_unlock(l) up_read(l)
#define write_lock(l) down_write(l)
#define write_unlock(l) up_write(l)
#define read_lock_irqsave(l, flags) do { (flags) = 0; down_read(l); } while (0)
#define read_unlock_irqrestore(l, flags) do { (void)(flags); up_read(l); } while (0)
#define write_lock_irqsave(l, flags) do { (flags) = 0; down_write(l); } while (0)
#define write_unlock_irqrestore(l, flags) do { (void)(flags); up_write(l); } while (0)

/* Counting semaphores (struct semaphore, sema_init, down, down_trylock, up): uvm_shared.h. */

/* Wait queues: a waiter sleeps on the queue's sequence number, which every wake changes, and
   checks its condition after reading it, so a wake between the check and the sleep is not lost. */
typedef struct wait_queue_head { volatile uint32_t sequence; } wait_queue_head_t;
#define DECLARE_WAIT_QUEUE_HEAD(name) wait_queue_head_t name = { 0 }
static inline void init_waitqueue_head(wait_queue_head_t *wq) { wq->sequence = 0; }
void wake_up_all(wait_queue_head_t *wq);
#define wake_up(wq) wake_up_all(wq)
#define wake_up_interruptible(wq) wake_up_all(wq)
#define wake_up_interruptible_all(wq) wake_up_all(wq)
/* Sleeps until the sequence differs from `seen` or timeout_ms passed (UINT32_MAX: no limit). */
void uvm_os_wait_queue_sleep(wait_queue_head_t *wq, uint32_t seen, unsigned timeout_ms);
#define wait_event(wq, cond)                                                               \
    do {                                                                                   \
        for (;;) {                                                                         \
            uint32_t _seen = __atomic_load_n(&(wq).sequence, __ATOMIC_ACQUIRE);            \
            if (cond) break;                                                               \
            uvm_os_wait_queue_sleep(&(wq), _seen, UINT32_MAX);                             \
        }                                                                                  \
    } while (0)
#define wait_event_interruptible(wq, cond) ({ wait_event(wq, cond); 0; })
#define wait_event_killable(wq, cond) ({ wait_event(wq, cond); 0; })
/* The jiffies left (at least 1) when cond became true, 0 when the time ran out. */
#define wait_event_timeout(wq, cond, timeout) ({                                           \
        unsigned long _deadline = jiffies + (unsigned long)(timeout);                      \
        long _left = (long)(timeout);                                                      \
        for (;;) {                                                                         \
            uint32_t _seen = __atomic_load_n(&(wq).sequence, __ATOMIC_ACQUIRE);            \
            if (cond) { if (_left < 1) _left = 1; break; }                                 \
            _left = (long)(_deadline - jiffies);                                           \
            if (_left <= 0) { _left = (cond) ? 1 : 0; break; }                             \
            uvm_os_wait_queue_sleep(&(wq), _seen, (unsigned)_left);                        \
        }                                                                                  \
        _left;                                                                             \
    })
#define wait_event_interruptible_timeout(wq, cond, timeout) wait_event_timeout(wq, cond, timeout)

/* Bit locks (wait_on_bit_lock, wake_up_bit): a waiter sleeps on the 32 bits holding the bit. */
int wait_on_bit_lock(unsigned long *word, int bit, unsigned mode);
void wake_up_bit(void *word, int bit);

/* Reference counts. */
typedef struct nv_kref { atomic_t refcount; } nv_kref_t;
static inline void nv_kref_init(nv_kref_t *k) { atomic_set(&k->refcount, 1); }
static inline void nv_kref_get(nv_kref_t *k) { atomic_inc(&k->refcount); }
static inline int nv_kref_put(nv_kref_t *k, void (*release)(nv_kref_t *))
{
    if (!atomic_dec_and_test(&k->refcount))
        return 0;
    release(k);
    return 1;
}
static inline unsigned nv_kref_read(const nv_kref_t *k) { return (unsigned)atomic_read(&k->refcount); }
struct kref { atomic_t refcount; };
static inline void kref_init(struct kref *k) { atomic_set(&k->refcount, 1); }
static inline void kref_get(struct kref *k) { atomic_inc(&k->refcount); }
static inline int kref_put(struct kref *k, void (*release)(struct kref *))
{
    if (!atomic_dec_and_test(&k->refcount))
        return 0;
    release(k);
    return 1;
}
static inline unsigned kref_read(const struct kref *k) { return (unsigned)atomic_read(&k->refcount); }

/* Deferred work on UVM's work thread (uvm_os.c): delayed work runs once its delay passed. */
struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);
struct work_struct {
    struct list_head entry;
    work_func_t func;
    bool pending;
};
struct delayed_work {
    struct work_struct work;
    uint64_t due_ms;
};
#define INIT_WORK(w, fn) do { INIT_LIST_HEAD(&(w)->entry); (w)->func = (fn); (w)->pending = false; } while (0)
#define INIT_DELAYED_WORK(dw, fn) do { INIT_WORK(&(dw)->work, fn); (dw)->due_ms = 0; } while (0)
static inline struct delayed_work *to_delayed_work(struct work_struct *work)
{ return container_of(work, struct delayed_work, work); }
/* false when it was already pending. */
bool schedule_delayed_work(struct delayed_work *dwork, unsigned long delay);
/* Takes it off the queue when it has not started; true when it was pending. */
bool cancel_delayed_work(struct delayed_work *dwork);
/* The same, and waits for a run that started. */
bool cancel_delayed_work_sync(struct delayed_work *dwork);

/* ==========================================================================================
 * Memory
 * ======================================================================================== */

#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define PAGE_ALIGN(x) ALIGN((x), PAGE_SIZE)
#define PAGE_ALIGNED(x) IS_ALIGNED((unsigned long)(x), PAGE_SIZE)
#define offset_in_page(p) ((unsigned long)(p) & ~PAGE_MASK)
#define PFN_DOWN(x) ((x) >> PAGE_SHIFT)
#define PFN_UP(x) (((x) + PAGE_SIZE - 1) >> PAGE_SHIFT)
#define MAX_ORDER 11
#define TASK_SIZE 0x0000800000000000ULL    /* the end of an app's address space (kernel/src/mm) */

/* Allocation flags. Every allocation here may sleep; the ones that are meaningful in nvrm are
   __GFP_ZERO (zeroed memory) and __GFP_DMA32 (below 4 GiB for the device). */
#define __GFP_DMA 0x01u
#define __GFP_HIGHMEM 0x02u
#define __GFP_DMA32 0x04u
#define __GFP_MOVABLE 0x08u
#define __GFP_RECLAIMABLE 0x10u
#define __GFP_HIGH 0x20u
#define __GFP_IO 0x40u
#define __GFP_FS 0x80u
#define __GFP_ZERO 0x100u
#define __GFP_ATOMIC 0x200u
#define __GFP_DIRECT_RECLAIM 0x400u
#define __GFP_KSWAPD_RECLAIM 0x800u
#define __GFP_NOWARN 0x1000u
#define __GFP_RETRY_MAYFAIL 0x2000u
#define __GFP_NOFAIL 0x4000u
#define __GFP_NORETRY 0x8000u
#define __GFP_MEMALLOC 0x10000u
#define __GFP_COMP 0x20000u
#define __GFP_NOMEMALLOC 0x40000u
#define __GFP_HARDWALL 0x80000u
#define __GFP_THISNODE 0x100000u
#define __GFP_ACCOUNT 0x200000u
#define __GFP_RECLAIM (__GFP_DIRECT_RECLAIM | __GFP_KSWAPD_RECLAIM)
#define GFP_ATOMIC (__GFP_HIGH | __GFP_ATOMIC | __GFP_KSWAPD_RECLAIM)
#define GFP_KERNEL (__GFP_RECLAIM | __GFP_IO | __GFP_FS)
#define GFP_NOWAIT __GFP_KSWAPD_RECLAIM
#define GFP_NOIO __GFP_RECLAIM
#define GFP_NOFS (__GFP_RECLAIM | __GFP_IO)
#define GFP_USER (__GFP_RECLAIM | __GFP_IO | __GFP_FS | __GFP_HARDWALL)
#define GFP_DMA32 __GFP_DMA32
#define GFP_HIGHUSER (GFP_USER | __GFP_HIGHMEM)
#define GFP_HIGHUSER_MOVABLE (GFP_HIGHUSER | __GFP_MOVABLE)
#define NV_UVM_GFP_FLAGS (GFP_KERNEL | __GFP_NOMEMALLOC)
#define NV_UVM_GFP_FLAGS_ACCOUNT NV_UVM_GFP_FLAGS

/* kmalloc and vmalloc memory (uvm_os.c): one heap. A kmalloc block carries its size (ksize); a
   vmalloc block is kept in a tree by address, so is_vmalloc_addr may be asked of any address
   (UVM asks it of addresses inside vmalloc blocks and of thread stacks). */
#define ZERO_SIZE_PTR ((void *)16)
#define ZERO_OR_NULL_PTR(p) ((unsigned long)(p) <= (unsigned long)ZERO_SIZE_PTR)
void *kmalloc(size_t size, gfp_t flags);
void *krealloc(const void *p, size_t size, gfp_t flags);
void kfree(const void *p);
size_t ksize(const void *p);
static inline void *kzalloc(size_t size, gfp_t flags) { return kmalloc(size, flags | __GFP_ZERO); }
static inline void *kmalloc_array(size_t n, size_t size, gfp_t flags)
{ return size && n > SIZE_MAX / size ? NULL : kmalloc(n * size, flags); }
static inline void *kcalloc(size_t n, size_t size, gfp_t flags) { return kmalloc_array(n, size, flags | __GFP_ZERO); }
#define kmalloc_node(size, flags, node) kmalloc(size, flags)
#define kzalloc_node(size, flags, node) kzalloc(size, flags)
void *vmalloc(unsigned long size);
void *vzalloc(unsigned long size);
void vfree(const void *p);
bool is_vmalloc_addr(const void *p);

/* Object caches: blocks of one size from the heap. */
struct kmem_cache;
struct kmem_cache *nv_kmem_cache_create(const char *name, unsigned size, unsigned align);
#define NV_KMEM_CACHE_CREATE(name, type) nv_kmem_cache_create(name, sizeof(type), 0)
#define NV_KMEM_CACHE_DESTROY(cache) do { if (cache) kmem_cache_destroy(cache); } while (0)
void kmem_cache_destroy(struct kmem_cache *cache);
void *kmem_cache_alloc(struct kmem_cache *cache, gfp_t flags);
void kmem_cache_free(struct kmem_cache *cache, void *object);
static inline void *kmem_cache_zalloc(struct kmem_cache *cache, gfp_t flags) { return kmem_cache_alloc(cache, flags | __GFP_ZERO); }
#define nv_kmem_cache_zalloc kmem_cache_zalloc

/* Pages (uvm_mm.c). A struct page describes one 4 KiB page of an allocation of alloc_pages: the
   descriptors of an allocation are one array, so page + i is its i-th page as UVM expects, and
   its pfn is its bus address >> PAGE_SHIFT. The memory is DMA memory of the GPU's claim (RM's
   nv_alloc_pages), write-back cached, mapped in nvrm for its whole life. */
struct uvm_os_pages;
struct address_space;
struct page {
    struct uvm_os_pages *pages;    /* the allocation */
    void *address;                 /* nvrm's mapping of this page */
    uint64_t dma;                  /* bus address */
    atomic_t refcount;             /* the allocation's first page counts for all of them */
    struct address_space *mapping;
    unsigned long index;
    unsigned long private;
    void *zone_device_data;        /* not used: no device pages */
    struct dev_pagemap *pgmap;
};
struct page *alloc_pages(gfp_t flags, unsigned order);
#define alloc_pages_node(node, flags, order) alloc_pages(flags, order)
#define alloc_page(flags) alloc_pages(flags, 0)
void __free_pages(struct page *page, unsigned order);
#define __free_page(page) __free_pages((page), 0)
static inline void *page_address(const struct page *page) { return page->address; }
static inline void *kmap(struct page *page) { return page->address; }
static inline void kunmap(struct page *page) { (void)page; }
static inline void *kmap_atomic(struct page *page) { return page->address; }
static inline void kunmap_atomic(void *addr) { (void)addr; }
#define kmap_local_page(page) kmap(page)
#define kunmap_local(addr) ((void)(addr))
static inline unsigned long page_to_pfn(const struct page *page) { return (unsigned long)(page->dma >> PAGE_SHIFT); }
static inline phys_addr_t page_to_phys(const struct page *page) { return page->dma; }
/* The page of an allocation of alloc_pages with this pfn; NULL for any other frame. */
struct page *pfn_to_page(unsigned long pfn);
/* The page of an allocation of alloc_pages that nvrm's address p is in; NULL otherwise. */
struct page *virt_to_page(const void *p);
#define vmalloc_to_page(p) virt_to_page(p)
#define virt_addr_valid(p) (virt_to_page(p) != NULL)
static inline void *phys_to_virt(phys_addr_t phys)
{
    struct page *page = pfn_to_page((unsigned long)(phys >> PAGE_SHIFT));
    return page ? (char *)page->address + (phys & ~PAGE_MASK) : NULL;
}
static inline phys_addr_t virt_to_phys(const void *p)
{
    struct page *page = virt_to_page(p);
    return page ? page->dma + ((uintptr_t)p & ~PAGE_MASK) : 0;
}
/* References: an allocation's pages share one count, held on its first page. */
void get_page(struct page *page);
void put_page(struct page *page);
int page_count(struct page *page);
void set_page_count(struct page *page, int count);
static inline int page_to_nid(const struct page *page) { (void)page; return 0; }
static inline void set_page_dirty(struct page *page) { (void)page; }
static inline void SetPageDirty(struct page *page) { (void)page; }
static inline int get_order(unsigned long size)
{ return size <= PAGE_SIZE ? 0 : fls64((size - 1) >> PAGE_SHIFT); }
static inline bool is_device_private_page(const struct page *page) { (void)page; return false; }
static inline bool is_zone_device_page(const struct page *page) { (void)page; return false; }

/* A contiguous mapping in nvrm of pages (vmap): nvrm maps each allocation whole already, so vmap
   of the pages of one allocation, in order, is their address; other page lists get a new shared
   object composed of the pages' allocations (uvm_mm.c). */
#define VM_MAP 0x04u
void *vmap(struct page **pages, unsigned count, unsigned long flags, pgprot_t prot);
void vunmap(const void *addr);

/* ==========================================================================================
 * Device memory access (MMIO): nvrm reaches registers through mappings of the BARs.
 * ======================================================================================== */

static inline void memset_io(volatile void *dst, int c, size_t n) { memset((void *)dst, c, n); }
static inline void memcpy_toio(volatile void *dst, const void *src, size_t n) { memcpy((void *)dst, src, n); }
static inline void memcpy_fromio(void *dst, const volatile void *src, size_t n) { memcpy(dst, (const void *)src, n); }
static inline uint32_t readl(const volatile void *addr) { return *(const volatile uint32_t *)addr; }
static inline void writel(uint32_t v, volatile void *addr) { *(volatile uint32_t *)addr = v; }
static inline uint64_t readq(const volatile void *addr) { return *(const volatile uint64_t *)addr; }
static inline void writeq(uint64_t v, volatile void *addr) { *(volatile uint64_t *)addr = v; }

/* ==========================================================================================
 * The GPU as a device: DMA and BARs (uvm_rm.c fills the PCI device of the GPU nvrm serves)
 * ======================================================================================== */

struct device {
    uint64_t dma_mask;
    uint64_t coherent_dma_mask;
    void *driver_data;
};
#define IORESOURCE_MEM_64 0x00100000UL
struct resource {
    resource_size_t start, end;
    unsigned long flags;
};
#define PCI_ROM_RESOURCE 6
struct pci_dev {
    struct device dev;
    unsigned short vendor, device;
    unsigned short subsystem_vendor, subsystem_device;
    unsigned domain;
    unsigned char bus_number, devfn;
    struct resource resource[PCI_ROM_RESOURCE + 1];
};
#define to_pci_dev(d) container_of((d), struct pci_dev, dev)
#define pci_resource_start(pdev, bar) ((pdev)->resource[bar].start)
#define pci_resource_len(pdev, bar) \
    ((pdev)->resource[bar].end ? (pdev)->resource[bar].end - (pdev)->resource[bar].start + 1 : 0)
static inline int dev_to_node(const struct device *dev) { (void)dev; return 0; }
/* The PCI BAR number of NVIDIA's BAR nv_bar_index (nv.h's NV_GPU_BAR_INDEX_*): a 64-bit BAR takes
   two numbers. */
static inline NvU8 nv_bar_index_to_os_bar_index(struct pci_dev *dev, NvU8 nv_bar_index)
{
    NvU8 bar = 0;
    for (NvU8 i = 0; i < nv_bar_index && bar < PCI_ROM_RESOURCE; i++)
        bar += (dev->resource[bar].flags & IORESOURCE_MEM_64) ? 2 : 1;
    return bar;
}

enum dma_data_direction { DMA_BIDIRECTIONAL = 0, DMA_TO_DEVICE = 1, DMA_FROM_DEVICE = 2, DMA_NONE = 3 };
#define DMA_MAPPING_ERROR (~(dma_addr_t)0)
#define DMA_BIT_MASK(n) (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))
/* A page's bus address for the GPU: the device reaches every page of alloc_pages (they are DMA
   memory of its claim). */
dma_addr_t dma_map_page(struct device *dev, struct page *page, size_t offset, size_t size,
                        enum dma_data_direction dir);
static inline void dma_unmap_page(struct device *dev, dma_addr_t addr, size_t size, enum dma_data_direction dir)
{ (void)dev; (void)addr; (void)size; (void)dir; }
static inline int dma_mapping_error(struct device *dev, dma_addr_t addr) { (void)dev; return addr == DMA_MAPPING_ERROR; }
/* No IOMMU translation between the GPU's addresses and physical ones (os_pci.c). */
static inline phys_addr_t dma_to_phys(struct device *dev, dma_addr_t addr) { (void)dev; return addr; }
void *dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma, gfp_t flags);
void dma_free_coherent(struct device *dev, size_t size, void *cpu, dma_addr_t dma);

/* Mapping device memory (a BAR range) into nvrm: nv_ioremap_cache / nv_iounmap (uvm_nvrm.h). */

/* ==========================================================================================
 * Protections and page-table words
 * ======================================================================================== */

#define _PAGE_PRESENT 0x001ULL
#define _PAGE_RW 0x002ULL
#define _PAGE_USER 0x004ULL
#define _PAGE_PWT 0x008ULL
#define _PAGE_PCD 0x010ULL
#define _PAGE_NX (1ULL << 63)
#define __pgprot(x) ((pgprot_t){ (x) })
#define pgprot_val(p) ((p).pgprot)
#define PAGE_NONE __pgprot(0)
#define PAGE_KERNEL __pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_NX)
#define PAGE_KERNEL_RO __pgprot(_PAGE_PRESENT | _PAGE_NX)
#define PAGE_KERNEL_NOENC PAGE_KERNEL
#define PAGE_SHARED __pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_USER | _PAGE_NX)
#define PAGE_READONLY __pgprot(_PAGE_PRESENT | _PAGE_USER | _PAGE_NX)
static inline pgprot_t pgprot_decrypted(pgprot_t prot) { return prot; }
static inline pgprot_t uvm_pgprot_decrypted(pgprot_t prot) { return prot; }
static inline pgprot_t pgprot_noncached(pgprot_t prot) { return __pgprot(pgprot_val(prot) | _PAGE_PCD | _PAGE_PWT); }
static inline pgprot_t pgprot_writecombine(pgprot_t prot) { return __pgprot(pgprot_val(prot) | _PAGE_PWT); }

/* ==========================================================================================
 * Client address spaces (uvm_mm.c)
 *
 * An mm_struct is a client app of nvrm, shared by every UVM file the app opened; a
 * vm_area_struct is one of its mappings of a UVM file: pager-backed shared memory nvrm created
 * for the mapping, which the client mapped at vm_start (the "file offset" vm_pgoff is vm_start
 * >> PAGE_SHIFT for a VA space, as on Linux). The client's other memory is no business of UVM's
 * here (find_vma does not find it), as HMM is not configured.
 * ======================================================================================== */

#define VM_NONE 0x0UL
#define VM_READ 0x1UL
#define VM_WRITE 0x2UL
#define VM_EXEC 0x4UL
#define VM_SHARED 0x8UL
#define VM_MAYREAD 0x10UL
#define VM_MAYWRITE 0x20UL
#define VM_MAYEXEC 0x40UL
#define VM_MAYSHARE 0x80UL
#define VM_PFNMAP 0x400UL
#define VM_LOCKED 0x2000UL
#define VM_IO 0x4000UL
#define VM_DONTCOPY 0x20000UL
#define VM_DONTEXPAND 0x40000UL
#define VM_ACCOUNT 0x100000UL
#define VM_NORESERVE 0x200000UL
#define VM_HUGETLB 0x400000UL
#define VM_DONTDUMP 0x4000000UL
#define VM_MIXEDMAP 0x10000000UL

#define VM_FAULT_OOM 0x0001u
#define VM_FAULT_SIGBUS 0x0002u
#define VM_FAULT_MAJOR 0x0004u
#define VM_FAULT_HWPOISON 0x0010u
#define VM_FAULT_SIGSEGV 0x0040u
#define VM_FAULT_NOPAGE 0x0100u
#define VM_FAULT_LOCKED 0x0200u
#define VM_FAULT_RETRY 0x0400u
#define VM_FAULT_ERROR (VM_FAULT_OOM | VM_FAULT_SIGBUS | VM_FAULT_SIGSEGV | VM_FAULT_HWPOISON)
static inline int vm_fault_to_errno(vm_fault_t fault, int foll_flags)
{
    (void)foll_flags;
    if (fault & VM_FAULT_OOM) return -ENOMEM;
    if (fault & (VM_FAULT_HWPOISON | VM_FAULT_SIGBUS | VM_FAULT_SIGSEGV)) return -EFAULT;
    return 0;
}

#define FAULT_FLAG_WRITE 0x01u
#define FAULT_FLAG_MKWRITE 0x02u
#define FAULT_FLAG_ALLOW_RETRY 0x04u
#define FAULT_FLAG_RETRY_NOWAIT 0x08u
#define FAULT_FLAG_KILLABLE 0x10u
#define FAULT_FLAG_TRIED 0x20u
#define FAULT_FLAG_USER 0x40u
#define FAULT_FLAG_REMOTE 0x80u
#define FOLL_WRITE 0x01u
#define FOLL_FORCE 0x10u
#define FOLL_LONGTERM 0x100u

struct vm_area_struct;
struct uvm_os_vma;

struct mm_struct {
    struct rw_semaphore mmap_lock;
    atomic_t mm_users;              /* mmget / mmput */
    atomic_t mm_count;              /* mmgrab / mmdrop: the structure itself */
    uint32_t app;                   /* the client app's ID */
    struct rb_root vmas;            /* struct vm_area_struct by vm_start (mmap_lock) */
    void *notifier_subscriptions;   /* no MMU notifiers */
};

struct vm_fault {
    struct vm_area_struct *vma;
    unsigned flags;                 /* FAULT_FLAG_* */
    unsigned long address;          /* page aligned */
    unsigned long real_address;
    unsigned long pgoff;
    struct page *page;
};

struct vm_operations_struct {
    void (*open)(struct vm_area_struct *vma);
    void (*close)(struct vm_area_struct *vma);
    vm_fault_t (*fault)(struct vm_fault *vmf);
    vm_fault_t (*page_mkwrite)(struct vm_fault *vmf);
    int (*access)(struct vm_area_struct *vma, unsigned long addr, void *buf, int len, int write);
};

struct file;
struct vm_area_struct {
    unsigned long vm_start, vm_end;     /* [vm_start, vm_end) in the client */
    struct mm_struct *vm_mm;
    pgprot_t vm_page_prot;              /* what vm_insert_page maps with */
    unsigned long vm_flags;
    unsigned long vm_pgoff;             /* the file offset of vm_start, in pages */
    const struct vm_operations_struct *vm_ops;
    void *vm_private_data;
    struct file *vm_file;
    void *anon_vma;                     /* never: UVM's mappings are all of a file */
    struct rb_node mm_node;             /* in vm_mm->vmas */
    struct list_head mapping_node;      /* in vm_file->f_mapping's list */
    struct uvm_os_vma *os;              /* the pager behind it (uvm_mm.c) */
};
static inline unsigned long vma_pages(const struct vm_area_struct *vma) { return (vma->vm_end - vma->vm_start) >> PAGE_SHIFT; }
static inline bool vma_is_anonymous(const struct vm_area_struct *vma) { return !vma->vm_ops; }
static inline void nv_vm_flags_set(struct vm_area_struct *vma, unsigned long flags) { vma->vm_flags |= flags; }
static inline void nv_vm_flags_clear(struct vm_area_struct *vma, unsigned long flags) { vma->vm_flags &= ~flags; }
#define vm_flags_set(vma, flags) nv_vm_flags_set(vma, flags)
#define vm_flags_clear(vma, flags) nv_vm_flags_clear(vma, flags)
pgprot_t vm_get_page_prot(unsigned long vm_flags);

/* The mapping of vm_area_struct `addr` (vm_start <= addr < vm_end) or the first above it; NULL. */
struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr);
struct vm_area_struct *find_vma_intersection(struct mm_struct *mm, unsigned long start, unsigned long end);
static inline struct vm_area_struct *vma_lookup(struct mm_struct *mm, unsigned long addr)
{
    struct vm_area_struct *vma = find_vma(mm, addr);
    return vma && vma->vm_start <= addr ? vma : NULL;
}

/* Binds the page to addr of the client's mapping (n_pager_supply): read-only unless
   vma->vm_page_prot has _PAGE_RW. 0 or -errno. */
int vm_insert_page(struct vm_area_struct *vma, unsigned long addr, struct page *page);
/* The same for `size` bytes of page frames from pfn, which must be pages of alloc_pages. */
int remap_pfn_range(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn, unsigned long size, pgprot_t prot);
/* Takes the pages of [offset, offset + len) of every mapping of the file away (n_pager_revoke);
   the next touch of one faults. */
void unmap_mapping_range(struct address_space *mapping, loff_t offset, loff_t len, int even_cows);

static inline void mmgrab(struct mm_struct *mm) { atomic_inc(&mm->mm_count); }
void mmdrop(struct mm_struct *mm);
static inline void mmget(struct mm_struct *mm) { atomic_inc(&mm->mm_users); }
static inline bool mmget_not_zero(struct mm_struct *mm) { return atomic_inc_not_zero(&mm->mm_users); }
void mmput(struct mm_struct *mm);
#define mmput_async(mm) mmput(mm)
static inline void mmap_read_lock(struct mm_struct *mm) { down_read(&mm->mmap_lock); }
static inline void mmap_read_unlock(struct mm_struct *mm) { up_read(&mm->mmap_lock); }
static inline void mmap_write_lock(struct mm_struct *mm) { down_write(&mm->mmap_lock); }
static inline void mmap_write_unlock(struct mm_struct *mm) { up_write(&mm->mmap_lock); }
#define nv_mmap_read_lock mmap_read_lock
#define nv_mmap_read_unlock mmap_read_unlock
#define nv_mmap_write_lock mmap_write_lock
#define nv_mmap_write_unlock mmap_write_unlock
static inline int nv_mm_rwsem_is_locked(struct mm_struct *mm) { return rwsem_is_locked(&mm->mmap_lock); }

/* Pinning client memory (UVM's tools buffers): the pages of the current client's argument arena,
   which nvrm maps through a view of the arena (nvrm_user_view); any other range, or another
   client's, is -EFAULT. Without HMM, UVM pins nothing else of a client's. */
long get_user_pages_remote(struct mm_struct *mm, unsigned long start, unsigned long nr_pages,
                           unsigned gup_flags, struct page **pages, int *locked);
#define NV_GET_USER_PAGES_REMOTE get_user_pages_remote
#define NV_PIN_USER_PAGES_REMOTE get_user_pages_remote
#define NV_PIN_USER_PAGES(start, nr_pages, gup_flags, pages) \
    get_user_pages_remote(current->mm, (start), (nr_pages), (gup_flags), (pages), NULL)
#define NV_UNPIN_USER_PAGE put_page
#define NV_GET_USER_PAGES NV_PIN_USER_PAGES
/* A fault in a client mapping from nvrm's side (handle_mm_fault): runs the mapping's fault
   handler as a touch of the client would. */
vm_fault_t handle_mm_fault(struct vm_area_struct *vma, unsigned long address, unsigned flags, void *regs);
#define UVM_HANDLE_MM_FAULT(vma, addr, flags) handle_mm_fault(vma, addr, flags, NULL)

/* ==========================================================================================
 * Copying from and to a client: the request's argument block, or the client's arena
 * (nvrm_serve.c). Both return the bytes NOT copied, as on Linux.
 * ======================================================================================== */

unsigned long copy_from_user(void *to, const void __user *from, unsigned long n);
unsigned long copy_to_user(void __user *to, const void *from, unsigned long n);
#define get_user(x, ptr) ({ __typeof__(*(ptr)) _gu; int _gr = copy_from_user(&_gu, (ptr), sizeof _gu) ? -EFAULT : 0; (x) = _gu; _gr; })
#define put_user(x, ptr) ({ __typeof__(*(ptr)) _pu = (x); copy_to_user((ptr), &_pu, sizeof _pu) ? -EFAULT : 0; })

/* ==========================================================================================
 * Files and character devices (uvm_serve.c): UVM's fops are called by nvrm's request server
 * ======================================================================================== */

#define MINORBITS 20
#define MINORMASK ((1u << MINORBITS) - 1)
#define MAJOR(dev) ((unsigned)((dev) >> MINORBITS))
#define MINOR(dev) ((unsigned)((dev) & MINORMASK))
#define MKDEV(ma, mi) (((dev_t)(ma) << MINORBITS) | (dev_t)(mi))

struct inode;
struct poll_table_struct;
typedef struct poll_table_struct poll_table;
struct seq_file;

struct file_operations {
    struct module *owner;
    int (*open)(struct inode *inode, struct file *filp);
    int (*release)(struct inode *inode, struct file *filp);
    long (*unlocked_ioctl)(struct file *filp, unsigned cmd, unsigned long arg);
    long (*compat_ioctl)(struct file *filp, unsigned cmd, unsigned long arg);
    int (*mmap)(struct file *filp, struct vm_area_struct *vma);
    unsigned (*poll)(struct file *filp, poll_table *wait);
    ssize_t (*read)(struct file *filp, char __user *buf, size_t count, loff_t *pos);
    loff_t (*llseek)(struct file *filp, loff_t offset, int whence);
};

/* What unmap_mapping_range works on: the mappings made through a file. */
struct address_space {
    struct inode *host;
    const void *a_ops;
    struct list_head mappings;      /* struct vm_area_struct (mapping_node) */
    mtx_t lock;
};
static inline void address_space_init_once(struct address_space *mapping)
{
    memset(mapping, 0, sizeof *mapping);
    INIT_LIST_HEAD(&mapping->mappings);
}

struct cdev {
    struct module *owner;
    const struct file_operations *ops;
    dev_t dev;
    unsigned count;
};
struct inode {
    dev_t i_rdev;
    struct cdev *i_cdev;
    struct address_space *i_mapping;
    struct address_space i_data;
};
static inline unsigned iminor(const struct inode *inode) { return MINOR(inode->i_rdev); }
static inline unsigned imajor(const struct inode *inode) { return MAJOR(inode->i_rdev); }

/* One UVM file a client opened. id is the number the client names it by (what a Linux
   process's descriptor would be): fget finds the file of that number among the current client
   app's. */
struct file {
    const struct file_operations *f_op;
    struct address_space *f_mapping;
    struct inode *f_inode;
    void *private_data;
    unsigned f_flags;
    atomic_long_t f_count;
    uint32_t app;                   /* the client app that opened it */
    int id;
    void *owner;                    /* the connection it is open in; NULL once it left it */
    struct list_head files_node;    /* in its connection's open files */
};
#define file_inode(f) ((f)->f_inode)
struct file *fget(unsigned fd);
void fput(struct file *filp);
static inline struct file *get_file(struct file *f) { atomic_long_inc(&f->f_count); return f; }
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2

void cdev_init(struct cdev *cdev, const struct file_operations *fops);
/* Registers fops for minors [MINOR(dev), MINOR(dev) + count): what nvrm opens when a client asks
   for UVM's device of that minor. */
int cdev_add(struct cdev *cdev, dev_t dev, unsigned count);
void cdev_del(struct cdev *cdev);
int alloc_chrdev_region(dev_t *dev, unsigned first_minor, unsigned count, const char *name);
void unregister_chrdev_region(dev_t first, unsigned count);

/* poll: a file's poll function names the wait queue that changes when it may become ready. */
struct poll_table_struct {
    wait_queue_head_t *queue;
    uint32_t sequence;              /* the queue's sequence number when poll_wait looked */
};
static inline void poll_wait(struct file *filp, wait_queue_head_t *queue, poll_table *pt)
{
    (void)filp;
    if (pt && queue && !pt->queue) {
        pt->queue = queue;
        pt->sequence = __atomic_load_n(&queue->sequence, __ATOMIC_ACQUIRE);
    }
}
#define POLLIN 0x0001u
#define POLLPRI 0x0002u
#define POLLOUT 0x0004u
#define POLLERR 0x0008u
#define POLLHUP 0x0010u
#define POLLNVAL 0x0020u
#define POLLRDNORM 0x0040u
#define POLLWRNORM 0x0100u
typedef unsigned __poll_t;

/* seq_file: text into a buffer (UVM's procfs files print through it; nvrm has no procfs, so
   only callers that make one themselves use it). */
struct seq_file {
    char *buf;
    size_t size, count;
    void *private;
};
int seq_printf(struct seq_file *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* procfs: nvrm has none (CONFIG_PROC_FS is not defined, uvm_procfs_is_enabled() is false), so
   UVM never creates entries; these answer the calls its disabled paths still compile. */
struct proc_dir_entry;
#define NV_CREATE_PROC_DIR(name, parent) ((void)(parent), (struct proc_dir_entry *)NULL)
#define NV_CREATE_PROC_FILE(filename, parent, name, data) ((void)(parent), (void)(data), (struct proc_dir_entry *)NULL)
static inline struct proc_dir_entry *proc_symlink(const char *name, struct proc_dir_entry *parent, const char *dest)
{ (void)name; (void)parent; (void)dest; return NULL; }
static inline void proc_remove(struct proc_dir_entry *entry) { (void)entry; }
/* uvm_procfs.h defines this only with CONFIG_PROC_FS: without procfs a file's read function
   (nv_procfs_read_<name>) is not made into one. */
#define UVM_DEFINE_SINGLE_PROCFS_FILE(name)

/* ==========================================================================================
 * Odds and ends
 * ======================================================================================== */

/* A hash of two words for UVM's hash tables (uvm_os.c). */
uint32_t jhash_2words(uint32_t a, uint32_t b, uint32_t initval);
/* Sorts with qsort; the swap function UVM may pass is not needed for that. */
void sort(void *base, size_t num, size_t size, int (*cmp)(const void *, const void *),
          void (*swap_fn)(void *, void *, int));
void get_random_bytes(void *buf, int nbytes);
static inline uint32_t get_random_u32(void) { uint32_t v; get_random_bytes(&v, sizeof v); return v; }
#define prandom_u32() get_random_u32()

/* Memory cgroups: none. */
typedef struct { void *new_memcg, *old_memcg; } uvm_memcg_context_t;
static inline void uvm_memcg_context_start(uvm_memcg_context_t *context, struct mm_struct *mm) { (void)context; (void)mm; }
static inline void uvm_memcg_context_end(uvm_memcg_context_t *context) { (void)context; }

/* The Linux features UVM asks for, and what nvrm has of them. */
#define UVM_THREAD_AFFINITY_SUPPORTED() 0
#define UVM_CDMM_PAGES_SUPPORTED() 0
#define UVM_IS_CONFIG_HMM() 0
#define UVM_HMM_RANGE_FAULT_SUPPORTED() 0
#define UVM_CAN_USE_DEVICE_PRIVATE_MEMREMAP_PAGES() 0
#define UVM_CAN_USE_MMU_NOTIFIERS() 0
#define UVM_PROVIDES_DMA_BUF_IMPORTER() 0
#define UVM_CGROUP_ACCOUNTING_SUPPORTED() 0
#define UVM_USE_DMA_IOVA_API() 0
#define UVM_FOR_EACH_SGTABLE_DMA_PAGE_PRESENT() 0

/* ==========================================================================================
 * Within the OS layer: what uvm_serve.c asks of uvm_os.c and uvm_mm.c (UVM never calls these)
 * ======================================================================================== */

/* Runs the calling thread as `task` (a client's request) until uvm_os_task_leave(previous). */
struct task_struct *uvm_os_task_enter(struct task_struct *task);
void uvm_os_task_leave(struct task_struct *previous);

/* A connection's address space: app `principal`, holding a pseudo mapping of the client's
   argument arena (no vm_ops, no file) so that find_vma finds the memory UVM's tools buffers may
   live in. One reference of each kind (mm_users, mm_count); NULL without memory. */
struct mm_struct *uvm_os_mm_create(uint32_t principal);
/* A new mapping of `file` of `bytes`: the pager-backed memory the client is to map (*memory, a
   handle it is sent), not yet a vm_area_struct; *token names it until uvm_os_map_commit. 0 or
   -errno. */
int uvm_os_map(struct mm_struct *mm, struct file *file, uint64_t bytes, uint64_t *token, n_handle *memory);
/* The client mapped it at `address`: the vm_area_struct is made and the file's mmap runs. On
   failure the memory is taken back (every touch of it fails). 0 or -errno. */
int uvm_os_map_commit(struct mm_struct *mm, uint64_t token, uint64_t address);
/* The client unmapped the mapping at `address`, or (address 0) gave up the uncommitted map
   `token`. 0 or -errno. */
int uvm_os_unmap(struct mm_struct *mm, uint64_t address, uint64_t token);
/* Ends the connection's fault thread (before the connection's end tears its mappings down). */
void uvm_os_mm_stop_faults(struct mm_struct *mm);

#include "nv-kthread-q.h"

#endif
