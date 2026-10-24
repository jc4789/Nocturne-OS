/* User processes: ELF64 loading and spawning. */
#include "kernel.h"
#include "arch/cpu.h"
#include "sys/sched.h"
#include "sys/proc.h"
#include "sys/syscall.h"
#include "sys/object.h"
#include "sys/stream.h"
#include "fs/store.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

struct elf64_ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} PACKED;

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} PACKED;

#define PT_LOAD 1
#define PF_X    1
#define PF_W    2

extern void user_trampoline(void);

static void kpush(struct task *t, uint64_t v) {
    t->ksp -= 8;
    *(uint64_t *)t->ksp = v;
}

int init_pid;

/* ---- app handles: an app's end is an event on its handle ---- */

struct app_record {
    int pid;
    int status;
    bool ended;
    struct wait_queue wait;
    struct object object; /* freed when the app has ended and its last handle is closed */
};

static struct app_record *app_record(struct handle *h) { return h->object->priv; }

static uint32_t app_ready(struct handle *h) { return app_record(h)->ended ? N_EVENT_APP_ENDED : 0; }
static void app_release(struct object *o) { kfree(o->priv); }
static const struct object_ops app_ops = {.kind = N_KIND_APP, .ready = app_ready, .release = app_release};

/* A record with no references yet. */
static struct object *app_new(void) {
    struct app_record *rec = kzalloc(sizeof *rec);
    if (!rec) return NULL;
    object_init(&rec->object, &app_ops, rec);
    return &rec->object;
}

int proc_app_create(struct handle **out) {
    struct object *o = app_new();
    if (!o) return -N_ERR_NO_MEMORY;
    *out = handle_open(o, false);
    if (!*out) { app_release(o); return -N_ERR_NO_MEMORY; }
    return 0;
}

/* The app's process is ending; BSP IRQ guard held. */
void proc_app_ended(struct object *o, int status) {
    struct app_record *rec = o->priv;
    rec->status = status;
    rec->ended = true;
    wq_wake_all(&rec->wait);
    object_notify(o);
}

bool proc_app_is(struct handle *h) { return handle_is(h, &app_ops); }

/* The app's identifier while it runs; 0 once it has ended. */
int proc_app_id(struct handle *h) { return app_record(h)->pid; }

int proc_app_pid(struct handle *h) {
    struct app_record *rec = app_record(h);
    uint64_t flags = irq_save();
    int pid = rec->ended ? 0 : rec->pid;
    irq_restore(flags);
    return pid;
}

int proc_app_result(struct handle *h, uint64_t timeout_ms, int *status) {
    struct app_record *rec = app_record(h);
    uint64_t deadline = uptime_ms() + timeout_ms;
    uint64_t flags = irq_save();
    while (!rec->ended) {
        uint64_t now = uptime_ms();
        if (timeout_ms != UINT64_MAX && now >= deadline) {
            irq_restore(flags);
            return timeout_ms ? -N_ERR_TIMEOUT : -N_ERR_NOT_READY;
        }
        if (current_task->killed) { irq_restore(flags); return -N_ERR_STOPPED; }
        if (timeout_ms == UINT64_MAX) wq_wait(&rec->wait);
        else wq_wait_timeout(&rec->wait, deadline - now);
    }
    *status = rec->status;
    irq_restore(flags);
    return 0;
}

/* A new handle on a running app, by the identifier the app list shows. An app
   the kernel started (proc_spawn_simple) gets its record here. */
int proc_app_open(int pid, struct handle **out) {
    struct object *spare = app_new();
    if (!spare) return -N_ERR_NO_MEMORY;
    uint64_t flags = irq_save();
    struct task *t = task_find(pid);
    struct object *o = NULL;
    if (t && t->is_user && !t->is_thread && t->state != TASK_ZOMBIE && !t->exiting) {
        if (!t->app) {
            ((struct app_record *)spare->priv)->pid = pid;
            object_ref(spare); /* the task's reference, as proc_launch takes */
            t->app = spare;
            spare = NULL;
        }
        o = t->app;
        object_ref(o);
    }
    irq_restore(flags);
    if (spare) app_release(spare);
    if (!o) return -N_ERR_NO_APP;
    *out = handle_open(o, false);
    object_unref(o);
    return *out ? 0 : -N_ERR_NO_MEMORY;
}

