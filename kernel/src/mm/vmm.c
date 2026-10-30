/* Virtual memory: 4-level paging, our own kernel page tables, per-process address spaces. */
#include "kernel.h"
#include "arch/cpu.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "arch/spin.h"
#include "sys/sched.h"
#include "sys/syscall.h"
#include "limine.h"

uint64_t kernel_pml4;
uint64_t pte_nx;

/* The kernel half's tables, which every space shares: any CPU maps kernel pages (the heap grows,
   a kernel stack is made, on whichever CPU allocates). A user space's lower half stays the BSP's,
   changed under its IRQ guard. Taken before pmm's lock. Removing or changing a present kernel
   translation needs a shootdown, which only the BSP starts. */
static struct spinlock kmap_lock;
static uint64_t tables_lock(uint64_t pml4, uint64_t va) {
    return pml4 == kernel_pml4 || va >= USER_TOP ? spin_lock(&kmap_lock) : irq_save();
}
static void tables_unlock(uint64_t pml4, uint64_t va, uint64_t flags) {
    if (pml4 == kernel_pml4 || va >= USER_TOP) spin_unlock(&kmap_lock, flags);
    else irq_restore(flags);
}

/* memory map regions that are RAM (usable, ACPI tables and NVS, bootloader, kernel): the direct
   map already reaches them write-back, so vmm_map_phys leaves them as they are. Neighbours are
   joined: the map is sorted, and a UEFI one has many small entries. */
struct ram_range { uint64_t base, end; };
static struct ram_range ram[256];
static unsigned nram;

static void ram_add(uint64_t base, uint64_t end) {
    if (nram && ram[nram - 1].end == base) ram[nram - 1].end = end;
    else if (nram < ARRAY_SIZE(ram)) ram[nram++] = (struct ram_range){base, end};
    else {
        static bool told; /* one line, not one per range left out */
        if (!told) kprintf("vmm: more than %u RAM ranges, from %p on not kept\n", (unsigned)ARRAY_SIZE(ram), (void *)base);
        told = true;
    }
}

static bool is_ram(uint64_t page) {
    for (unsigned i = 0; i < nram; i++)
        if (page >= ram[i].base && page < ram[i].end) return true;
    return false;
}

extern char __kernel_start[], __kernel_end[], __text_start[], __rodata_start[], __data_start[];

/* Fixed storage, shared by pml4 rather than copied into each task. The BSP
 * owns all page-table mutations but one: an AP fills a reserved leaf its app
 * touched (vmm_ap_demand_fault). Other CPUs otherwise only walk them (hardware
 * or vmm_get_pte) and learn about revoked translations through tlb_shootdown.
 * Saturation is not an approximate value: untracked spaces use the old walk.
 * A slot is also the space's PCID (slot + 1); gen counts its revocations so a
 * CPU that last ran the space under an older gen flushes that PCID on entry. */
#define USER_PAGE_COUNTS 128
struct user_page_count { uint64_t pml4, owned; uint32_t gen; };
static struct user_page_count user_page_counts[USER_PAGE_COUNTS];

static struct user_page_count *user_page_count_find(uint64_t pml4) {
    for (unsigned i=0;i<USER_PAGE_COUNTS;i++)
        if (user_page_counts[i].pml4==pml4) return &user_page_counts[i];
    return NULL;
}

static void user_page_count_register(uint64_t pml4) {
    for (unsigned i=0;i<USER_PAGE_COUNTS;i++) if (!user_page_counts[i].pml4) {
        /* A new gen retires whatever an earlier owner of this PCID left cached. */
        uint32_t gen=user_page_counts[i].gen+1;
        user_page_counts[i]=(struct user_page_count){0,0,gen};
        __atomic_store_n(&user_page_counts[i].pml4,pml4,__ATOMIC_RELEASE);
        return;
    }
}

static void user_page_count_forget(uint64_t pml4) {
    struct user_page_count *c=user_page_count_find(pml4);
    if (c) { c->pml4=0; c->owned=0; }
}

static void user_page_count_change(uint64_t pml4,uint64_t va,uint64_t old,uint64_t now) {
    if (va>=USER_TOP || pml4==kernel_pml4) return;
    bool before=(old&PTE_P) && !(old&PTE_SHARED);
    bool after=(now&PTE_P) && !(now&PTE_SHARED);
    if (before==after) return;
    struct user_page_count *c=user_page_count_find(pml4);
    if (!c) return;
    /* an AP adds the pages it fills (vmm_ap_demand_fault) */
    if (after) __atomic_add_fetch(&c->owned,1,__ATOMIC_RELAXED);
    else { ASSERT(__atomic_load_n(&c->owned,__ATOMIC_RELAXED)); __atomic_sub_fetch(&c->owned,1,__ATOMIC_RELAXED); }
}

static inline uint64_t *tbl(uint64_t phys) { return (uint64_t *)phys_to_virt(phys & PTE_ADDR); }

/* ---- TLB coherence ----
 * Adding a translation needs no invalidation anywhere: no CPU caches a
 * not-present entry. Changing or removing a present one is published by
 * tlb_shootdown: the BSP invalidates locally, IPIs exactly the CPUs that have
 * the space loaded and waits for their ACK, so the old frame/permission is
 * dead everywhere before the caller frees or reuses it. User threads keep
 * running on their APs; nothing is retired to the BSP.
 * With PCID each tracked space keeps its own tag, so switching spaces (an AP
 * launch/retire, a BSP context switch) no longer discards the whole TLB. A
 * CPU that is not running the space when it is revoked keeps stale entries
 * under that tag, and vmm_switch flushes them on its next entry because the
 * space's gen moved past the one it last synchronised with. */
#define CR4_PGE      (1ULL << 7)
#define CR4_PCIDE    (1ULL << 17)
#define CR3_NOFLUSH  (1ULL << 63)
#define TLB_RANGE_MAX 32 /* above this a local flush is cheaper than invlpg per page */

static bool pcid_on;
static uint64_t cpu_loaded[CPU_MAX_COUNT];          /* pml4 in CR3, published before its gen is read */
static uint32_t cpu_gen[CPU_MAX_COUNT][USER_PAGE_COUNTS];
static bool cpu_pcid0_user[CPU_MAX_COUNT];          /* PCID 0 holds an untracked user space */
static struct { uint64_t pml4, va, pages; int slot; uint32_t gen; } tlb_req;
static uint64_t tlb_req_seq, tlb_ack[CPU_MAX_COUNT];
/* AP kernel copies to/from user memory (net_ap_syscall) run with IRQs off and
   cannot take the shootdown IPI. They announce the space first; a revocation
   waits for them instead of clearing a PTE under a running copy. */
