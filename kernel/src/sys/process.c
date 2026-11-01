/* Processes (sys/process.h): their records, starts and ends, their families, and finding and
   stopping them by pid. Their threads are sys/thread.c's. */
#include "kernel.h"
#include "arch/cpu.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "sys/object.h"
#include "sys/proc.h"
#include "sys/process.h"
#include "sys/sched.h"
#include "sys/syscall.h"
#include "sys/thread.h"
#include "sys/wait.h"

struct process *process_new(const char *name) {
    struct process *p = kzalloc(sizeof *p);
    if (!p) return NULL;
    strlcpy(p->name, name, sizeof p->name);
    p->refs = 1; /* until it has ended */
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
    sys_shared_cleanup(p);
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

int process_stop_all(void) {
    int alive = 0;
    uint64_t f = spin_lock(&thread_lock);
    for (struct thread *t = thread_list; t; t = t->all_next) {
        struct process *p = t->process;
        if (!p || p->main != t || p->ended) continue;
        alive++;
        if (!t->killed) thread_kill_locked(t);
    }
    spin_unlock(&thread_lock, f);
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
