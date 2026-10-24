/* UVM's OS layer in nvrm, the generic part (uvm_os.h): printing, the module's parameters, bits,
 * red-black trees, time, tasks and kernel threads, locks and waits, deferred work, the heap,
 * NVIDIA's kthread queues (nv-kthread-q.h) and what uvm_common.h leaves to uvm_common.c, which
 * is not vendored (GPLv2-or-later) and is written here anew.
 *
 * uvm_mm.c has pages, DMA and the clients' address spaces; uvm_serve.c the files and requests;
 * uvm_rm.c the RM side. Everything here is ISO C and Nocturne's native ABI.
 */
#include "uvm_common.h"

#include <threads.h>

/* ==========================================================================================
 * Printing
 * ======================================================================================== */

/* A thread's message so far: UVM prints a line in pieces (KERN_CONT), so a line goes to the log
   when its newline comes. */
static _Thread_local char line[512];
static _Thread_local size_t line_length;

static void line_flush(void)
{
    if (!line_length)
        return;
    line[line_length] = '\0';
    nvrm_log("%s", line);
    line_length = 0;
}

int vprintk(const char *fmt, va_list ap)
{
    char text[1024];
    int n = vsnprintf(text, sizeof text, fmt, ap);
    if (n < 0)
        return n;
    size_t length = (size_t)n < sizeof text ? (size_t)n : sizeof text - 1;
    for (size_t i = 0; i < length; i++) {
        if (text[i] == '\n') {
            line_flush();
            continue;
        }
        if (line_length == sizeof line - 1)
            line_flush();
        line[line_length++] = text[i];
    }
    return n;
}

int printk(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vprintk(fmt, ap);
    va_end(ap);
    return n;
}

int scnprintf(char *buf, size_t size, const char *fmt, ...)
{
    if (!size)
        return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    if (n < 0)
        return 0;
    return (size_t)n < size ? n : (int)(size - 1);
}

/* 10 messages per call site every 5 seconds. */
#define RATELIMIT_INTERVAL_MS 5000u
#define RATELIMIT_BURST 10u

bool uvm_os_ratelimit_pass(struct uvm_os_ratelimit *rl)
{
    uint64_t now = nvrm_now_ns() / NSEC_PER_MSEC + 1;   /* never 0: 0 is "not begun" */
    unsigned missed = 0;
    mtx_lock(&rl->lock);
    if (!rl->begin_ms || now - rl->begin_ms >= RATELIMIT_INTERVAL_MS) {
        missed = rl->missed;
        rl->begin_ms = now;
        rl->printed = rl->missed = 0;
    }
    bool pass = rl->printed < RATELIMIT_BURST;
    if (pass)
        rl->printed++;
    else
        rl->missed++;
    mtx_unlock(&rl->lock);
    if (missed)
        printk("nvidia-uvm: %u messages from one place were not printed\n", missed);
    return pass;
}

/* The return addresses up the frame-pointer chain (UVM and the OS layer keep frame pointers)
   while the frames stay inside the thread's stack. */
void dump_stack(void)
{
    uintptr_t low, high;
    if (thread_stack_bounds(&low, &high) < 0) {
        printk("nvidia-uvm: call stack: (the thread's stack is unknown)\n");
        return;
    }
    char text[640];
    size_t length = (size_t)scnprintf(text, sizeof text, "nvidia-uvm: call stack:");
    const uintptr_t *frame = __builtin_frame_address(0);
    for (int depth = 0; depth < 40; depth++) {
        uintptr_t at = (uintptr_t)frame;
        if (at < low || at % sizeof(uintptr_t) || high - at < 2 * sizeof(uintptr_t))
            break;
        if (!frame[1])
            break;
        length += (size_t)scnprintf(text + length, sizeof text - length, " %lx", (unsigned long)frame[1]);
        if (frame[0] <= at)
            break;
        frame = (const uintptr_t *)frame[0];
    }
    printk("%s\n", text);
}

void panic(const char *fmt, ...)
{
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    printk("nvidia-uvm: fatal: %s\n", text);
    dump_stack();
    abort();
}

/* ==========================================================================================
 * Module parameters (UVM_OPTIONS) and what uvm_common.h declares
 * ======================================================================================== */

int uvm_debug_prints = 0;
module_param(uvm_debug_prints, int, S_IRUGO | S_IWUSR);
int uvm_enable_builtin_tests = 0;
module_param(uvm_enable_builtin_tests, int, S_IRUGO);
int uvm_release_asserts = 1;
module_param(uvm_release_asserts, int, S_IRUGO);
int uvm_release_asserts_dump_stack = 0;
module_param(uvm_release_asserts_dump_stack, int, S_IRUGO);
int uvm_release_asserts_set_global_error = 0;
module_param(uvm_release_asserts_set_global_error, int, S_IRUGO);
bool uvm_release_asserts_set_global_error_for_tests = false;

extern const struct uvm_os_param __start_uvm_params[], __stop_uvm_params[];

static bool param_set(const struct uvm_os_param *p, const char *value)
{
    char *end = NULL;
    switch (p->type) {
    case 'i': {
        long v = strtol(value, &end, 0);
        if (*value == '\0' || *end || v < INT_MIN || v > INT_MAX)
            return false;
        *(int *)p->value = (int)v;
        return true;
    }
    case 'u':
    case 'l': {
        if (*value == '-')
            return false;
        unsigned long long v = strtoull(value, &end, 0);
        if (*value == '\0' || *end || (p->type == 'u' && v > UINT_MAX) || v > ULONG_MAX)
            return false;
        if (p->type == 'u')
            *(unsigned *)p->value = (unsigned)v;
        else
            *(unsigned long *)p->value = (unsigned long)v;
        return true;
    }
    case 'b':
        if (!strcmp(value, "1") || !strcmp(value, "y") || !strcmp(value, "Y"))
            *(bool *)p->value = true;
        else if (!strcmp(value, "0") || !strcmp(value, "n") || !strcmp(value, "N"))
            *(bool *)p->value = false;
        else
            return false;
        return true;
    case 's': {
        size_t n = strlen(value) + 1;
        char *copy = malloc(n);   /* kept for nvrm's life, as a module's parameter is */
        if (!copy)
            return false;
        memcpy(copy, value, n);
        *(char **)p->value = copy;
        return true;
    }
    }
    return false;
}

