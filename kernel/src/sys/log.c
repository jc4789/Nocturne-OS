/* The kernel's log (sys/log.h): every CPU's ring of records, the writers, and the readers' cursors.

   ---- the rings ----
   A ring is a power of two bytes of records, each a header and its text, a multiple of
   RECORD_ALIGN bytes, so that what is left before the ring's end always holds a header: a record
   that would not fit there goes to the ring's start, after a pad record that fills the rest. head
   is the bytes ever reserved, tail where the oldest whole record starts; a record's place is its
   offset in that stream (in the ring: the place modulo the size).

   A writer, interrupts off (so that it stays on its CPU and only an NMI comes in between),
   reserves its room by moving head with a compare-exchange, moves tail past the oldest records the
   room overlaps (a compare-exchange each), writes the header and the text, and seals the record
   last: seal is its place + 1, stored with release. An NMI that writes meanwhile reserves after it
   and moves tail the same way. Two records in flight take at most 4 * LOG_RECORD_MAX bytes (each
   with its pad), which no ring is smaller than, so tail never reaches a record still being written.

   A reader takes a record when its seal is its place + 1 (a record of an earlier lap at the same
   offset has another), copies it, and looks at tail again: tail passes a record before any of its
   bytes change, so a copy made while tail was not past it is whole. Otherwise the record was
   overwritten meanwhile: the reader goes on from tail and counts the bytes it missed.

   ---- the order ----
   A cursor reads in rounds: it looks at the rings written since its last round once (where each
   ring's head is, whether its next record is whole) and takes the records the round found, the
   soonest first (a heap of the rings by the ns of their next record), up to where each head was.
   The rings' records come out in the order of their time but for the records written while the
   round was taken, which the next round puts after it. A new cursor's first round, and every
   round of the panic's, looks at every ring.

   ---- telling the readers ----
   A writer sets its ring's bit in `written` (a bit a ring, set only when it is clear) and, when no
   drain is owed yet, owes one and wakes the "log" thread: directly when it holds no spinlock, else
   by an interrupt to its own CPU (told_isr), which comes once its interrupts are on again and
   wakes the thread there (the writer may hold a wait queue's lock, a run queue's, any). The
   thread takes the written bits, hands them to every cursor (each cursor's `pending`), and then,
   outside any interrupt: the serial port is filled (dev/serial.c), the readers waiting in
   log_wait are woken, the system log stream's watchers told. The work of a round grows with the
   rings written and the readers, not with the CPUs, and none of it is in an interrupt. Before
   log_start there is one CPU, interrupts off and no thread: the boot screen is drawn by the writer
   (fbcon_boot_draw), and the serial port has not started. */
#include "kernel.h"
#include "arch/apic.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/smp.h"
#include "arch/topology.h"
#include "dev/fbcon.h"
#include "dev/serial.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "sys/clock.h"
#include "sys/log.h"
#include "sys/thread.h"
#include "sys/wait.h"
#include "sys/stream.h"
#include "arch/spin.h"

struct log_record {
    uint64_t seal;   /* its place + 1, stored last: the record is whole */
    uint64_t ns;     /* clock_now_ns as it was written */
    uint32_t size;   /* the bytes it takes: header, text, padding */
    uint32_t length; /* text bytes */
    int32_t task;    /* the writer's task ID, 0 for none */
    uint8_t level;   /* enum log_level */
    uint8_t kind;    /* RECORD_* */
    uint16_t unused;
    char text[];
};
#define RECORD_ALIGN 32u
_Static_assert(sizeof(struct log_record) == RECORD_ALIGN, "a record's header is its alignment");
/* The largest record: the text, the formatter's NUL after it, the header, rounded up. */
#define LOG_RECORD_MAX ALIGN_UP(sizeof(struct log_record) + LOG_TEXT_MAX + 1, RECORD_ALIGN)
enum { RECORD_LINE, RECORD_CONSOLE, RECORD_PAD };

struct log_ring {
    uint64_t head;  /* bytes ever reserved */
    uint64_t tail;  /* the oldest whole record's place */
    uint64_t size;  /* a power of two */
    uint8_t *data;
} __attribute__((aligned(64)));