static uint64_t cpu_uaccess[CPU_MAX_COUNT], vmm_revoking;
static uint64_t tlb_shootdowns, tlb_ipis;

static int space_slot(uint64_t pml4) {
    for (unsigned i=0;i<USER_PAGE_COUNTS;i++)
        if (__atomic_load_n(&user_page_counts[i].pml4,__ATOMIC_ACQUIRE)==pml4) return (int)i;
    return -1;
}

static void flush_range_local(uint64_t va, uint64_t pages) {
    if (pages > TLB_RANGE_MAX) { write_cr3(read_cr3() & ~CR3_NOFLUSH); return; } /* current PCID */
    for (uint64_t i = 0; i < pages; i++) invlpg(va + i * PAGE_SIZE);
}

void vmm_switch(uint64_t pml4) {
    unsigned cpu = cpu_current_index();
    if ((read_cr3() & PTE_ADDR) == pml4) return;
    /* Dekker with tlb_shootdown: either it sees this CPU in the space and IPIs
       it, or this CPU sees the gen it bumped and flushes the tag below. */
    __atomic_store_n(&cpu_loaded[cpu], pml4, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!pcid_on) { write_cr3(pml4); return; }
    if (pml4 == kernel_pml4) {
        write_cr3(pml4 | (cpu_pcid0_user[cpu] ? 0 : CR3_NOFLUSH));
        cpu_pcid0_user[cpu] = false;
        return;
    }
    int slot = space_slot(pml4);
    if (slot < 0) { write_cr3(pml4); cpu_pcid0_user[cpu] = true; return; }
    uint32_t gen = __atomic_load_n(&user_page_counts[slot].gen, __ATOMIC_ACQUIRE);
    bool fresh = cpu_gen[cpu][slot] == gen;
    cpu_gen[cpu][slot] = gen;
    write_cr3(pml4 | (uint64_t)(slot + 1) | (fresh ? CR3_NOFLUSH : 0));
}

void vmm_flush_all(void) {
    /* Any PGE transition drops every entry: global ones and all PCIDs. */
    uint64_t cr4 = read_cr4();
    write_cr4(cr4 ^ CR4_PGE);
    write_cr4(cr4);
}

/* AP: the shootdown IPI. Runs with IRQs off in either user or idle context. */
void vmm_tlb_ipi(void) {
    unsigned cpu = cpu_current_index();
    uint64_t seq = __atomic_load_n(&tlb_req_seq, __ATOMIC_ACQUIRE);
    if (!tlb_req.pml4) vmm_flush_all(); /* shared kernel half: cached under every tag */
    else if ((read_cr3() & PTE_ADDR) == tlb_req.pml4) {
        flush_range_local(tlb_req.va, tlb_req.pages);
        if (tlb_req.slot >= 0) cpu_gen[cpu][tlb_req.slot] = tlb_req.gen;
    }
    __atomic_store_n(&tlb_ack[cpu], seq, __ATOMIC_RELEASE);
}

/* An AP spinning with IRQs off (arch/spin.h): a shootdown it has not acknowledged is served here,
   so the BSP never waits on a CPU that waits on a lock the BSP holds. A request that did not name
   this CPU is served too; that only flushes. */
void vmm_tlb_poll(void) {
    unsigned cpu = cpu_current_index();
    if (cpu && __atomic_load_n(&tlb_req_seq, __ATOMIC_ACQUIRE) != __atomic_load_n(&tlb_ack[cpu], __ATOMIC_RELAXED))
        vmm_tlb_ipi();
}

bool vmm_ap_uaccess_begin(void) {
    unsigned cpu = cpu_current_index();
    uint64_t pml4 = current_task->pml4;
    __atomic_store_n(&cpu_uaccess[cpu], pml4, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&vmm_revoking, __ATOMIC_RELAXED) != pml4) return true;
    __atomic_store_n(&cpu_uaccess[cpu], 0, __ATOMIC_RELEASE);
    return false;
}

void vmm_ap_uaccess_end(void) {
    __atomic_store_n(&cpu_uaccess[cpu_current_index()], 0, __ATOMIC_RELEASE);
}

bool vmm_ap_user_ok(const void *pointer, size_t length, bool write) {
    uint64_t address = (uint64_t)pointer;
    if (!length) return true;
    if (address >= USER_TOP || length > USER_TOP - address) return false;
    for (uint64_t at = ALIGN_DOWN(address, PAGE_SIZE); at < address + length; at += PAGE_SIZE) {
        uint64_t pte = vmm_get_pte(current_task->pml4, at);
        if ((pte & (PTE_P | PTE_U)) != (PTE_P | PTE_U) || (write && !(pte & PTE_W))) return false;
    }
    return true;
}

bool vmm_ap_user_begin(const void *pointer, size_t length, bool write) {
    if (!vmm_ap_uaccess_begin()) return false;
    if (vmm_ap_user_ok(pointer, length, write)) return true;
    vmm_ap_uaccess_end();
    return false;
}

static bool tlb_wait(uint64_t *word, uint64_t want, bool equal) {
    uint64_t since = rdtsc(), spins = 0;
    while ((__atomic_load_n(word, __ATOMIC_ACQUIRE) == want) != equal) {
        cpu_runner_check();
        if ((tsc_hz && rdtsc() - since > tsc_hz) || ++spins > 100000000) return false;
        pause();
    }
    return true;
}

/* BSP, before clearing or narrowing present user PTEs of pml4. */
static void revoke_begin(uint64_t pml4) {
    if (pml4 == kernel_pml4) return;
    __atomic_store_n(&vmm_revoking, pml4, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++)
        if (!tlb_wait(&cpu_uaccess[cpu], pml4, false)) panic("vmm: AP user copy did not finish");
}

/* BSP: drop [va, va+pages) of pml4 from every TLB (kernel addresses: from every
   space). Called with the PTEs already changed and IRQs disabled. */