/* UVM_OPTIONS="name=value;name=value" (',' and ' ' separate too). */
void uvm_os_params_init(void)
{
    static const char key[] = "UVM_OPTIONS=";
    const char *options = NULL;
    for (char *const *s = n_settings; s && *s; s++)
        if (!strncmp(*s, key, sizeof key - 1))
            options = *s + sizeof key - 1;
    if (!options)
        return;
    while (*options) {
        size_t n = strcspn(options, ";, ");
        if (n) {
            char item[160];
            if (n < sizeof item) {
                memcpy(item, options, n);
                item[n] = '\0';
                char *value = strchr(item, '=');
                const struct uvm_os_param *p = NULL;
                if (value) {
                    *value++ = '\0';
                    for (p = __start_uvm_params; p < __stop_uvm_params; p++)
                        if (!strcmp(p->name, item))
                            break;
                }
                if (!value || p == __stop_uvm_params)
                    nvrm_log("nvrm: UVM_OPTIONS: \"%.*s\" names no UVM parameter", (int)n, options);
                else if (!param_set(p, value))
                    nvrm_log("nvrm: UVM_OPTIONS: \"%s\" is not a value for %s", value, p->name);
                else
                    nvrm_log("nvrm: UVM parameter %s=%s", p->name, value);
            }
            else {
                nvrm_log("nvrm: UVM_OPTIONS: an option of %zu characters is too long", n);
            }
        }
        options += n;
        if (*options)
            options++;
    }
}

bool uvm_debug_prints_enabled(void)
{
    return uvm_debug_prints != 0;
}

/* Places to stop at in a debugger, nothing more. */
noinline void on_uvm_test_fail(void)
{
    __asm__ volatile("" ::: "memory");
}

noinline void on_uvm_assert(void)
{
    __asm__ volatile("" ::: "memory");
}

void uvm_uuid_string(char *buffer, const NvProcessorUuid *uuid)
{
    static const char hex[] = "0123456789abcdef";
    char *out = buffer;
    for (unsigned i = 0; i < sizeof uuid->uuid; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            *out++ = '-';
        *out++ = hex[uuid->uuid[i] >> 4];
        *out++ = hex[uuid->uuid[i] & 0xF];
    }
    *out = '\0';
}

/* A waiting loop gives the CPU away each turn (except on the interrupt thread, which spins), and
   asks for a warning every 10 seconds it has waited. */
#define SPIN_WARNING_NS (10ULL * NSEC_PER_SEC)

NV_STATUS uvm_spin_loop(uvm_spin_loop_t *spin)
{
    if (in_interrupt())
        cpu_relax();
    else
        thrd_yield();
    NvU64 now = NV_GETTIME();
    if (now - spin->print_time_ns >= SPIN_WARNING_NS) {
        spin->print_time_ns = now;
        return NV_ERR_TIMEOUT_RETRY;
    }
    return NV_OK;
}

/* UVM passes errno numbers of either sign. */
NV_STATUS errno_to_nv_status(int errno_code)
{
    switch (errno_code < 0 ? -errno_code : errno_code) {
    case 0: return NV_OK;
    case E2BIG:
    case EINVAL: return NV_ERR_INVALID_ARGUMENT;
    case EBADF:
    case EBADFD: return NV_ERR_INVALID_OBJECT_HANDLE;
    case EFAULT: return NV_ERR_INVALID_ADDRESS;
    case ENOMEM:
    case ENOSPC: return NV_ERR_NO_MEMORY;
    case ENOENT:
    case ESRCH: return NV_ERR_OBJECT_NOT_FOUND;
    case ENODEV:
    case ENXIO: return NV_ERR_INVALID_DEVICE;
    case EPERM:
    case EACCES: return NV_ERR_INSUFFICIENT_PERMISSIONS;
    case EBUSY:
    case EAGAIN: return NV_ERR_BUSY_RETRY;
    case EEXIST: return NV_ERR_IN_USE;
    case EINTR: return NV_ERR_SIGNAL_PENDING;
    case ETIMEDOUT:
    case ETIME: return NV_ERR_TIMEOUT;
    case ERANGE:
    case EOVERFLOW: return NV_ERR_OUT_OF_RANGE;
    case ENOSYS:
    case ENOTTY:
    case EOPNOTSUPP:
    case ENOTSUPP: return NV_ERR_NOT_SUPPORTED;
    case EIO: return NV_ERR_OPERATING_SYSTEM;
    default: return NV_ERR_GENERIC;
    }
}

/* A positive errno (UVM negates it). */
int nv_status_to_errno(NV_STATUS status)
{
    switch (status) {
    case NV_OK: return 0;
    case NV_ERR_INVALID_ARGUMENT:
    case NV_ERR_INVALID_PARAMETER:
    case NV_ERR_INVALID_REQUEST: return EINVAL;
    case NV_ERR_INVALID_ADDRESS: return EFAULT;
    case NV_ERR_NO_MEMORY: return ENOMEM;
    case NV_ERR_OBJECT_NOT_FOUND: return ENOENT;
    case NV_ERR_INVALID_DEVICE:
    case NV_ERR_GPU_IS_LOST: return ENODEV;
    case NV_ERR_INSUFFICIENT_PERMISSIONS: return EPERM;
    case NV_ERR_BUSY_RETRY:
    case NV_ERR_IN_USE:
    case NV_ERR_STATE_IN_USE: return EBUSY;
    case NV_ERR_SIGNAL_PENDING: return EINTR;
    case NV_ERR_TIMEOUT: return ETIMEDOUT;
    case NV_ERR_OUT_OF_RANGE: return ERANGE;
    case NV_ERR_NOT_SUPPORTED: return EOPNOTSUPP;
    case NV_ERR_INVALID_OBJECT_HANDLE: return EBADF;
    default: return EIO;
    }
}

/* For messages only: the app the thread serves and the thread. */
unsigned uvm_get_stale_process_id(void)
{
    return (unsigned)current->tgid;
}

unsigned uvm_get_stale_thread_id(void)
{
    return (unsigned)thread_id();
}

/* ==========================================================================================
 * Bitmaps
 * ======================================================================================== */

/* The bits of the last word of an nbits-bit map that belong to it. */
static unsigned long last_word_mask(unsigned nbits)
{
    return nbits % BITS_PER_LONG ? (1UL << (nbits % BITS_PER_LONG)) - 1 : ~0UL;
}

unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset)
{
    if (offset >= size)
        return size;
    unsigned long i = offset / BITS_PER_LONG;
    unsigned long word = addr[i] & (~0UL << (offset % BITS_PER_LONG));
    for (;;) {
        if (word) {
            unsigned long bit = i * BITS_PER_LONG + __ffs(word);
            return bit < size ? bit : size;
        }
        if (++i * BITS_PER_LONG >= size)
            return size;
        word = addr[i];
    }
}

unsigned long find_next_zero_bit(const unsigned long *addr, unsigned long size, unsigned long offset)
{
    if (offset >= size)
        return size;
    unsigned long i = offset / BITS_PER_LONG;
    unsigned long word = ~addr[i] & (~0UL << (offset % BITS_PER_LONG));
    for (;;) {
        if (word) {
            unsigned long bit = i * BITS_PER_LONG + __ffs(word);
            return bit < size ? bit : size;
        }
        if (++i * BITS_PER_LONG >= size)
            return size;
        word = ~addr[i];
    }
}

unsigned long find_last_bit(const unsigned long *addr, unsigned long size)
{
    if (!size)
        return 0;
    unsigned long i = (size - 1) / BITS_PER_LONG;
    unsigned long word = addr[i] & last_word_mask((unsigned)(size % BITS_PER_LONG));
    for (;;) {
        if (word)
            return i * BITS_PER_LONG + __fls(word);
        if (!i--)
            return size;
        word = addr[i];
    }
}

