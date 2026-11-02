/* サービス settings：選択の保存、宣言の表、型と範囲の確かめ、権限の確かめ、見張り。領域の意味を持たない。

   変更の段は、検証、適用（カーネルの領域は SYS_SETTINGS_APPLY、ユーザー空間の領域はその接続）、保存、
   見張りへの知らせ。適用が断られれば何も変えない。保存に失敗すれば適用を戻す。どちらも理由の番号を呼ぶ側へ
   返す。確認の要る鍵は期限つきの試しで、期限はここが数える。試した者が死んでも戻る。

   一つのスレッドが事象の行列を回す。止まらないように、接続への送りは行列に積み、N_EVENT_SENDABLE の度に
   一通ずつ送る。ユーザー空間の持ち主の答えも待たない：変更を pending の列に置いて次の事象へ進む。

   保存は私的な領域 N_REGION_SETTINGS の一つの document。頭に二つの版の記録（struct version）があり、それぞれ
   が本体の場所と CRC を持つ。新しい本体は古い版の場所へ書き、media へ届いてから古い版の記録を書き換える。
   途中で切れても新しい方の版が残り、読むときは世代の新しい順に確かめて、壊れていれば前の版へ戻る。 */
#include <nocturne.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "authority_abi.h"
#include "settings_abi.h"
#include "settings_table.h"

#define NS_PER_MS 1000000ull
#define APPLY_ANSWER_MS 5000u     /* ユーザー空間の持ち主が答える期限 */
#define TRIAL_MS_MOST 120000u     /* 試しの期限の上限 */
#define OUTGOING_BYTES_MOST (4u << 20) /* 読まない接続に積む上限。超えた接続は閉じる。 */
#define LIST_PAGE 32u             /* LIST の一通の宣言の数 */

/* 事象の token。接続は構造体の番地（送りの待ちはその |1）。 */
enum { TOKEN_INLET = 1, TOKEN_WATCH_BASE = 16 };

struct outgoing {
    struct outgoing *next;
    size_t bytes;
    uint8_t data[];
};
struct conn {
    struct conn *next;
    n_handle handle;
    bool closed, watching, send_armed;
    struct n_setting_id watch;
    struct n_identity watcher;
    struct outgoing *head, *tail;
    size_t queued;
};
/* 宣言。指は寿命の間ずっと有効。 */
struct known {
    struct n_setting_decl d;
    uint32_t program;   /* N_SCOPE_PROGRAM の持ち主 */
    bool kernel;        /* カーネルの領域の鍵 */
    struct conn *owner; /* ユーザー空間の領域の持ち主 */
};
struct value {
    struct n_setting_id id;
    uint32_t length;
    uint8_t *bytes;
};
struct bytes {
    uint8_t *data;
    uint32_t length;
};
struct trial {
    struct trial *next;
    struct known *known;
    struct n_setting_id id;
    struct bytes wanted, before;
    uint64_t deadline; /* ns */
};
/* 適用の答えを待つ変更。 */
struct change {
    struct change *next;
    uint32_t op, serial;
    struct conn *client;
    struct known *known;
    struct n_setting_id id;
    struct bytes wanted, before;
    uint32_t trial_ms;
    struct conn *owner;
    uint32_t apply_serial;
    uint64_t deadline;
};

static n_handle queue, inlet;
static n_handle store = -1, apply_cap = -1;
static n_handle store_cap = -1;
static n_handle kernel_watch[N_DOMAIN_COUNT];
static struct conn *conns, *dead_conns;
static struct known **knowns;
static size_t known_count, known_capacity;
static struct value *values;
static size_t value_count, value_capacity;
static struct trial *trials;
static struct change *changes;
static uint32_t active_user;
static uint32_t next_apply_serial = 1;
static uint8_t *message; /* 受け取りの buffer */

static void say(const char *what, n_result r) {
    printf("settings: %s (%s)\n", what, n_result_text(r));
    fflush(stdout);
}

/* ---- バイト列 ---- */
static bool bytes_set(struct bytes *b, const void *data, uint32_t length) {
    uint8_t *copy = malloc(length ? length : 1);
    if (!copy) return false;
    if (length) memcpy(copy, data, length);
    free(b->data);
    b->data = copy;
    b->length = length;
    return true;
}
static void bytes_free(struct bytes *b) {
    free(b->data);
    b->data = NULL;
    b->length = 0;
}

/* ---- 宣言の表：(domain, key, program) の順 ---- */
static int known_compare(uint32_t domain, uint32_t key, uint32_t program, const struct known *k) {
    if (domain != k->d.domain) return domain < k->d.domain ? -1 : 1;
    if (program != k->program) return program < k->program ? -1 : 1;
    if (key != k->d.key) return key < k->d.key ? -1 : 1;
    return 0;
}
static size_t known_place(uint32_t domain, uint32_t key, uint32_t program, bool *found) {
    size_t lo = 0, hi = known_count;
    *found = false;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = known_compare(domain, key, program, knowns[mid]);
        if (!c) {
            *found = true;
            return mid;
        }
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}
static struct known *known_find(uint32_t domain, uint32_t key, uint32_t program) {
    bool found;
    size_t at = known_place(domain, key, program, &found);
    return found ? knowns[at] : NULL;
}

static bool name_valid(const char *s) {
    if (!*s) return false;
    size_t n = 0;
    for (; s[n] && n < N_SETTING_NAME_MAX; n++) {
        char c = s[n];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    }
    return n < N_SETTING_NAME_MAX;
}
static bool ended(const char *s, size_t cap) { return memchr(s, 0, cap) != NULL; }

static uint32_t fixed_size(uint32_t type) {
    switch (type) {
    case N_SETTING_BOOL: case N_SETTING_INT: case N_SETTING_ENUM: return sizeof(int32_t);
    case N_SETTING_ENDPOINT: return sizeof(struct n_sockaddr);
    case N_SETTING_MONITOR_MODE: return sizeof(struct n_setting_monitor_mode);
    default: return 0;
    }
}

static int64_t options_count(const struct n_setting_decl *d) {
    int64_t count = 0;
    size_t at = 0;
    while (at < sizeof d->options && d->options[at]) {
        const char *end = memchr(d->options + at, 0, sizeof d->options - at);
        if (!end) return -1;
        at = (size_t)(end - d->options) + 1;
        count++;
    }
    return count;
}

