/* Virtual memory: 4-level paging, the kernel's half (the same in every space) and the apps'
   address spaces (struct vmm_space), and every CPU's TLB kept true to them.

   Tables. The kernel half's change under kernel_space.lock, a space's lower half under its own
   lock; no one holds two tables locks, and pmm's locks come after either. Hardware and the
   lockless walks (walk) see a table only once it is zeroed: an entry is published with one store.
   The kernel half's tables are never freed. A space's go when it ends (vmm_space_end), once no
   CPU holds it; a lockless walk of a space is its own process's threads', or one under thread_lock
   while it is still its process's (sys/process.c takes it from the process under thread_lock
   before it ends it).
   A lower-half entry above the leaves is made P|W|U and never changes, and the lower half has no
   large pages: the leaves alone say what a page allows. The kernel half's leaves are global.

   TLBs. A change that adds to what a translation allows (a page where none was, write access,
   execution) is shot down nowhere: a CPU that still has the narrower translation faults on it,
   which drops it (SDM 3A 5.10.4.3, APM 2 5.5.3), and the fault's handler finds the access allowed
   now (vmm_fault_spurious) and lets the code run on. A change that takes from a translation (a
   page unmapped, write access or execution taken, a memory type changed) is made under the
   tables lock and shot down after the lock is let go, before the call returns; the frames that
   left the tables go back only after that (struct vmm_gather). No spinlock is held across a
   shootdown, which waits for other CPUs.

   A CPU holds a space while CR3 has it: those CPUs are its cpus. A space's gen counts its
   shootdowns. Each CPU keeps the spaces it held last in PCID slots, each with the gen up to which
   the CPU dropped that space's old translations (seen): loading a space again keeps them only
   when no shootdown came between. A CPU that goes on to a kernel task keeps the space in CR3
   (lazy) until a shootdown of it comes, and leaves it then.

   A shootdown's request is the sender's CPU's own (tlb_request); each receiver has an inbox, a bit
   per sender (tlb_cpu.inbox), and answers by counting down the request's counter of its last-level
   cache: the answers of one cache's CPUs share a line, and no line takes every CPU's. Shootdowns
   never wait for one another: a sender serves its own inbox while it waits, and so does a CPU that
   spins for a lock with its interrupts off (vmm_tlb_poll). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/cpuset.h"
#include "arch/smp.h"
#include "arch/spin.h"
#include "arch/topology.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sys/clock.h"
#include "sys/process.h"
#include "sys/sched.h"
#include "sys/syscall.h"
#include "limine.h"

uint64_t kernel_pml4;

#define CR3_NOFLUSH   (1ULL << 63)
#define TLB_RANGE_MAX 32 /* above this a whole flush is cheaper than invlpg per page */
#define TLB_SLOW_NS   (5 * NS_PER_S) /* a shootdown not answered for this long is reported */
#define PCID_SLOTS    8
#define LINE_WORDS    16 /* the 32-bit counters of a cache line */
#define LARGE         (1ULL << 21)
#define MMIO_FLAGS    (PTE_W | PTE_PCD | PTE_PWT | PTE_NX) /* uncached: PAT index 3 */

struct vmm_space {
    uint64_t pml4;        /* physical */
    uint64_t id;          /* never used again: a PCID slot names its space by it */
    uint64_t gen;         /* the shootdowns of its lower half so far */
    uint64_t refs;
    uint64_t *cpus;       /* the CPUs holding it in CR3 (arch/cpuset.h) */
    struct spinlock lock; /* its tables, owned and dead */
    uint64_t owned;       /* present leaves of its own frames (not PTE_SHARED) */
    uint32_t pending;     /* changes made under lock whose shootdown has not ended */
    bool dead;
};

/* The kernel half: lock guards its tables; it has no cpus, being every online CPU's. */
static struct vmm_space kernel_space;
static uint64_t next_space_id = 1;
static bool pcid_on;

struct pcid_slot {
    uint64_t id, seen, used; /* the space's id, its gen dropped up to, the CPU's stamp when loaded */
};
PERCPU static struct tlb_cpu {
    struct vmm_space *loaded; /* in CR3; NULL: the kernel's tables */
    bool lazy;                /* a kernel task runs over loaded: it leaves at loaded's next shootdown */
    unsigned slot;            /* loaded's */
    unsigned llc;             /* the CPU's, topology's */
    uint64_t stamp;
    struct pcid_slot slots[PCID_SLOTS];
    uint64_t *inbox;          /* the CPUs whose request waits for this one's answer */
    uint64_t shootdowns, ipis, evictions;
} __attribute__((aligned(64))) tlb_cpu;
PERCPU static struct tlb_request {
    struct vmm_space *space; /* the kernel half: kernel_space */
    uint64_t va, pages, gen;
    uint32_t *left;          /* [topology.llcs], a line each: the receivers of that LLC yet to answer */
    uint64_t *llcs;          /* the LLCs it waits on */
} __attribute__((aligned(64))) tlb_request;

static inline uint64_t *tbl(uint64_t entry) { return (uint64_t *)phys_to_virt(entry & PTE_ADDR); }
/* The index of va in a table of level (4: the PML4, 1: the leaves). */
static inline unsigned idx(uint64_t va, int level) { return (va >> (3 + 9 * level)) & 511; }
/* A per-CPU count only its CPU adds to, read by any. */
static inline void tally(uint64_t *n) { __atomic_store_n(n, *n + 1, __ATOMIC_RELAXED); }

/* ---- RAM ----
   The memory map's regions that are RAM (usable, ACPI tables and NVS, bootloader, kernel), joined
   where they touch, by base (the map is sorted): the direct map reaches them write-back, so
   vmm_map_phys leaves them as they are. */
struct ram_range {
    uint64_t base, end;
};
static struct ram_range *ram;
static size_t nram;

static void ram_add(uint64_t base, uint64_t end) {
    if (nram && base <= ram[nram - 1].end) ram[nram - 1].end = MAX(ram[nram - 1].end, end);
    else ram[nram++] = (struct ram_range){base, end};
}

static bool is_ram(uint64_t page) {
    size_t lo = 0, hi = nram;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (page < ram[mid].base) hi = mid;
        else if (page >= ram[mid].end) lo = mid + 1;
        else return true;
    }
    return false;
}

/* ---- this CPU's TLB ---- */

/* The current PCID's translations of [va, va + pages), all of them above TLB_RANGE_MAX pages,
   global ones excepted; either drops the current PCID's paging-structure caches too. */
static void flush_local(struct tlb_cpu *me, uint64_t va, uint64_t pages) {
    if (pages > TLB_RANGE_MAX) invpcid(INVPCID_ONE, pcid_on ? me->slot + 1 : 0, 0);
    else for (uint64_t i = 0; i < pages; i++) invlpg(va + i * PAGE_SIZE);
}

/* The kernel half's leaves are global: invlpg drops a global translation whatever the PCID (SDM 3A
   5.10.4.1), and the kernel's tables above the leaves are never taken away, so no PCID's
   paging-structure caches hold anything stale of it. */
