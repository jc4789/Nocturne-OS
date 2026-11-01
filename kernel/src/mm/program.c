/* Read-only ELF segments: every process running a program maps one set of frames for them, so
 * concurrent instances (a worker pool) hold one copy of their code and constants, and a start of a
 * program that is still or was recently running (a compiler's driver, then its compiler and its
 * linker) neither reads nor copies them again. A segment's frames come on first touch: the pages
 * around the touched one are read from the document (PROGRAM_AROUND), so a large program costs what
 * it runs of itself, not its size. A program is its document's content, known by its identity
 * (store_document_identity): a start of a changed document reads it anew, and the processes running
 * the older content keep theirs. Pages are read through a READ opening, which keeps reading the
 * content it opened even when the document is changed, moved or removed. A program joins the cache
 * when a start has mapped all of it. Programs that no process runs stay, the most recently started
 * first, while they hold at most a sixteenth of memory and an eighth stays free; an app that asks
 * for more memory than is free gets theirs. An idle program closes its openings, so it holds no
 * removed or replaced content on C:; the next start gives its own.
 *
 * programs_mutex guards the cache and each program's starts and state. No fault takes it but one
 * that finds memory short. A program's segments are its one start's while it is new (not in the
 * cache) and stay as they are once it is cached. */
#include "mm/shared_parts.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "fs/store.h"

struct program_segment {
    struct program_segment *next;
    struct shared_pages *pages;
    uint64_t offset, va, filesz, memsz;
    bool exec;
};
enum { PROGRAM_NEW, PROGRAM_CACHED, PROGRAM_RETIRED };
struct program {
    struct program *next;
    uint64_t document;
    void *identity;
    size_t identity_bytes;
    struct program_segment *segments;
    size_t starts; /* starts holding it */
    int state;
};
static struct mutex programs_mutex;
static struct program *programs; /* the cache, the most recently started first */

static void program_free(struct program *p) {
    while (p->segments) {
        struct program_segment *s = p->segments;
        p->segments = s->next;
        object_unref(&s->pages->object);
        kfree(s);
    }
    kfree(p->identity);
    kfree(p);
}
/* Whether no process maps it, reads a page of it, or starts it. The mutex held, which keeps the
   answer true: a program no start holds gets no mapping (program_map runs for a start), and a page
   is read only through a mapping. */
static bool program_idle(const struct program *p) {
    if (p->starts) return false;
    for (const struct program_segment *s = p->segments; s; s = s->next)
        if (__atomic_load_n(&s->pages->object.refs, __ATOMIC_RELAXED) != 1) return false;
    return true;
}
/* Its frames, when it is idle; else 0. The mutex held. */
static uint64_t program_idle_pages(const struct program *p) {
    uint64_t pages = 0;
    if (!program_idle(p)) return 0;
    for (const struct program_segment *s = p->segments; s; s = s->next)
        pages += __atomic_load_n(&s->pages->resident, __ATOMIC_RELAXED);
    return pages;
}
uint64_t programs_idle(void) {
    uint64_t pages = 0;
    mutex_lock(&programs_mutex);
    for (const struct program *p = programs; p; p = p->next) pages += program_idle_pages(p);
    mutex_unlock(&programs_mutex);
    return pages;
}
/* Programs no process runs leave the cache, the least recently started first, while they hold more
   than a sixteenth of memory, or fewer than an eighth of it and `need` more pages are free. */