/* The boot's ring: every record written before log_init (CPU 0's, before the clock runs), read as
   ring 0 for good. What the boot says before then is a few hundred lines (CPUID's leaves, the ACPI
   tables, the topology, the memory) whose count does not grow with the CPUs: 32 KiB holds them,
   and more overwrites the oldest, which the readers then say. */
#define BOOT_RING (32u * 1024)
_Static_assert(BOOT_RING >= 4 * LOG_RECORD_MAX, "two records in flight fit the boot ring");
static uint8_t boot_data[BOOT_RING] __attribute__((aligned(RECORD_ALIGN)));
static struct log_ring boot_ring = {.size = BOOT_RING, .data = boot_data};
static struct log_ring *boot_list[1] = {&boot_ring};

/* The rings: 0 the boot's, 1 + i CPU i's. ring_count is stored after rings, with release. */
static struct log_ring **rings = boot_list;
static unsigned ring_count = 1;

/* What the log keeps: 1/LOG_RAM_SHARE of the memory, shared by the CPUs, each ring a power of two
   of at least LOG_RING_MIN bytes. 32 GiB and 12 CPUs give each 1 MiB; 1 TiB and 1024 CPUs 512 KiB. */
#define LOG_RAM_SHARE 2048u
#define LOG_RING_MIN (64u * 1024)
_Static_assert(LOG_RING_MIN >= 4 * LOG_RECORD_MAX, "two records in flight fit a ring");

static bool telling_on; /* writers tell the drain: from log_start, but in a sleep */
static uint64_t told;   /* moved on each time the readers are told */
static struct wait_queue told_wq;

/* The rings written since the drain last took them, a bit a ring (log_init: a word per 64). */
static uint64_t *written;
static unsigned written_words;
static uint64_t drain_owed; /* a writer (or the serial port) asked for a drain */
static struct wait_queue drain_wq;
/* The cursors the drain hands the written bits to (all but the panic's). A leaf. */
static struct spinlock cursors_lock;
static struct log_cursor *cursors;

/* ---- writing ---- */

static inline struct log_record *record_at(const struct log_ring *r, uint64_t place) {
    return (struct log_record *)(r->data + (place & (r->size - 1)));
}

static struct log_ring **ring_list(unsigned *count) {
    *count = __atomic_load_n(&ring_count, __ATOMIC_ACQUIRE);
    return __atomic_load_n(&rings, __ATOMIC_RELAXED);
}

/* Interrupts off: the ring this CPU writes, and its index. */
static struct log_ring *own_ring(unsigned *index) {
    unsigned count;
    struct log_ring **list = ring_list(&count);
    *index = count > 1 ? 1 + cpu_current_index() : 0;
    return list[*index];
}

static int32_t own_task(void) {
    if (__atomic_load_n(&ring_count, __ATOMIC_RELAXED) <= 1) return 0; /* the boot: no task yet */
    struct thread *t = thread_current();
    return t ? t->tid : 0;
}

/* The room up to end is the writer's, its record from start: tail passes the oldest records the
   room overlaps. A size that is not a record's (only memory overwritten makes one) sends tail to
   start: what was before the writer's record is lost. */