static void flush_kernel_local(uint64_t va, uint64_t pages) {
    if (pages > TLB_RANGE_MAX) vmm_flush_all();
    else for (uint64_t i = 0; i < pages; i++) invlpg(va + i * PAGE_SIZE);
}

void vmm_flush_all(void) { invpcid(INVPCID_ALL, 0, 0); }

/* Interrupts off: CR3 to the kernel's tables (PCID 0, which never held a lower half), and this CPU
   out of the space's cpus after. */
static void leave(struct tlb_cpu *me) {
    struct vmm_space *s = me->loaded;
    write_cr3(kernel_pml4 | (pcid_on ? CR3_NOFLUSH : 0));
    me->loaded = NULL;
    me->lazy = false;
    cpuset_remove(s->cpus, cpu_current_index());
}

/* Interrupts off: s into CR3. This CPU joins s's cpus before it reads s's gen, and a sender
   advances gen before it reads cpus (both sequentially consistent): the sender asks this CPU, or
   this CPU sees the shootdown's gen and drops what its slot kept of s. It leaves the space it held
   only after CR3 changed: until then a shootdown of that space asks it too. */
static void load(struct tlb_cpu *me, struct vmm_space *s) {
    unsigned self = cpu_current_index(), k = 0;
    struct vmm_space *before = me->loaded;
    cpuset_add(s->cpus, self);
    uint64_t gen = __atomic_load_n(&s->gen, __ATOMIC_SEQ_CST);
    bool keep = false;
    if (pcid_on) {
        bool found = false;
        for (unsigned i = 0; i < PCID_SLOTS && !found; i++) {
            if (me->slots[i].id == s->id) found = true, k = i;
            else if (me->slots[i].used < me->slots[k].used) k = i;
        }
        keep = found && me->slots[k].seen == gen;
        if (!found && me->slots[k].id) tally(&me->evictions);
    }
    me->slots[k] = (struct pcid_slot){.id = s->id, .seen = gen, .used = ++me->stamp};
    me->slot = k;
    me->loaded = s;
    me->lazy = false;
    write_cr3(s->pml4 | (pcid_on ? k + 1 : 0) | (keep ? CR3_NOFLUSH : 0));
    if (before) cpuset_remove(before->cpus, self);
}

void vmm_switch(struct vmm_space *s) {
    struct tlb_cpu *me = this_cpu_ptr(tlb_cpu);
    if (!s) me->lazy = me->loaded != NULL;
    else if (s == me->loaded) me->lazy = false; /* a shootdown that came meanwhile made it leave */
    else load(me, s);
}

void vmm_leave(void) {
    uint64_t f = irq_save();
    struct tlb_cpu *me = this_cpu_ptr(tlb_cpu);
    if (me->loaded) leave(me);
    irq_restore(f);
}

/* ---- shootdowns ---- */

/* Interrupts off: what request r asks of this CPU. */
static void serve(struct tlb_cpu *me, const struct tlb_request *r) {
    struct vmm_space *s = r->space;
    if (s == &kernel_space) {
        flush_kernel_local(r->va, r->pages);
        return;
    }
    if (s != me->loaded) return; /* its slot's seen is behind s's gen now: dropped when loaded again */
    if (me->lazy) {
        leave(me);
        return;
    }
    struct pcid_slot *slot = &me->slots[me->slot];
    if (r->gen <= slot->seen) return; /* a whole flush after the change dropped it */
    if (r->gen == slot->seen + 1 && r->pages <= TLB_RANGE_MAX) {
        flush_local(me, r->va, r->pages);
        slot->seen = r->gen;
        return;
    }
    /* every change before the gen read here is dropped by the flush after it */
    uint64_t gen = __atomic_load_n(&s->gen, __ATOMIC_ACQUIRE);
    flush_local(me, 0, UINT64_MAX);
    slot->seen = gen;
}

/* Every request in this CPU's inbox, served and answered. */
static void drain(struct tlb_cpu *me) {
    if (!me->inbox) return; /* before vmm_init: no other CPU runs */
    for (size_t w = 0, words = cpuset_words(topology.cpus); w < words; w++) {
        if (!__atomic_load_n(&me->inbox[w], __ATOMIC_RELAXED)) continue;
        uint64_t f = irq_save();
        uint64_t senders = __atomic_exchange_n(&me->inbox[w], 0, __ATOMIC_ACQUIRE);
        for (; senders; senders &= senders - 1) {
            struct tlb_request *r = &per_cpu(tlb_request, w * 64 + (unsigned)__builtin_ctzll(senders));
            serve(me, r);
            /* r is its sender's again once its count reaches 0 */
            __atomic_sub_fetch(&r->left[me->llc * LINE_WORDS], 1, __ATOMIC_RELEASE);
        }
        irq_restore(f);
    }
}

void vmm_tlb_poll(void) { drain(this_cpu_ptr(tlb_cpu)); }

/* VEC_TLB_SHOOTDOWN's handler (irq_request_system, vmm_init). */
static enum irq_result tlb_interrupt(void *context) {
    (void)context;
    vmm_tlb_poll();
    return IRQ_HANDLED;
}

static void report_slow(const struct tlb_request *r, const uint64_t *targets, unsigned self) {
    kprintf("vmm: cpu%u has waited %us for a TLB shootdown; unanswered:", self, (unsigned)(TLB_SLOW_NS / NS_PER_S));
    for (unsigned llc = 0; llc < topology.llcs; llc++) {
        uint32_t left = __atomic_load_n(&r->left[llc * LINE_WORDS], __ATOMIC_RELAXED);
        if (left) kprintf(" LLC %u: %u", llc, left);
    }
    kprintf(", not taken by");
    cpuset_each(cpu, targets, topology.cpus)
        if (cpuset_has(per_cpu(tlb_cpu, cpu).inbox, self)) kprintf(" cpu%u", cpu);
    kprintf("\n");
}

/* Every CPU's old translations of [va, va + pages) of s (the kernel half's: of every space)
   dropped before it returns, the tables changed already. Not from an interrupt's handler or under
   a spinlock: it waits for the other CPUs, serving its own inbox meanwhile, with preemption off,
   since the request and the inbox bit it posts are this CPU's. */