static bool decl_valid(const struct n_setting_decl *d) {
    if (!name_valid(d->name) || !ended(d->label, sizeof d->label) || !ended(d->about, sizeof d->about) ||
        !ended(d->fallback_text, sizeof d->fallback_text) || d->scope > N_SCOPE_PROGRAM || d->apply > N_APPLY_NONE)
        return false;
    if ((d->scope == N_SCOPE_PROGRAM) != (d->domain == N_DOMAIN_PROGRAM)) return false;
    if (d->scope == N_SCOPE_PROGRAM && d->apply != N_APPLY_NONE) return false;
    switch (d->type) {
    case N_SETTING_BOOL:
        return d->min == 0 && d->max == 1 && (d->fallback == 0 || d->fallback == 1);
    case N_SETTING_INT:
        return d->min >= INT32_MIN && d->max <= INT32_MAX && d->min <= d->max && d->fallback >= d->min &&
               d->fallback <= d->max;
    case N_SETTING_ENUM:
        return d->min == 0 && d->max >= 0 && options_count(d) == d->max + 1 && d->fallback >= 0 && d->fallback <= d->max;
    case N_SETTING_STRING:
        return d->max >= 0 && d->max <= N_SETTING_VALUE_MAX && (int64_t)strlen(d->fallback_text) <= d->max;
    case N_SETTING_ENDPOINT:
    case N_SETTING_MONITOR_MODE:
        return true;
    default:
        return false;
    }
}

/* 宣言を足すか置き換える。 */
static struct known *known_add(const struct n_setting_decl *d, uint32_t program) {
    bool found;
    size_t at = known_place(d->domain, d->key, program, &found);
    if (found) {
        knowns[at]->d = *d;
        return knowns[at];
    }
    if (known_count == known_capacity) {
        size_t grown = known_capacity ? known_capacity * 2 : 64;
        struct known **list = realloc(knowns, grown * sizeof *list);
        if (!list) return NULL;
        knowns = list;
        known_capacity = grown;
    }
    struct known *k = calloc(1, sizeof *k);
    if (!k) return NULL;
    k->d = *d;
    k->program = program;
    memmove(knowns + at + 1, knowns + at, (known_count - at) * sizeof *knowns);
    knowns[at] = k;
    known_count++;
    return k;
}

/* ---- 値の表：struct n_setting_id の順 ---- */
static int id_compare(const struct n_setting_id *a, const struct n_setting_id *b) {
    if (a->domain != b->domain) return a->domain < b->domain ? -1 : 1;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    if (a->device != b->device) return a->device < b->device ? -1 : 1;
    if (a->user != b->user) return a->user < b->user ? -1 : 1;
    if (a->program != b->program) return a->program < b->program ? -1 : 1;
    return 0;
}
static size_t value_place(const struct n_setting_id *id, bool *found) {
    size_t lo = 0, hi = value_count;
    *found = false;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = id_compare(id, &values[mid].id);
        if (!c) {
            *found = true;
            return mid;
        }
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}
static struct value *value_find(const struct n_setting_id *id) {
    bool found;
    size_t at = value_place(id, &found);
    return found ? &values[at] : NULL;
}
static bool value_put(const struct n_setting_id *id, const void *data, uint32_t length) {
    bool found;
    size_t at = value_place(id, &found);
    uint8_t *copy = malloc(length ? length : 1);
    if (!copy) return false;
    if (length) memcpy(copy, data, length);
    if (found) {
        free(values[at].bytes);
    } else {
        if (value_count == value_capacity) {
            size_t grown = value_capacity ? value_capacity * 2 : 64;
            struct value *list = realloc(values, grown * sizeof *list);
            if (!list) {
                free(copy);
                return false;
            }
            values = list;
            value_capacity = grown;
        }
        memmove(values + at + 1, values + at, (value_count - at) * sizeof *values);
        value_count++;
        values[at].id = *id;
    }
    values[at].length = length;
    values[at].bytes = copy;
    return true;
}
static void value_remove(const struct n_setting_id *id) {
    bool found;
    size_t at = value_place(id, &found);
    if (!found) return;
    free(values[at].bytes);
    memmove(values + at, values + at + 1, (value_count - at - 1) * sizeof *values);
    value_count--;
}

/* ---- 保存 ---- */
#define VERSION_MAGIC 0x54455354u /* "TSET" */
#define VERSION_FORMAT 1u
#define VERSION_SLOT 4096u
#define BODY_FIRST (2u * VERSION_SLOT)
struct version {
    uint32_t magic, format;
    uint64_t generation;
    uint64_t offset, capacity, length;
    uint32_t count;
    uint32_t body_crc;
    uint32_t check; /* この欄の前までの CRC */
};
struct record {
    struct n_setting_id id;
    uint32_t length;
    uint32_t reserved;
};
static struct version versions[2];
static int newest = -1;

static uint32_t crc32_of(const void *data, size_t n) {
    const uint8_t *p = data;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++) crc = crc >> 1 ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
static bool version_valid(const struct version *v) {
    return v->magic == VERSION_MAGIC && v->format == VERSION_FORMAT && v->length <= v->capacity &&
           v->offset >= BODY_FIRST && v->check == crc32_of(v, offsetof(struct version, check));
}

static n_result store_read(uint64_t offset, void *out, size_t n) {
    for (size_t at = 0; at < n;) {
        n_result got = n_document_read_at(store, offset + at, (uint8_t *)out + at, n - at);
        if (got < 0) return got;
        if (!got) return -N_ERR_IO;
        at += (size_t)got;
    }
    return 0;
}
static n_result store_write(uint64_t offset, const void *data, size_t n) {
    for (size_t at = 0; at < n;) {
        n_result put = n_document_write_at(store, offset + at, (const uint8_t *)data + at, n - at);
        if (put < 0) return put;
        if (!put) return -N_ERR_IO;
        at += (size_t)put;
    }
    return 0;
}