static void make_room(struct log_ring *r, uint64_t start, uint64_t end) {
    uint64_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    while (end - tail > r->size) {
        uint32_t size = record_at(r, tail)->size;
        uint64_t next = size >= RECORD_ALIGN && size % RECORD_ALIGN == 0 && size <= r->size ? tail + size : start;
        if (__atomic_compare_exchange_n(&r->tail, &tail, next, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            tail = next;
    }
}

/* Interrupts off: room for a record of length text bytes, its size written, the rest the caller's
   until it seals it. */
static struct log_record *reserve(struct log_ring *r, size_t length, uint64_t *place) {
    uint32_t size = (uint32_t)ALIGN_UP(sizeof(struct log_record) + length + 1, RECORD_ALIGN);
    uint64_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED), pad, end;
    do {
        uint64_t room = r->size - (head & (r->size - 1));
        pad = room < size ? room : 0;
        end = head + pad + size;
    } while (!__atomic_compare_exchange_n(&r->head, &head, end, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    make_room(r, head + pad, end);
    __atomic_thread_fence(__ATOMIC_RELEASE); /* tail is past the room before a byte of it changes */
    if (pad) {
        struct log_record *p = record_at(r, head);
        p->ns = 0; /* the soonest: a reader passes it at once */
        p->size = (uint32_t)pad;
        p->length = 0;
        p->kind = RECORD_PAD;
        __atomic_store_n(&p->seal, head + 1, __ATOMIC_RELEASE);
    }
    *place = head + pad;
    struct log_record *rec = record_at(r, *place);
    rec->size = size;
    return rec;
}

static void seal(struct log_record *rec, uint64_t place, uint64_t ns, uint8_t level, uint8_t kind, size_t length) {
    rec->ns = ns;
    rec->task = own_task();
    rec->level = level;
    rec->kind = kind;
    rec->length = (uint32_t)length;
    __atomic_store_n(&rec->seal, place + 1, __ATOMIC_RELEASE);
}

/* A drain owed: the "log" thread woken, at once when this CPU holds no spinlock and is not in an
   NMI, else by the interrupt to itself. */
static void drain_wake(void) {
    if (!in_nmi() && spin_none_here()) wq_wake_all(&drain_wq);
    else apic_send_ipi(cpu_current_index(), VEC_LOG);
}

void log_kick(void) {
    if (!__atomic_exchange_n(&drain_owed, 1, __ATOMIC_SEQ_CST)) drain_wake();
}

/* Interrupts off, on ring's CPU, a record sealed: the drain learns of it. False before log_start,
   when the caller draws the boot screen itself, and in a sleep (log_suspend to log_resume, which
   drains then). The fence orders the seal before the looks at the bit and at drain_owed; the drain
   clears drain_owed before it takes the bits, and takes a bit before a reader reads the ring:
   either the drain owed finds the record, or this owes another. */
static bool tell(unsigned ring) {
    if (!__atomic_load_n(&telling_on, __ATOMIC_ACQUIRE)) return false;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint64_t *word = &written[ring / 64], bit = 1ull << (ring % 64);
    if (!(__atomic_load_n(word, __ATOMIC_RELAXED) & bit)) __atomic_fetch_or(word, bit, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&drain_owed, __ATOMIC_RELAXED) && !__atomic_exchange_n(&drain_owed, 1, __ATOMIC_SEQ_CST))
        drain_wake();
    return true;
}

int kvlog(enum log_level level, const char *fmt, va_list ap) {
    va_list again;
    va_copy(again, ap);
    uint64_t flags = irq_save();
    uint64_t ns = clock_now_ns();
    int whole = kvsnprintf(NULL, 0, fmt, ap); /* measured, then formatted in place */
    size_t length = MIN((size_t)MAX(whole, 0), (size_t)LOG_TEXT_MAX), place;
    unsigned index;
    struct log_ring *r = own_ring(&index);
    struct log_record *rec = reserve(r, length, &place);
    int put = kvsnprintf(rec->text, length + 1, fmt, again);
    seal(rec, place, ns, (uint8_t)level, RECORD_LINE, MIN((size_t)MAX(put, 0), length));
    bool telling = tell(index);
    irq_restore(flags);
    va_end(again);
    if (!telling) fbcon_boot_draw();
    return (int)length;
}

void klog(enum log_level level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvlog(level, fmt, ap);
    va_end(ap);
}

int kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = kvlog(LOG_INFO, fmt, ap);
    va_end(ap);
    return n;
}

static bool continuation(char c) { return ((uint8_t)c & 0xC0) == 0x80; }

/* s's n bytes as records of kind, each at most LOG_TEXT_MAX: cut after the piece's last newline
   when more follows, else not inside a UTF-8 sequence (at most its 3 continuation bytes back). */
static void put_text(uint8_t level, uint8_t kind, const char *s, size_t n) {
    while (n) {
        size_t piece = MIN(n, (size_t)LOG_TEXT_MAX);
        if (piece < n) {
            size_t cut = piece;
            while (cut && s[cut - 1] != '\n') cut--;
            if (!cut) {
                cut = piece;
                for (unsigned back = 0; back < 3 && cut > 1 && continuation(s[cut]); back++) cut--;
            }
            piece = cut;
        }
        uint64_t flags = irq_save(), ns = clock_now_ns(), place;
        unsigned index;
        struct log_ring *r = own_ring(&index);
        struct log_record *rec = reserve(r, piece, &place);
        memcpy(rec->text, s, piece);
        seal(rec, place, ns, level, kind, piece);
        bool telling = tell(index);
        irq_restore(flags);
        if (!telling) fbcon_boot_draw();
        s += piece;
        n -= piece;
    }
}

void log_text(enum log_level level, const char *s, size_t n) { put_text((uint8_t)level, RECORD_LINE, s, n); }
void log_console(const char *s, size_t n) { put_text(LOG_INFO, RECORD_CONSOLE, s, n); }

/* ---- the drain's interrupt ---- */

static enum irq_result told_isr(void *context) {
    (void)context;
    wq_wake_all(&drain_wq);
    return IRQ_HANDLED;
}

uint64_t log_told(void) { return __atomic_load_n(&told, __ATOMIC_ACQUIRE); }

bool log_wait(uint64_t seen, uint64_t deadline) { return wq_wait_unless(&told_wq, &told, seen, deadline); }

/* ---- cursors ---- */

struct log_cursor {
    unsigned flags, rings; /* rings: the places it has room for */
    struct log_cursor *next; /* in cursors, the drain's list */
    uint64_t *pending;     /* [written_words] the rings written since its last round (the drain's bits);
                              NULL for the panic's, whose every round looks at every ring */
    bool whole;            /* its next round looks at every ring */
    uint64_t *at;          /* [rings] the place of each ring's next record to read */
    uint64_t *end;         /* [rings] this round: where the ring's head was when it was looked at */
    uint64_t *key;         /* [rings] this round: the ns of the ring's next record */
    unsigned *heap;        /* [rings] this round's rings with a whole record next, the soonest first */
    unsigned count;        /* in heap */
    uint64_t lost;         /* bytes overwritten before it got to them, not said yet */
    /* the record being given out */
    bool have, open, cr;   /* open: a line is not ended yet; cr: its '\r' is out (CRLF) */
    uint64_t open_id;      /* whose line is open */
    uint64_t ns;
    unsigned ring;
    int32_t task;
    uint8_t level, kind;
    uint32_t length, done;
    char stage[128];       /* a prefix, a line's end or a note, given out before what follows */
    uint32_t stage_len, stage_at;
    char text[LOG_TEXT_MAX];
};

#define CURSOR_ARRAY_BYTES (3 * sizeof(uint64_t) + sizeof(unsigned))

/* Before the oldest record each ring holds now: what was overwritten before is not this reader's
   loss. Its first round looks at every ring. */
static void cursor_setup(struct log_cursor *c, unsigned flags, unsigned n, uint64_t *arrays) {
    struct log_cursor *next = c->next;
    uint64_t *pending = c->pending;
    memset(c, 0, offsetof(struct log_cursor, text));
    memset(arrays, 0, n * CURSOR_ARRAY_BYTES);
    c->next = next;
    c->pending = pending;
    c->whole = true;
    c->flags = flags;
    c->rings = n;
    c->at = arrays;
    c->end = arrays + n;
    c->key = arrays + 2 * (size_t)n;
    c->heap = (unsigned *)(arrays + 3 * (size_t)n);
    unsigned count;
    struct log_ring **list = ring_list(&count);
    for (unsigned i = 0; i < MIN(count, n); i++) c->at[i] = __atomic_load_n(&list[i]->tail, __ATOMIC_ACQUIRE);
}

/* The panic's cursor: the boot ring's own until log_init makes one for every ring. */
static uint64_t boot_cursor_arrays[(CURSOR_ARRAY_BYTES + sizeof(uint64_t) - 1) / sizeof(uint64_t)];
static struct log_cursor boot_cursor;
static struct log_cursor *panic_cursor;

/* words: the written bits it is handed (0 for the panic's, which looks at every ring). */
static struct log_cursor *cursor_make(unsigned flags, unsigned words) {
    unsigned n = __atomic_load_n(&ring_count, __ATOMIC_ACQUIRE);
    struct log_cursor *c = kzalloc(sizeof *c + n * CURSOR_ARRAY_BYTES + words * sizeof(uint64_t));
    if (!c) return NULL;
    if (words) c->pending = (uint64_t *)((uint8_t *)(c + 1) + n * CURSOR_ARRAY_BYTES);
    cursor_setup(c, flags, n, (uint64_t *)(c + 1));
    if (c->pending) {
        uint64_t f = spin_lock(&cursors_lock);
        c->next = cursors;
        cursors = c;
        spin_unlock(&cursors_lock, f);
    }
    return c;
}

struct log_cursor *log_cursor_new(unsigned flags) { return cursor_make(flags, written_words); }

void log_cursor_free(struct log_cursor *c) {
    if (c && c->pending) {
        uint64_t f = spin_lock(&cursors_lock);
        struct log_cursor **at = &cursors;
        while (*at != c) at = &(*at)->next;
        *at = c->next;
        spin_unlock(&cursors_lock, f);
    }
    kfree(c);
}

void log_cursor_rewind(struct log_cursor *c) { cursor_setup(c, c->flags, c->rings, c->at); }

struct log_cursor *log_panic_cursor(void) {
    struct log_cursor *c = __atomic_load_n(&panic_cursor, __ATOMIC_ACQUIRE);
    if (c) return c;
    if (!boot_cursor.at) cursor_setup(&boot_cursor, 0, 1, boot_cursor_arrays);
    return &boot_cursor;
}

static bool sooner(const struct log_cursor *c, unsigned a, unsigned b) {
    return c->key[a] != c->key[b] ? c->key[a] < c->key[b] : a < b; /* the boot's ring first */
}

static void sift_down(struct log_cursor *c, unsigned k) {
    for (;;) {
        unsigned least = k, l = 2 * k + 1, r = l + 1;
        if (l < c->count && sooner(c, c->heap[l], c->heap[least])) least = l;
        if (r < c->count && sooner(c, c->heap[r], c->heap[least])) least = r;
        if (least == k) return;
        unsigned t = c->heap[k];
        c->heap[k] = c->heap[least];
        c->heap[least] = t;
        k = least;
    }
}

static void heap_push(struct log_cursor *c, unsigned ring, uint64_t ns) {
    c->key[ring] = ns;
    unsigned k = c->count++;
    c->heap[k] = ring;
    while (k && sooner(c, c->heap[k], c->heap[(k - 1) / 2])) {
        unsigned p = (k - 1) / 2, t = c->heap[k];
        c->heap[k] = c->heap[p];
        c->heap[p] = t;
        k = p;
    }
}

static void heap_pop(struct log_cursor *c) {
    c->heap[0] = c->heap[--c->count];
    sift_down(c, 0);
}

/* Ring i's record at the cursor, when the round reaches it and it is whole: its ns. */
static bool next_whole(const struct log_cursor *c, struct log_ring *const *list, unsigned i, uint64_t *ns) {
    uint64_t place = c->at[i];
    if (place >= c->end[i]) return false;
    const struct log_record *rec = record_at(list[i], place);
    if (__atomic_load_n(&rec->seal, __ATOMIC_ACQUIRE) != place + 1) return false;
    *ns = rec->ns;
    return true;
}

/* Ring i into the round: where its head is, and its next record when that is whole. */
static void scan_ring(struct log_cursor *c, struct log_ring *const *list, unsigned i) {
    c->end[i] = __atomic_load_n(&list[i]->head, __ATOMIC_ACQUIRE);
    uint64_t tail = __atomic_load_n(&list[i]->tail, __ATOMIC_ACQUIRE), ns;
    if (c->at[i] < tail) {
        c->lost += tail - c->at[i];
        c->at[i] = tail;
    }
    if (next_whole(c, list, i, &ns)) heap_push(c, i, ns);
}

/* A new round: the rings written since the last one looked at once (every ring when whole). */
static void scan(struct log_cursor *c) {
    unsigned count;
    struct log_ring **list = ring_list(&count);
    count = MIN(count, c->rings);
    c->count = 0;
    if (!c->pending || c->whole) {
        if (c->pending)
            for (unsigned w = 0; w < written_words; w++) __atomic_store_n(&c->pending[w], 0, __ATOMIC_RELAXED);
        c->whole = false;
        for (unsigned i = 0; i < count; i++) scan_ring(c, list, i);
        return;
    }
    for (unsigned w = 0; w < written_words; w++) {
        if (!__atomic_load_n(&c->pending[w], __ATOMIC_RELAXED)) continue;
        for (uint64_t bits = __atomic_exchange_n(&c->pending[w], 0, __ATOMIC_ACQUIRE); bits; bits &= bits - 1) {
            unsigned i = w * 64 + (unsigned)__builtin_ctzll(bits);
            if (i < count) scan_ring(c, list, i);
        }
    }
}

static bool wanted(const struct log_cursor *c, uint8_t kind) {
    return kind == RECORD_LINE || (kind == RECORD_CONSOLE && (c->flags & LOG_CURSOR_CONSOLE));
}

/* The next record in time order into the cursor: false when none past it is whole. */
static bool take(struct log_cursor *c) {
    unsigned count;
    struct log_ring **list = ring_list(&count);
    for (;;) {
        if (!c->count) {
            scan(c);
            if (!c->count) return false;
        }
        unsigned i = c->heap[0];
        struct log_ring *r = list[i];
        uint64_t place = c->at[i], next;
        const struct log_record *rec = record_at(r, place);
        if (__atomic_load_n(&rec->seal, __ATOMIC_ACQUIRE) != place + 1) { /* overwritten since the look */
            heap_pop(c);
            continue;
        }
        uint32_t size = rec->size, length = rec->length;
        uint8_t kind = rec->kind;
        bool sane = size >= RECORD_ALIGN && size % RECORD_ALIGN == 0 && size <= r->size &&
                    length <= size - sizeof *rec && length <= LOG_TEXT_MAX;
        if (sane && wanted(c, kind)) {
            c->ns = rec->ns;
            c->task = rec->task;
            c->level = rec->level;
            memcpy(c->text, rec->text, length);
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&r->tail, __ATOMIC_RELAXED) > place) { /* overwritten while copied: the next round counts it */
            heap_pop(c);
            continue;
        }
        if (!sane) { /* not a record (memory overwritten): the rest of the round's part of the ring is lost */
            c->lost += c->end[i] - place;
            c->at[i] = c->end[i];
            heap_pop(c);
            continue;
        }
        c->at[i] = place + size;
        if (next_whole(c, list, i, &next)) {
            c->key[i] = next;
            sift_down(c, 0);
        } else {
            heap_pop(c);
        }
        if (!wanted(c, kind)) continue;
        c->have = true;
        c->ring = i;
        c->kind = kind;
        c->length = length;
        c->done = 0;
        return true;
    }
}