/* The name a task runs under: the program document's name. */
static const char *program_name(const char *path) {
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/') name = p + 1;
    return name;
}

/* Whether the launcher gave the setting name ("NAME=..."). */
static bool setting_given(const struct proc_launch *l, const char *name) {
    size_t n = strlen(name);
    for (int i = 0; i < l->setting_count; i++)
        if (!strncmp(l->settings[i], name, n) && l->settings[i][n] == '=') return true;
    return false;
}

/* n bytes of the program document doc from offset into t's memory at va, through buf
   (LOAD_CHUNK bytes): a program is never read whole into the kernel's memory. */
#define LOAD_CHUNK (256 * 1024)
static int load_bytes(struct task *t, struct handle *doc, uint64_t offset, uint64_t va, uint64_t n, uint8_t *buf) {
    for (uint64_t done = 0; done < n;) {
        size_t k = (size_t)MIN(n - done, (uint64_t)LOAD_CHUNK);
        int64_t got = store_document_read(doc, offset + done, buf, k, false);
        if (got < 0) return (int)got;
        if ((size_t)got != k) return -N_ERR_IO;
        if (vmm_copy_to_space(t->pml4, va + done, buf, k) < 0) return -N_ERR_NO_MEMORY;
        done += k;
    }
    return 0;
}

/* n bytes of doc from offset, all of them, into the kernel's buf. */
static int read_exactly(struct handle *doc, uint64_t offset, void *buf, size_t n) {
    int64_t got = store_document_read(doc, offset, buf, n, false);
    return got < 0 ? (int)got : (size_t)got != n ? -N_ERR_NOT_PROGRAM : 0;
}