static void tlb_shootdown(uint64_t pml4, uint64_t va, uint64_t pages) {
    cpu_require_bsp();
    bool kernel = va >= USER_TOP || pml4 == kernel_pml4;
    int slot = -1;
    uint32_t gen = 0;
    tlb_shootdowns++;
    if (!kernel && pcid_on && (slot = space_slot(pml4)) >= 0)
        gen = __atomic_add_fetch(&user_page_counts[slot].gen, 1, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (kernel) {
        if (pcid_on || pages > TLB_RANGE_MAX) vmm_flush_all(); /* invlpg reaches only the current PCID */
        else flush_range_local(va, pages);
    } else if ((read_cr3() & PTE_ADDR) == pml4) {
        flush_range_local(va, pages);
        if (slot >= 0) cpu_gen[0][slot] = gen;
    }
    if (!cpu_tlb_online()) { __atomic_store_n(&vmm_revoking, 0, __ATOMIC_RELEASE); return; }
    ASSERT(!cpu_jobs_active()); /* a worker callback must not change or free mappings */
    tlb_req.pml4 = kernel ? 0 : pml4; tlb_req.va = va; tlb_req.pages = pages;
    tlb_req.slot = slot; tlb_req.gen = gen;
    uint64_t seq = tlb_req_seq + 1;
    __atomic_store_n(&tlb_req_seq, seq, __ATOMIC_RELEASE);
    uint32_t sent = 0;
    for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++) {
        if (!kernel && __atomic_load_n(&cpu_loaded[cpu], __ATOMIC_RELAXED) != pml4) continue;
        if (cpu_tlb_ipi(cpu)) { sent |= 1u << cpu; tlb_ipis++; }
    }
    for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++)
        if ((sent & (1u << cpu)) && !tlb_wait(&tlb_ack[cpu], seq, true))
            panic("vmm: TLB shootdown ACK timeout cpu %u", cpu);
    __atomic_store_n(&vmm_revoking, 0, __ATOMIC_RELEASE);
}

void vmm_tlb_stats(uint64_t *shootdowns, uint64_t *ipis) { *shootdowns = tlb_shootdowns; *ipis = tlb_ipis; }

/* Each CPU, with CR3 = kernel_pml4 (PCID 0): enable PCIDs when the BSP did. */
void vmm_init_cpu(void) {
    unsigned cpu = cpu_current_index();
    cpu_loaded[cpu] = kernel_pml4;
    if (pcid_on) write_cr4(read_cr4() | CR4_PCIDE);
}

/* Split a 2 MiB / 1 GiB mapping into the next level down. */
static void split_huge(uint64_t *entry, int level) {
    uint64_t e = *entry;
    uint64_t base = e & PTE_ADDR & ~((level == 2 ? (1ULL << 21) : (1ULL << 30)) - 1);
    uint64_t newt = pmm_alloc_zeroed();
    if (!newt) panic("vmm: out of memory splitting page");
    uint64_t *t = tbl(newt);
    uint64_t flags = e & (0xFFF | PTE_NX) & ~PTE_PS;
    bool pat = e & PTE_PAT2M;
    for (int i = 0; i < 512; i++) {
        if (level == 2) {
            t[i] = (base + i * PAGE_SIZE) | flags | (pat ? PTE_PAT4K : 0);
        } else {
            t[i] = (base + (uint64_t)i * (1ULL << 21)) | flags | PTE_PS | (pat ? PTE_PAT2M : 0);
        }
    }
    *entry = newt | (e & (PTE_P | PTE_W | PTE_U));
}

/* *stale: a cached translation or paging-structure entry changed (huge split,
   upper-level permission upgrade), so the caller must shoot down va. */
static uint64_t *next_level(uint64_t *table, int idx, bool create, bool user, int level, bool *stale) {
    uint64_t e = table[idx];
    if (e & PTE_P) {
        if (e & PTE_PS) {
            split_huge(&table[idx], level);
            e = table[idx];
            *stale = true;
        }
        if (user && !(e & PTE_U)) { table[idx] |= PTE_U; *stale = true; }
        return tbl(e);
    }
    if (!create) return NULL;
    uint64_t n = pmm_alloc_zeroed();
    if (!n) return NULL;
    table[idx] = n | PTE_P | PTE_W | (user ? PTE_U : 0);
    return tbl(n);
}

static bool vmm_map_page_locked(uint64_t pml4, uint64_t va, uint64_t pa, uint64_t flags) {
    bool user = flags & PTE_U, stale = false;
    uint64_t *l4 = tbl(pml4);
    uint64_t *l3 = next_level(l4, (va >> 39) & 511, true, user, 4, &stale);
    uint64_t *l2 = l3 ? next_level(l3, (va >> 30) & 511, true, user, 3, &stale) : NULL;
    uint64_t *l1 = l2 ? next_level(l2, (va >> 21) & 511, true, user, 2, &stale) : NULL;
    if (!l1) { if (stale) tlb_shootdown(pml4, va, 1); return false; }
    uint64_t *leaf=&l1[(va >> 12) & 511], old=*leaf, now=(pa & PTE_ADDR) | flags | PTE_P;
    if ((old & PTE_P) && old != now) { revoke_begin(pml4); stale = true; }
    *leaf = now;
    user_page_count_change(pml4,va,old,now);
    /* A fresh leaf was never cached anywhere; only a replaced one is shot down. */
    if (stale) tlb_shootdown(pml4, va, 1);
    return true;
}

bool vmm_map_page(uint64_t pml4, uint64_t va, uint64_t pa, uint64_t flags) {
    /* Retiring APs does not exclude another BSP syscall. In particular, the
       next_level lookup/allocate/publication must not lose a sibling's table. */
    uint64_t saved = tables_lock(pml4, va);
    bool result = vmm_map_page_locked(pml4, va, pa, flags);
    tables_unlock(pml4, va, saved);
    return result;
}