void programs_trim(uint64_t need) {
    uint64_t budget = pmm_total_pages() / 16, floor = pmm_total_pages() / 8 + need;
    for (;;) {
        uint64_t idle = 0;
        struct program **last = NULL, *gone = NULL;
        mutex_lock(&programs_mutex);
        for (struct program **link = &programs; *link; link = &(*link)->next) {
            uint64_t pages = program_idle_pages(*link);
            if (pages) {
                idle += pages;
                last = link;
            }
        }
        if (last && (idle > budget || pmm_free_pages() < floor)) {
            gone = *last;
            *last = gone->next;
            gone->state = PROGRAM_RETIRED;
        }
        mutex_unlock(&programs_mutex);
        if (!gone) break;
        program_free(gone);
    }
    /* The idle ones that stay let go of their openings (nothing can read a page of them). */
    for (;;) {
        struct handle *source = NULL;
        mutex_lock(&programs_mutex);
        for (struct program *p = programs; p && !source; p = p->next) {
            if (!program_idle(p)) continue;
            for (struct program_segment *s = p->segments; s && !source; s = s->next) {
                spin_take(&s->pages->lock);
                source = s->pages->source;
                s->pages->source = NULL;
                spin_give(&s->pages->lock);
            }
        }
        mutex_unlock(&programs_mutex);
        if (!source) return;
        handle_close(source);
    }
}
bool frames_free(uint64_t pages) {
    if (pages <= pmm_free_pages()) return true;
    programs_trim(pages);
    return pages <= pmm_free_pages();
}
struct program *program_get(struct handle *doc) {
    struct n_entry entry;
    void *identity;
    size_t bytes;
    if (store_document_info(doc, &entry) < 0 || store_document_identity(doc, &identity, &bytes) < 0) return NULL;
    programs_trim(0);
    struct program *p = NULL;
    mutex_lock(&programs_mutex);
    for (struct program **link = &programs; *link; link = &(*link)->next)
        if ((*link)->document == entry.id && (*link)->identity_bytes == bytes && !memcmp((*link)->identity, identity, bytes)) {
            p = *link;
            *link = p->next;
            p->next = programs;
            programs = p; /* the most recently started */
            p->starts++;
            break;
        }
    mutex_unlock(&programs_mutex);
    if (p) {
        kfree(identity);
        return p;
    }
    if (!(p = kzalloc(sizeof *p))) {
        kfree(identity);
        return NULL;
    }
    *p = (struct program){.document = entry.id, .identity = identity, .identity_bytes = bytes, .starts = 1, .state = PROGRAM_NEW};
    return p;
}
void program_put(struct program *p, bool complete) {
    if (!p) return;
    struct program *replaced = NULL;
    mutex_lock(&programs_mutex);
    if (p->state == PROGRAM_NEW && complete && p->segments) {
        /* An older content of the document stays with the processes running it. */
        for (struct program **link = &programs; *link; link = &(*link)->next)
            if ((*link)->document == p->document) {
                replaced = *link;
                *link = replaced->next;
                replaced->state = PROGRAM_RETIRED;
                break;
            }
        p->next = programs;
        programs = p;
        p->state = PROGRAM_CACHED;
    }
    bool gone = !--p->starts && p->state != PROGRAM_CACHED;
    if (replaced && replaced->starts) replaced = NULL; /* its last start lets go of it */
    mutex_unlock(&programs_mutex);
    if (replaced) program_free(replaced);
    if (gone) program_free(p);
}
/* owner is a process being started (sys/proc.c): no thread of it runs yet. */
int program_map(struct process *owner, struct program *prog, struct handle *doc, uint64_t offset, uint64_t va,
                uint64_t filesz, uint64_t memsz, bool exec) {
    uint64_t start = ALIGN_DOWN(va, PAGE_SIZE), end = ALIGN_UP(va + memsz, PAGE_SIZE);
    struct shared_pages *p = NULL;
    struct program_segment *fresh = NULL;
    for (struct program_segment *s = prog->segments; s; s = s->next)
        if (s->offset == offset && s->va == va && s->filesz == filesz && s->memsz == memsz && s->exec == exec) {
            p = s->pages;
            break;
        }
    if (!p) {
        /* Others may map a cached program's frames already: only a new one gets more. */
        if (prog->state != PROGRAM_NEW) return -N_ERR_INVALID;
        if (!(fresh = kmalloc(sizeof *fresh))) return -N_ERR_NO_MEMORY;
        int64_t r = shared_object(end - start, true, &p);
        if (r < 0) {
            kfree(fresh);
            return (int)r;
        }
        p->source_offset = offset;
        p->source_bytes = filesz;
        p->source_skip = va - start;
    }
    struct shared_mapping *m = kzalloc(sizeof *m);
    if (!m) {
        if (fresh) {
            shared_release(&p->object);
            kfree(fresh);
        }
        return -N_ERR_NO_MEMORY;
    }
    /* The frames a peer read already are mapped now; the others on first touch. The start holds
       the program, so its segment and frames stay. */
    uint64_t flags = PTE_U | PTE_SHARED | PTE_SHARED_RO | (exec ? 0 : PTE_NX);
    bool ok = true;
    for (size_t i = 0; ok && i < p->count && __atomic_load_n(&p->resident, __ATOMIC_RELAXED); i++) {
        uint64_t frame = demand_frame(p, i);
        ok = !frame || !vmm_map_page(owner->vmm_space, start + i * PAGE_SIZE, frame, flags);
    }
    if (!ok) {
        vmm_user_free(owner->vmm_space, start, end - start);
        kfree(m);
        if (fresh) {
            shared_release(&p->object);
            kfree(fresh);
        }
        return -N_ERR_NO_MEMORY;
    }
    uint64_t lock = spin_lock(&owner->space.lock);
    spin_take(&owner->mappings_lock);
    spin_take(&p->lock);
    if (!p->source) p->source = handle_ref(doc); /* a new segment's, or an idle one's: the same content */
    if (fresh) {
        object_ref(&p->object); /* the program's */
        *fresh = (struct program_segment){.next = prog->segments, .pages = p, .offset = offset, .va = va, .filesz = filesz,
                                          .memsz = memsz, .exec = exec};
        prog->segments = fresh;
    }
    object_ref(&p->object); /* the mapping's */
    m->object = &p->object;
    m->va = start;
    m->flags = flags;
    m->bytes = end - start;
    m->owner = owner;
    m->span.start = start;
    m->span.length = end - start;
    span_insert(&owner->shared_mappings, &m->span);
    link_add(&p->mappings, &m->in_object, m);
    spin_give(&p->lock);
    spin_give(&owner->mappings_lock);
    spin_unlock(&owner->space.lock, lock);
    return 0;
}
/* Pages a first touch of a program segment reads together: the touched one and the others of its
   aligned 64 KiB that no process has read, in one read from the document. */