static void shoot(struct vmm_space *s, uint64_t va, uint64_t pages) {
    ASSERT(!in_interrupt() && !this_cpu(spins_held));
    preempt_disable();
    struct tlb_cpu *me = this_cpu_ptr(tlb_cpu);
    struct tlb_request *r = this_cpu_ptr(tlb_request);
    unsigned self = cpu_current_index();
    bool kernel = s == &kernel_space;
    tally(&me->shootdowns);
    r->space = s;
    r->va = va;
    r->pages = pages;
    /* the changes before the targets are read: a CPU that joins after reads the gen (load), or
       comes online after and drops everything (vmm_online) */
    if (kernel) {
        r->gen = 0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    } else {
        r->gen = __atomic_add_fetch(&s->gen, 1, __ATOMIC_SEQ_CST);
    }
    uint64_t f = irq_save();
    serve(me, r);
    irq_restore(f);
    if (cpu_online_count() < 2) {
        preempt_enable();
        return;
    }
    const uint64_t *targets = kernel ? cpu_online_set() : s->cpus;
    uint64_t bit = 1ULL << (self % 64);
    cpuset_each(cpu, targets, topology.cpus) {
        if (cpu == self) continue;
        unsigned llc = topology.cpu[cpu].llc;
        uint32_t *left = &r->left[llc * LINE_WORDS];
        uint64_t *box = &per_cpu(tlb_cpu, cpu).inbox[self / 64];
        r->llcs[llc / 64] |= 1ULL << (llc % 64);
        __atomic_add_fetch(left, 1, __ATOMIC_RELAXED);
        uint64_t had = __atomic_fetch_or(box, bit, __ATOMIC_SEQ_CST);
        /* a CPU leaving the online set (a sleep) looks at its inbox after it left: it took the bit,
           or this takes it back */
        if (!cpu_online_at(cpu)) {
            if (__atomic_fetch_and(box, ~bit, __ATOMIC_SEQ_CST) & bit) __atomic_sub_fetch(left, 1, __ATOMIC_RELAXED);
            continue;
        }
        /* a word that was set has its IPI on the way, and its receiver takes the whole word */
        if (!had && cpu_tlb_ipi(cpu)) tally(&me->ipis);
    }
    uint64_t since = clock_now_ns();
    bool told = false;
    for (size_t w = 0, words = cpuset_words(topology.llcs); w < words; w++) {
        for (uint64_t llcs = r->llcs[w]; llcs; llcs &= llcs - 1) {
            const uint32_t *left = &r->left[(w * 64 + (unsigned)__builtin_ctzll(llcs)) * LINE_WORDS];
            while (__atomic_load_n(left, __ATOMIC_ACQUIRE)) {
                drain(me);
                pause();
                if (!told && clock_now_ns() - since > TLB_SLOW_NS) {
                    report_slow(r, targets, self);
                    told = true;
                }
            }
        }
        r->llcs[w] = 0;
    }
    preempt_enable();
}

void vmm_tlb_stats(uint64_t *shootdowns, uint64_t *ipis, uint64_t *evictions) {
    *shootdowns = *ipis = *evictions = 0;
    for (unsigned cpu = 0; cpu < topology.cpus; cpu++) {
        struct tlb_cpu *c = &per_cpu(tlb_cpu, cpu);
        *shootdowns += __atomic_load_n(&c->shootdowns, __ATOMIC_RELAXED);
        *ipis += __atomic_load_n(&c->ipis, __ATOMIC_RELAXED);
        *evictions += __atomic_load_n(&c->evictions, __ATOMIC_RELAXED);
    }
}

/* ---- gathered changes ---- */

static void gather_init(struct vmm_gather *g, struct vmm_space *s) {
    *g = (struct vmm_gather){.space = s, .lo = UINT64_MAX, .hi = 0};
}

void vmm_gather_init(struct vmm_gather *g) { gather_init(g, &kernel_space); }

/* The space's lock held: a translation of the page at va went or narrowed. The space counts g as
   pending from its first change to the end of its shootdown. */
static void gather_add(struct vmm_gather *g, uint64_t va) {
    if (!g->counted) {
        __atomic_add_fetch(&g->space->pending, 1, __ATOMIC_RELAXED);
        g->counted = true;
    }
    g->lo = MIN(g->lo, va);
    g->hi = MAX(g->hi, va + PAGE_SIZE);
}

/* The space's lock held, at each step of an operation that takes from [start, end): another
   operation changed the tables and has not shot its changes down yet, some maybe in this range.
   This one then shoots its whole range before it returns, as if it had made them: it never
   returns while a CPU may still use what it was to take away. */
static void gather_overlap(struct vmm_gather *g, uint64_t start, uint64_t end) {
    if (__atomic_load_n(&g->space->pending, __ATOMIC_ACQUIRE) <= (g->counted ? 1u : 0u)) return;
    g->lo = MIN(g->lo, start);
    g->hi = MAX(g->hi, end);
}

void vmm_gather_finish(struct vmm_gather *g) {
    if (g->lo < g->hi) shoot(g->space, g->lo, (g->hi - g->lo) / PAGE_SIZE);
    if (g->counted) __atomic_sub_fetch(&g->space->pending, 1, __ATOMIC_RELEASE);
    pmm_list_free(&g->frames);
}

/* ---- tables ---- */

/* A large page's entry (level 2: 2 MiB, 3: 1 GiB) becomes a table of the next level that maps the
   same: the translation is the same, so no TLB holds anything it contradicts (SDM 3A 5.10.2.3).
   False without memory for it. */
static bool split_large(uint64_t *entry, int level) {
    uint64_t e = *entry, size = level == 2 ? LARGE : 1ULL << 30;
    uint64_t base = e & PTE_ADDR & ~(size - 1), table = pmm_alloc_zeroed();
    if (!table) return false;
    uint64_t *t = tbl(table), flags = e & (0xFFF | PTE_NX) & ~PTE_PS;
    bool pat = e & PTE_PAT2M;
    for (unsigned i = 0; i < 512; i++)
        t[i] = level == 2 ? (base + i * PAGE_SIZE) | flags | (pat ? PTE_PAT4K : 0)
                          : (base + i * LARGE) | flags | PTE_PS | (pat ? PTE_PAT2M : 0);
    __atomic_store_n(entry, table | (e & (PTE_P | PTE_W | PTE_U)), __ATOMIC_RELEASE);
    return true;
}

/* The tables lock held: the table below table[i] (level: the level of table's entries), made when
   create, P|W and in the lower half U too; a large page there is split into one. NULL when there
   is none, or no memory for one. */
static uint64_t *next_level(uint64_t *table, unsigned i, int level, bool create, bool user) {
    uint64_t e = table[i];
    if (e & PTE_P) {
        if (e & PTE_PS) {
            ASSERT(!user);
            if (!split_large(&table[i], level)) return NULL;
            e = table[i];
        }
        ASSERT(!user || (e & (PTE_W | PTE_U)) == (PTE_W | PTE_U));
        return tbl(e);
    }
    if (!create) return NULL;
    uint64_t t = pmm_alloc_zeroed();
    if (!t) return NULL;
    __atomic_store_n(&table[i], t | PTE_P | PTE_W | (user ? PTE_U : 0), __ATOMIC_RELEASE);
    return tbl(t);
}

/* The tables lock held: the 4 KiB leaf of va in s (the kernel half: kernel_space), its tables made
   when create; NULL when there is none, or no memory for one. */
static uint64_t *leaf_of(struct vmm_space *s, uint64_t va, bool create) {
    uint64_t *t = tbl(s->pml4);
    for (int level = 4; t && level > 1; level--) t = next_level(t, idx(va, level), level, create, va < USER_TOP);
    return t ? &t[idx(va, 1)] : NULL;
}