/* 本体を値の表にする。すべて確かめてから入れ替える。 */
static bool body_load(const struct version *v) {
    uint8_t *body = malloc(v->length ? v->length : 1);
    if (!body) return false;
    bool good = store_read(v->offset, body, v->length) >= 0 && crc32_of(body, v->length) == v->body_crc;
    struct value *list = NULL;
    uint32_t made = 0;
    if (good && v->count) {
        list = calloc(v->count, sizeof *list);
        good = list != NULL;
    }
    size_t at = 0;
    for (uint32_t i = 0; good && i < v->count; i++) {
        struct record r;
        if (v->length - at < sizeof r) {
            good = false;
            break;
        }
        memcpy(&r, body + at, sizeof r);
        at += sizeof r;
        size_t padded = ((size_t)r.length + 7) & ~(size_t)7;
        if (r.length > N_SETTING_VALUE_MAX || v->length - at < padded || (list[i].bytes = malloc(r.length ? r.length : 1)) == NULL) {
            good = false;
            break;
        }
        list[i].id = r.id;
        list[i].length = r.length;
        memcpy(list[i].bytes, body + at, r.length);
        at += padded;
        made++;
        if (i && id_compare(&list[i - 1].id, &list[i].id) >= 0) good = false; /* 表の順が崩れている */
    }
    free(body);
    if (!good) {
        for (uint32_t i = 0; i < made; i++) free(list[i].bytes);
        free(list);
        return false;
    }
    values = list;
    value_count = value_capacity = v->count;
    return true;
}

static void store_load(void) {
    n_result r = store_cap >= 0 ? __syscall(SYS_STORE_PRIVATE_OPEN, store_cap, N_REGION_SETTINGS, 0, 0, 0) : -N_ERR_DENIED;
    if (r < 0) {
        say("cannot open the store", r);
        return;
    }
    store = (n_handle)r;
    bool ok[2];
    for (int i = 0; i < 2; i++)
        ok[i] = store_read((uint64_t)i * VERSION_SLOT, &versions[i], sizeof versions[i]) >= 0 && version_valid(&versions[i]);
    for (int tries = 0; tries < 2; tries++) {
        int pick = ok[0] && (!ok[1] || versions[0].generation > versions[1].generation) ? 0 : ok[1] ? 1 : -1;
        if (pick < 0) break;
        if (body_load(&versions[pick])) {
            if (tries) say("the newest version is broken; the one before is back", -N_ERR_IO);
            newest = pick;
            return;
        }
        ok[pick] = false;
        memset(&versions[pick], 0, sizeof versions[pick]);
    }
    memset(versions, 0, sizeof versions);
}

/* 全部を次の版として書く。0 か、保存の答え。 */
static n_result store_save(void) {
    if (store < 0) return -N_ERR_NOT_READY;
    size_t need = 0;
    for (size_t i = 0; i < value_count; i++) need += sizeof(struct record) + (((size_t)values[i].length + 7) & ~(size_t)7);
    uint8_t *body = calloc(1, need ? need : 1);
    if (!body) return -N_ERR_NO_MEMORY;
    size_t at = 0;
    for (size_t i = 0; i < value_count; i++) {
        struct record r = {.id = values[i].id, .length = values[i].length};
        memcpy(body + at, &r, sizeof r);
        at += sizeof r;
        if (values[i].length) memcpy(body + at, values[i].bytes, values[i].length);
        at += ((size_t)values[i].length + 7) & ~(size_t)7;
    }
    int old = newest < 0 ? 0 : !newest;
    const struct version *now = newest < 0 ? NULL : &versions[newest];
    struct version next = {.magic = VERSION_MAGIC, .format = VERSION_FORMAT,
                           .generation = (now ? now->generation : 0) + 1, .length = need, .count = (uint32_t)value_count};
    /* 古い版の場所に入ればそこへ。入らなければ全部の後へ、余裕を持たせて。今の版の場所には触れない。 */
    const struct version *free_place = &versions[old];
    if (free_place->magic == VERSION_MAGIC && free_place->capacity >= need) {
        next.offset = free_place->offset;
        next.capacity = free_place->capacity;
    } else {
        uint64_t end = BODY_FIRST;
        for (int i = 0; i < 2; i++)
            if (versions[i].magic == VERSION_MAGIC && versions[i].offset + versions[i].capacity > end)
                end = versions[i].offset + versions[i].capacity;
        next.offset = end;
        next.capacity = need * 2 < 4096 ? 4096 : need * 2;
    }
    next.body_crc = crc32_of(body, need);
    next.check = crc32_of(&next, offsetof(struct version, check));
    n_result r = store_write(next.offset, body, need);
    free(body);
    if (r >= 0) r = n_document_commit(store, N_COMMIT_WAIT);
    if (r >= 0) r = store_write((uint64_t)old * VERSION_SLOT, &next, sizeof next);
    if (r >= 0) r = n_document_commit(store, N_COMMIT_WAIT);
    if (r < 0) return r;
    versions[old] = next;
    newest = old;
    return 0;
}

/* ---- 接続への送り ---- */
static void conn_close(struct conn *c);

static void send_arm(struct conn *c) {
    if (c->send_armed || c->closed) return;
    if (n_events_watch(queue, c->handle, N_EVENT_SENDABLE, (uint64_t)(uintptr_t)c | 1) >= 0) c->send_armed = true;
}
static void conn_queue(struct conn *c, const void *a, size_t an, const void *b, size_t bn) {
    if (!c || c->closed) return;
    struct outgoing *o = malloc(sizeof *o + an + bn);
    if (!o || c->queued + an + bn > OUTGOING_BYTES_MOST) {
        free(o);
        conn_close(c);
        return;
    }
    o->next = NULL;
    o->bytes = an + bn;
    memcpy(o->data, a, an);
    if (bn) memcpy(o->data + an, b, bn);
    if (c->tail) c->tail->next = o;
    else c->head = o;
    c->tail = o;
    c->queued += o->bytes;
    send_arm(c);
}
static void conn_send_one(struct conn *c) {
    struct outgoing *o = c->head;
    if (o) {
        n_result r = n_channel_send(c->handle, o->data, o->bytes, -1);
        if (r < 0) {
            conn_close(c);
            return;
        }
        c->head = o->next;
        if (!c->head) c->tail = NULL;
        c->queued -= o->bytes;
        free(o);
    }
    if (!c->head && c->send_armed) {
        n_events_unwatch(queue, (uint64_t)(uintptr_t)c | 1);
        c->send_armed = false;
    }
}
static void reply(struct conn *c, uint32_t op, uint32_t serial, n_result result, uint32_t reason, uint32_t more,
                  const void *data, uint32_t length) {
    struct n_settings_reply m = {.op = op, .serial = serial, .result = (int32_t)result, .reason = reason,
                                 .length = length, .more = more};
    conn_queue(c, &m, sizeof m, data, length);
}

