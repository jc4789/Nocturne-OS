/* Processes (sys/process.h): their records, starts and ends, their families, and finding and
   stopping them by pid. Their threads are sys/thread.c's. */
#include "kernel.h"
#include "arch/cpu.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/shared.h"
#include "mm/vmm.h"
#include "sys/object.h"
#include "sys/proc.h"
#include "sys/process.h"
#include "sys/clock.h"
#include "sys/log.h"
#include "sys/sched.h"
#include "sys/thread.h"
#include "sys/wait.h"

struct process *process_new(const char *name) {
    struct process *p = kzalloc(sizeof *p);
    if (!p) return NULL;
    strlcpy(p->name, name, sizeof p->name);
    p->refs = 1; /* until it has ended */
    p->mappings_lock.rank = SPIN_SHARED_MAPPINGS;
    handle_table_init(&p->handles);
    struct thread *t = thread_new(name, p);
    if (!t) {
        kfree(p);
        return NULL;
    }
    uint64_t f = spin_lock(&thread_lock);
    p->main = t;
    spin_unlock(&thread_lock, f);
    return p;
}

void process_start(struct process *p, struct process *parent, struct object *app) {
    uint64_t f = spin_lock(&thread_lock); /* its family's links */
    p->pid = p->main->tid; /* found by it from here on */
    p->app = app;
    if (parent) {
        p->parent = parent;
        p->sibling_next = parent->children;
        p->sibling_prev = &parent->children;
        if (parent->children) parent->children->sibling_prev = &p->sibling_next;
        parent->children = p;
    }
    spin_unlock(&thread_lock, f);
    sched_make_ready(p->main);
}

/* p's space and its layout go, none of its threads running over it any more: the thread of p that
   ends it leaves it first. */
static void process_space_end(struct process *p) {
    if (!p->vmm_space) return;
    shared_process_end(p);
    struct vmm_space *old = p->vmm_space;
    /* under thread_lock: one who read the space under it is done with its tables */
    uint64_t f = spin_lock(&thread_lock);
    p->vmm_space = NULL;
    spin_unlock(&thread_lock, f);
    if (thread_current()->process == p) vmm_leave();
    vmm_space_end(old);
    vmm_space_put(old);
}

void process_discard(struct process *p) {
    /* nobody waits for it: its handles are closed and it is freed right away */
    handle_table_close(&p->handles);
    process_space_end(p);
    thread_free(p->main);
    kfree(p);
}

void process_put(struct process *p) {
    if (__atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL)) return;
    thread_free(p->main);
    kfree(p);
}

uint64_t process_pages(struct process *p) { return p->vmm_space ? vmm_user_pages(p->vmm_space) : 0; }

#define REPORT_TOP 8
struct memory_report {
    struct { int pid; char name[32]; uint64_t kb, messages; } top[REPORT_TOP];
    int n, apps;
    uint64_t kb, messages; /* every app's together */
};

/* thread_walk: an app counted, and kept among the largest. */
static void report_one(struct thread *t, void *arg) {
    struct memory_report *r = arg;
    struct process *p = process_of_main(t);
    if (!p) return;
    uint64_t kb = process_pages(p) * 4, messages = __atomic_load_n(&p->messages, __ATOMIC_RELAXED);
    r->apps++;
    r->kb += kb;
    r->messages += messages;
    int at = r->n < REPORT_TOP ? r->n++ : REPORT_TOP;
    while (at > 0 && r->top[at - 1].kb < kb) {
        if (at < REPORT_TOP) r->top[at] = r->top[at - 1];
        at--;
    }
    if (at < REPORT_TOP) {
        r->top[at].pid = p->pid;
        strlcpy(r->top[at].name, p->name, sizeof r->top[at].name);
        r->top[at].kb = kb;
        r->top[at].messages = messages;
    }
}

/* The largest apps, so the owner of the memory is visible, with where their private pages sit; and
   every app's together, since many middling ones can outweigh the largest. */