/* The lower half, s's lock held: the leaves' table holding va, NULL when there is none; *next: the
   end of what it, or the missing table above it, covers. */
static uint64_t *leaves_of(struct vmm_space *s, uint64_t va, uint64_t *next) {
    uint64_t *t = tbl(s->pml4);
    for (int level = 4; level > 1; level--) {
        uint64_t e = t[idx(va, level)], span = 1ULL << (3 + 9 * level);
        if (!(e & PTE_P)) {
            *next = ALIGN_DOWN(va, span) + span;
            return NULL;
        }
        t = tbl(e);
    }
    *next = ALIGN_DOWN(va, LARGE) + LARGE;
    return t;
}

/* Lockless: the leaf mapping va, *size its page's size; NULL when a table above it is missing. The
   tables it reads stay (see the top); the leaf may change meanwhile. */
static uint64_t *walk(uint64_t pml4, uint64_t va, uint64_t *size) {
    uint64_t *t = tbl(pml4);
    for (int level = 4; level > 1; level--) {
        uint64_t *p = &t[idx(va, level)], e = __atomic_load_n(p, __ATOMIC_ACQUIRE);
        if (!(e & PTE_P)) return NULL;
        if (e & PTE_PS) {
            *size = 1ULL << (3 + 9 * level);
            return p;
        }
        t = tbl(e);
    }
    *size = PAGE_SIZE;
    return &t[idx(va, 1)];
}

static uint64_t translate(uint64_t pml4, uint64_t va) {
    uint64_t size, *p = walk(pml4, va, &size), e = p ? __atomic_load_n(p, __ATOMIC_ACQUIRE) : 0;
    if (!(e & PTE_P)) return 0;
    return (e & PTE_ADDR & ~(size - 1)) + (va & (size - 1));
}

/* ---- the kernel half ---- */

/* Boot, the first CPU alone, before CR3 has these tables: written over as it is (neighbours in the
   memory map may share a page), with 2 MiB pages where the run allows. flags: the 4 KiB form. */
static void boot_map(uint64_t va, uint64_t pa, uint64_t size, uint64_t flags) {
    uint64_t large = (flags & ~PTE_PAT4K) | (flags & PTE_PAT4K ? PTE_PAT2M : 0) | PTE_PS;
    for (uint64_t end = pa + size; pa < end;) {
        uint64_t *l2 = tbl(kernel_pml4);
        for (int level = 4; l2 && level > 2; level--) l2 = next_level(l2, idx(va, level), level, true, false);
        uint64_t *pde = l2 ? &l2[idx(va, 2)] : NULL;
        if (pde && !((va | pa) & (LARGE - 1)) && end - pa >= LARGE && (!(*pde & PTE_P) || (*pde & PTE_PS))) {
            *pde = pa | large | PTE_G | PTE_P;
            va += LARGE;
            pa += LARGE;
            continue;
        }
        uint64_t *l1 = pde ? next_level(l2, idx(va, 2), 2, true, false) : NULL;
        if (!l1) panic("vmm: no memory for the kernel's page tables");
        l1[idx(va, 1)] = pa | flags | PTE_G | PTE_P;
        va += PAGE_SIZE;
        pa += PAGE_SIZE;
    }
}

bool vmm_kernel_map(uint64_t va, uint64_t pa, uint64_t flags) {
    ASSERT(va >= USER_TOP);
    uint64_t f = spin_lock(&kernel_space.lock);
    uint64_t *leaf = leaf_of(&kernel_space, va, true);
    if (leaf && (*leaf & PTE_P)) panic("vmm: kernel page %p mapped twice", (void *)va);
    if (leaf) __atomic_store_n(leaf, (pa & PTE_ADDR) | flags | PTE_G | PTE_P, __ATOMIC_RELEASE);
    spin_unlock(&kernel_space.lock, f);
    return leaf != NULL;
}

uint64_t vmm_kernel_translate(uint64_t va) { return translate(kernel_pml4, va); }

/* A present direct-map leaf maps its page as flags (the 4 KiB form) asks: the direct map's
   addresses are its pages', so only the access and the memory type are compared. */
static bool maps_as(uint64_t leaf, uint64_t size, uint64_t flags) {
    const uint64_t access = PTE_W | PTE_U | PTE_PWT | PTE_PCD | PTE_NX;
    bool pat = leaf & (size == PAGE_SIZE ? PTE_PAT4K : PTE_PAT2M);
    return (leaf & access) == (flags & access) && pat == !!(flags & PTE_PAT4K);
}

/* Device memory is mapped once, uncached, and stays: boot maps the memory map's reserved regions
   so, and what lies outside the map gets its pages here. A page mapped otherwise (RAM, the
   framebuffer) is not changed: that would need a shootdown, and x86 wants one memory type for a
   page in every mapping (vmm_direct_type changes RAM's). */
void *vmm_map_mmio(uint64_t phys, uint64_t size) {
    uint64_t end = ALIGN_UP(phys + size, PAGE_SIZE);
    for (uint64_t p = ALIGN_DOWN(phys, PAGE_SIZE); p < end; p += PAGE_SIZE) {
        uint64_t va = hhdm_offset + p, size, f = spin_lock(&kernel_space.lock);
        uint64_t *e = walk(kernel_pml4, va, &size), have = e ? *e : 0;
        uint64_t *leaf = have & PTE_P ? NULL : leaf_of(&kernel_space, va, true);
        bool other = (have & PTE_P) && !maps_as(have, size, MMIO_FLAGS);
        if (leaf) __atomic_store_n(leaf, p | MMIO_FLAGS | PTE_G | PTE_P, __ATOMIC_RELEASE);
        spin_unlock(&kernel_space.lock, f);
        if (other) kprintf("vmm: %p is mapped as another memory type, not as device memory\n", (void *)p);
        if (other || (!(have & PTE_P) && !leaf)) return NULL;
    }
    return phys_to_virt(phys);
}

/* Page by page: a request may run across several ranges, and only what lies in none of them
   (device memory) becomes uncached. RAM stays write-back for everyone else who uses it. */
void *vmm_map_phys(uint64_t phys, uint64_t size) {
    uint64_t end = ALIGN_UP(phys + size, PAGE_SIZE);
    for (uint64_t p = ALIGN_DOWN(phys, PAGE_SIZE); p < end; p += PAGE_SIZE)
        if (!is_ram(p) && !vmm_map_mmio(p, PAGE_SIZE)) return NULL;
    return phys_to_virt(phys);
}

/* RAM a device reaches uncached or write-combining: x86 wants every mapping of a page to agree
   on its memory type, so the direct map takes the type the user mappings get. The 2 MiB pages
   around the pages are split first (a split changes no translation, so one that finds no memory
   leaves every type as it was), then the leaves change, a table's worth under the lock at a time;
   the pages are shot down once, and the lines cached while they were write-back are flushed out
   of every cache (clflush works whatever the type of the page). */