/* ---- 権限 ---- */
static bool peer_of(struct conn *c, struct n_identity *out) {
    return n_channel_peer(c->handle, out) >= 0;
}
/* 要求の id を鍵の scope の形に揃える。呼ぶ側のものでなければ false。 */
static bool target(const struct known *k, const struct n_identity *who, struct n_setting_id *id) {
    switch (k->d.scope) {
    case N_SCOPE_MACHINE:
        id->device = 0; id->user = 0; id->program = 0;
        return true;
    case N_SCOPE_DEVICE:
        id->user = 0; id->program = 0;
        return true;
    case N_SCOPE_USER:
        id->device = 0; id->program = 0;
        if (!id->user) id->user = who->user;
        return id->user != 0;
    case N_SCOPE_PROGRAM:
        id->device = 0;
        id->program = who->program;
        id->user = who->user;
        return who->program != 0;
    }
    return false;
}
static bool may_read(const struct known *k, const struct n_identity *who, const struct n_setting_id *id) {
    if (k->d.scope == N_SCOPE_USER) return id->user == who->user || who->cls >= N_CLASS_ADMIN;
    return true; /* 機械と装置は誰でも読める。プログラムは target が本人へ揃えた。 */
}
static bool may_write(const struct known *k, const struct n_identity *who, const struct n_setting_id *id) {
    switch (k->d.scope) {
    case N_SCOPE_MACHINE:
    case N_SCOPE_DEVICE: return who->cls >= N_CLASS_ADMIN;
    case N_SCOPE_USER: return id->user == who->user || who->cls >= N_CLASS_SYSTEM;
    case N_SCOPE_PROGRAM: return true;
    }
    return false;
}

/* ---- 見張り ---- */
static bool watch_matches(const struct conn *c, const struct known *k, const struct n_setting_id *id) {
    if (c->watch.domain != N_SETTING_ANY && c->watch.domain != id->domain) return false;
    if (c->watch.key != N_SETTING_ANY && c->watch.key != id->key) return false;
    if (k->d.scope == N_SCOPE_PROGRAM && (c->watcher.program != id->program || c->watcher.user != id->user)) return false;
    return may_read(k, &c->watcher, id);
}
static void notify(const struct known *k, const struct n_setting_id *id, uint32_t what, uint32_t remaining_ms) {
    struct n_settings_changed m = {.id = *id, .what = what, .remaining_ms = remaining_ms};
    for (struct conn *c = conns; c; c = c->next)
        if (c->watching && !c->closed && watch_matches(c, k, id)) conn_queue(c, &m, sizeof m, NULL, 0);
}

/* ---- 既定の値と今の値 ---- */
static uint32_t fallback_bytes(const struct known *k, void *out) {
    switch (k->d.type) {
    case N_SETTING_BOOL: case N_SETTING_INT: case N_SETTING_ENUM: {
        int32_t v = (int32_t)k->d.fallback;
        memcpy(out, &v, sizeof v);
        return sizeof v;
    }
    case N_SETTING_STRING: {
        uint32_t n = (uint32_t)strlen(k->d.fallback_text);
        memcpy(out, k->d.fallback_text, n);
        return n;
    }
    default: {
        uint32_t n = fixed_size(k->d.type);
        memset(out, 0, n);
        return n;
    }
    }
}
static struct trial *trial_find(const struct n_setting_id *id) {
    for (struct trial *t = trials; t; t = t->next)
        if (!id_compare(&t->id, id)) return t;
    return NULL;
}

/* 値の確かめ。enum n_setting_reason。 */
static uint32_t value_check(const struct known *k, const void *data, uint32_t length) {
    if (k->d.type == N_SETTING_STRING) return length > k->d.max ? N_SETTING_OUT_OF_RANGE : N_SETTING_OK;
    if (length != fixed_size(k->d.type)) return N_SETTING_WRONG_TYPE;
    if (k->d.type == N_SETTING_BOOL || k->d.type == N_SETTING_INT || k->d.type == N_SETTING_ENUM) {
        int32_t v;
        memcpy(&v, data, sizeof v);
        if (v < k->d.min || v > k->d.max) return N_SETTING_OUT_OF_RANGE;
    }
    return N_SETTING_OK;
}

/* ---- 適用 ---- */
#define APPLY_WAITING 1
/* 持ち主へ渡す。0 か -N_ERR_*。ユーザー空間の持ち主で答えを待つなら APPLY_WAITING（serial で答える）。 */
static n_result apply_start(struct known *k, const struct n_setting_id *id, const void *data, uint32_t length, uint32_t serial) {
    if (k->d.apply == N_APPLY_NONE) return 0;
    if (k->d.scope == N_SCOPE_USER && id->user != active_user) return 0; /* 前面に来たときに当てる */
    if (k->kernel) return __syscall(SYS_SETTINGS_APPLY, apply_cap, k->d.domain, k->d.key, (long)data, length);
    if (!k->owner) return -N_ERR_NOT_FOUND;
    struct n_settings_request m = {.op = N_SETTINGS_APPLY, .serial = serial, .length = length, .id = *id};
    conn_queue(k->owner, &m, sizeof m, data, length);
    return APPLY_WAITING;
}

static void change_free(struct change *ch) {
    bytes_free(&ch->wanted);
    bytes_free(&ch->before);
    free(ch);
}
static void trial_free(struct trial *t) {
    for (struct trial **p = &trials; *p; p = &(*p)->next)
        if (*p == t) {
            *p = t->next;
            break;
        }
    bytes_free(&t->wanted);
    bytes_free(&t->before);
    free(t);
}