static void stage(struct log_cursor *c, const char *s) {
    for (; *s && c->stage_len < sizeof c->stage; s++) {
        if (*s == '\n' && (c->flags & LOG_CURSOR_CRLF) && c->stage_len + 1 < sizeof c->stage)
            c->stage[c->stage_len++] = '\r';
        c->stage[c->stage_len++] = *s;
    }
}

static void stage_reset(struct log_cursor *c) { c->stage_len = c->stage_at = 0; }

/* The open line ends before another's text. */
static void stage_line_end(struct log_cursor *c) {
    stage(c, "\n");
    c->open = false;
}

static void stage_prefix(struct log_cursor *c) {
    static const char *const marks[] = {[LOG_ERROR] = " E", [LOG_WARN] = " W", [LOG_INFO] = "", [LOG_DEBUG] = " D"};
    char who[16], prefix[64];
    if (c->task) ksnprintf(who, sizeof who, "%d", c->task);
    else strlcpy(who, "-", sizeof who);
    ksnprintf(prefix, sizeof prefix, "[%5lu.%06lu %u:%s%s] ", c->ns / NS_PER_S, c->ns % NS_PER_S / NS_PER_US,
              c->ring ? c->ring - 1 : 0, who, c->level < ARRAY_SIZE(marks) ? marks[c->level] : " ?");
    stage(c, prefix);
}

