/* Threads (sys/thread.h): their records, ids and registry; kernel and app threads' starts, joins,
   detaches, ends and kills. What ended is freed by the CPU that left it, right after the switch
   (thread_left), or by its joiner. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/fpu.h"
#include "arch/gdt.h"
#include "mm/heap.h"
#include "mm/vmalloc.h"
#include "mm/vmm.h"
#include "sys/clock.h"
#include "sys/object.h"
#include "sys/process.h"
#include "sys/sched.h"
#include "sys/syscall.h"
#include "sys/thread.h"
#include "sys/wait.h"

struct spinlock thread_lock;
struct thread *thread_list;
static struct id_table thread_ids;
/* The next id looked at. Ids go round the positive int32 range (the ABI's), skipping those held:
   an id comes back only after every other id was given since, so a stale one (a message's sender,
   an app list a task manager read) names no new thread until 2^31 have started. */
static int tid_next = 1;

extern void kthread_trampoline(void);
extern void user_trampoline(void);

/* Kernel stacks a CPU keeps from the threads that ended there for the next it makes: a thread that
   ends and one that starts cost no mapping and no shootdown. Two, as a CPU's churn of threads is
   one ending while another starts; more would hold memory on every CPU for little. */
#define KSTACK_KEPT 2
PERCPU static struct kstack_stash {
    uint8_t *stack[KSTACK_KEPT];
    unsigned count;
} kstack_stash;

static uint8_t *kstack_alloc(void) {
    uint64_t f = irq_save();
    struct kstack_stash *s = this_cpu_ptr(kstack_stash);
    uint8_t *stack = s->count ? s->stack[--s->count] : NULL;
    irq_restore(f);
    if (!stack) return vmalloc(KSTACK_PAGES);
    return stack;
}

static void kstack_free(uint8_t *stack) {
    uint64_t f = irq_save();
    struct kstack_stash *s = this_cpu_ptr(kstack_stash);
    bool kept = s->count < KSTACK_KEPT;
    if (kept) s->stack[s->count++] = stack;
    irq_restore(f);
    if (!kept) vfree(stack, KSTACK_PAGES);
}

/* ---- the registry ---- */

struct thread *thread_find(int tid) {
    if (tid <= 0) return NULL;
    struct id_link *l = id_table_find(&thread_ids, (uint64_t)tid);
    return l ? (struct thread *)((char *)l - __builtin_offsetof(struct thread, id)) : NULL;
}

size_t thread_count(void) { return __atomic_load_n(&thread_ids.count, __ATOMIC_RELAXED); }

/* thread_lock held, the registry with room for one more: an id no thread holds, 0 when every one
   is held. Of count + 1 ids in a row at most count are held, so the look ends within that many. */
static int tid_take(void) {
    for (size_t looked = 0; looked <= thread_ids.count; looked++) {
        int id = tid_next;
        tid_next = id == INT32_MAX ? 1 : id + 1;
        if (!id_table_find(&thread_ids, (uint64_t)id)) return id;
    }
    return 0;
}

/* thread_lock held: t leaves its process's list of threads. */
static void process_unlist(struct thread *t) {
    *t->process_prev = t->process_next;
    if (t->process_next) t->process_next->process_prev = t->process_prev;
    t->process_next = NULL;
    t->process_prev = NULL;
}

/* A thread's record and its extended state's area after it, in one allocation (kfree frees both). */
static struct thread *thread_record(void) {
    struct thread *t = kzalloc(sizeof *t + fpu_storage_bytes());
    if (!t) return NULL;
    t->fpu = fpu_state_in(t + 1);
    fpu_state_init(t->fpu);
    return t;
}

/* A record with its kernel stack, its id and its place in the registry (and in p's list when it is
   one of p's other threads), or NULL without memory. */
static struct thread *thread_listed(const char *name, struct process *p, uint8_t *kstack) {
    struct thread *t = kstack ? thread_record() : NULL;
    if (!t) return NULL;
    t->kstack = kstack;
    strlcpy(t->name, name, sizeof t->name);
    t->process = p;
    thread_wait_init(&t->wait);
    t->start_ns = clock_now_ns();
    t->ksp = (uint64_t)t->kstack + KSTACK_PAGES * PAGE_SIZE;
    uint64_t f;
    if (id_table_room(&thread_ids, &thread_lock, &f) < 0 || !(t->tid = tid_take())) {
        spin_unlock(&thread_lock, f);
        kfree(t);
        return NULL;
    }
    t->id.id = (uint64_t)t->tid;
    id_table_add(&thread_ids, &t->id);
    t->all_next = thread_list;
    t->all_prev = &thread_list;
    if (thread_list) thread_list->all_prev = &t->all_next;
    thread_list = t;
    if (p && p->main) { /* one of p's other threads, whose record keeps p's */
        t->process_next = p->threads;
        t->process_prev = &p->threads;
        if (p->threads) p->threads->process_prev = &t->process_next;
        p->threads = t;
        __atomic_add_fetch(&p->refs, 1, __ATOMIC_RELAXED);
    }
    spin_unlock(&thread_lock, f);
    return t;
}