static bool map_2m(uint64_t pml4, uint64_t va, uint64_t pa, uint64_t flags) {
    /* Currently boot-only kernel mappings. A future user huge-map path must
     * not leave a four-KiB-leaf counter active without huge-page accounting. */
    if (va<USER_TOP) user_page_count_forget(pml4);
    uint64_t *l4 = tbl(pml4);
    bool stale = false; /* boot: only this CPU runs, and invlpg follows */
    uint64_t *l3 = next_level(l4, (va >> 39) & 511, true, false, 4, &stale);
    if (!l3) return false;
    uint64_t *l2 = next_level(l3, (va >> 30) & 511, true, false, 3, &stale);
    if (!l2) return false;
    uint64_t *e = &l2[(va >> 21) & 511];
    if ((*e & PTE_P) && !(*e & PTE_PS)) {
        /* a page table already lives here; fall back to 4K pages */
        uint64_t f4 = flags & ~PTE_PAT2M;
        if (flags & PTE_PAT2M) f4 |= PTE_PAT4K;
        for (int i = 0; i < 512; i++) vmm_map_page(pml4, va + i * PAGE_SIZE, pa + i * PAGE_SIZE, f4);
        return true;
    }
    *e = (pa & PTE_ADDR) | flags | PTE_PS | PTE_P;
    invlpg(va);
    return true;
}

static uint64_t *walk(uint64_t pml4, uint64_t va, bool *huge) {
    uint64_t *l4 = tbl(pml4);
    uint64_t e = l4[(va >> 39) & 511];
    if (!(e & PTE_P)) return NULL;
    uint64_t *l3 = tbl(e);
    e = l3[(va >> 30) & 511];
    if (!(e & PTE_P)) return NULL;
    if (e & PTE_PS) { *huge = true; return &l3[(va >> 30) & 511]; }
    uint64_t *l2 = tbl(e);
    e = l2[(va >> 21) & 511];
    if (!(e & PTE_P)) return NULL;
    if (e & PTE_PS) { *huge = true; return &l2[(va >> 21) & 511]; }
    uint64_t *l1 = tbl(e);
    *huge = false;
    return &l1[(va >> 12) & 511];
}

uint64_t vmm_get_pte(uint64_t pml4, uint64_t va) {
    bool huge = false;
    uint64_t *p = walk(pml4, va, &huge);
    if (!p || !(*p & PTE_P)) return 0;
    return *p;
}

uint64_t vmm_get_leaf(uint64_t pml4, uint64_t va) {
    bool huge = false;
    uint64_t *p = walk(pml4, va, &huge);
    return p ? *p : 0;
}

uint64_t vmm_translate(uint64_t pml4, uint64_t va) {
    bool huge = false;
    uint64_t *p = walk(pml4, va, &huge);
    if (!p || !(*p & PTE_P)) return 0;
    if (huge) return (*p & PTE_ADDR & ~((1ULL << 21) - 1)) + (va & ((1ULL << 21) - 1));
    return (*p & PTE_ADDR) + (va & 0xFFF);
}

/* Clears present 4 KiB leaves of [va, va+pages) and shoots the range down
   once; old[i] receives each previous PTE (0 when nothing was mapped). The
   frames stay valid for the caller to free only after this returns. */
static void unmap_range(uint64_t pml4, uint64_t va, uint64_t pages, uint64_t *old) {
    uint64_t flags = tables_lock(pml4, va);
    bool any = false;
    for (uint64_t i = 0; i < pages; i++) {
        bool huge = false;
        uint64_t *p = walk(pml4, va + i * PAGE_SIZE, &huge);
        old[i] = 0;
        if (!p || huge) continue;
        /* Drops a reservation marker, never cached; unless an AP filled it meanwhile
           (vmm_ap_demand_fault), when it is taken away as any present page. */
        uint64_t e = *p;
        if (!(e & PTE_P) && __atomic_compare_exchange_n(p, &e, 0, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            continue;
        if (!any) revoke_begin(pml4);
        any = true;
        old[i] = *p;
        *p = 0;
        user_page_count_change(pml4, va + i * PAGE_SIZE, old[i], 0);
    }
    if (any) tlb_shootdown(pml4, va, pages);
    tables_unlock(pml4, va, flags);
}

#define UNMAP_BATCH 64

uint64_t vmm_unmap_page(uint64_t pml4, uint64_t va) {
    uint64_t old;
    unmap_range(pml4, va, 1, &old);
    return old;
}

static void map_range(uint64_t va, uint64_t pa, uint64_t size, uint64_t flags4k, uint64_t flags2m) {
    uint64_t end = pa + size;
    while (pa < end) {
        if ((pa & ((1ULL << 21) - 1)) == 0 && (va & ((1ULL << 21) - 1)) == 0 && end - pa >= (1ULL << 21)) {
            map_2m(kernel_pml4, va, pa, flags2m);
            pa += 1ULL << 21;
            va += 1ULL << 21;
        } else {
            vmm_map_page(kernel_pml4, va, pa, flags4k);
            pa += PAGE_SIZE;
            va += PAGE_SIZE;
        }
    }
}

void vmm_init(struct limine_memmap_response *mm, struct limine_executable_address_response *ka) {
    /* no-execute pages, if the CPU has them (EFER.NXE must be on before any PTE uses bit 63) */
    uint32_t a, b, c, d;
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000001) {
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        if (d & (1u << 20)) {
            wrmsr(0xC0000080, rdmsr(0xC0000080) | (1ULL << 11));
            pte_nx = PTE_NX;
        }
    }
    /* write-protect also applies to the kernel, so read-only pages really are read-only */
    write_cr0(read_cr0() | (1ULL << 16));

    kernel_pml4 = pmm_alloc_zeroed();
    uint64_t *l4 = tbl(kernel_pml4);
    /* pre-create every upper-half PDPT so all address spaces share kernel mappings */
    for (int i = 256; i < 512; i++) l4[i] = pmm_alloc_zeroed() | PTE_P | PTE_W;

    /* higher-half direct map of every memory map region */
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        uint64_t base = ALIGN_DOWN(e->base, PAGE_SIZE);
        uint64_t end = ALIGN_UP(e->base + e->length, PAGE_SIZE);
        if (e->type == LIMINE_MEMMAP_FRAMEBUFFER) {
            /* write-combining: PAT index 5 */
            map_range(hhdm_offset + base, base, end - base, PTE_W | PTE_PWT | PTE_PAT4K | pte_nx,
                      PTE_W | PTE_PWT | PTE_PAT2M | pte_nx);
        } else if (e->type == LIMINE_MEMMAP_BAD_MEMORY) {
            continue;
        } else {
            if (e->type != LIMINE_MEMMAP_RESERVED) ram_add(base, end);
            map_range(hhdm_offset + base, base, end - base, PTE_W | pte_nx, PTE_W | pte_nx);
        }
    }

    /* the kernel image: code read-only + executable, rodata read-only, everything else writable;
       only code is executable */
    uint64_t kstart = ALIGN_DOWN((uint64_t)__kernel_start, PAGE_SIZE);
    uint64_t kend = ALIGN_UP((uint64_t)__kernel_end, PAGE_SIZE);
    for (uint64_t va = kstart; va < kend; va += PAGE_SIZE) {
        uint64_t f = PTE_G | PTE_W | pte_nx;
        if (va >= (uint64_t)__text_start && va < (uint64_t)__rodata_start) f = PTE_G;
        else if (va >= (uint64_t)__rodata_start && va < (uint64_t)__data_start) f = PTE_G | pte_nx;
        vmm_map_page(kernel_pml4, va, va - ka->virtual_base + ka->physical_base, f);
    }
    write_cr3(kernel_pml4);
    cpuid(1, 0, &a, &b, &c, &d);
    pcid_on = (c & (1u << 17)) && !cmdline_has("nopcid");
    vmm_init_cpu();
    kprintf("vmm: PCID %s, TLB shootdown by IPI\n", pcid_on ? "on" : "off");
}