void bitmap_zero(unsigned long *dst, unsigned nbits)
{
    memset(dst, 0, BITS_TO_LONGS(nbits) * sizeof(unsigned long));
}

void bitmap_fill(unsigned long *dst, unsigned nbits)
{
    memset(dst, 0xFF, BITS_TO_LONGS(nbits) * sizeof(unsigned long));
}

void bitmap_copy(unsigned long *dst, const unsigned long *src, unsigned nbits)
{
    memmove(dst, src, BITS_TO_LONGS(nbits) * sizeof(unsigned long));
}

bool bitmap_and(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned nbits)
{
    unsigned words = BITS_TO_LONGS(nbits);
    unsigned long any = 0;
    for (unsigned i = 0; i < words; i++) {
        dst[i] = a[i] & b[i];
        any |= i + 1 == words ? dst[i] & last_word_mask(nbits) : dst[i];
    }
    return any != 0;
}

void bitmap_or(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned nbits)
{
    for (unsigned i = 0; i < BITS_TO_LONGS(nbits); i++)
        dst[i] = a[i] | b[i];
}

void bitmap_xor(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned nbits)
{
    for (unsigned i = 0; i < BITS_TO_LONGS(nbits); i++)
        dst[i] = a[i] ^ b[i];
}

bool bitmap_andnot(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned nbits)
{
    unsigned words = BITS_TO_LONGS(nbits);
    unsigned long any = 0;
    for (unsigned i = 0; i < words; i++) {
        dst[i] = a[i] & ~b[i];
        any |= i + 1 == words ? dst[i] & last_word_mask(nbits) : dst[i];
    }
    return any != 0;
}

void bitmap_complement(unsigned long *dst, const unsigned long *src, unsigned nbits)
{
    for (unsigned i = 0; i < BITS_TO_LONGS(nbits); i++)
        dst[i] = ~src[i];
}

bool bitmap_equal(const unsigned long *a, const unsigned long *b, unsigned nbits)
{
    unsigned words = BITS_TO_LONGS(nbits);
    for (unsigned i = 0; i < words; i++) {
        unsigned long differ = a[i] ^ b[i];
        if (i + 1 == words)
            differ &= last_word_mask(nbits);
        if (differ)
            return false;
    }
    return true;
}

bool bitmap_intersects(const unsigned long *a, const unsigned long *b, unsigned nbits)
{
    unsigned words = BITS_TO_LONGS(nbits);
    for (unsigned i = 0; i < words; i++) {
        unsigned long both = a[i] & b[i];
        if (i + 1 == words)
            both &= last_word_mask(nbits);
        if (both)
            return true;
    }
    return false;
}

bool bitmap_subset(const unsigned long *a, const unsigned long *b, unsigned nbits)
{
    unsigned words = BITS_TO_LONGS(nbits);
    for (unsigned i = 0; i < words; i++) {
        unsigned long extra = a[i] & ~b[i];
        if (i + 1 == words)
            extra &= last_word_mask(nbits);
        if (extra)
            return false;
    }
    return true;
}

bool bitmap_empty(const unsigned long *src, unsigned nbits)
{
    return find_first_bit(src, nbits) >= nbits;
}

bool bitmap_full(const unsigned long *src, unsigned nbits)
{
    return find_first_zero_bit(src, nbits) >= nbits;
}

unsigned bitmap_weight(const unsigned long *src, unsigned nbits)
{
    unsigned words = BITS_TO_LONGS(nbits), weight = 0;
    for (unsigned i = 0; i < words; i++)
        weight += (unsigned)hweight_long(i + 1 == words ? src[i] & last_word_mask(nbits) : src[i]);
    return weight;
}

void bitmap_set(unsigned long *map, unsigned start, unsigned len)
{
    while (len) {
        unsigned bit = start % BITS_PER_LONG;
        unsigned n = min(len, (unsigned)BITS_PER_LONG - bit);
        unsigned long mask = (n == BITS_PER_LONG ? ~0UL : ((1UL << n) - 1)) << bit;
        map[start / BITS_PER_LONG] |= mask;
        start += n;
        len -= n;
    }
}

void bitmap_clear(unsigned long *map, unsigned start, unsigned len)
{
    while (len) {
        unsigned bit = start % BITS_PER_LONG;
        unsigned n = min(len, (unsigned)BITS_PER_LONG - bit);
        unsigned long mask = (n == BITS_PER_LONG ? ~0UL : ((1UL << n) - 1)) << bit;
        map[start / BITS_PER_LONG] &= ~mask;
        start += n;
        len -= n;
    }
}

/* The source word i of src for a shift, with the bits past nbits taken as 0. */
static unsigned long shift_source(const unsigned long *src, long i, unsigned words, unsigned nbits)
{
    if (i < 0 || (unsigned long)i >= words)
        return 0;
    return (unsigned)i + 1 == words ? src[i] & last_word_mask(nbits) : src[i];
}

void bitmap_shift_left(unsigned long *dst, const unsigned long *src, unsigned shift, unsigned nbits)
{
    unsigned words = BITS_TO_LONGS(nbits);
    long by_words = shift / BITS_PER_LONG;
    unsigned by_bits = shift % BITS_PER_LONG;
    /* From the top down, so dst may be src. */
    for (long k = (long)words - 1; k >= 0; k--) {
        unsigned long upper = shift_source(src, k - by_words, words, nbits);
        unsigned long lower = shift_source(src, k - by_words - 1, words, nbits);
        dst[k] = by_bits ? (upper << by_bits) | (lower >> (BITS_PER_LONG - by_bits)) : upper;
    }
    if (words)
        dst[words - 1] &= last_word_mask(nbits);
}

void bitmap_shift_right(unsigned long *dst, const unsigned long *src, unsigned shift, unsigned nbits)
{
    unsigned words = BITS_TO_LONGS(nbits);
    long by_words = shift / BITS_PER_LONG;
    unsigned by_bits = shift % BITS_PER_LONG;
    /* From the bottom up, so dst may be src. */
    for (long k = 0; k < (long)words; k++) {
        unsigned long lower = shift_source(src, k + by_words, words, nbits);
        unsigned long upper = shift_source(src, k + by_words + 1, words, nbits);
        dst[k] = by_bits ? (lower >> by_bits) | (upper << (BITS_PER_LONG - by_bits)) : lower;
    }
}

/* ==========================================================================================
 * Red-black trees
 * ======================================================================================== */

static void rb_replace_child(struct rb_root *root, struct rb_node *parent, struct rb_node *old,
                             struct rb_node *node)
{
    if (!parent)
        root->rb_node = node;
    else if (parent->rb_left == old)
        parent->rb_left = node;
    else
        parent->rb_right = node;
}

