/* 身元と能力の持ち主（sys/authority.h）。 */
#include "kernel.h"
#include "arch/spin.h"
#include "mm/heap.h"
#include "sys/authority.h"
#include "sys/object.h"
#include "sys/process.h"
#include "sys/syscall.h"

const struct n_identity identity_kernel = {.cls = N_CLASS_SYSTEM};

/* 窓の系が起こすアプリの身元。決まるまでは何の権限も無い級。 */
static struct spinlock session_lock;
static struct n_identity session_identity = {.cls = N_CLASS_USER};

const struct n_identity *identity_current(void) {
    struct process *p = process_current();
    return p ? &p->identity : &identity_kernel;
}

bool identity_at_least(enum n_class cls) { return identity_current()->cls >= (uint32_t)cls; }

static bool identity_same(const struct n_identity *a, const struct n_identity *b) {
    return a->user == b->user && a->session == b->session && a->cls == b->cls && a->program == b->program;
}

bool identity_covers(const struct n_identity *holder, const struct n_identity *other) {
    return holder->cls == N_CLASS_SYSTEM || holder->cls > other->cls || identity_same(holder, other);
}

bool identity_may_reach(const struct n_identity *who, const struct n_identity *target) {
    if (who->cls == N_CLASS_SYSTEM) return true;
    if (who->cls == N_CLASS_ADMIN) return target->cls != N_CLASS_SYSTEM;
    return target->cls == N_CLASS_USER && target->user == who->user && target->session == who->session;
}

int identity_for_child(const struct n_launch *l, struct n_identity *child) {
    const struct n_identity *self = identity_current();
    *child = *self;
    if (!l->identity) return 0;
    struct n_identity wanted;
    int r = copy_from_user(&wanted, l->identity, sizeof wanted);
    if (r < 0) return r;
    if (wanted.cls > self->cls) return -N_ERR_DENIED;
    if (wanted.user != self->user || wanted.session != self->session || wanted.program != self->program) {
        r = cap_check(l->identity_grant, N_CAP_IDENTITY_GRANT);
        if (r < 0) return r;
    }
    *child = wanted;
    return 0;
}

/* ---- 能力：object の中身は種類だけ。持つことが権限で、渡すことが委譲。 ---- */

struct capability {
    enum n_cap kind;
    struct object object;
};

static void cap_release(struct object *o) { kfree(o->priv); }
static const struct object_ops cap_ops = {.kind = N_KIND_CAPABILITY, .release = cap_release};

int cap_new(enum n_cap kind, struct handle **out) {
    struct capability *c = kmalloc(sizeof *c);
    if (!c) return -N_ERR_NO_MEMORY;
    c->kind = kind;
    object_init(&c->object, &cap_ops, c);
    *out = handle_open(&c->object, 0);
    if (!*out) {
        kfree(c);
        return -N_ERR_NO_MEMORY;
    }
    return 0;
}

static enum n_cap cap_kind(struct handle *h) { return ((struct capability *)h->object->priv)->kind; }

int cap_check(int number, enum n_cap kind) {
    struct handle *h = handle_get(number);
    if (!h) return -N_ERR_NO_HANDLE;
    int r = handle_is(h, &cap_ops) && cap_kind(h) == kind ? 0 : -N_ERR_DENIED;
    handle_close(h);
    return r;
}

int64_t sys_identity_self(struct n_identity *out) {
    return copy_to_user(out, identity_current(), sizeof *out);
}

/* 根は全部を、ほかの能力は自分と同じ種類だけを切り出す。 */
int64_t sys_cap_derive(int cap, uint64_t kind) {
    if (kind >= N_CAP_COUNT) return -N_ERR_INVALID;
    struct handle *h = handle_get(cap);
    if (!h) return -N_ERR_NO_HANDLE;
    bool allowed = handle_is(h, &cap_ops) && (cap_kind(h) == N_CAP_ROOT || cap_kind(h) == (enum n_cap)kind);
    handle_close(h);
    if (!allowed) return -N_ERR_DENIED;
    struct handle *made;
    int r = cap_new((enum n_cap)kind, &made);
    return r < 0 ? r : handle_install(made);
}

int64_t sys_cap_kind(int cap) {
    struct handle *h = handle_get(cap);
    if (!h) return -N_ERR_NO_HANDLE;
    int64_t r = handle_is(h, &cap_ops) ? (int64_t)cap_kind(h) : -N_ERR_WRONG_KIND;
    handle_close(h);
    return r;
}

static bool cap_of_kind(struct handle *h, void *kind) {
    return handle_is(h, &cap_ops) && cap_kind(h) == *(enum n_cap *)kind;
}

int64_t sys_cap_find(uint64_t kind) {
    if (kind >= N_CAP_COUNT) return -N_ERR_INVALID;
    enum n_cap wanted = (enum n_cap)kind;
    int number = handle_find(cap_of_kind, &wanted);
    return number < 0 ? -N_ERR_NOT_FOUND : number;
}