bool vmm_direct_type(const uint64_t *frames, uint64_t base, size_t pages, uint64_t type) {
    struct vmm_gather g;
    vmm_gather_init(&g);
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < pages;) {
            uint64_t f = spin_lock(&kernel_space.lock);
            for (size_t stop = MIN(pages, i + 512); i < stop; i++) {
                uint64_t va = hhdm_offset + (frames ? frames[i] : base + i * PAGE_SIZE);
                uint64_t *leaf = leaf_of(&kernel_space, va, false); /* RAM: in the direct map */
                if (!leaf) {
                    spin_unlock(&kernel_space.lock, f);
                    return false;
                }
                if (!pass || !(*leaf & PTE_P)) continue;
                __atomic_store_n(leaf, (*leaf & ~(PTE_PWT | PTE_PCD | PTE_PAT4K)) | type, __ATOMIC_RELEASE);
                g.lo = MIN(g.lo, va);
                g.hi = MAX(g.hi, va + PAGE_SIZE);
            }
            spin_unlock(&kernel_space.lock, f);
        }
    vmm_gather_finish(&g);
    if (type)
        for (size_t i = 0; i < pages; i++) {
            uint8_t *p = phys_to_virt(frames ? frames[i] : base + i * PAGE_SIZE);
            for (size_t at = 0; at < PAGE_SIZE; at += cpu_features.clflush_bytes)
                __asm__ volatile("clflush (%0)" ::"r"(p + at) : "memory");
        }
    __asm__ volatile("mfence" ::: "memory");
    return true;
}

void vmm_kernel_unmap(struct vmm_gather *g, uint64_t va, size_t pages, bool frames) {
    uint64_t f = spin_lock(&kernel_space.lock);
    for (size_t i = 0; i < pages; i++) {
        uint64_t at = va + i * PAGE_SIZE, size;
        uint64_t *p = walk(kernel_pml4, at, &size), e = p && size == PAGE_SIZE ? *p : 0;
        if (!(e & PTE_P)) continue;
        __atomic_store_n(p, 0, __ATOMIC_RELEASE);
        if (frames) pmm_list_add(&g->frames, e & PTE_ADDR);
        g->lo = MIN(g->lo, at);
        g->hi = MAX(g->hi, at + PAGE_SIZE);
    }
    spin_unlock(&kernel_space.lock, f);
}

extern char __kernel_start[], __kernel_end[], __text_start[], __rodata_start[], __data_start[];

/* Each CPU's inbox and its request's counters, on its home node. */
static void tlb_buffers(void) {
    size_t inbox = ALIGN_UP(cpuset_words(topology.cpus) * sizeof(uint64_t), 64);
    size_t left = (size_t)topology.llcs * LINE_WORDS * sizeof(uint32_t);
    size_t pages = ALIGN_UP(inbox + left + cpuset_words(topology.llcs) * sizeof(uint64_t), PAGE_SIZE) / PAGE_SIZE;
    for (unsigned cpu = 0; cpu < topology.cpus; cpu++) {
        uint64_t run = pmm_alloc_run(pages, topology.cpu[cpu].home);
        if (!run) panic("vmm: no memory for cpu%u's shootdowns", cpu);
        uint8_t *p = phys_to_virt(run);
        memset(p, 0, pages * PAGE_SIZE);
        per_cpu(tlb_cpu, cpu).inbox = (uint64_t *)p;
        per_cpu(tlb_request, cpu).left = (uint32_t *)(p + inbox);
        per_cpu(tlb_request, cpu).llcs = (uint64_t *)(p + inbox + left);
    }
}

void vmm_init(struct limine_memmap_response *mm, struct limine_executable_address_response *ka) {
    if (!(kernel_pml4 = pmm_alloc_zeroed())) panic("vmm: no memory for the kernel's page tables");
    kernel_space.pml4 = kernel_pml4;
    kernel_space.refs = 1;
    /* every PDPT of the kernel half now: every space's PML4 shares them (vmm_space_new) */
    uint64_t *l4 = tbl(kernel_pml4);
    for (unsigned i = 256; i < 512; i++) {
        uint64_t t = pmm_alloc_zeroed();
        if (!t) panic("vmm: no memory for the kernel's page tables");
        l4[i] = t | PTE_P | PTE_W;
    }
    if (!(ram = kmalloc(mm->entry_count * sizeof *ram))) panic("vmm: no memory for the RAM ranges");
    /* the direct map of every region of the memory map */
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        uint64_t base = ALIGN_DOWN(e->base, PAGE_SIZE), end = ALIGN_UP(e->base + e->length, PAGE_SIZE);
        uint64_t flags = PTE_W | PTE_NX; /* write-back */
        if (e->type == LIMINE_MEMMAP_BAD_MEMORY) continue;
        if (e->type == LIMINE_MEMMAP_FRAMEBUFFER) flags |= PTE_PWT | PTE_PAT4K; /* write-combining: PAT index 5 */
        else if (e->type == LIMINE_MEMMAP_RESERVED) flags = MMIO_FLAGS; /* the firmware's and devices': as vmm_map_mmio */
        else ram_add(base, end);
        boot_map(hhdm_offset + base, base, end - base, flags);
    }
    /* the kernel image: code read-only and executable, rodata read-only, the rest writable */
    uint64_t kstart = ALIGN_DOWN((uint64_t)__kernel_start, PAGE_SIZE), kend = ALIGN_UP((uint64_t)__kernel_end, PAGE_SIZE);
    for (uint64_t va = kstart; va < kend; va += PAGE_SIZE) {
        uint64_t f = PTE_W | PTE_NX;
        if (va >= (uint64_t)__text_start && va < (uint64_t)__rodata_start) f = 0;
        else if (va >= (uint64_t)__rodata_start && va < (uint64_t)__data_start) f = PTE_NX;
        boot_map(va, va - ka->virtual_base + ka->physical_base, PAGE_SIZE, f);
    }
    tlb_buffers();
    if (!irq_request_system(&(struct irq_spec){.name = "TLB shootdown", .handler = tlb_interrupt, .node = -1},
                            VEC_TLB_SHOOTDOWN))
        panic("vmm: VEC_TLB_SHOOTDOWN is taken");
    write_cr3(kernel_pml4);
    pcid_on = cpu_features.pcid && !cmdline_has("nopcid");
    vmm_init_cpu();
    if (pcid_on) kprintf("vmm: PCID on, %u spaces kept per CPU\n", PCID_SLOTS);
    else kprintf("vmm: PCID off, a CPU keeps no space's translations across a switch\n");
}

/* With CR3 = kernel_pml4 (PCID 0): PCIDs on when the first CPU has them; what the slots kept is
   gone with a sleep. */
void vmm_init_cpu(void) {
    struct tlb_cpu *me = this_cpu_ptr(tlb_cpu);
    ASSERT(!me->loaded);
    memset(me->slots, 0, sizeof me->slots);
    me->llc = topology.cpu[cpu_current_index()].llc;
    if (pcid_on) write_cr4(read_cr4() | CR4_PCIDE);
}