static void rb_rotate_left(struct rb_root *root, struct rb_node *x)
{
    struct rb_node *y = x->rb_right;
    x->rb_right = y->rb_left;
    if (y->rb_left)
        y->rb_left->rb_parent = x;
    y->rb_parent = x->rb_parent;
    rb_replace_child(root, x->rb_parent, x, y);
    y->rb_left = x;
    x->rb_parent = y;
}

static void rb_rotate_right(struct rb_root *root, struct rb_node *x)
{
    struct rb_node *y = x->rb_left;
    x->rb_left = y->rb_right;
    if (y->rb_right)
        y->rb_right->rb_parent = x;
    y->rb_parent = x->rb_parent;
    rb_replace_child(root, x->rb_parent, x, y);
    y->rb_right = x;
    x->rb_parent = y;
}

static bool rb_is_red(const struct rb_node *node)
{
    return node && node->rb_red;
}

void rb_insert_color(struct rb_node *node, struct rb_root *root)
{
    struct rb_node *parent;
    while ((parent = node->rb_parent) && parent->rb_red) {
        struct rb_node *grandparent = parent->rb_parent;   /* a red node is never the root */
        if (parent == grandparent->rb_left) {
            struct rb_node *uncle = grandparent->rb_right;
            if (rb_is_red(uncle)) {
                uncle->rb_red = parent->rb_red = false;
                grandparent->rb_red = true;
                node = grandparent;
                continue;
            }
            if (node == parent->rb_right) {
                rb_rotate_left(root, parent);
                node = parent;
                parent = node->rb_parent;
            }
            parent->rb_red = false;
            grandparent->rb_red = true;
            rb_rotate_right(root, grandparent);
        }
        else {
            struct rb_node *uncle = grandparent->rb_left;
            if (rb_is_red(uncle)) {
                uncle->rb_red = parent->rb_red = false;
                grandparent->rb_red = true;
                node = grandparent;
                continue;
            }
            if (node == parent->rb_left) {
                rb_rotate_right(root, parent);
                node = parent;
                parent = node->rb_parent;
            }
            parent->rb_red = false;
            grandparent->rb_red = true;
            rb_rotate_left(root, grandparent);
        }
    }
    root->rb_node->rb_red = false;
}

/* After a black node was taken out from under parent: x (possibly NULL) is one black short. */
static void rb_erase_fixup(struct rb_root *root, struct rb_node *x, struct rb_node *parent)
{
    while (x != root->rb_node && !rb_is_red(x)) {
        if (x == parent->rb_left) {
            struct rb_node *w = parent->rb_right;
            if (w->rb_red) {
                w->rb_red = false;
                parent->rb_red = true;
                rb_rotate_left(root, parent);
                w = parent->rb_right;
            }
            if (!rb_is_red(w->rb_left) && !rb_is_red(w->rb_right)) {
                w->rb_red = true;
                x = parent;
                parent = x->rb_parent;
                continue;
            }
            if (!rb_is_red(w->rb_right)) {
                w->rb_left->rb_red = false;
                w->rb_red = true;
                rb_rotate_right(root, w);
                w = parent->rb_right;
            }
            w->rb_red = parent->rb_red;
            parent->rb_red = false;
            w->rb_right->rb_red = false;
            rb_rotate_left(root, parent);
            x = root->rb_node;
            break;
        }
        else {
            struct rb_node *w = parent->rb_left;
            if (w->rb_red) {
                w->rb_red = false;
                parent->rb_red = true;
                rb_rotate_right(root, parent);
                w = parent->rb_left;
            }
            if (!rb_is_red(w->rb_left) && !rb_is_red(w->rb_right)) {
                w->rb_red = true;
                x = parent;
                parent = x->rb_parent;
                continue;
            }
            if (!rb_is_red(w->rb_left)) {
                w->rb_right->rb_red = false;
                w->rb_red = true;
                rb_rotate_left(root, w);
                w = parent->rb_left;
            }
            w->rb_red = parent->rb_red;
            parent->rb_red = false;
            w->rb_left->rb_red = false;
            rb_rotate_right(root, parent);
            x = root->rb_node;
            break;
        }
    }
    if (x)
        x->rb_red = false;
}

void rb_erase(struct rb_node *node, struct rb_root *root)
{
    struct rb_node *child, *parent;
    bool was_red;
    if (!node->rb_left || !node->rb_right) {
        child = node->rb_left ? node->rb_left : node->rb_right;
        parent = node->rb_parent;
        was_red = node->rb_red;
        if (child)
            child->rb_parent = parent;
        rb_replace_child(root, parent, node, child);
    }
    else {
        /* The successor takes the node's place and colour. */
        struct rb_node *next = node->rb_right;
        while (next->rb_left)
            next = next->rb_left;
        child = next->rb_right;
        was_red = next->rb_red;
        if (next->rb_parent == node) {
            parent = next;
        }
        else {
            parent = next->rb_parent;
            parent->rb_left = child;
            if (child)
                child->rb_parent = parent;
            next->rb_right = node->rb_right;
            node->rb_right->rb_parent = next;
        }
        next->rb_left = node->rb_left;
        node->rb_left->rb_parent = next;
        next->rb_parent = node->rb_parent;
        rb_replace_child(root, node->rb_parent, node, next);
        next->rb_red = node->rb_red;
    }
    if (!was_red)
        rb_erase_fixup(root, child, parent);
}

struct rb_node *rb_first(const struct rb_root *root)
{
    struct rb_node *node = root->rb_node;
    if (!node)
        return NULL;
    while (node->rb_left)
        node = node->rb_left;
    return node;
}

struct rb_node *rb_next(const struct rb_node *node)
{
    if (RB_EMPTY_NODE(node))
        return NULL;
    if (node->rb_right) {
        node = node->rb_right;
        while (node->rb_left)
            node = node->rb_left;
        return (struct rb_node *)node;
    }
    struct rb_node *parent;
    while ((parent = node->rb_parent) && node == parent->rb_right)
        node = parent;
    return parent;
}

struct rb_node *rb_prev(const struct rb_node *node)
{
    if (RB_EMPTY_NODE(node))
        return NULL;
    if (node->rb_left) {
        node = node->rb_left;
        while (node->rb_right)
            node = node->rb_right;
        return (struct rb_node *)node;
    }
    struct rb_node *parent;
    while ((parent = node->rb_parent) && node == parent->rb_left)
        node = parent;
    return parent;
}

/* ==========================================================================================
 * The index -> pointer map (a red-black tree of entries)
 * ======================================================================================== */

struct radix_entry {
    struct rb_node node;
    unsigned long index;
    void *item;
};

static struct radix_entry *radix_find(const struct radix_tree_root *root, unsigned long index)
{
    struct rb_node *node = root->tree.rb_node;
    while (node) {
        struct radix_entry *e = rb_entry(node, struct radix_entry, node);
        if (index < e->index)
            node = node->rb_left;
        else if (index > e->index)
            node = node->rb_right;
        else
            return e;
    }
    return NULL;
}

void *radix_tree_lookup(const struct radix_tree_root *root, unsigned long index)
{
    struct radix_entry *e = radix_find(root, index);
    return e ? e->item : NULL;
}