/* 値を保存する。失敗すれば表を元へ戻す。 */
static n_result store_value(const struct n_setting_id *id, const void *data, uint32_t length, const struct bytes *before,
                            bool had_before) {
    if (!value_put(id, data, length)) return -N_ERR_NO_MEMORY;
    n_result r = store_save();
    if (r < 0) {
        if (had_before) value_put(id, before->data, before->length);
        else value_remove(id);
    }
    return r;
}

/* 適用の答えが出た。 */
static void change_finish(struct change *ch, n_result applied) {
    struct known *k = ch->known;
    if (applied < 0) {
        uint32_t reason = applied == -N_ERR_TIMEOUT || applied == -N_ERR_NOT_FOUND ? N_SETTING_APPLY_GONE : N_SETTING_APPLY_REFUSED;
        reply(ch->client, ch->op, ch->serial, applied, reason, 0, NULL, 0);
        change_free(ch);
        return;
    }
    if (ch->op == N_SETTINGS_SET) {
        bool had = value_find(&ch->id) != NULL;
        n_result saved = store_value(&ch->id, ch->wanted.data, ch->wanted.length, &ch->before, had);
        if (saved < 0) {
            apply_start(k, &ch->id, ch->before.data, ch->before.length, 0);
            reply(ch->client, ch->op, ch->serial, saved, N_SETTING_SAVE_FAILED, 0, NULL, 0);
        } else {
            reply(ch->client, ch->op, ch->serial, 0, N_SETTING_OK, 0, NULL, 0);
            notify(k, &ch->id, N_SETTING_CHANGED, 0);
        }
    } else { /* N_SETTINGS_TRY */
        struct trial *t = trial_find(&ch->id);
        if (!t) {
            t = calloc(1, sizeof *t);
            if (!t || !bytes_set(&t->before, ch->before.data, ch->before.length)) {
                free(t);
                apply_start(k, &ch->id, ch->before.data, ch->before.length, 0);
                reply(ch->client, ch->op, ch->serial, -N_ERR_NO_MEMORY, N_SETTING_APPLY_REFUSED, 0, NULL, 0);
                change_free(ch);
                return;
            }
            t->known = k;
            t->id = ch->id;
            t->next = trials;
            trials = t;
        }
        bytes_set(&t->wanted, ch->wanted.data, ch->wanted.length);
        t->deadline = n_clock_monotonic() + ch->trial_ms * NS_PER_MS;
        reply(ch->client, ch->op, ch->serial, 0, N_SETTING_OK, ch->trial_ms, NULL, 0);
        notify(k, &ch->id, N_SETTING_ON_TRIAL, ch->trial_ms);
    }
    change_free(ch);
}

static bool change_pending(const struct n_setting_id *id) {
    for (struct change *ch = changes; ch; ch = ch->next)
        if (!id_compare(&ch->id, id)) return true;
    return false;
}

static void change_begin(struct conn *c, uint32_t op, uint32_t serial, struct known *k, const struct n_setting_id *id,
                         const void *data, uint32_t length, const void *revert_to, uint32_t revert_length, uint32_t trial_ms) {
    struct change *ch = calloc(1, sizeof *ch);
    if (!ch || !bytes_set(&ch->wanted, data, length)) {
        free(ch);
        reply(c, op, serial, -N_ERR_NO_MEMORY, N_SETTING_APPLY_REFUSED, 0, NULL, 0);
        return;
    }
    ch->op = op;
    ch->serial = serial;
    ch->client = c;
    ch->known = k;
    ch->id = *id;
    ch->trial_ms = trial_ms;
    const struct trial *on = trial_find(id);
    const struct value *v = value_find(id);
    bool kept = true;
    if (on) kept = bytes_set(&ch->before, on->before.data, on->before.length);
    else if (v) kept = bytes_set(&ch->before, v->bytes, v->length);
    else if (revert_length) kept = bytes_set(&ch->before, revert_to, revert_length);
    else {
        uint8_t fallback[N_SETTING_TEXT_MAX + sizeof(struct n_setting_monitor_mode) + sizeof(struct n_sockaddr)];
        kept = bytes_set(&ch->before, fallback, fallback_bytes(k, fallback));
    }
    if (!kept) {
        reply(c, op, serial, -N_ERR_NO_MEMORY, N_SETTING_APPLY_REFUSED, 0, NULL, 0);
        change_free(ch);
        return;
    }
    uint32_t apply_serial = next_apply_serial++;
    if (!next_apply_serial) next_apply_serial = 1;
    n_result r = apply_start(k, id, data, length, apply_serial);
    if (r == APPLY_WAITING) {
        ch->owner = k->owner;
        ch->apply_serial = apply_serial;
        ch->deadline = n_clock_monotonic() + APPLY_ANSWER_MS * NS_PER_MS;
        ch->next = changes;
        changes = ch;
        return;
    }
    change_finish(ch, r);
}

/* 試しの終わり：前の値へ戻す。 */
static void trial_back(struct trial *t) {
    apply_start(t->known, &t->id, t->before.data, t->before.length, 0);
    notify(t->known, &t->id, N_SETTING_REVERTED, 0);
    trial_free(t);
}

/* ---- 文の処理 ---- */
#define SCRATCH_BYTES (N_SETTING_TEXT_MAX + sizeof(struct n_setting_monitor_mode) + sizeof(struct n_sockaddr))
/* 今の値：試しの値、保存された値、カーネルの今の値、既定の順。scratch は SCRATCH_BYTES。 */
static const void *current_bytes(const struct known *k, const struct n_setting_id *id, void *scratch, uint32_t *length) {
    const struct trial *t = trial_find(id);
    if (t) {
        *length = t->wanted.length;
        return t->wanted.data;
    }
    const struct value *v = value_find(id);
    if (v) {
        *length = v->length;
        return v->bytes;
    }
    /* カーネルの鍵は今の値。一覧の鍵は装置ごとの選択が無ければ既定（全部 0）。 */
    if (k->kernel && k->d.type != N_SETTING_MONITOR_MODE) {
        uint32_t size = fixed_size(k->d.type);
        n_result r = __syscall(SYS_SETTINGS_EFFECTIVE, k->d.domain, k->d.key, (long)scratch, SCRATCH_BYTES, 0);
        if (r >= 0 && (uint32_t)r == size) {
            *length = size;
            return scratch;
        }
    }
    *length = fallback_bytes(k, scratch);
    return scratch;
}
static void answer_get(struct conn *c, const struct n_settings_request *m, struct known *k, const struct n_setting_id *id) {
    uint8_t scratch[SCRATCH_BYTES];
    uint32_t length;
    const void *data = current_bytes(k, id, scratch, &length);
    reply(c, m->op, m->serial, 0, N_SETTING_OK, 0, data, length);
}