/* No kernel-half shootdown asked this CPU before it was online. */
void vmm_online(void) { vmm_flush_all(); }

/* ---- address spaces ---- */

struct vmm_space *vmm_space_new(void) {
    struct vmm_space *s = kzalloc(sizeof *s);
    uint64_t *cpus = kzalloc(cpuset_words(topology.cpus) * sizeof *cpus), pml4 = pmm_alloc_zeroed();
    if (!s || !cpus || !pml4) {
        kfree(s);
        kfree(cpus);
        if (pml4) pmm_free(pml4);
        return NULL;
    }
    uint64_t *n = tbl(pml4), *k = tbl(kernel_pml4);
    for (unsigned i = 256; i < 512; i++) n[i] = k[i];
    s->pml4 = pml4;
    s->cpus = cpus;
    s->refs = 1;
    s->id = __atomic_fetch_add(&next_space_id, 1, __ATOMIC_RELAXED);
    return s;
}

void vmm_space_get(struct vmm_space *s) { __atomic_add_fetch(&s->refs, 1, __ATOMIC_RELAXED); }

void vmm_space_put(struct vmm_space *s) {
    if (__atomic_sub_fetch(&s->refs, 1, __ATOMIC_ACQ_REL)) return;
    ASSERT(s->dead && cpuset_next(s->cpus, 0, topology.cpus) == topology.cpus);
    pmm_free(s->pml4);
    kfree(s->cpus);
    kfree(s);
}

/* The lower half's tables below entry (level: the level of the entries of its table) and the
   frames of the space's own pages, freed: no CPU reaches them any more. */
static void free_tables(uint64_t entry, int level) {
    uint64_t *t = tbl(entry);
    for (unsigned i = 0; i < 512; i++) {
        uint64_t e = t[i];
        if (!(e & PTE_P)) continue;
        if (level > 1) free_tables(e, level - 1);
        else if (!(e & PTE_SHARED)) pmm_free(e & PTE_ADDR);
    }
    pmm_free(entry & PTE_ADDR);
}

/* Dead under its lock, the space changes no more: every operation on it looks first. The
   operations that took pages out before free them themselves. */
void vmm_space_end(struct vmm_space *s) {
    uint64_t f = spin_lock(&s->lock);
    ASSERT(!s->dead);
    s->dead = true;
    s->owned = 0;
    spin_unlock(&s->lock, f);
    /* every CPU off it: one that still holds it lazily leaves on this */
    shoot(s, 0, UINT64_MAX);
    ASSERT(cpuset_next(s->cpus, 0, topology.cpus) == topology.cpus);
    uint64_t *l4 = tbl(s->pml4);
    for (unsigned i = 0; i < 256; i++) {
        if (l4[i] & PTE_P) free_tables(l4[i], 3);
        l4[i] = 0;
    }
}

/* ---- a space's lower half ---- */

/* s's lock held: owned follows a leaf that changed from old to now. */
static void count(struct vmm_space *s, uint64_t old, uint64_t now) {
    bool before = (old & PTE_P) && !(old & PTE_SHARED), after = (now & PTE_P) && !(now & PTE_SHARED);
    if (before == after) return;
    ASSERT(after || s->owned);
    __atomic_store_n(&s->owned, after ? s->owned + 1 : s->owned - 1, __ATOMIC_RELAXED);
}

/* A present leaf old may become now with no shootdown: the same page and memory type, and no access
   taken away (write access or execution added, or the same). */
static bool raises(uint64_t old, uint64_t now) {
    const uint64_t same = PTE_ADDR | PTE_U | PTE_PWT | PTE_PCD | PTE_PAT4K | PTE_G | PTE_SHARED;
    return !((old ^ now) & same) && (!(old & PTE_W) || (now & PTE_W)) && ((old & PTE_NX) || !(now & PTE_NX));
}

int vmm_map_page(struct vmm_space *s, uint64_t va, uint64_t pa, uint64_t flags) {
    ASSERT(va < USER_TOP);
    uint64_t now = (pa & PTE_ADDR) | flags | PTE_P, f = spin_lock(&s->lock);
    uint64_t *leaf = s->dead ? NULL : leaf_of(s, va, true);
    int r = s->dead ? -N_ERR_CLOSED : leaf ? 0 : -N_ERR_NO_MEMORY;
    if (leaf) {
        uint64_t old = *leaf;
        if (!(old & PTE_P)) {
            __atomic_store_n(leaf, now, __ATOMIC_RELEASE);
            count(s, old, now);
        } else if (!raises(old, now)) {
            r = -N_ERR_EXISTS;
        } else if ((old & ~(PTE_A | PTE_D)) != now) {
            __atomic_store_n(leaf, now | (old & (PTE_A | PTE_D)), __ATOMIC_RELEASE);
        }
    }
    spin_unlock(&s->lock, f);
    return r;
}

uint64_t vmm_translate(struct vmm_space *s, uint64_t va) { return translate(s->pml4, va); }

uint64_t vmm_get_leaf(struct vmm_space *s, uint64_t va) {
    uint64_t size, *p = walk(s->pml4, va, &size);
    return p ? __atomic_load_n(p, __ATOMIC_ACQUIRE) : 0;
}

uint64_t vmm_get_pte(struct vmm_space *s, uint64_t va) {
    uint64_t e = vmm_get_leaf(s, va);
    return e & PTE_P ? e : 0;
}

uint64_t vmm_user_pages(struct vmm_space *s) { return __atomic_load_n(&s->owned, __ATOMIC_RELAXED); }

void vmm_user_pages_split(struct vmm_space *s, uint64_t out[6]) {
    for (int i = 0; i < 6; i++) out[i] = 0;
    uint64_t *l4 = tbl(s->pml4);
    for (unsigned a = 0; a < 256; a++) {
        if (!(l4[a] & PTE_P)) continue;
        uint64_t *l3 = tbl(l4[a]);
        out[5]++;
        for (unsigned b = 0; b < 512; b++) {
            if (!(l3[b] & PTE_P)) continue;
            uint64_t *l2 = tbl(l3[b]);
            out[5]++;
            for (unsigned c = 0; c < 512; c++) {
                if (!(l2[c] & PTE_P)) continue;
                uint64_t *l1 = tbl(l2[c]);
                uint64_t va = ((uint64_t)a << 39) | ((uint64_t)b << 30) | ((uint64_t)c << 21);
                int region = va < USER_LOW_BASE ? 0 : va < USER_MMAP_BASE ? 1
                           : va < USER_STACK_TOP - USER_STACK_MAX ? 2 : 3;
                out[5]++;
                for (unsigned d = 0; d < 512; d++)
                    if (l1[d] & PTE_P) out[l1[d] & PTE_SHARED ? 4 : region]++;
            }
        }
    }
}

static uint64_t prot_flags(int prot) {
    return PTE_U | (prot & VM_W ? PTE_W : 0) | (prot & VM_X ? 0 : PTE_NX);
}