int radix_tree_insert(struct radix_tree_root *root, unsigned long index, void *item)
{
    struct rb_node **link = &root->tree.rb_node, *parent = NULL;
    while (*link) {
        struct radix_entry *e = rb_entry(*link, struct radix_entry, node);
        parent = *link;
        if (index < e->index)
            link = &parent->rb_left;
        else if (index > e->index)
            link = &parent->rb_right;
        else
            return -EEXIST;
    }
    struct radix_entry *e = malloc(sizeof *e);
    if (!e)
        return -ENOMEM;
    e->index = index;
    e->item = item;
    rb_link_node(&e->node, parent, link);
    rb_insert_color(&e->node, &root->tree);
    return 0;
}

void *radix_tree_delete(struct radix_tree_root *root, unsigned long index)
{
    struct radix_entry *e = radix_find(root, index);
    if (!e)
        return NULL;
    void *item = e->item;
    rb_erase(&e->node, &root->tree);
    free(e);
    return item;
}

void **radix_tree_iter_find(const struct radix_tree_root *root, struct radix_tree_iter *iter, unsigned long start)
{
    struct rb_node *node = root->tree.rb_node;
    struct radix_entry *found = NULL;
    while (node) {
        struct radix_entry *e = rb_entry(node, struct radix_entry, node);
        if (e->index >= start) {
            found = e;
            node = node->rb_left;
        }
        else {
            node = node->rb_right;
        }
    }
    if (!found)
        return NULL;
    iter->index = found->index;
    iter->node = &found->node;
    return &found->item;
}

/* ==========================================================================================
 * Time
 * ======================================================================================== */

void msleep(unsigned ms)
{
    n_sleep_ms(ms);
}

void udelay(unsigned long us)
{
    uint64_t end = nvrm_now_ns() + (uint64_t)us * NSEC_PER_USEC;
    while (nvrm_now_ns() < end)
        cpu_relax();
}

/* A sleep of a millisecond or more is the system's; a shorter one gives the CPU away until the
   time passed. */
void usleep_range(unsigned long min_us, unsigned long max_us)
{
    (void)max_us;
    if (min_us >= USEC_PER_MSEC) {
        n_sleep_ms((min_us + USEC_PER_MSEC - 1) / USEC_PER_MSEC);
        return;
    }
    uint64_t end = nvrm_now_ns() + (uint64_t)min_us * NSEC_PER_USEC;
    while (nvrm_now_ns() < end)
        thrd_yield();
}

/* ==========================================================================================
 * Tasks and kernel threads
 * ======================================================================================== */

static _Thread_local struct task_struct *current_task;
/* The tasks made for threads UVM did not start (RM's, the request server's): freed at their
   thread's end. */
static tss_t foreign_task_key;
static once_flag foreign_task_once = ONCE_FLAG_INIT;
static bool foreign_task_key_made;

static void foreign_task_free(void *task)
{
    free(task);
}

static void foreign_task_key_make(void)
{
    foreign_task_key_made = tss_create(&foreign_task_key, foreign_task_free) == thrd_success;
}

static void task_set_stack(struct task_struct *task)
{
    uintptr_t low, high;
    if (thread_stack_bounds(&low, &high) >= 0)
        task->stack = (void *)low;
}

struct task_struct *get_current(void)
{
    struct task_struct *task = current_task;
    if (likely(task))
        return task;
    task = calloc(1, sizeof *task);
    if (!task)
        panic("no memory for a thread's task");
    task->flags = PF_KTHREAD;
    snprintf(task->comm, sizeof task->comm, "nvrm");
    task_set_stack(task);
    call_once(&foreign_task_once, foreign_task_key_make);
    if (foreign_task_key_made)
        tss_set(foreign_task_key, task);
    current_task = task;
    return task;
}

struct task_struct *uvm_os_task_enter(struct task_struct *task)
{
    struct task_struct *previous = get_current();
    if (!task->stack)
        task_set_stack(task);
    current_task = task;
    return previous;
}

void uvm_os_task_leave(struct task_struct *previous)
{
    current_task = previous;
}

static int kthread_main(void *arg)
{
    struct task_struct *task = arg;
    current_task = task;
    task_set_stack(task);
    for (;;) {
        uint32_t state = __atomic_load_n(&task->run_state, __ATOMIC_ACQUIRE);
        if (state != UVM_OS_KTHREAD_CREATED)
            break;
        wait_on_address(&task->run_state, UVM_OS_KTHREAD_CREATED, UINT32_MAX);
    }
    int code = task->run_state == UVM_OS_KTHREAD_RUN ? task->thread_fn(task->thread_data) : -EINTR;
    task->exit_code = code;
    current_task = NULL;
    return code;
}

struct task_struct *kthread_create(int (*fn)(void *), void *data, const char *namefmt, ...)
{
    struct task_struct *task = calloc(1, sizeof *task);
    if (!task)
        return ERR_PTR(-ENOMEM);
    va_list ap;
    va_start(ap, namefmt);
    vsnprintf(task->comm, sizeof task->comm, namefmt, ap);
    va_end(ap);
    task->flags = PF_KTHREAD;
    task->thread_fn = fn;
    task->thread_data = data;
    int made = thrd_create(&task->thread, kthread_main, task);
    if (made != thrd_success) {
        printk("nvidia-uvm: thread \"%s\" not started (%s)\n", task->comm,
               made == thrd_nomem ? "no memory" : "nvrm has as many threads as it may");
        free(task);
        return ERR_PTR(made == thrd_nomem ? -ENOMEM : -EAGAIN);
    }
    return task;
}

void wake_up_process(struct task_struct *task)
{
    uint32_t created = UVM_OS_KTHREAD_CREATED;
    if (task->thread_fn &&
        __atomic_compare_exchange_n(&task->run_state, &created, UVM_OS_KTHREAD_RUN, false, __ATOMIC_RELEASE,
                                    __ATOMIC_RELAXED))
        wake_address(&task->run_state, 0);
}

int kthread_stop(struct task_struct *task)
{
    atomic_set(&task->should_stop, 1);
    uint32_t created = UVM_OS_KTHREAD_CREATED;
    if (__atomic_compare_exchange_n(&task->run_state, &created, UVM_OS_KTHREAD_CANCELLED, false, __ATOMIC_RELEASE,
                                    __ATOMIC_RELAXED))
        wake_address(&task->run_state, 0);
    int result;
    thrd_join(task->thread, &result);
    int code = task->exit_code;
    free(task);
    return code;
}

bool kthread_should_stop(void)
{
    return atomic_read(&current->should_stop) != 0;
}

/* ==========================================================================================
 * CPUs and the memory node
 * ======================================================================================== */