/* Whose text a record is: a line goes on across records of the same writer. */
static uint64_t writer_of(const struct log_cursor *c) {
    if (c->kind == RECORD_CONSOLE) return 1;
    return c->task ? 2 | (uint64_t)(uint32_t)c->task << 8 : 3 | (uint64_t)c->ring << 8;
}

size_t log_read(struct log_cursor *c, char *out, size_t cap) {
    size_t n = 0;
    while (n < cap) {
        if (c->stage_at < c->stage_len) {
            out[n++] = c->stage[c->stage_at++];
            continue;
        }
        stage_reset(c);
        if (c->have) {
            if (c->done == c->length) {
                c->have = false;
                continue;
            }
            char ch = c->text[c->done];
            if (!c->open && c->kind == RECORD_LINE) {
                stage_prefix(c);
                c->open = true;
                continue;
            }
            if (ch == '\n' && (c->flags & LOG_CURSOR_CRLF) && !c->cr) {
                out[n++] = '\r';
                c->cr = true;
                continue;
            }
            out[n++] = ch;
            c->done++;
            c->cr = false;
            c->open = ch != '\n';
            continue;
        }
        if (c->lost) {
            char note[112];
            if (c->open) stage_line_end(c);
            ksnprintf(note, sizeof note, "[log: %lu bytes of records were overwritten before this reader read them]\n",
                      c->lost);
            stage(c, note);
            c->lost = 0;
            continue;
        }
        if (!take(c)) break;
        uint64_t writer = writer_of(c);
        if (c->open && writer != c->open_id) stage_line_end(c);
        c->open_id = writer;
    }
    return n;
}