int proc_launch(const char *path, const struct proc_launch *l) {
    int argc = l->argc;
    char **argv = l->argv;
    uint64_t *pointers = NULL;
    struct elf64_phdr *ph = NULL;
    uint8_t *buf = NULL;
    struct program *program = NULL;
    struct handle *doc;
    int r =store_document_open(path, N_DOCUMENT_READ, false, &doc);
    if (r < 0) return r;
    struct elf64_ehdr eh;
    int64_t size = store_document_size(doc);
    if (size < 0) r = (int)size;
    else if ((uint64_t)size < sizeof eh) r = -N_ERR_NOT_PROGRAM;
    else r = read_exactly(doc, 0, &eh, sizeof eh);
    if (r >= 0 && (memcmp(eh.ident, "\x7f" "ELF", 4) || eh.ident[4] != 2 || eh.machine != 0x3E || eh.type != 2 ||
                   eh.phentsize != sizeof *ph || eh.phoff > (uint64_t)size ||
                   (uint64_t)eh.phnum * sizeof *ph > (uint64_t)size - eh.phoff))
        r = -N_ERR_NOT_PROGRAM;
    if (r >= 0 && !(ph = kmalloc(eh.phnum ? eh.phnum * sizeof *ph : 1))) r = -N_ERR_NO_MEMORY;
    if (r >= 0) r = read_exactly(doc, eh.phoff, ph, eh.phnum * sizeof *ph);
    if (r >= 0 && !(buf = kmalloc(LOAD_CHUNK))) r = -N_ERR_NO_MEMORY;
    if (r < 0) {
        kfree(ph);
        handle_close(doc);
        return r;
    }
    struct task *t = task_alloc(program_name(path));
    if (!t) {
        kfree(buf);
        kfree(ph);
        handle_close(doc);
        return -N_ERR_NO_MEMORY;
    }
    t->is_user = true;
    t->pml4 = vmm_new_space();
    if (!t->pml4) goto fail;

    uint64_t top = 0;
    program = program_get(doc);
    for (int i = 0; i < eh.phnum; i++) {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0) continue;
        if (ph[i].vaddr >= USER_MMAP_BASE || ph[i].memsz > USER_MMAP_BASE - ph[i].vaddr ||
            ph[i].filesz > ph[i].memsz || ph[i].offset > (uint64_t)size || ph[i].filesz > (uint64_t)size - ph[i].offset)
            goto fail;
        int prot = (ph[i].flags & PF_W ? VM_W : 0) | (ph[i].flags & PF_X ? VM_X : 0);
        /* programs built by tcc before crt0's _start section was marked executable */
        if (!(prot & VM_X) && eh.entry >= ph[i].vaddr && eh.entry - ph[i].vaddr < ph[i].memsz) {
            kprintf("exec: %s: entry point is not executable (old tcc build?); allowing it\n", path);
            prot |= VM_X;
        }
        if (ph[i].vaddr + ph[i].memsz > top) top = ph[i].vaddr + ph[i].memsz;
        /* A read-only segment whose pages no other segment touches is shared
           with every task running the program. */
        bool alone = program && !(prot & VM_W);
        uint64_t first = ALIGN_DOWN(ph[i].vaddr, PAGE_SIZE), last = ALIGN_UP(ph[i].vaddr + ph[i].memsz, PAGE_SIZE);
        for (int j = 0; alone && j < eh.phnum; j++)
            if (j != i && ph[j].type == PT_LOAD && ph[j].memsz &&
                ALIGN_DOWN(ph[j].vaddr, PAGE_SIZE) < last && ALIGN_UP(ph[j].vaddr + ph[j].memsz, PAGE_SIZE) > first)
                alone = false;
        int shared = alone ? program_map(t, program, ph[i].vaddr, ph[i].filesz, ph[i].memsz, prot & VM_X) : -1;
        if (shared == 0) continue; /* its bytes are there */
        /* Otherwise the file's bytes are copied now; the zero tail (.bss) gets frames on first touch. */
        if (shared < 0 && vmm_user_reserve(t->pml4, ph[i].vaddr, ph[i].memsz, prot) < 0) goto fail;
        if ((r = load_bytes(t, doc, ph[i].offset, ph[i].vaddr, ph[i].filesz, buf)) < 0) goto fail;
    }
    program_put(program, true);
    program = NULL;
    kfree(buf);
    buf = NULL;
    kfree(ph);
    ph = NULL;
    handle_close(doc);
    doc = NULL;
    t->mmap_next = USER_MMAP_BASE;
    t->low_base = t->low_next = MAX(USER_LOW_BASE, ALIGN_UP(top, PAGE_SIZE)); /* above the program image */

    /* The settings: the launcher's, then those of the system it did not give: the user's folder
       and temporary folder. */
    char user[sizeof "USERFOLDER=C:\\Users\\" + 255], temp[sizeof user + sizeof "\\AppData\\Temp"];
    const char *own[2];
    int owned = 0;
    if (!setting_given(l, "USERFOLDER")) {
        ksnprintf(user, sizeof user, "USERFOLDER=%s", store_user_folder());
        own[owned++] = user;
    }
    if (!setting_given(l, "TEMP")) {
        ksnprintf(temp, sizeof temp, "TEMP=%s", store_user_temp());
        own[owned++] = temp;
    }
#define SETTING(i) ((i) < l->setting_count ? l->settings[i] : own[(i) - l->setting_count])

    /* User stack: the argument and setting strings, then their two
       null-terminated pointer arrays. They must fit well inside the stack. */
    _Static_assert(N_LAUNCH_MAX_BYTES <= USER_STACK_MAX / 2, "launch strings must leave the stack room");
    int settings = l->setting_count + owned;
    size_t strings = 0;
    for (int i = 0; i < argc; i++) strings += strlen(argv[i]) + 1;
    for (int i = 0; i < settings; i++) strings += strlen(SETTING(i)) + 1;
    size_t pointer_bytes = ((size_t)argc + 1 + (size_t)settings + 1) * 8;
    if (strings + pointer_bytes > N_LAUNCH_MAX_BYTES) { r = -N_ERR_TOO_MANY_ARGUMENTS; goto fail; }
    pointers = kmalloc(pointer_bytes);
    if (!pointers) { r = -N_ERR_NO_MEMORY; goto fail; }
    /* pre-fault what the start needs plus 64 KiB; the rest grows on demand */
    uint64_t prefault = ALIGN_UP(strings + pointer_bytes + 64 + 65536, PAGE_SIZE);
    if (vmm_user_alloc(t->pml4, USER_STACK_TOP - prefault, prefault, VM_W) < 0) { r = -N_ERR_NO_MEMORY; goto fail; }
    if (handle_table_reserve(t, l->handle_count) < 0) { r = -N_ERR_NO_MEMORY; goto fail; }
    uint64_t sp = USER_STACK_TOP;
    for (int i = settings - 1; i >= 0; i--) {
        size_t n = strlen(SETTING(i)) + 1;
        sp -= n;
        if (vmm_copy_to_space(t->pml4, sp, SETTING(i), n) < 0) goto fail;
        pointers[argc + 1 + i] = sp;
    }