/* Enough CPUs for every cpu_index the system gives. */
unsigned uvm_os_cpu_count(void)
{
    static unsigned count;
    unsigned n = __atomic_load_n(&count, __ATOMIC_RELAXED);
    if (n)
        return n;
    struct n_cpuinfo info;
    n = cpu_info(&info) >= 0 ? max(info.detected_cpus, info.online_cpus) : 0;
    n = clamp(n, 1u, (unsigned)NR_CPUS);
    __atomic_store_n(&count, n, __ATOMIC_RELAXED);
    return n;
}

static struct cpumask every_cpu;
static once_flag every_cpu_once = ONCE_FLAG_INIT;

static void every_cpu_fill(void)
{
    bitmap_set(every_cpu.bits, 0, uvm_os_cpu_count());
}

const struct cpumask *cpumask_of_node(int node)
{
    static const struct cpumask none;
    if (node != 0)
        return &none;
    call_once(&every_cpu_once, every_cpu_fill);
    return &every_cpu;
}

nodemask_t node_possible_map = { { 1UL } };
nodemask_t node_online_map = { { 1UL } };
nodemask_t node_states[NR_NODE_STATES] = { [0 ... NR_NODE_STATES - 1] = { { 1UL } } };

/* ==========================================================================================
 * Readers-writer semaphores
 * ======================================================================================== */

void init_rwsem(struct rw_semaphore *sem)
{
    memset(sem, 0, sizeof *sem);
}

void down_read(struct rw_semaphore *sem)
{
    mtx_lock(&sem->lock);
    while (sem->writer || sem->writers_waiting)
        cnd_wait(&sem->readers_go, &sem->lock);
    sem->readers++;
    mtx_unlock(&sem->lock);
}

int down_read_trylock(struct rw_semaphore *sem)
{
    mtx_lock(&sem->lock);
    bool taken = !sem->writer && !sem->writers_waiting;
    if (taken)
        sem->readers++;
    mtx_unlock(&sem->lock);
    return taken;
}

void up_read(struct rw_semaphore *sem)
{
    mtx_lock(&sem->lock);
    if (--sem->readers == 0 && sem->writers_waiting)
        cnd_signal(&sem->writers_go);
    mtx_unlock(&sem->lock);
}

void down_write(struct rw_semaphore *sem)
{
    mtx_lock(&sem->lock);
    sem->writers_waiting++;
    while (sem->writer || sem->readers)
        cnd_wait(&sem->writers_go, &sem->lock);
    sem->writers_waiting--;
    sem->writer = true;
    mtx_unlock(&sem->lock);
}

int down_write_trylock(struct rw_semaphore *sem)
{
    mtx_lock(&sem->lock);
    bool taken = !sem->writer && !sem->readers;
    if (taken)
        sem->writer = true;
    mtx_unlock(&sem->lock);
    return taken;
}

void up_write(struct rw_semaphore *sem)
{
    mtx_lock(&sem->lock);
    sem->writer = false;
    if (sem->writers_waiting)
        cnd_signal(&sem->writers_go);
    else
        cnd_broadcast(&sem->readers_go);
    mtx_unlock(&sem->lock);
}

/* The writer becomes a reader; readers that wait are let in only when no writer waits. */
void downgrade_write(struct rw_semaphore *sem)
{
    mtx_lock(&sem->lock);
    sem->writer = false;
    sem->readers++;
    if (!sem->writers_waiting)
        cnd_broadcast(&sem->readers_go);
    mtx_unlock(&sem->lock);
}

/* ==========================================================================================
 * Wait queues and bit locks
 * ======================================================================================== */

void wake_up_all(wait_queue_head_t *wq)
{
    __atomic_add_fetch(&wq->sequence, 1, __ATOMIC_RELEASE);
    wake_address(&wq->sequence, 0);
}

void uvm_os_wait_queue_sleep(wait_queue_head_t *wq, uint32_t seen, unsigned timeout_ms)
{
    wait_on_address(&wq->sequence, seen, timeout_ms);
}

/* The 32 bits of the word that hold the bit (x86-64 is little-endian). */
static volatile uint32_t *bit_half(void *word, int bit)
{
    return (volatile uint32_t *)word + bit / 32;
}

int wait_on_bit_lock(unsigned long *word, int bit, unsigned mode)
{
    (void)mode;
    while (test_and_set_bit_lock(bit, word)) {
        volatile uint32_t *half = bit_half(word, bit);
        uint32_t seen = __atomic_load_n(half, __ATOMIC_ACQUIRE);
        if (seen & (1u << (bit % 32)))
            wait_on_address(half, seen, UINT32_MAX);
    }
    return 0;
}

void wake_up_bit(void *word, int bit)
{
    wake_address(bit_half(word, bit), 0);
}

/* ==========================================================================================
 * Deferred work: one thread runs the delayed work items in the order they are due.
 * ======================================================================================== */

static mtx_t work_lock;
static LIST_HEAD(work_queue);              /* by due_ms */
static struct work_struct *work_running;
static volatile uint32_t work_changes;     /* the thread sleeps on it */
static volatile uint32_t work_finished;    /* bumped as an item's run ends */
static thrd_t work_thread;
static bool work_thread_up, work_stopping;

static uint64_t now_ms(void)
{
    return nvrm_now_ns() / NSEC_PER_MSEC;
}

static void work_changed(void)
{
    __atomic_add_fetch(&work_changes, 1, __ATOMIC_RELEASE);
    wake_address(&work_changes, 0);
}

static int work_main(void *arg)
{
    (void)arg;
    mtx_lock(&work_lock);
    while (!work_stopping) {
        uint32_t seen = work_changes;
        unsigned wait_ms = UINT32_MAX;
        if (!list_empty(&work_queue)) {
            struct delayed_work *first = to_delayed_work(list_first_entry(&work_queue, struct work_struct, entry));
            uint64_t now = now_ms();
            if (first->due_ms <= now) {
                struct work_struct *work = &first->work;
                list_del_init(&work->entry);
                work->pending = false;
                work_running = work;
                mtx_unlock(&work_lock);
                work->func(work);
                mtx_lock(&work_lock);
                work_running = NULL;
                __atomic_add_fetch(&work_finished, 1, __ATOMIC_RELEASE);
                wake_address(&work_finished, 0);
                continue;
            }
            wait_ms = (unsigned)min(first->due_ms - now, (uint64_t)UINT32_MAX - 1);
        }
        mtx_unlock(&work_lock);
        wait_on_address(&work_changes, seen, wait_ms);
        mtx_lock(&work_lock);
    }
    mtx_unlock(&work_lock);
    return 0;
}

bool schedule_delayed_work(struct delayed_work *dwork, unsigned long delay)
{
    struct work_struct *work = &dwork->work;
    mtx_lock(&work_lock);
    if (work->pending) {
        mtx_unlock(&work_lock);
        return false;
    }
    if (!work_thread_up) {
        if (thrd_create(&work_thread, work_main, NULL) != thrd_success) {
            mtx_unlock(&work_lock);
            printk("nvidia-uvm: deferred work not run: its thread cannot be started\n");
            return false;
        }
        work_thread_up = true;
    }
    dwork->due_ms = now_ms() + delay;
    struct work_struct *pos;
    list_for_each_entry(pos, &work_queue, entry)
        if (to_delayed_work(pos)->due_ms > dwork->due_ms)
            break;
    list_add_tail(&work->entry, &pos->entry);
    work->pending = true;
    work_changed();
    mtx_unlock(&work_lock);
    return true;
}