void *vmm_map_mmio(uint64_t phys, uint64_t size) {
    uint64_t base = ALIGN_DOWN(phys, PAGE_SIZE);
    uint64_t end = ALIGN_UP(phys + size, PAGE_SIZE);
    for (uint64_t p = base; p < end; p += PAGE_SIZE)
        if (!vmm_map_page(kernel_pml4, hhdm_offset + p, p, PTE_W | PTE_PCD | PTE_PWT | pte_nx)) return NULL;
    return phys_to_virt(phys);
}

/* RAM a device reaches uncached or write-combining: x86 wants every mapping of a page to agree
   on its memory type, so the direct map takes the type the user mappings get. type: the leaf's
   PWT/PCD/PAT bits, 0 for write-back. The pages are frames[i], or base + i pages when frames is
   NULL. The 2 MiB pages around them are split (false, and nothing changed, when there is no
   memory for that); the run is shot down once; then lines cached while it was write-back are
   flushed out of every cache (clflush works whatever the type of the page). BSP. */
bool vmm_direct_type(const uint64_t *frames, uint64_t base, size_t pages, uint64_t type) {
    uint64_t f = spin_lock(&kmap_lock), splits = 0, last = ~0ull;
    for (size_t i = 0; i < pages; i++) {
        uint64_t va = hhdm_offset + (frames ? frames[i] : base + i * PAGE_SIZE);
        bool huge = false;
        if (walk(kernel_pml4, va, &huge) && huge && ALIGN_DOWN(va, 1ULL << 21) != last) {
            splits++;
            last = ALIGN_DOWN(va, 1ULL << 21);
        }
    }
    if (splits && pmm_free_pages() < splits + 16) { spin_unlock(&kmap_lock, f); return false; }
    for (size_t i = 0; i < pages; i++) {
        uint64_t va = hhdm_offset + (frames ? frames[i] : base + i * PAGE_SIZE);
        bool stale = false;
        uint64_t *l3 = next_level(tbl(kernel_pml4), (va >> 39) & 511, false, false, 4, &stale);
        uint64_t *l2 = l3 ? next_level(l3, (va >> 30) & 511, false, false, 3, &stale) : NULL;
        uint64_t *l1 = l2 ? next_level(l2, (va >> 21) & 511, false, false, 2, &stale) : NULL;
        if (!l1 || !(l1[(va >> 12) & 511] & PTE_P)) continue;
        uint64_t *leaf = &l1[(va >> 12) & 511];
        *leaf = (*leaf & ~(PTE_PWT | PTE_PCD | PTE_PAT4K)) | type;
    }
    /* scattered frames: a whole flush (a range above TLB_RANGE_MAX) */
    if (pages) tlb_shootdown(kernel_pml4, hhdm_offset + (frames ? 0 : base), frames ? TLB_RANGE_MAX + 1 : pages);
    if (type)
        for (size_t i = 0; i < pages; i++) {
            uint8_t *p = phys_to_virt(frames ? frames[i] : base + i * PAGE_SIZE);
            for (size_t at = 0; at < PAGE_SIZE; at += 64) __asm__ volatile("clflush (%0)" ::"r"(p + at) : "memory");
        }
    __asm__ volatile("mfence" ::: "memory");
    spin_unlock(&kmap_lock, f);
    return true;
}

/* Page by page: a request may run across several ranges, and only what lies in none of them
   (device memory) becomes uncached. RAM stays write-back for everyone else who uses it. */
void *vmm_map_phys(uint64_t phys, uint64_t size) {
    uint64_t end = ALIGN_UP(phys + size, PAGE_SIZE);
    for (uint64_t p = ALIGN_DOWN(phys, PAGE_SIZE); p < end; p += PAGE_SIZE)
        if (!is_ram(p) && !vmm_map_mmio(p, PAGE_SIZE)) return NULL;
    return phys_to_virt(phys);
}

/* ---- user address spaces ---- */

uint64_t vmm_new_space(void) {
    uint64_t p = pmm_alloc_zeroed();
    if (!p) return 0;
    uint64_t *n = tbl(p), *k = tbl(kernel_pml4);
    for (int i = 256; i < 512; i++) n[i] = k[i];
    user_page_count_register(p); /* fresh lower half is empty */
    return p;
}

static void free_level(uint64_t phys, int level) {
    uint64_t *t = tbl(phys);
    for (int i = 0; i < 512; i++) {
        uint64_t e = t[i];
        if (!(e & PTE_P)) continue;
        if (level == 1) {
            if (!(e & PTE_SHARED)) pmm_free(e & PTE_ADDR);
        } else if (!(e & PTE_PS)) {
            free_level(e & PTE_ADDR, level - 1);
        }
    }
    pmm_free(phys);
}

void vmm_free_space(uint64_t pml4) {
    sched_quiesce_space(pml4);
    user_page_count_forget(pml4); /* ACK precedes invalidation and physical reuse */
    uint64_t *t = tbl(pml4);
    for (int i = 0; i < 256; i++) {
        if (t[i] & PTE_P) free_level(t[i] & PTE_ADDR, 3);
    }
    pmm_free(pml4);
}