#undef SETTING
    for (int i = argc - 1; i >= 0; i--) {
        size_t n = strlen(argv[i]) + 1;
        sp -= n;
        if (vmm_copy_to_space(t->pml4, sp, argv[i], n) < 0) goto fail;
        pointers[i] = sp;
    }
    pointers[argc] = 0;
    pointers[argc + 1 + settings] = 0;
    sp = ALIGN_DOWN(sp, 16);
    sp -= pointer_bytes;
    sp = ALIGN_DOWN(sp, 16);
    if (vmm_copy_to_space(t->pml4, sp, pointers, pointer_bytes) < 0) goto fail;
    uint64_t argv_ptr = sp, settings_ptr = sp + ((uint64_t)argc + 1) * 8;
    kfree(pointers);
    pointers = NULL;
    sp -= 8; /* fake return address slot keeps the ABI alignment */

    /* initial interrupt frame */
    struct regs frame;
    memset(&frame, 0, sizeof frame);
    frame.rip = eh.entry;
    frame.cs = USER_CS;
    frame.rflags = 0x202;
    frame.rsp = sp;
    frame.ss = USER_DS;
    frame.rdi = argc;
    frame.rsi = argv_ptr;
    frame.rdx = settings_ptr;
    t->ksp -= sizeof frame;
    memcpy((void *)t->ksp, &frame, sizeof frame);
    kpush(t, (uint64_t)user_trampoline);
    for (int i = 0; i < 6; i++) kpush(t, 0);

    for (size_t i = 0; i < l->handle_count; i++)
        if (l->handles[i]) t->handles[i] = handle_ref(l->handles[i]);
    t->parent = l->parent;
    if (l->app) {
        /* The task's own reference keeps the record until it has reported. */
        object_ref(l->app);
        ((struct app_record *)l->app->priv)->pid = t->pid;
        t->app = l->app;
        t->app_launched = true;
    }
    sched_make_ready(t);
    return t->pid;

fail:
    if (r >= 0) r = -N_ERR_NOT_PROGRAM;
    kfree(pointers);
    program_put(program, false);
    kfree(buf);
    kfree(ph);
    if (doc) handle_close(doc);
    if (t->pml4) {
        sys_shared_cleanup(t);
        vmm_free_space(t->pml4);
    }
    t->pml4 = kernel_pml4;
    t->state = TASK_ZOMBIE;
    t->parent = NULL;
    /* nobody waits for it: free right away */
    {
        uint64_t f = irq_save();
        struct task **pp = &task_list;
        while (*pp && *pp != t) pp = &(*pp)->all_next;
        if (*pp) *pp = t->all_next;
        irq_restore(f);
        vfree(t->kstack, KSTACK_PAGES);
        if (t->handles != t->initial_handles) kfree(t->handles);
        kfree(t);
    }
    return r;
}

int proc_spawn_simple(const char *path, const char *arg1, struct task *parent) {
    char *argv[3] = {(char *)path, (char *)arg1, NULL};
    struct handle *nul = stream_null_open();
    struct handle *handles[3] = {nul, nul, nul};
    char folder[sizeof "FOLDER=C:\\Users\\" + 255];
    ksnprintf(folder, sizeof folder, "FOLDER=%s", store_user_folder());
    char *settings[] = {folder};
    struct proc_launch l = {.argc = arg1 ? 2 : 1, .argv = argv, .setting_count = 1, .settings = settings,
                            .handles = handles, .handle_count = 3, .parent = parent};
    int r = proc_launch(path, &l);
    handle_close(nul);
    return r;
}

uint64_t proc_user_pages(struct task *t) {
    if (!t->is_user || t->is_thread || t->pml4 == kernel_pml4) return 0;
    return vmm_user_pages(t->pml4);
}