static bool work_cancel(struct work_struct *work, bool sync)
{
    mtx_lock(&work_lock);
    bool was_pending = work->pending;
    if (was_pending) {
        list_del_init(&work->entry);
        work->pending = false;
    }
    while (sync && work_running == work) {
        uint32_t seen = work_finished;
        mtx_unlock(&work_lock);
        wait_on_address(&work_finished, seen, UINT32_MAX);
        mtx_lock(&work_lock);
    }
    mtx_unlock(&work_lock);
    return was_pending;
}

bool cancel_delayed_work(struct delayed_work *dwork)
{
    return work_cancel(&dwork->work, false);
}

bool cancel_delayed_work_sync(struct delayed_work *dwork)
{
    return work_cancel(&dwork->work, true);
}

void uvm_os_stop(void)
{
    mtx_lock(&work_lock);
    if (!work_thread_up) {
        mtx_unlock(&work_lock);
        return;
    }
    if (!list_empty(&work_queue))
        printk("nvidia-uvm: deferred work still queued at UVM's stop is dropped\n");
    work_stopping = true;
    work_changed();
    mtx_unlock(&work_lock);
    thrd_join(work_thread, NULL);
    mtx_lock(&work_lock);
    INIT_LIST_HEAD(&work_queue);
    work_thread_up = work_stopping = false;
    mtx_unlock(&work_lock);
}

/* ==========================================================================================
 * The heap
 * ======================================================================================== */

/* A kmalloc block: its size before it, keeping malloc's 16-byte alignment. */
#define KMALLOC_MAGIC 0x6B6D616C6C6F6321ULL
struct kmalloc_header {
    size_t size;
    uint64_t magic;
};

static struct kmalloc_header *kmalloc_header(const void *p)
{
    struct kmalloc_header *h = (struct kmalloc_header *)p - 1;
    if (h->magic != KMALLOC_MAGIC)
        panic("kfree/ksize/krealloc of %p, which kmalloc did not return", p);
    return h;
}

void *kmalloc(size_t size, gfp_t flags)
{
    if (!size)
        return ZERO_SIZE_PTR;
    if (size > SIZE_MAX - sizeof(struct kmalloc_header))
        return NULL;
    struct kmalloc_header *h = (flags & __GFP_ZERO) ? calloc(1, sizeof *h + size) : malloc(sizeof *h + size);
    if (!h)
        return NULL;
    h->size = size;
    h->magic = KMALLOC_MAGIC;
    return h + 1;
}

void kfree(const void *p)
{
    if (ZERO_OR_NULL_PTR(p))
        return;
    struct kmalloc_header *h = kmalloc_header(p);
    h->magic = 0;
    free(h);
}

size_t ksize(const void *p)
{
    return ZERO_OR_NULL_PTR(p) ? 0 : kmalloc_header(p)->size;
}

void *krealloc(const void *p, size_t size, gfp_t flags)
{
    if (ZERO_OR_NULL_PTR(p))
        return kmalloc(size, flags);
    if (!size) {
        kfree(p);
        return ZERO_SIZE_PTR;
    }
    if (size > SIZE_MAX - sizeof(struct kmalloc_header))
        return NULL;
    struct kmalloc_header *h = kmalloc_header(p);
    size_t old = h->size;
    h = realloc(h, sizeof *h + size);
    if (!h)
        return NULL;
    if ((flags & __GFP_ZERO) && size > old)
        memset((char *)(h + 1) + old, 0, size - old);
    h->size = size;
    return h + 1;
}

/* vmalloc blocks by address, so that is_vmalloc_addr finds the block an address is in. */
struct vmalloc_block {
    struct rb_node node;
    size_t size;
    uint64_t pad;          /* the block's memory stays 16-byte aligned after this header */
};
static mtx_t vmalloc_lock;
static struct rb_root vmalloc_blocks;
static volatile uint32_t vmalloc_count;

static uintptr_t vmalloc_start(const struct vmalloc_block *b)
{
    return (uintptr_t)(b + 1);
}

void *vmalloc(unsigned long size)
{
    if (size > SIZE_MAX - sizeof(struct vmalloc_block))
        return NULL;
    struct vmalloc_block *b = malloc(sizeof *b + (size ? size : 1));
    if (!b)
        return NULL;
    b->size = size ? size : 1;
    uintptr_t start = vmalloc_start(b);
    mtx_lock(&vmalloc_lock);
    struct rb_node **link = &vmalloc_blocks.rb_node, *parent = NULL;
    while (*link) {
        parent = *link;
        link = start < vmalloc_start(rb_entry(parent, struct vmalloc_block, node)) ? &parent->rb_left : &parent->rb_right;
    }
    rb_link_node(&b->node, parent, link);
    rb_insert_color(&b->node, &vmalloc_blocks);
    __atomic_add_fetch(&vmalloc_count, 1, __ATOMIC_RELAXED);
    mtx_unlock(&vmalloc_lock);
    return (void *)start;
}

