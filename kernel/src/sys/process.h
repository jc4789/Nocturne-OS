/* Processes: an app's handles, address space, family and app record, which its threads share
   (sys/process.c). */
#pragma once
#include "kernel.h"
#include "sys/object.h"
#include "sys/thread.h"
#include "sys/wait.h"

struct vmm_space;
struct space_op;
struct mmap_hole;
struct shared_mapping;

struct process {
    int pid;                  /* its main thread's tid once it started (process_find finds it then) */
    char name[32];
    struct thread *main;      /* its first thread: its end ends the process (process_end) */
    struct thread *threads;   /* its other threads whose records are kept (sys/thread.c; thread_lock) */
    bool exiting;             /* its end has begun: no new thread, no new app record (thread_lock) */
    bool ended;               /* its threads, handles and space are gone (thread_lock) */
    int exit_code;
    /* The record is kept while the process has not ended, and by each of its other threads' records
       (a detached one may be freed after the process ended): the last to let go frees it. */
    int refs;
    /* The app that launched it, NULL for one the kernel started or once its launcher ended; its
       children are the apps it launched that still run. thread_lock guards these links. */
    struct process *parent, *children, *sibling_next, **sibling_prev;
    struct object *app;       /* its app record reports the end */
    struct wait_queue wait;   /* one of its threads ended (thread_join, thread_end_others) */
    struct handle_table handles; /* sys/object.h */
    struct vmm_space *vmm_space; /* its app's page tables (a reference, mm/vmm.c); NULL once it ended,
                                    cleared under thread_lock: a space read under it stays meanwhile */
    /* Its address-space layout (sys/syscall.c, until mm takes it over). space's lock guards the
       reservation regions' frontiers (mmap_next, low_next) and holes, the ranges its threads are
       changing (space_ops) and space_ended; space holds the threads waiting for such a range. It
       comes before shared_lock (syscall.c), the page tables' locks and pmm's, and no two
       processes' are held at once. shared_mappings changes with both space's lock and shared_lock
       held: either is enough to read it. low_base is set before the process runs. */
    struct wait_queue space;
    struct space_op *space_ops;
    bool space_ended;         /* its mappings went at its end: nothing is reserved or freed again */
    uint64_t mmap_next;
    struct mmap_hole *mmap_holes; /* freed anonymous ranges below mmap_next, by address */
    /* The low region [low_base, USER_LOW_TOP): native memory reserved with
       N_MEMORY_LOW for code that holds 32-bit absolute addresses. */
    uint64_t low_base, low_next;
    struct mmap_hole *low_holes;
    struct shared_mapping *shared_mappings; /* shared pages this app has mapped, also being built */
};

/* The calling thread's process; NULL in a kernel thread. */
static inline struct process *process_current(void) {
    struct thread *t = thread_current();
    return t ? t->process : NULL;
}

/* A process and its main thread (named name too), not started: the caller gives it its space,
   handles and first frame (sys/proc.c), then starts it or discards it. NULL without memory. */
struct process *process_new(const char *name);
/* Its main thread runs; parent and app (either may be NULL) become its launcher and its record. */
void process_start(struct process *p, struct process *parent, struct object *app);
/* A process that never started: its handles and space go, then the process. */
void process_discard(struct process *p);
/* thread_lock held: the running process with that pid, or NULL. */
struct process *process_find(int pid);
int process_kill(int pid);
/* Every process launched below pid's (and pid's itself with include_self) is stopped: how many. */
int process_kill_tree(int pid, bool include_self);
int process_stop_all(void); /* power change: stop every app; returns how many have not ended yet */
/* SYS_EXIT or a fatal app exception: the whole app ends with code, from any of its threads. */
NORETURN void process_exit(int code);
/* thread_exit in p's main thread: every other thread ends, then its handles, space and family. */
NORETURN void process_end(struct process *p, int code);
/* A reference to p's record goes (sys/thread.c, thread_left): the last frees it. */
void process_put(struct process *p);