/* s's lock held, the leaf present: it gains prot's access (a raise). False when a read-only
   shared page was to be written or run. */
static bool grant(uint64_t *leaf, int prot) {
    uint64_t now = *leaf;
    if ((now & PTE_SHARED_RO) && (prot & (VM_W | VM_X))) return false;
    if (prot & VM_W) now |= PTE_W;
    if (prot & VM_X) now &= ~PTE_NX;
    if (now != *leaf) __atomic_store_n(leaf, now, __ATOMIC_RELEASE);
    return true;
}

/* Reserved pages filled together with a touched one: the following leaves of
   the same table, so ascending first touches fault once per 64 KiB. */
#define DEMAND_AROUND 16

/* s's lock held: a first touch of the reserved page at va (write: a write), and the reserved pages
   after it in its table (while memory is not short): 1 when va's page is present now, 0 when it is
   no reserved page it may touch so, -N_ERR_NO_MEMORY when no frame came for it. */
static int demand_fill(struct vmm_space *s, uint64_t va, bool write) {
    uint64_t page = ALIGN_DOWN(va, PAGE_SIZE), next, *l1 = leaves_of(s, page, &next);
    if (!l1) return 0;
    int result = 0;
    for (unsigned i = idx(page, 1), n = 0; i < 512 && n < DEMAND_AROUND; i++, n++) {
        uint64_t e = l1[i];
        if (!n && (e & PTE_P)) return 1; /* a sibling filled it first */
        if ((e & (PTE_P | PTE_DEMAND)) != PTE_DEMAND || (!n && write && !(e & PTE_W))) break;
        uint64_t frame = !n || pmm_free_pages() >= 4096 ? pmm_alloc_zeroed() : 0;
        if (!frame) return n ? result : -N_ERR_NO_MEMORY;
        uint64_t now = frame | (e & ~PTE_DEMAND) | PTE_P;
        __atomic_store_n(&l1[i], now, __ATOMIC_RELEASE);
        count(s, e, now);
        result = 1;
    }
    return result;
}

/* Fresh zeroed pages. Pages that are already mapped keep their contents and gain the requested
   access (two ELF segments can share a page); reserved ones are filled. */
int vmm_user_alloc(struct vmm_space *s, uint64_t va, uint64_t size, int prot) {
    uint64_t start = ALIGN_DOWN(va, PAGE_SIZE), end = ALIGN_UP(va + size, PAGE_SIZE), frame = 0;
    int result = 0;
    uint64_t f = spin_lock(&s->lock);
    for (uint64_t a = start; a < end && !result;) {
        uint64_t *leaf = s->dead ? NULL : leaf_of(s, a, true);
        if (!leaf) {
            result = s->dead ? -N_ERR_CLOSED : -N_ERR_NO_MEMORY;
            break;
        }
        if ((*leaf & (PTE_P | PTE_DEMAND)) == PTE_DEMAND && demand_fill(s, a, false) < 0) {
            result = -N_ERR_NO_MEMORY;
            break;
        }
        if (*leaf & PTE_P) {
            if (!grant(leaf, prot)) result = -N_ERR_DENIED;
            a += PAGE_SIZE;
            continue;
        }
        if (!frame) { /* outside the lock; the page is looked at again with it */
            spin_unlock(&s->lock, f);
            frame = pmm_alloc_zeroed();
            f = spin_lock(&s->lock);
            if (!frame) result = -N_ERR_NO_MEMORY;
            continue;
        }
        uint64_t old = *leaf, now = frame | prot_flags(prot) | PTE_P;
        __atomic_store_n(leaf, now, __ATOMIC_RELEASE);
        count(s, old, now);
        frame = 0;
        a += PAGE_SIZE;
    }
    spin_unlock(&s->lock, f);
    if (frame) pmm_free(frame);
    return result;
}

/* Pages that receive a zeroed frame only when first touched: a non-present leaf holds PTE_DEMAND
   and the page's access. Present pages keep their contents and gain the access, as with
   vmm_user_alloc. Only the page tables are allocated here, a table's worth under the lock at a
   time. */
int vmm_user_reserve(struct vmm_space *s, uint64_t va, uint64_t size, int prot) {
    uint64_t start = ALIGN_DOWN(va, PAGE_SIZE), end = ALIGN_UP(va + size, PAGE_SIZE), marker = prot_flags(prot) | PTE_DEMAND;
    int result = 0;
    for (uint64_t a = start; a < end && !result;) {
        uint64_t stop = MIN(end, ALIGN_DOWN(a, LARGE) + LARGE), f = spin_lock(&s->lock);
        if (s->dead) result = -N_ERR_CLOSED;
        for (; a < stop && !result; a += PAGE_SIZE) {
            uint64_t *leaf = leaf_of(s, a, true);
            if (!leaf) {
                result = -N_ERR_NO_MEMORY;
                break;
            }
            uint64_t now = *leaf, want = marker;
            if (now & PTE_P) {
                if (!grant(leaf, prot)) result = -N_ERR_DENIED;
                continue;
            }
            if (now & PTE_DEMAND) {
                want = now;
                if (prot & VM_W) want |= PTE_W;
                if (prot & VM_X) want &= ~PTE_NX;
            }
            __atomic_store_n(leaf, want, __ATOMIC_RELEASE);
        }
        spin_unlock(&s->lock, f);
    }
    return result;
}

int vmm_demand_fault(struct vmm_space *s, uint64_t va, bool write) {
    if (va >= USER_TOP) return 0;
    uint64_t f = spin_lock(&s->lock);
    int result = s->dead ? 0 : demand_fill(s, va, write);
    spin_unlock(&s->lock, f);
    return result;
}

/* mprotect: exactly these permissions on already-mapped user pages. shared: pages of one shared
   mapping, whose owner checked prot against the mapping's access and keeps it for the pages that
   are not present yet (they are skipped here); a read-only shared leaf (PTE_SHARED_RO, a paged
   page bound for reading) gains no write access. A table's worth under the lock at a time; the
   leaves that lost access are shot down before it returns. */