/* Identical present/non-shared leaf count to proc_user_pages' former walk.
 * No task-local TTL, guessed quota, allocation, or per-sample full scan. */
uint64_t vmm_user_pages(uint64_t pml4) {
    if (!pml4 || pml4==kernel_pml4) return 0;
    struct user_page_count *c=user_page_count_find(pml4);
    if (c) return c->owned;
    uint64_t n = 0;
    uint64_t *l4 = tbl(pml4);
    for (int a = 0; a < 256; a++) {
        if (!(l4[a] & PTE_P)) continue;
        uint64_t *l3 = phys_to_virt(l4[a] & PTE_ADDR);
        for (int b = 0; b < 512; b++) {
            if (!(l3[b] & PTE_P)) continue;
            uint64_t *l2 = phys_to_virt(l3[b] & PTE_ADDR);
            for (int c = 0; c < 512; c++) {
                if (!(l2[c] & PTE_P)) continue;
                uint64_t *l1 = phys_to_virt(l2[c] & PTE_ADDR);
                for (int d = 0; d < 512; d++)
                    if ((l1[d] & PTE_P) && !(l1[d] & PTE_SHARED)) n++;
            }
        }
    }
    return n;
}

/* Diagnostic split of an address space's private pages: [0] program image and
 * sbrk heap, [1] low reservations, [2] the reservation region, [3] main stack,
 * [4] shared leaves mapped, [5] page-table frames. A full walk: only for the
 * rare out-of-memory report. */
void vmm_user_pages_split(uint64_t pml4, uint64_t out[6]) {
    for (int i = 0; i < 6; i++) out[i] = 0;
    if (!pml4 || pml4 == kernel_pml4) return;
    uint64_t *l4 = tbl(pml4);
    for (int a = 0; a < 256; a++) {
        if (!(l4[a] & PTE_P)) continue;
        uint64_t *l3 = phys_to_virt(l4[a] & PTE_ADDR);
        out[5]++;
        for (int b = 0; b < 512; b++) {
            if (!(l3[b] & PTE_P)) continue;
            uint64_t *l2 = phys_to_virt(l3[b] & PTE_ADDR);
            out[5]++;
            for (int c = 0; c < 512; c++) {
                if (!(l2[c] & PTE_P)) continue;
                uint64_t *l1 = phys_to_virt(l2[c] & PTE_ADDR);
                uint64_t va = ((uint64_t)a << 39) | ((uint64_t)b << 30) | ((uint64_t)c << 21);
                int region = va < USER_LOW_BASE ? 0 : va < USER_MMAP_BASE ? 1
                           : va < USER_STACK_TOP - USER_STACK_MAX ? 2 : 3;
                out[5]++;
                for (int d = 0; d < 512; d++) {
                    if (!(l1[d] & PTE_P)) continue;
                    out[l1[d] & PTE_SHARED ? 4 : region]++;
                }
            }
        }
    }
}

static uint64_t prot_flags(int prot) {
    return PTE_U | (prot & VM_W ? PTE_W : 0) | (prot & VM_X ? 0 : pte_nx);
}

/* Map fresh zeroed pages. Pages that are already mapped keep their contents and gain the
   requested permissions (two ELF segments can share a page). */
int vmm_user_alloc(uint64_t pml4, uint64_t va, uint64_t size, int prot) {
    /* Fresh pages need no other CPU's cooperation. Only granting W/X on an
       already-present page changes a cached entry; that range is shot down
       once at the end (a stale narrower entry would fault as a violation). */
    uint64_t start = ALIGN_DOWN(va, PAGE_SIZE), end = ALIGN_UP(va + size, PAGE_SIZE);
    uint64_t lo = end, hi = start;
    int result = 0;
    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        uint64_t f = irq_save();
        bool huge = false;
        uint64_t *pte = walk(pml4, a, &huge);
        if (pte && !huge && (*pte & (PTE_P | PTE_DEMAND)) == PTE_DEMAND) {
            /* a reserved page, which an AP may be filling too: filled as a first touch is */
            int filled = vmm_demand_fault(pml4, a, false);
            if (filled < 0) { irq_restore(f); result = filled; break; }
        }
        if (pte && (*pte & PTE_P)) {
            uint64_t now = *pte;
            if (huge) result = -N_ERR_INVALID;
            else if ((now & PTE_SHARED_RO) && (prot & (VM_W | VM_X))) result = -N_ERR_DENIED;
            else {
                if (prot & VM_W) now |= PTE_W;
                if (prot & VM_X) now &= ~PTE_NX;
                if (now != *pte) { *pte = now; lo = MIN(lo, a); hi = MAX(hi, a + PAGE_SIZE); }
            }
            irq_restore(f);
            if (result) break;
            continue;
        }
        irq_restore(f);
        uint64_t p = pmm_alloc_zeroed();
        if (!p) { result = -N_ERR_NO_MEMORY; break; }
        if (!vmm_map_page(pml4, a, p, prot_flags(prot))) {
            pmm_free(p);
            result = -N_ERR_NO_MEMORY;
            break;
        }
    }
    if (lo < hi) {
        uint64_t f = irq_save();
        tlb_shootdown(pml4, lo, (hi - lo) / PAGE_SIZE);
        irq_restore(f);
    }
    return result;
}

/* Reserve pages that receive a zeroed frame only when first touched: a
   non-present leaf holds PTE_DEMAND and the page's protection. Present pages
   keep their contents and gain the permissions, as with vmm_user_alloc. Only
   the page tables are allocated here. */