#define PROGRAM_AROUND 16
int program_fill(struct shared_pages *p, struct handle *source, size_t index) {
    size_t first = ALIGN_DOWN(index, (size_t)PROGRAM_AROUND), last = MIN(first + PROGRAM_AROUND, p->count);
    if (!frames_free(4096 + (last - first))) first = index, last = index + 1; /* memory is short: what is touched only */
    while (first < index && demand_frame(p, first)) first++;
    while (last > index + 1 && demand_frame(p, last - 1)) last--;
    size_t bytes = (last - first) * PAGE_SIZE;
    uint8_t *buf = kzalloc(bytes);
    if (!buf) return -N_ERR_NO_MEMORY;
    /* the window's part of [source_skip, source_skip + source_bytes) */
    uint64_t lo = MAX((uint64_t)first * PAGE_SIZE, (uint64_t)p->source_skip),
             hi = MIN((uint64_t)last * PAGE_SIZE, p->source_skip + p->source_bytes);
    int r = 0;
    if (lo < hi) {
        int64_t got = store_document_read(source, p->source_offset + (lo - p->source_skip), buf + (lo - first * PAGE_SIZE), hi - lo, false);
        r = got < 0 ? (int)got : (uint64_t)got != hi - lo ? -N_ERR_IO : 0;
    }
    for (size_t i = first; r >= 0 && i < last; i++) {
        if (demand_frame(p, i)) continue;
        uint64_t frame = pmm_alloc();
        if (!frame) {
            if (i == index) r = -N_ERR_NO_MEMORY;
            continue;
        }
        memcpy(phys_to_virt(frame), buf + (i - first) * PAGE_SIZE, PAGE_SIZE);
        if (demand_adopt(p, i, frame) < 0 && i == index) r = -N_ERR_NO_MEMORY;
    }
    kfree(buf);
    return r;
}