int vmm_user_protect(struct vmm_space *s, uint64_t va, uint64_t size, int prot, bool shared) {
    uint64_t start = ALIGN_DOWN(va, PAGE_SIZE), end = ALIGN_UP(va + size, PAGE_SIZE), want = prot_flags(prot);
    for (uint64_t a = start; !shared && a < end;) {
        uint64_t stop = MIN(end, ALIGN_DOWN(a, LARGE) + LARGE), next, f = spin_lock(&s->lock);
        uint64_t *l1 = s->dead ? NULL : leaves_of(s, a, &next);
        int r = 0;
        for (; a < stop && !r; a += PAGE_SIZE) {
            uint64_t e = l1 ? l1[idx(a, 1)] : 0;
            if (!(e & (PTE_P | PTE_DEMAND)) || !(e & PTE_U)) r = -N_ERR_NO_MEMORY;
            else if ((e & PTE_SHARED_RO) && (prot & (VM_W | VM_X))) r = -N_ERR_DENIED;
        }
        spin_unlock(&s->lock, f);
        if (r) return r;
    }
    struct vmm_gather g;
    gather_init(&g, s);
    for (uint64_t a = start; a < end;) {
        uint64_t stop = MIN(end, ALIGN_DOWN(a, LARGE) + LARGE), next, f = spin_lock(&s->lock);
        if (s->dead) {
            spin_unlock(&s->lock, f);
            break;
        }
        gather_overlap(&g, start, end);
        uint64_t *l1 = leaves_of(s, a, &next);
        for (; l1 && a < stop; a += PAGE_SIZE) {
            uint64_t *p = &l1[idx(a, 1)], e = *p;
            if (!(e & (shared ? PTE_P : PTE_P | PTE_DEMAND))) continue;
            uint64_t bits = shared && (e & PTE_SHARED_RO) ? PTE_NX : PTE_W | PTE_NX, now = (e & ~bits) | (want & bits);
            if (now == e) continue;
            __atomic_store_n(p, now, __ATOMIC_RELEASE);
            if ((e & PTE_P) && !raises(e, now)) gather_add(&g, a);
        }
        a = l1 ? stop : MIN(end, next);
        spin_unlock(&s->lock, f);
    }
    vmm_gather_finish(&g);
    return 0;
}

/* Takes write access away from the present leaves of [va, va+size) and marks every one of them a
   read-only shared page (PTE_SHARED_RO: no n_memory_protect gives it back), also those an
   n_memory_protect had left without it. The leaves that had write access are shot down before it
   returns, so no CPU writes through an old translation. */
void vmm_user_write_protect(struct vmm_space *s, uint64_t va, uint64_t size) {
    uint64_t start = ALIGN_DOWN(va, PAGE_SIZE), end = ALIGN_UP(va + size, PAGE_SIZE);
    struct vmm_gather g;
    gather_init(&g, s);
    for (uint64_t a = start; a < end;) {
        uint64_t stop = MIN(end, ALIGN_DOWN(a, LARGE) + LARGE), next, f = spin_lock(&s->lock);
        if (s->dead) {
            spin_unlock(&s->lock, f);
            break;
        }
        gather_overlap(&g, start, end);
        uint64_t *l1 = leaves_of(s, a, &next);
        for (; l1 && a < stop; a += PAGE_SIZE) {
            uint64_t *p = &l1[idx(a, 1)], e = *p, now = (e & ~PTE_W) | PTE_SHARED_RO;
            if (!(e & PTE_P) || now == e) continue;
            __atomic_store_n(p, now, __ATOMIC_RELEASE);
            if (e & PTE_W) gather_add(&g, a);
        }
        a = l1 ? stop : MIN(end, next);
        spin_unlock(&s->lock, f);
    }
    vmm_gather_finish(&g);
}

/* The leaves of [va, va+size) cleared, reservations too, a table's worth under the lock at a time;
   the present ones are shot down, and the frames of the space's own pages freed after. */
void vmm_user_free(struct vmm_space *s, uint64_t va, uint64_t size) {
    uint64_t start = ALIGN_DOWN(va, PAGE_SIZE), end = ALIGN_UP(va + size, PAGE_SIZE);
    struct vmm_gather g;
    gather_init(&g, s);
    for (uint64_t a = start; a < end;) {
        uint64_t stop = MIN(end, ALIGN_DOWN(a, LARGE) + LARGE), next, f = spin_lock(&s->lock);
        if (s->dead) {
            spin_unlock(&s->lock, f);
            break;
        }
        gather_overlap(&g, start, end);
        uint64_t *l1 = leaves_of(s, a, &next);
        for (; l1 && a < stop; a += PAGE_SIZE) {
            uint64_t *p = &l1[idx(a, 1)], e = *p;
            if (!e) continue;
            __atomic_store_n(p, 0, __ATOMIC_RELEASE);
            if (!(e & PTE_P)) continue; /* a reservation: never cached */
            count(s, e, 0);
            gather_add(&g, a);
            if (!(e & PTE_SHARED)) pmm_list_add(&g.frames, e & PTE_ADDR);
        }
        a = l1 ? stop : MIN(end, next);
        spin_unlock(&s->lock, f);
    }
    vmm_gather_finish(&g);
}

/* The loader's writes into a space it builds: through the direct map, the pages made present. */
int vmm_copy_to_space(struct vmm_space *s, uint64_t va, const void *src, size_t n) {
    const uint8_t *from = src;
    while (n) {
        uint64_t pa = vmm_translate(s, va);
        if (!pa) {
            int d = vmm_demand_fault(s, va, false); /* the loader also fills read-only pages */
            if (d <= 0) return d < 0 ? d : -N_ERR_BAD_ADDRESS;
            pa = vmm_translate(s, va);
            if (!pa) return -N_ERR_BAD_ADDRESS;
        }
        size_t chunk = MIN(n, PAGE_SIZE - (va & 0xFFF));
        if (from) memcpy(phys_to_virt(pa), from, chunk);
        else memset(phys_to_virt(pa), 0, chunk);
        if (from) from += chunk;
        va += chunk;
        n -= chunk;
    }
    return 0;
}

bool vmm_fault_spurious(uint64_t addr, uint64_t err) {
    struct process *p = process_current();
    if (!p || !p->vmm_space || addr >= USER_TOP || (err & 8)) return false; /* 8: a reserved bit set */
    uint64_t need = PTE_P | PTE_U | (err & 2 ? PTE_W : 0), *table = tbl(p->vmm_space->pml4);
    for (int level = 4; level >= 1; level--) {
        uint64_t e = __atomic_load_n(&table[idx(addr, level)], __ATOMIC_ACQUIRE);
        if ((e & need) != need || ((err & 16) && (e & PTE_NX))) return false;
        table = tbl(e);
    }
    return true;
}

/* The current app's page fault at addr (the error code), from ring 3 or from the kernel's copy of
   its memory (user_copy), interrupts on: an access its tables allow now, a stack that grows, a
   reserved page touched first, a shared page (its pager may be asked and waited for). True when it
   may run on. */
bool vmm_handle_user_fault(uint64_t addr, uint64_t err) {
    struct process *p = process_current();
    if (!p || !p->vmm_space || addr >= USER_TOP) return false;
    if (vmm_fault_spurious(addr, err)) return true;
    /* A protection violation is final, except a write to a page a pager gave read access to:
       its pager may give write access (shared_demand_fault decides). */
    if (err & 1) return (err & 2) && !(err & 16) && shared_demand_fault(addr, true);
    if (addr >= USER_STACK_TOP - USER_STACK_MAX && addr < USER_STACK_TOP)
        return vmm_user_alloc(p->vmm_space, ALIGN_DOWN(addr, PAGE_SIZE), PAGE_SIZE, VM_W) == 0;
    return shared_demand_fault(addr, err & 2);
}