bool log_pending(const struct log_cursor *c) {
    if (c->stage_at < c->stage_len || c->have || c->lost || c->count) return true;
    if (c->pending && !c->whole) {
        for (unsigned w = 0; w < written_words; w++)
            if (__atomic_load_n(&c->pending[w], __ATOMIC_RELAXED)) return true;
        return false;
    }
    unsigned count;
    struct log_ring **list = ring_list(&count);
    count = MIN(count, c->rings);
    for (unsigned i = 0; i < count; i++) {
        uint64_t place = c->at[i];
        if (place < __atomic_load_n(&list[i]->tail, __ATOMIC_ACQUIRE)) return true;
        if (place < __atomic_load_n(&list[i]->head, __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&record_at(list[i], place)->seal, __ATOMIC_ACQUIRE) == place + 1)
            return true;
    }
    return false;
}

static void reverse(char *s, size_t n) {
    for (size_t a = 0, b = n; a + 1 < b; a++, b--) {
        char t = s[a];
        s[a] = s[b - 1];
        s[b - 1] = t;
    }
}

size_t log_newest(struct log_cursor *c, char *buf, size_t max) {
    if (!max) return 0;
    char piece[256];
    size_t total = 0, got;
    while ((got = log_read(c, piece, sizeof piece)))
        for (size_t k = 0; k < got; k++) buf[total++ % max] = piece[k];
    if (total <= max) return total;
    size_t first = total % max; /* the oldest byte kept: turned to the front */
    reverse(buf, first);
    reverse(buf + first, max - first);
    reverse(buf, max);
    return max;
}