int vmm_user_reserve(uint64_t pml4, uint64_t va, uint64_t size, int prot) {
    uint64_t start = ALIGN_DOWN(va, PAGE_SIZE), end = ALIGN_UP(va + size, PAGE_SIZE);
    uint64_t lo = end, hi = start, marker = prot_flags(prot) | PTE_DEMAND;
    int result = 0;
    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        uint64_t f = irq_save();
        bool stale = false;
        uint64_t *l4 = tbl(pml4);
        uint64_t *l3 = next_level(l4, (a >> 39) & 511, true, true, 4, &stale);
        uint64_t *l2 = l3 ? next_level(l3, (a >> 30) & 511, true, true, 3, &stale) : NULL;
        uint64_t *l1 = l2 ? next_level(l2, (a >> 21) & 511, true, true, 2, &stale) : NULL;
        if (stale) tlb_shootdown(pml4, a, 1);
        if (!l1) { irq_restore(f); result = -N_ERR_NO_MEMORY; break; }
        uint64_t *leaf = &l1[(a >> 12) & 511], now = *leaf;
        if (!(now & PTE_P)) {
            uint64_t want = marker;
            if (now & PTE_DEMAND) {
                want = now;
                if (prot & VM_W) want |= PTE_W;
                if (prot & VM_X) want &= ~PTE_NX;
            }
            /* An AP may fill a reserved leaf meanwhile (vmm_ap_demand_fault): now is then
               the present page, which gains the permissions below. */
            __atomic_compare_exchange_n(leaf, &now, want, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        }
        if (now & PTE_P) {
            if ((now & PTE_SHARED_RO) && (prot & (VM_W | VM_X))) result = -N_ERR_DENIED;
            else {
                if (prot & VM_W) now |= PTE_W;
                if (prot & VM_X) now &= ~PTE_NX;
                if (now != *leaf) { *leaf = now; lo = MIN(lo, a); hi = MAX(hi, a + PAGE_SIZE); }
            }
        }
        irq_restore(f);
        if (result) break;
    }
    if (lo < hi) {
        uint64_t f = irq_save();
        tlb_shootdown(pml4, lo, (hi - lo) / PAGE_SIZE);
        irq_restore(f);
    }
    return result;
}

/* Reserved pages filled together with a touched one: the following leaves of
   the same table, so ascending first touches fault once per 64 KiB. */
#define DEMAND_AROUND 16

/* A first touch of the reserved page at va (write: a write), and the reserved pages after it in
   its table (while memory is not short): 1 when va's page is present now, 0 when it is no reserved
   page it may touch so, -N_ERR_NO_MEMORY when no frame came for it. Any CPU: two may fill one
   table at once, and the BSP change reserved leaves meanwhile: a leaf is filled by
   compare-and-swap. */
static int demand_fill(uint64_t pml4, uint64_t va, bool write) {
    bool huge = false;
    uint64_t page = ALIGN_DOWN(va, PAGE_SIZE), *pte = walk(pml4, page, &huge);
    if (!pte || huge) return 0;
    uint64_t *l1 = pte - ((page >> 12) & 511);
    int result = 0;
    for (unsigned idx = (page >> 12) & 511, n = 0; idx < 512 && n < DEMAND_AROUND; idx++, n++) {
        uint64_t e = __atomic_load_n(&l1[idx], __ATOMIC_RELAXED);
        if (!n && (e & PTE_P)) return 1; /* a sibling filled it first */
        if ((e & (PTE_P | PTE_DEMAND)) != PTE_DEMAND || (!n && write && !(e & PTE_W))) break;
        uint64_t frame = !n || pmm_free_pages() >= 4096 ? pmm_alloc_zeroed() : 0;
        if (!frame) return n ? result : -N_ERR_NO_MEMORY;
        uint64_t now = frame | (e & ~PTE_DEMAND) | PTE_P;
        if (!__atomic_compare_exchange_n(&l1[idx], &e, now, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            pmm_free(frame);
            return n ? result : (e & PTE_P) != 0;
        }
        user_page_count_change(pml4, page + n * PAGE_SIZE, e, now);
        result = 1;
    }
    return result;
}

int vmm_demand_fault(uint64_t pml4, uint64_t va, bool write) {
    if (va >= USER_TOP || !pml4 || pml4 == kernel_pml4) return 0;
    uint64_t f = irq_save();
    int result = demand_fill(pml4, va, write);
    irq_restore(f);
    return result;
}

/* ---- first touches on an AP ----
   An app thread's first touch of a reserved page on an AP is filled there instead of a trip to the
   BSP and back. The fill runs inside an AP user access, so a revocation of the space
   (revoke_begin) waits for it. */
static uint64_t ap_touches, ap_touch_cycles; /* served on an AP: count, TSC cycles (prof) */

bool vmm_ap_demand_fault(uint64_t va, uint64_t err) {
    if ((err & 1) || va >= USER_TOP) return false; /* a protection violation: the BSP decides */
    bool filled = false;
    uint64_t began = rdtsc();
    if (vmm_ap_uaccess_begin()) {
        filled = demand_fill(current_task->pml4, va, err & 2) > 0;
        vmm_ap_uaccess_end();
    }
    if (filled) {
        __atomic_add_fetch(&ap_touches, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&ap_touch_cycles, rdtsc() - began, __ATOMIC_RELAXED);
    }
    return filled;
}

void vmm_ap_touch_stats(uint64_t *touches, uint64_t *cycles) {
    *touches = __atomic_load_n(&ap_touches, __ATOMIC_RELAXED);
    *cycles = __atomic_load_n(&ap_touch_cycles, __ATOMIC_RELAXED);
}

/* mprotect: set exactly these permissions on already-mapped user pages. shared: pages of one shared
   mapping, whose owner checked prot against the mapping's access and keeps it for the pages that
   are not present yet (they are skipped here); a read-only shared leaf (PTE_SHARED_RO, a paged
   page bound for reading) gains no write access. */
int vmm_user_protect(uint64_t pml4, uint64_t va, uint64_t size, int prot, bool shared) {
    uint64_t start = ALIGN_DOWN(va, PAGE_SIZE), end = ALIGN_UP(va + size, PAGE_SIZE);
    uint64_t f = irq_save();
    for (uint64_t a = start; !shared && a < end; a += PAGE_SIZE) {
        bool huge = false;
        uint64_t *pte = walk(pml4, a, &huge);
        if (!pte || !(*pte & (PTE_P | PTE_DEMAND)) || huge || !(*pte & PTE_U)) { irq_restore(f); return -N_ERR_NO_MEMORY; }
        if ((*pte & PTE_SHARED_RO) && (prot & (VM_W | VM_X))) { irq_restore(f); return -N_ERR_DENIED; }
    }
    if (start < end) revoke_begin(pml4);
    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        bool huge = false;
        uint64_t *pte = walk(pml4, a, &huge);
        if (shared && (!pte || huge || !(*pte & PTE_P))) continue;
        uint64_t bits = shared && (*pte & PTE_SHARED_RO) ? PTE_NX : PTE_W | PTE_NX;
        *pte = (*pte & ~bits) | (prot_flags(prot) & bits);
    }
    if (start < end) tlb_shootdown(pml4, start, (end - start) / PAGE_SIZE);
    irq_restore(f);
    return 0;
}