void process_memory_report(struct process *who, int64_t inc) {
    static uint64_t last_report; /* any CPU: the one that moves it on reports */
    uint64_t now = clock_now_ns(), last = __atomic_load_n(&last_report, __ATOMIC_RELAXED);
    if (last && now - last < 10 * NS_PER_S) return;
    if (!__atomic_compare_exchange_n(&last_report, &last, now, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    struct memory_report r = {0};
    thread_walk(report_one, &r);
    kprintf("[memory] pid %d (%s): no pages for %ld more bytes, %lu pages free; %d apps, %luK, %luK in messages; largest:",
            who->pid, who->name, (long)inc, (unsigned long)pmm_free_pages(), r.apps, (unsigned long)r.kb,
            (unsigned long)(r.messages / 1024));
    for (int i = 0; i < r.n; i++) kprintf(" %d/%s %luK", r.top[i].pid, r.top[i].name, (unsigned long)r.top[i].kb);
    kprintf("\n");
    /* each of the largest looked up again by pid, its page tables walked without the lock */
    for (int i = 0; i < r.n; i++) {
        uint64_t flags = spin_lock(&thread_lock);
        struct process *p = process_find(r.top[i].pid);
        struct vmm_space *space = p ? p->vmm_space : NULL;
        unsigned threads = 0;
        if (space) {
            vmm_space_get(space);
            for (struct thread *q = p->threads; q; q = q->process_next)
                if (q->sched.state != THREAD_ZOMBIE) threads++;
        }
        spin_unlock(&thread_lock, flags);
        if (!space) continue;
        uint64_t s[6];
        vmm_user_pages_split(space, s);
        vmm_space_put(space);
        kprintf("[memory]  %d/%s image %luK low %luK reserved %luK stack %luK shared %luK tables %luK messages %luK "
                "threads %u\n",
                r.top[i].pid, r.top[i].name, (unsigned long)s[0] * 4, (unsigned long)s[1] * 4, (unsigned long)s[2] * 4,
                (unsigned long)s[3] * 4, (unsigned long)s[4] * 4, (unsigned long)s[5] * 4,
                (unsigned long)(r.top[i].messages / 1024), threads);
    }
}

struct process *process_find(int pid) {
    struct thread *t = thread_find(pid);
    struct process *p = t ? t->process : NULL;
    return p && p->main == t && p->pid == pid && !p->ended ? p : NULL;
}

int process_kill(int pid) {
    uint64_t f = spin_lock(&thread_lock);
    struct process *p = process_find(pid);
    if (p) thread_kill_locked(p->main); /* its end ends its other threads */
    spin_unlock(&thread_lock, f);
    return p ? 0 : -N_ERR_NO_APP;
}

/* thread_lock held: the process after c in a walk of root's family below it (root first), NULL
   after the last. */
static struct process *family_next(struct process *c, struct process *root) {
    if (c->children) return c->children;
    for (; c != root; c = c->parent)
        if (c->sibling_next) return c->sibling_next;
    return NULL;
}

int process_kill_tree(int pid, bool include_self) {
    int killed = 0;
    uint64_t f = spin_lock(&thread_lock);
    struct process *root = process_find(pid);
    for (struct process *c = root; c; c = family_next(c, root)) {
        if ((c == root && !include_self) || c->ended || c->main->killed) continue;
        thread_kill_locked(c->main);
        killed++;
    }
    spin_unlock(&thread_lock, f);
    return killed;
}

/* thread_walk: an app's main thread is killed, and counted while the app has not ended. */
static void stop_one(struct thread *t, void *arg) {
    struct process *p = t->process;
    if (!p || p->main != t || p->ended) return;
    ++*(int *)arg;
    if (!t->killed) thread_kill_locked(t);
}

int process_stop_all(void) {
    int alive = 0;
    thread_walk(stop_one, &alive);
    return alive;
}

/* thread_lock held: t, unless it is self or done already, ends with code. */
static void kill_with(struct thread *t, struct thread *self, int code) {
    if (t == self || t->sched.state == THREAD_ZOMBIE || t->killed) return;
    thread_kill_locked(t);
    t->kill_code = code;
}

NORETURN void process_exit(int code) {
    struct thread *self = thread_current();
    struct process *p = self->process;
    if (self != p->main && !p->exiting) {
        /* Ending the app from one of its other threads (ISO C exit, or a fault that may abandon a
           pool descriptor or a shared mutex) cannot be done by ending just this thread: the main
           thread and every sibling are killed so the app ends with this code and no one waits for
           a dead holder. The main thread's end then waits for them before its space goes. */
        uint64_t f = spin_lock(&thread_lock);
        kill_with(p->main, self, code);
        for (struct thread *t = p->threads; t; t = t->process_next) kill_with(t, self, code);
        spin_unlock(&thread_lock, f);
    }
    thread_exit(code);
}

NORETURN void process_end(struct process *p, int code) {
    p->exit_code = code;
    /* Every other thread ends first: nothing reaches its space or window buffers after that. */
    thread_end_others(p);
    /* The last thread of p: its handles close (a device claim's first, sys/object.h: each owner's
       state ends in its object's close), then its space goes. */
    handle_table_close(&p->handles);
    process_space_end(p);
    struct object *app = p->app;
    p->app = NULL;
    if (app) {
        /* An app reports through its handle; no one collects it. */
        proc_app_ended(app, p->exit_code);
        object_unref(app);
    }
    cli();
    spin_take(&thread_lock);
    /* its children are orphans now, and it leaves its parent's */
    for (struct process *c = p->children, *next; c; c = next) {
        next = c->sibling_next;
        c->parent = NULL;
        c->sibling_next = NULL;
        c->sibling_prev = NULL;
    }
    p->children = NULL;
    if (p->parent) {
        *p->sibling_prev = p->sibling_next;
        if (p->sibling_next) p->sibling_next->sibling_prev = p->sibling_prev;
        p->parent = NULL;
        p->sibling_next = NULL;
        p->sibling_prev = NULL;
    }
    p->ended = true;
    p->main->sched.state = THREAD_ZOMBIE; /* the CPU that leaves it lets go of p (thread_left) */
    spin_give(&thread_lock);
    schedule();
    panic("ended process %d was scheduled", p->pid);
}