size_t log_capacity(void) {
    unsigned count;
    struct log_ring **list = ring_list(&count);
    size_t bytes = 0;
    for (unsigned i = 0; i < count; i++) bytes += list[i]->size;
    return bytes;
}

/* ---- the start ---- */

void log_init(void) {
    unsigned cpus = topology.cpus;
    uint64_t share = pmm_total_pages() * PAGE_SIZE / LOG_RAM_SHARE / cpus, size = LOG_RING_MIN;
    while (size * 2 <= share) size *= 2;
    struct log_ring **list = kzalloc((cpus + 1) * sizeof *list);
    written_words = (cpus + 1 + 63) / 64;
    if (!(written = kzalloc(written_words * sizeof *written))) panic("log: no memory for the rings' written bits");
    uint64_t header_pages = ALIGN_UP((uint64_t)cpus * sizeof(struct log_ring), PAGE_SIZE) / PAGE_SIZE;
    uint64_t headers = pmm_alloc_contig(header_pages, 64);
    if (!list || !headers) panic("log: no memory for the table of %u rings", cpus);
    struct log_ring *own = phys_to_virt(headers);
    memset(own, 0, header_pages * PAGE_SIZE);
    list[0] = &boot_ring;
    for (unsigned i = 0; i < cpus; i++) {
        uint64_t data = pmm_alloc_run(size / PAGE_SIZE, topology.cpu[i].home); /* the writer's node */
        if (!data) panic("log: no %lu KiB for CPU %u's ring", size >> 10, i);
        own[i].size = size;
        own[i].data = phys_to_virt(data);
        memset(own[i].data, 0, size); /* no seal yet: a stale one could look like its place's */
        list[1 + i] = &own[i];
    }
    __atomic_store_n(&rings, list, __ATOMIC_RELEASE);
    __atomic_store_n(&ring_count, cpus + 1, __ATOMIC_RELEASE);
    __atomic_store_n(&panic_cursor, cursor_make(0, 0), __ATOMIC_RELEASE);
    kprintf("log: a ring of %lu KiB for each of %u CPUs\n", size >> 10, cpus);
    if (boot_ring.tail)
        kprintf("log: the boot's first %lu bytes of records were overwritten before the rings were made\n",
                boot_ring.tail);
}