void *vzalloc(unsigned long size)
{
    void *p = vmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

/* The block p is in (vmalloc_lock held). */
static struct vmalloc_block *vmalloc_find(uintptr_t p)
{
    struct rb_node *node = vmalloc_blocks.rb_node;
    struct vmalloc_block *below = NULL;
    while (node) {
        struct vmalloc_block *b = rb_entry(node, struct vmalloc_block, node);
        if (p < vmalloc_start(b)) {
            node = node->rb_left;
        }
        else {
            below = b;
            node = node->rb_right;
        }
    }
    return below && p - vmalloc_start(below) < below->size ? below : NULL;
}

bool is_vmalloc_addr(const void *p)
{
    if (!__atomic_load_n(&vmalloc_count, __ATOMIC_RELAXED))
        return false;
    mtx_lock(&vmalloc_lock);
    bool found = vmalloc_find((uintptr_t)p) != NULL;
    mtx_unlock(&vmalloc_lock);
    return found;
}

void vfree(const void *p)
{
    if (!p)
        return;
    mtx_lock(&vmalloc_lock);
    struct vmalloc_block *b = vmalloc_find((uintptr_t)p);
    if (!b || vmalloc_start(b) != (uintptr_t)p) {
        mtx_unlock(&vmalloc_lock);
        panic("vfree of %p, which vmalloc did not return", p);
    }
    rb_erase(&b->node, &vmalloc_blocks);
    __atomic_sub_fetch(&vmalloc_count, 1, __ATOMIC_RELAXED);
    mtx_unlock(&vmalloc_lock);
    free(b);
}

/* Object caches. Objects aligned no more than malloc's are kmalloc blocks (kfree works on them
   too); more strictly aligned ones come from aligned_alloc. */
struct kmem_cache {
    char name[32];
    size_t size, align;
};

struct kmem_cache *nv_kmem_cache_create(const char *name, unsigned size, unsigned align)
{
    if (align & (align - 1))
        return NULL;
    struct kmem_cache *cache = calloc(1, sizeof *cache);
    if (!cache)
        return NULL;
    snprintf(cache->name, sizeof cache->name, "%s", name);
    cache->size = size ? size : 1;
    cache->align = align;
    return cache;
}

void kmem_cache_destroy(struct kmem_cache *cache)
{
    free(cache);
}

void *kmem_cache_alloc(struct kmem_cache *cache, gfp_t flags)
{
    if (cache->align <= 16)
        return kmalloc(cache->size, flags);
    size_t size = ALIGN(cache->size, cache->align);
    void *object = aligned_alloc(cache->align, size);
    if (object && (flags & __GFP_ZERO))
        memset(object, 0, size);
    return object;
}

void kmem_cache_free(struct kmem_cache *cache, void *object)
{
    if (cache->align <= 16)
        kfree(object);
    else
        free(object);
}

/* ==========================================================================================
 * Odds and ends
 * ======================================================================================== */

/* Bob Jenkins' lookup3 (public domain), its hash of two 32-bit words. */
static uint32_t rotate32(uint32_t x, unsigned k)
{
    return (x << k) | (x >> (32 - k));
}

uint32_t jhash_2words(uint32_t a, uint32_t b, uint32_t initval)
{
    uint32_t c = 0xDEADBEEFu + (2u << 2) + initval;
    a += c;
    b += c;
    c ^= b; c -= rotate32(b, 14);
    a ^= c; a -= rotate32(c, 11);
    b ^= a; b -= rotate32(a, 25);
    c ^= b; c -= rotate32(b, 16);
    a ^= c; a -= rotate32(c, 4);
    b ^= a; b -= rotate32(a, 14);
    c ^= b; c -= rotate32(b, 24);
    return c;
}

void sort(void *base, size_t num, size_t size, int (*cmp)(const void *, const void *),
          void (*swap_fn)(void *, void *, int))
{
    (void)swap_fn;
    qsort(base, num, size, cmp);
}

void get_random_bytes(void *buf, int nbytes)
{
    if (nbytes <= 0)
        return;
    if (n_random(buf, (size_t)nbytes) >= 0)
        return;
    /* The system's pool did not answer: a sequence from the clock (what UVM wants random
       numbers for is spreading work and test data, not secrets). */
    uint64_t x = nvrm_now_ns() ^ (uintptr_t)buf;
    for (int i = 0; i < nbytes; i++) {
        x += 0x9E3779B97F4A7C15ULL;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        ((uint8_t *)buf)[i] = (uint8_t)(z ^ (z >> 31));
    }
}

int seq_printf(struct seq_file *m, const char *fmt, ...)
{
    if (m->count >= m->size)
        return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(m->buf + m->count, m->size - m->count, fmt, ap);
    va_end(ap);
    if (n < 0)
        return n;
    if ((size_t)n >= m->size - m->count) {
        m->count = m->size;   /* overflowed: the caller may grow the buffer and print again */
        return -1;
    }
    m->count += (size_t)n;
    return 0;
}

/* ==========================================================================================
 * NVIDIA's kthread queues (nv-kthread-q.h) on UVM's kernel threads
 * ======================================================================================== */

static int kthread_q_main(void *arg)
{
    nv_kthread_q_t *q = arg;
    for (;;) {
        down(&q->q_sem);
        if (atomic_read(&q->main_loop_should_exit))
            break;
        spin_lock(&q->q_lock);
        nv_kthread_q_item_t *item = list_first_entry_or_null(&q->q_list_head, nv_kthread_q_item_t, q_list_node);
        if (item)
            list_del_init(&item->q_list_node);
        spin_unlock(&q->q_lock);
        /* The item may be freed or scheduled again by its own function. */
        if (item)
            item->function_to_run(item->function_args);
    }
    return 0;
}

int nv_kthread_q_init_on_node(nv_kthread_q_t *q, const char *qname, int preferred_node)
{
    (void)preferred_node;   /* one memory node */
    memset(q, 0, sizeof *q);
    INIT_LIST_HEAD(&q->q_list_head);
    spin_lock_init(&q->q_lock);
    sema_init(&q->q_sem, 0);
    struct task_struct *thread = kthread_create(kthread_q_main, q, "%s", qname);
    if (IS_ERR(thread))
        return (int)PTR_ERR(thread);
    q->q_kthread = thread;
    wake_up_process(thread);
    return 0;
}

int nv_kthread_q_init(nv_kthread_q_t *q, const char *qname)
{
    return nv_kthread_q_init_on_node(q, qname, NV_KTHREAD_NO_NODE);
}

void nv_kthread_q_item_init(nv_kthread_q_item_t *q_item, nv_q_func_t function_to_run, void *function_args)
{
    INIT_LIST_HEAD(&q_item->q_list_node);
    q_item->function_to_run = function_to_run;
    q_item->function_args = function_args;
}

int nv_kthread_q_schedule_q_item(nv_kthread_q_t *q, nv_kthread_q_item_t *q_item)
{
    if (!q->q_kthread || atomic_read(&q->main_loop_should_exit))
        return 0;
    spin_lock(&q->q_lock);
    if (!list_empty(&q_item->q_list_node)) {
        spin_unlock(&q->q_lock);
        return 0;
    }
    list_add_tail(&q_item->q_list_node, &q->q_list_head);
    spin_unlock(&q->q_lock);
    up(&q->q_sem);
    return 1;
}

struct q_flush {
    nv_kthread_q_item_t item;
    volatile uint32_t done;
};

static void q_flush_done(void *arg)
{
    struct q_flush *flush = arg;
    __atomic_store_n(&flush->done, 1, __ATOMIC_RELEASE);
    wake_address(&flush->done, 0);
}

/* Everything queued before it has run once the flush item runs: the queue runs in order. */
static void q_flush_once(nv_kthread_q_t *q)
{
    struct q_flush flush = { .done = 0 };
    nv_kthread_q_item_init(&flush.item, q_flush_done, &flush);
    if (!nv_kthread_q_schedule_q_item(q, &flush.item))
        return;
    while (!__atomic_load_n(&flush.done, __ATOMIC_ACQUIRE))
        wait_on_address(&flush.done, 0, UINT32_MAX);
}

/* Twice: the second waits for what the items of the first scheduled (nv-kthread-q.h). */
void nv_kthread_q_flush(nv_kthread_q_t *q)
{
    q_flush_once(q);
    q_flush_once(q);
}

void nv_kthread_q_stop(nv_kthread_q_t *q)
{
    if (!q->q_kthread)
        return;
    q->is_unload_flush_ongoing = true;
    nv_kthread_q_flush(q);
    q->is_unload_flush_ongoing = false;
    atomic_set(&q->main_loop_should_exit, 1);
    up(&q->q_sem);
    kthread_stop(q->q_kthread);
    q->q_kthread = NULL;
}