static void answer_list(struct conn *c, const struct n_settings_request *m, const struct n_identity *who) {
    struct n_setting_decl *page = malloc(LIST_PAGE * sizeof *page);
    if (!page) {
        reply(c, m->op, m->serial, -N_ERR_NO_MEMORY, N_SETTING_OK, 0, NULL, 0);
        return;
    }
    uint32_t count = 0;
    size_t at = m->argument;
    for (; at < known_count && count < LIST_PAGE; at++) {
        const struct known *k = knowns[at];
        if (k->d.scope == N_SCOPE_PROGRAM && (k->program != who->program || !who->program)) continue;
        page[count++] = k->d;
    }
    reply(c, m->op, m->serial, 0, N_SETTING_OK, at < known_count ? (uint32_t)at : 0, page, count * (uint32_t)sizeof *page);
    free(page);
}

static void answer_declare(struct conn *c, const struct n_settings_request *m, const struct n_identity *who, const void *data) {
    struct n_setting_decl d;
    if (m->length != sizeof d) {
        reply(c, m->op, m->serial, -N_ERR_INVALID, N_SETTING_BAD_DECLARATION, 0, NULL, 0);
        return;
    }
    memcpy(&d, data, sizeof d);
    if (!decl_valid(&d)) {
        reply(c, m->op, m->serial, -N_ERR_INVALID, N_SETTING_BAD_DECLARATION, 0, NULL, 0);
        return;
    }
    uint32_t program = 0;
    if (d.scope == N_SCOPE_PROGRAM) {
        if (!who->program) {
            reply(c, m->op, m->serial, -N_ERR_DENIED, N_SETTING_NOT_ALLOWED, 0, NULL, 0);
            return;
        }
        program = who->program;
    } else if (d.domain >= N_DOMAIN_COUNT || who->cls < N_CLASS_ADMIN) {
        reply(c, m->op, m->serial, -N_ERR_DENIED, N_SETTING_NOT_ALLOWED, 0, NULL, 0);
        return;
    }
    const struct known *there = known_find(d.domain, d.key, program);
    if (there && there->kernel) { /* カーネルの領域の鍵は表のもの */
        reply(c, m->op, m->serial, -N_ERR_EXISTS, N_SETTING_BAD_DECLARATION, 0, NULL, 0);
        return;
    }
    struct known *k = known_add(&d, program);
    if (!k) {
        reply(c, m->op, m->serial, -N_ERR_NO_MEMORY, N_SETTING_OK, 0, NULL, 0);
        return;
    }
    if (d.scope != N_SCOPE_PROGRAM) k->owner = c;
    reply(c, m->op, m->serial, 0, N_SETTING_OK, 0, NULL, 0);
}

/* 利用者が前面になった。その人の値を持ち主へ当て直す。 */
static void session_changed(uint32_t user) {
    active_user = user;
    for (size_t i = 0; i < value_count; i++) {
        const struct value *v = &values[i];
        if (v->id.user != user || v->id.program) continue;
        struct known *k = known_find(v->id.domain, v->id.key, 0);
        if (!k || k->d.scope != N_SCOPE_USER) continue;
        n_result r = apply_start(k, &v->id, v->bytes, v->length, 0);
        if (r < 0) say("a setting of the user was refused", r);
    }
}

/* 起動の後、機械と装置の値を当てる。 */
static void apply_machine_values(void) {
    for (size_t i = 0; i < value_count; i++) {
        const struct value *v = &values[i];
        struct known *k = known_find(v->id.domain, v->id.key, 0);
        if (!k || !k->kernel || (k->d.scope != N_SCOPE_MACHINE && k->d.scope != N_SCOPE_DEVICE)) continue;
        n_result r = apply_start(k, &v->id, v->bytes, v->length, 0);
        if (r < 0) say("a kept setting was refused", r);
    }
}