/* The rings written since the last round, handed to every cursor: true when there were some. */
static bool written_take(void) {
    bool any = false;
    uint64_t f = spin_lock(&cursors_lock);
    for (unsigned w = 0; w < written_words; w++) {
        if (!__atomic_load_n(&written[w], __ATOMIC_RELAXED)) continue;
        uint64_t bits = __atomic_exchange_n(&written[w], 0, __ATOMIC_ACQ_REL);
        if (!bits) continue;
        any = true;
        for (struct log_cursor *c = cursors; c; c = c->next) __atomic_fetch_or(&c->pending[w], bits, __ATOMIC_RELEASE);
    }
    spin_unlock(&cursors_lock, f);
    return any;
}

/* The "log" thread: a round for each drain owed. The serial port may ask to be filled again at a
   time (a port without its interrupt, serial_drain): the wait ends then too. */
static void drain_thread(void *arg) {
    (void)arg;
    uint64_t again = CLOCK_NEVER;
    for (;;) {
        wq_wait_unless(&drain_wq, &drain_owed, 0, again);
        __atomic_exchange_n(&drain_owed, 0, __ATOMIC_SEQ_CST);
        bool fresh = written_take();
        again = serial_drain();
        if (!fresh) continue;
        __atomic_add_fetch(&told, 1, __ATOMIC_SEQ_CST);
        wq_wake_all(&told_wq);
        stream_system_log_told();
    }
}

void log_start(void) {
    if (!irq_request_system(&(struct irq_spec){.name = "log", .handler = told_isr, .node = -1}, VEC_LOG))
        panic("log: VEC_LOG is taken");
    fbcon_start();
    serial_start();
    if (!kthread_create("log", drain_thread, NULL)) panic("log: no memory for the drain's thread");
    __atomic_store_n(&telling_on, true, __ATOMIC_RELEASE);
    log_kick(); /* what was written since the two started, before a writer could tell them */
}

void log_suspend(void) {
    __atomic_store_n(&telling_on, false, __ATOMIC_RELEASE);
    serial_suspend();
}

/* The drain runs once the scheduler does again: it sends what the port had not sent and tells
   the readers of what was written meanwhile (every ring's bit: the writers did not tell). */
void log_resume(void) {
    serial_resume();
    for (unsigned w = 0; w < written_words; w++) __atomic_store_n(&written[w], ~0ull, __ATOMIC_RELAXED);
    __atomic_store_n(&telling_on, true, __ATOMIC_RELEASE);
    __atomic_store_n(&drain_owed, 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&drain_wq);
}