/* t's record leaves the registry (and its process's list); its kernel stack and record go. */
static void thread_release(struct thread *t) {
    uint64_t f = spin_lock(&thread_lock);
    id_table_drop(&thread_ids, &t->id);
    *t->all_prev = t->all_next;
    if (t->all_next) t->all_next->all_prev = t->all_prev;
    if (t->process_prev) process_unlist(t);
    spin_unlock(&thread_lock, f);
    kstack_free(t->kstack);
    kfree(t);
}

struct thread *thread_new(const char *name, struct process *p) {
    uint8_t *kstack = kstack_alloc();
    struct thread *t = thread_listed(name, p, kstack);
    if (!t) {
        if (kstack) kstack_free(kstack);
        return NULL;
    }
    sched_thread_add(t);
    return t;
}

struct thread *thread_new_idle(const char *name, unsigned node) {
    uint8_t *kstack = vmalloc_node(KSTACK_PAGES, node);
    struct thread *t = thread_listed(name, NULL, kstack);
    if (!t && kstack) vfree(kstack, KSTACK_PAGES);
    return t;
}

void thread_free(struct thread *t) {
    sched_thread_remove(t);
    thread_release(t);
}

/* One of p's other threads that never ran: out of p's list, and freed. */
static void thread_discard(struct thread *t) {
    struct process *p = t->process;
    thread_free(t);
    process_put(p);
}

void thread_left(struct thread *t) {
    struct process *p = t->process;
    if (!p) { /* a kernel thread: no one joins it */
        thread_free(t);
        return;
    }
    if (t == p->main) { /* the process ended: its record no longer stays for that */
        process_put(p);
        return;
    }
    uint64_t f = spin_lock(&thread_lock);
    t->ended = true;
    bool gone = t->detached;
    if (gone) process_unlist(t);
    /* its joiner, or the main thread ending p, which waits for every other thread's end */
    wq_wake_all(&p->wait);
    spin_unlock(&thread_lock, f);
    if (gone) {
        thread_free(t);
        process_put(p);
    }
}

/* ---- starts ---- */

static void push(struct thread *t, uint64_t v) {
    t->ksp -= 8;
    *(uint64_t *)t->ksp = v;
}

void thread_enter_user(struct thread *t, const struct regs *frame) {
    t->ksp -= sizeof *frame;
    memcpy((void *)t->ksp, frame, sizeof *frame);
    push(t, (uint64_t)user_trampoline); /* ret target of context_switch */
    for (unsigned i = 0; i < 6; i++) push(t, 0); /* rbp, rbx, r12-r15 */
}

#define ANY_CPU UINT32_MAX

static struct thread *kthread_new(const char *name, void (*fn)(void *), void *arg, unsigned cpu) {
    struct thread *t = thread_new(name, NULL);
    if (!t) return NULL;
    if (cpu != ANY_CPU) sched_thread_bind(t, cpu);
    push(t, (uint64_t)kthread_trampoline); /* ret target of context_switch */
    push(t, 0);                           /* rbp */
    push(t, 0);                           /* rbx */
    push(t, (uint64_t)fn);                /* r12 */
    push(t, (uint64_t)arg);               /* r13 */
    push(t, 0);                           /* r14 */
    push(t, 0);                           /* r15 */
    sched_make_ready(t);
    return t;
}

struct thread *kthread_create(const char *name, void (*fn)(void *), void *arg) {
    return kthread_new(name, fn, arg, ANY_CPU);
}

struct thread *kthread_create_on(const char *name, void (*fn)(void *), void *arg, unsigned cpu) {
    return kthread_new(name, fn, arg, cpu);
}

void kthread_start(void (*fn)(void *), void *arg) {
    irq_restore(0x200); /* interrupts on: a switch due since sched_post_switch comes here */
    fn(arg);
    thread_exit(0);
}

/* t's stack in its process's mmap region goes back, with the unmapped page at each end. */
static void user_stack_free(struct thread *t) {
    struct process *p = t->process;
    if (t->user_stack) {
        vmm_user_free(p->vmm_space, t->user_stack, t->user_stack_size);
        mmap_hole_add(p, t->user_stack - PAGE_SIZE, t->user_stack_size + 2 * PAGE_SIZE);
    }
    t->user_stack = t->user_stack_size = 0;
}