static void on_request(struct conn *c, const struct n_settings_request *m, size_t size) {
    struct n_identity who;
    if (!peer_of(c, &who)) {
        conn_close(c);
        return;
    }
    const uint8_t *data = (const uint8_t *)(m + 1);
    if (size != sizeof *m + (size_t)m->length + m->extra) {
        reply(c, m->op, m->serial, -N_ERR_INVALID, N_SETTING_WRONG_TYPE, 0, NULL, 0);
        return;
    }
    switch (m->op) {
    case N_SETTINGS_LIST: answer_list(c, m, &who); return;
    case N_SETTINGS_DECLARE: answer_declare(c, m, &who, data); return;
    case N_SETTINGS_WATCH:
        c->watching = true;
        c->watch = m->id;
        c->watcher = who;
        reply(c, m->op, m->serial, 0, N_SETTING_OK, 0, NULL, 0);
        return;
    case N_SETTINGS_SESSION:
        if (who.cls < N_CLASS_SYSTEM) {
            reply(c, m->op, m->serial, -N_ERR_DENIED, N_SETTING_NOT_ALLOWED, 0, NULL, 0);
            return;
        }
        session_changed(m->argument);
        reply(c, m->op, m->serial, 0, N_SETTING_OK, 0, NULL, 0);
        return;
    default: break;
    }
    struct n_setting_id id = m->id;
    uint32_t program = id.domain == N_DOMAIN_PROGRAM ? who.program : 0;
    struct known *k = known_find(id.domain, id.key, program);
    if (!k) {
        reply(c, m->op, m->serial, -N_ERR_NOT_FOUND, N_SETTING_UNKNOWN_KEY, 0, NULL, 0);
        return;
    }
    if (!target(k, &who, &id)) {
        reply(c, m->op, m->serial, -N_ERR_DENIED, N_SETTING_NOT_ALLOWED, 0, NULL, 0);
        return;
    }
    if (m->op == N_SETTINGS_GET) {
        if (!may_read(k, &who, &id)) reply(c, m->op, m->serial, -N_ERR_DENIED, N_SETTING_NOT_ALLOWED, 0, NULL, 0);
        else answer_get(c, m, k, &id);
        return;
    }
    if (!may_write(k, &who, &id)) {
        reply(c, m->op, m->serial, -N_ERR_DENIED, N_SETTING_NOT_ALLOWED, 0, NULL, 0);
        return;
    }
    if (change_pending(&id)) {
        reply(c, m->op, m->serial, -N_ERR_BUSY, N_SETTING_BUSY, 0, NULL, 0);
        return;
    }
    switch (m->op) {
    case N_SETTINGS_SET:
    case N_SETTINGS_TRY: {
        bool conditional = m->op == N_SETTINGS_SET && m->argument == 1;
        uint32_t reason = value_check(k, data, m->length);
        if (reason == N_SETTING_OK && m->op == N_SETTINGS_SET && k->d.apply == N_APPLY_TRIAL) reason = N_SETTING_NEEDS_TRIAL;
        if (reason == N_SETTING_OK && m->extra && !conditional) reason = value_check(k, data + m->length, m->extra);
        if (reason != N_SETTING_OK) {
            reply(c, m->op, m->serial, -N_ERR_INVALID, reason, 0, NULL, 0);
            return;
        }
        if (conditional) {
            uint8_t scratch[SCRATCH_BYTES];
            uint32_t length;
            const void *now = current_bytes(k, &id, scratch, &length);
            if (length != m->extra || (length && memcmp(now, data + m->length, length))) {
                reply(c, m->op, m->serial, -N_ERR_BUSY, N_SETTING_NOT_THE_SAME, 0, NULL, 0);
                return;
            }
        }
        uint32_t ms = m->op == N_SETTINGS_TRY ? (m->argument < TRIAL_MS_MOST ? m->argument : TRIAL_MS_MOST) : 0;
        change_begin(c, m->op, m->serial, k, &id, data, m->length, data + m->length, conditional ? 0 : m->extra, ms);
        return;
    }
    case N_SETTINGS_COMMIT: {
        struct trial *t = trial_find(&id);
        if (!t) {
            reply(c, m->op, m->serial, -N_ERR_NOT_FOUND, N_SETTING_NO_TRIAL, 0, NULL, 0);
            return;
        }
        struct value *v = value_find(&id);
        struct bytes before = {0};
        bool had = v != NULL;
        if (had && !bytes_set(&before, v->bytes, v->length)) {
            reply(c, m->op, m->serial, -N_ERR_NO_MEMORY, N_SETTING_SAVE_FAILED, 0, NULL, 0);
            return;
        }
        n_result saved = store_value(&id, t->wanted.data, t->wanted.length, &before, had);
        bytes_free(&before);
        if (saved < 0) {
            trial_back(t);
            reply(c, m->op, m->serial, saved, N_SETTING_SAVE_FAILED, 0, NULL, 0);
            return;
        }
        trial_free(t);
        reply(c, m->op, m->serial, 0, N_SETTING_OK, 0, NULL, 0);
        notify(k, &id, N_SETTING_CHANGED, 0);
        return;
    }
    case N_SETTINGS_REVERT: {
        struct trial *t = trial_find(&id);
        if (!t) {
            reply(c, m->op, m->serial, -N_ERR_NOT_FOUND, N_SETTING_NO_TRIAL, 0, NULL, 0);
            return;
        }
        trial_back(t);
        reply(c, m->op, m->serial, 0, N_SETTING_OK, 0, NULL, 0);
        return;
    }
    default:
        reply(c, m->op, m->serial, -N_ERR_INVALID, N_SETTING_OK, 0, NULL, 0);
    }
}

/* 領域の持ち主の答え。 */
static void on_apply_answer(struct conn *c, const struct n_settings_reply *m) {
    if (!m->serial) return; /* 戻す適用の答えは待っていない */
    for (struct change **p = &changes; *p; p = &(*p)->next) {
        struct change *ch = *p;
        if (ch->owner == c && ch->apply_serial == m->serial) {
            *p = ch->next;
            change_finish(ch, m->result);
            return;
        }
    }
}

static void conn_receive(struct conn *c) {
    n_result n = n_channel_receive(c->handle, message, N_SETTINGS_MESSAGE_MAX, 0, NULL);
    if (n == -N_ERR_NOT_READY) return;
    if (n < 0) {
        conn_close(c);
        return;
    }
    uint32_t op;
    if ((size_t)n < sizeof op) {
        conn_close(c);
        return;
    }
    memcpy(&op, message, sizeof op);
    if (op == N_SETTINGS_APPLY && (size_t)n >= sizeof(struct n_settings_reply)) {
        on_apply_answer(c, (const struct n_settings_reply *)message);
    } else if ((size_t)n >= sizeof(struct n_settings_request)) {
        on_request(c, (const struct n_settings_request *)message, (size_t)n);
    } else {
        conn_close(c);
    }
}

static void conn_close(struct conn *c) {
    if (c->closed) return;
    c->closed = true;
    n_events_unwatch(queue, (uint64_t)(uintptr_t)c);
    if (c->send_armed) n_events_unwatch(queue, (uint64_t)(uintptr_t)c | 1);
    n_handle_close(c->handle);
    for (struct conn **p = &conns; *p; p = &(*p)->next)
        if (*p == c) {
            *p = c->next;
            break;
        }
    c->next = dead_conns;
    dead_conns = c;
    while (c->head) {
        struct outgoing *o = c->head;
        c->head = o->next;
        free(o);
    }
    c->tail = NULL;
    /* 呼んだ者がいなくなった変更は答えを捨てる。持ち主がいなくなった鍵は適用できない。 */
    for (struct change *ch = changes; ch; ch = ch->next)
        if (ch->client == c) ch->client = NULL;
    for (size_t i = 0; i < known_count; i++)
        if (knowns[i]->owner == c) knowns[i]->owner = NULL;
    for (struct change **p = &changes; *p;) {
        struct change *ch = *p;
        if (ch->owner == c) {
            *p = ch->next;
            change_finish(ch, -N_ERR_NOT_FOUND);
        } else {
            p = &ch->next;
        }
    }
}