/* Takes write access away from the present 4 KiB leaves of [va, va+size) and marks every one of
   them a read-only shared page (PTE_SHARED_RO: no n_memory_protect gives it back), also those an
   n_memory_protect had left without it. The leaves that had write access are shot down in
   batches before this returns, so no CPU writes through an old entry. */
void vmm_user_write_protect(uint64_t pml4, uint64_t va, uint64_t size) {
    uint64_t start = ALIGN_DOWN(va, PAGE_SIZE), end = ALIGN_UP(va + size, PAGE_SIZE);
    for (uint64_t a = start; a < end; a += UNMAP_BATCH * PAGE_SIZE) {
        uint64_t n = MIN((uint64_t)UNMAP_BATCH, (end - a) / PAGE_SIZE), f = irq_save();
        bool any = false;
        for (uint64_t i = 0; i < n; i++) {
            bool huge = false;
            uint64_t *p = walk(pml4, a + i * PAGE_SIZE, &huge);
            if (!p || huge || !(*p & PTE_P)) continue;
            if (*p & PTE_W) {
                if (!any) revoke_begin(pml4);
                any = true;
            }
            *p = (*p & ~PTE_W) | PTE_SHARED_RO;
        }
        if (any) tlb_shootdown(pml4, a, n);
        irq_restore(f);
    }
}

void vmm_user_free(uint64_t pml4, uint64_t va, uint64_t size) {
    uint64_t start = ALIGN_DOWN(va, PAGE_SIZE), end = ALIGN_UP(va + size, PAGE_SIZE);
    uint64_t old[UNMAP_BATCH];
    for (uint64_t a = start; a < end; a += UNMAP_BATCH * PAGE_SIZE) {
        uint64_t n = MIN((uint64_t)UNMAP_BATCH, (end - a) / PAGE_SIZE);
        unmap_range(pml4, a, n, old);
        for (uint64_t i = 0; i < n; i++)
            if ((old[i] & PTE_P) && !(old[i] & PTE_SHARED)) pmm_free(old[i] & PTE_ADDR);
    }
}

int vmm_copy_to_space(uint64_t pml4, uint64_t va, const void *src, size_t n) {
    sched_quiesce_space(pml4);
    const uint8_t *s = src;
    while (n) {
        uint64_t pa = vmm_translate(pml4, va);
        if (!pa) {
            int d = vmm_demand_fault(pml4, va, false); /* the loader also fills read-only pages */
            if (d <= 0) return d < 0 ? d : -N_ERR_BAD_ADDRESS;
            pa = vmm_translate(pml4, va);
            if (!pa) return -N_ERR_BAD_ADDRESS;
        }
        size_t chunk = MIN(n, PAGE_SIZE - (va & 0xFFF));
        if (s) memcpy(phys_to_virt(pa), s, chunk);
        else memset(phys_to_virt(pa), 0, chunk);
        if (s) s += chunk;
        va += chunk;
        n -= chunk;
    }
    return 0;
}

/* Demand-grow user stacks and fill reserved shared pages. */
bool vmm_handle_user_fault(uint64_t addr, uint64_t err) {
    struct task *t = current_task;
    if (!t || !t->pml4 || t->pml4 == kernel_pml4) return false;
    /* A protection violation is final, except a write to a page a pager gave read access to:
       its pager may give write access (shared_demand_fault decides). */
    if (err & 1) return (err & 2) && !(err & 16) && shared_demand_fault(addr, true);
    if (addr >= USER_STACK_TOP - USER_STACK_MAX && addr < USER_STACK_TOP) {
        return vmm_user_alloc(t->pml4, ALIGN_DOWN(addr, PAGE_SIZE), PAGE_SIZE, VM_W) == 0;
    }
    return shared_demand_fault(addr, err & 2);
}

/* ---- vmalloc: virtually contiguous kernel memory from scattered frames ---- */
static uint64_t vmalloc_next = VMALLOC_BASE;

void *vmalloc(size_t pages) {
    uint64_t va = __atomic_fetch_add(&vmalloc_next, (pages + 1) * PAGE_SIZE, __ATOMIC_RELAXED); /* a guard page */
    for (size_t i = 0; i < pages; i++) {
        uint64_t p = pmm_alloc_zeroed();
        if (!p) {
            vfree((void *)va, i);
            return NULL;
        }
        vmm_map_page(kernel_pml4, va + i * PAGE_SIZE, p, PTE_W | pte_nx);
    }
    return (void *)va;
}

static void unmap_kernel(uint64_t va, size_t pages, bool free_frames) {
    uint64_t old[UNMAP_BATCH];
    for (size_t i = 0; i < pages; i += UNMAP_BATCH) {
        size_t n = MIN((size_t)UNMAP_BATCH, pages - i);
        unmap_range(kernel_pml4, va + i * PAGE_SIZE, n, old);
        for (size_t j = 0; free_frames && j < n; j++) if (old[j] & PTE_P) pmm_free(old[j] & PTE_ADDR);
    }
}

/* Existing frames (frames[i], or a run from pa) at fresh kernel addresses (e.g. read+execute only). */
static void *map_frames(const uint64_t *frames, uint64_t pa, size_t pages, uint64_t flags) {
    uint64_t va = __atomic_fetch_add(&vmalloc_next, (pages + 1) * PAGE_SIZE, __ATOMIC_RELAXED);
    for (size_t i = 0; i < pages; i++)
        if (!vmm_map_page(kernel_pml4, va + i * PAGE_SIZE, frames ? frames[i] : pa + i * PAGE_SIZE, flags)) {
            unmap_kernel(va, i, false);
            return NULL;
        }
    return (void *)va;
}
void *vmalloc_map_phys(uint64_t pa, size_t pages, uint64_t flags) { return map_frames(NULL, pa, pages, flags); }
void *vmalloc_map_frames(const uint64_t *frames, size_t pages, uint64_t flags) {
    return map_frames(frames, 0, pages, flags);
}
void vunmap(void *ptr, size_t pages) { unmap_kernel((uint64_t)ptr, pages, false); }

void vfree(void *ptr, size_t pages) { unmap_kernel((uint64_t)ptr, pages, true); }