int thread_create(uint64_t entry, uint64_t function, uint64_t argument, uint64_t stack_bytes) {
    struct process *p = process_current();
    if (!p || p->exiting || p->main->killed) return -N_ERR_STOPPED;
    /* No size cap of its own: the address region and free memory bound it. */
    if (stack_bytes > UINT64_MAX - 3 * PAGE_SIZE) return -N_ERR_INVALID;
    stack_bytes = stack_bytes ? ALIGN_UP(stack_bytes, PAGE_SIZE) : THREAD_STACK_DEFAULT;
    struct thread *t = thread_new(p->name, p);
    if (!t) return -N_ERR_NO_MEMORY;
    /* Unmapped page at each end, distinct from every other thread's stack. */
    uint64_t reserved = mmap_reserve(p, stack_bytes + PAGE_SIZE);
    if (reserved) {
        t->user_stack = reserved + PAGE_SIZE;
        t->user_stack_size = stack_bytes;
    }
    if (!reserved || vmm_user_reserve(p->vmm_space, t->user_stack, t->user_stack_size, VM_W) < 0) {
        user_stack_free(t);
        thread_discard(t);
        return -N_ERR_NO_MEMORY;
    }
    struct regs frame;
    memset(&frame, 0, sizeof frame);
    frame.rip = entry;
    frame.cs = USER_CS;
    frame.ss = USER_DS;
    frame.rflags = 0x202;
    frame.rsp = t->user_stack + t->user_stack_size - 8;
    frame.rdi = function;
    frame.rsi = argument;
    thread_enter_user(t, &frame);
    int tid = t->tid;
    sched_make_ready(t);
    return tid;
}

/* ---- joins and ends ---- */

/* thread_lock held: tid names one of p's other threads. */
static struct thread *other_thread(struct process *p, int tid) {
    struct thread *t = thread_find(tid);
    return t && t->process == p && t != p->main ? t : NULL;
}

int thread_detach(int tid) {
    struct process *p = process_current();
    uint64_t f = spin_lock(&thread_lock);
    struct thread *t = other_thread(p, tid), *gone = NULL;
    int r = 0;
    if (!t || t->joining || t->detached) r = -N_ERR_INVALID;
    else if (t->ended) { /* its CPU has left it already: freed here */
        process_unlist(t);
        gone = t;
    } else t->detached = true; /* the CPU that leaves it after its end frees it (thread_left) */
    spin_unlock(&thread_lock, f);
    if (gone) {
        thread_free(gone);
        process_put(p);
    }
    return r;
}

int thread_join(int tid) {
    struct process *p = process_current();
    struct thread *self = thread_current();
    uint64_t f = spin_lock(&thread_lock);
    struct thread *t = other_thread(p, tid);
    if (!t || t == self || t->joining || t->detached) {
        spin_unlock(&thread_lock, f);
        return -N_ERR_INVALID;
    }
    t->joining = true;
    for (;;) {
        if (self->killed || p->exiting) {
            t->joining = false;
            spin_unlock(&thread_lock, f);
            wq_wake_all(&p->wait);
            return -N_ERR_STOPPED;
        }
        if (t->ended) break;
        wq_wait_locked(&p->wait, &thread_lock);
        spin_take(&thread_lock);
    }
    process_unlist(t);
    spin_unlock(&thread_lock, f);
    thread_free(t);
    process_put(p);
    return 0;
}

void thread_kill_locked(struct thread *t) {
    if (!t->killed) t->kill_code = 130;
    __atomic_store_n(&t->killed, true, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&t->sched.state, __ATOMIC_SEQ_CST) == THREAD_BLOCKED) {
        if (wq_take_blocked(t, false)) sched_make_ready(t);
    } else {
        cpu_kick(__atomic_load_n(&t->sched.cpu, __ATOMIC_ACQUIRE));
    }
}

void thread_end_others(struct process *p) {
    uint64_t f = spin_lock(&thread_lock);
    p->exiting = true;
    for (;;) {
        struct thread *alive = NULL, *ended = NULL;
        for (struct thread *c = p->threads; c; c = c->process_next) {
            if (!c->ended) {
                if (!c->killed) thread_kill_locked(c);
                alive = c;
            } else if (!c->joining) {
                ended = c;
                break;
            }
        }
        if (ended) {
            process_unlist(ended);
            spin_unlock(&thread_lock, f);
            thread_free(ended);
            process_put(p);
            f = spin_lock(&thread_lock);
            continue;
        }
        if (!alive) break;
        wq_wait_locked(&p->wait, &thread_lock);
        spin_take(&thread_lock);
    }
    spin_unlock(&thread_lock, f);
}

NORETURN void thread_exit(int code) {
    struct thread *t = thread_current();
    if (t->sched.idle) panic("idle thread tried to exit");
    struct handle *cancel = t->net_setup_cancel;
    t->net_setup_cancel = NULL;
    handle_close(cancel);
    sti();
    struct process *p = t->process;
    if (p && t == p->main) process_end(p, code);
    if (p) {
        /* The thread never returns to ring 3, so its user stack is dead now: an unjoined ended
           thread keeps only its kernel stack and record. Once the app is ending, its space goes
           whole after its threads have ended instead. */
        if (!p->exiting) user_stack_free(t);
        t->user_stack = t->user_stack_size = 0;
        /* Off the space before it is seen ended: once the main thread sees no thread left, the
           space ends (process_end), and no CPU may run this one over it then. */
        vmm_leave();
    }
    /* No handle closes, no child is orphaned, the space stays: they are the process's. The end is
       set under thread_lock, where the kills look at it; the CPU that leaves the thread for good
       frees it or tells its joiner (thread_left). */
    cli();
    spin_take(&thread_lock);
    t->sched.state = THREAD_ZOMBIE;
    spin_give(&thread_lock);
    schedule();
    panic("ended thread %d was scheduled", t->tid);
}