static void conn_add(n_handle h) {
    struct conn *c = calloc(1, sizeof *c);
    if (!c) {
        n_handle_close(h);
        return;
    }
    c->handle = h;
    n_result r = n_events_watch(queue, h, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, (uint64_t)(uintptr_t)c);
    if (r < 0) {
        n_handle_close(h);
        free(c);
        return;
    }
    c->next = conns;
    conns = c;
}

/* カーネルの領域の今の値が適用の外で変わった（タスクバーの音量など）。保存し、見張りへ知らせる。 */
static void kernel_domain_changed(uint32_t domain) {
    n_watch_take(kernel_watch[domain]);
    uint8_t now[sizeof(struct n_sockaddr) + sizeof(int32_t)];
    for (size_t i = 0; i < known_count; i++) {
        struct known *k = knowns[i];
        if (k->d.domain != domain || !k->kernel || k->d.apply == N_APPLY_RESTART || k->d.type == N_SETTING_MONITOR_MODE) continue;
        struct n_setting_id id = {.domain = domain, .key = k->d.key};
        if (k->d.scope == N_SCOPE_USER) {
            if (!active_user) continue;
            id.user = active_user;
        }
        if (trial_find(&id) || change_pending(&id)) continue;
        n_result r = __syscall(SYS_SETTINGS_EFFECTIVE, domain, k->d.key, (long)now, sizeof now, 0);
        uint32_t size = fixed_size(k->d.type);
        if (r < 0 || (uint32_t)r != size) continue;
        const struct value *v = value_find(&id);
        uint8_t fallback[sizeof now];
        const void *kept = v ? v->bytes : fallback;
        uint32_t kept_length = v ? v->length : fallback_bytes(k, fallback);
        if (kept_length == size && !memcmp(kept, now, size)) continue;
        struct bytes before = {0};
        bool had = v != NULL;
        if (had && !bytes_set(&before, v->bytes, v->length)) continue;
        n_result saved = store_value(&id, now, size, &before, had);
        bytes_free(&before);
        if (saved < 0) say("a value the system changed cannot be kept", saved);
        else notify(k, &id, N_SETTING_CHANGED, 0);
    }
}

/* 受け口に来た相手を全部受ける。false は管理者が名前を閉じた知らせで、止まる。 */
static bool accept_connections(void) {
    for (;;) {
        n_handle end = n_inlet_accept(inlet, 0);
        if (end == -N_ERR_NOT_READY) return true;
        if (end == -N_ERR_CLOSED) return false;
        if (end >= 0) conn_add(end);
    }
}

static uint64_t next_deadline(void) {
    uint64_t soonest = N_CLOCK_NEVER;
    for (struct trial *t = trials; t; t = t->next)
        if (t->deadline < soonest) soonest = t->deadline;
    for (struct change *ch = changes; ch; ch = ch->next)
        if (ch->deadline < soonest) soonest = ch->deadline;
    return soonest;
}
static void run_deadlines(void) {
    uint64_t now = n_clock_monotonic();
    for (struct trial *t = trials, *next; t; t = next) {
        next = t->next;
        if (t->deadline <= now) trial_back(t);
    }
    for (struct change **p = &changes; *p;) {
        struct change *ch = *p;
        if (ch->deadline <= now) {
            *p = ch->next;
            change_finish(ch, -N_ERR_TIMEOUT);
        } else {
            p = &ch->next;
        }
    }
}

int main(void) {
    queue = n_events_create();
    message = malloc(N_SETTINGS_MESSAGE_MAX);
    if (queue < 0 || !message) return 1;
    store_cap = n_cap_find(N_CAP_STORE_PRIVATE);
    apply_cap = n_cap_find(N_CAP_SETTINGS_APPLY);
    for (size_t i = 0; i < sizeof settings_table / sizeof *settings_table; i++) {
        const struct n_setting_decl *d = &settings_table[i];
        struct known *k = decl_valid(d) ? known_add(d, 0) : NULL;
        if (!k) {
            say(d->name, -N_ERR_INVALID);
            continue;
        }
        k->kernel = d->apply != N_APPLY_NONE;
    }
    store_load();
    for (uint32_t domain = 0; domain < N_DOMAIN_COUNT; domain++) {
        kernel_watch[domain] = (n_handle)__syscall(SYS_SETTINGS_WATCH, domain, 0, 0, 0, 0);
        if (kernel_watch[domain] >= 0) n_events_watch(queue, kernel_watch[domain], N_EVENT_RECEIVABLE, TOKEN_WATCH_BASE + domain);
    }
    apply_machine_values();
    /* 値を読み終え、カーネルへ適用した後で名前を持つ。名前があることが準備の済んだ証拠。 */
    inlet = n_service_listen(N_SETTINGS_SERVICE);
    if (inlet < 0 || n_events_watch(queue, inlet, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, TOKEN_INLET) < 0) return 1;
    for (;;) {
        struct n_event events[32];
        n_result n = n_events_wait(queue, events, sizeof events / sizeof *events, next_deadline());
        if (n < 0) return 1;
        for (n_result i = 0; i < n; i++) {
            uint64_t token = events[i].token;
            if (token == TOKEN_INLET) {
                if (!accept_connections()) return 0;
            } else if (token >= TOKEN_WATCH_BASE && token < TOKEN_WATCH_BASE + N_DOMAIN_COUNT) {
                kernel_domain_changed((uint32_t)(token - TOKEN_WATCH_BASE));
            } else {
                struct conn *c = (struct conn *)(uintptr_t)(token & ~(uint64_t)1);
                if (c->closed) continue;
                if (token & 1) conn_send_one(c);
                else if (events[i].events & N_EVENT_RECEIVABLE) conn_receive(c);
                else conn_close(c);
            }
        }
        run_deadlines();
        while (dead_conns) {
            struct conn *c = dead_conns;
            dead_conns = c->next;
            free(c);
        }
    }
}
