/* HAL の ABI（docs/hal.md）：機械の装置の木を、カーネルとプログラムが同じ形で読む。

   装置の身元は性質（鍵と値）の並びである。鍵は 32 bit の名前で、バスを足す者が自分の名前を作る。HAL は鍵の意味を
   知らず、ここにバスの名前は無い。知られたバスの名前は hal_buses.h にある。 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 名前空間の 3 文字と、その中の番号。 */
#define N_HAL_KEY(a, b, c, n) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(n))

/* どの装置も持つ性質：バスの名前。 */
#define N_HAL_PROP_BUS N_HAL_KEY('h', 'a', 'l', 1)

/* 性質。size が 0 なら value は数。size が正なら value はその装置のバイトの区画の中の位置で、区画は
   カーネルでは struct hal_device、プログラムでは n_hal_device_bytes が持つ。同じ鍵が複数あってよい
   （ACPI の _CID）。 */
struct n_hal_prop {
    uint32_t key, size;
    uint64_t value;
};

/* 規則の一項：鍵の性質のどれかが合えば合う。数は (値 & mask) == value。バイトは完全に同じ並び。 */
struct n_hal_term {
    uint32_t key, size;
    uint64_t value, mask;
    const void *bytes;
};
/* 規則：全部の項が合えば合う。項の数が多いほど具体的で、具体的な規則の実装が先に試される。 */
struct n_hal_rule {
    const struct n_hal_term *terms;
    uint32_t count;
};
#define N_HAL_NUMBER(k, v) {.key = (k), .value = (v), .mask = UINT64_MAX}
#define N_HAL_MASKED(k, v, m) {.key = (k), .value = (v), .mask = (m)}
#define N_HAL_BYTES(k, p, n) {.key = (k), .size = (n), .bytes = (p)}
#define N_HAL_TEXT(k, s) {.key = (k), .size = sizeof(s) - 1, .bytes = (s)}
#define N_HAL_RULE(...)                                                                                            \
    {(const struct n_hal_term[]){__VA_ARGS__},                                                                     \
     sizeof((const struct n_hal_term[]){__VA_ARGS__}) / sizeof(struct n_hal_term)}

static inline bool n_hal_term_holds(const struct n_hal_term *t, const struct n_hal_prop *props, uint32_t count,
                                    const uint8_t *bytes) {
    for (uint32_t i = 0; i < count; i++) {
        const struct n_hal_prop *p = &props[i];
        if (p->key != t->key || p->size != t->size) continue;
        if (!t->size) {
            if ((p->value & t->mask) == t->value) return true;
            continue;
        }
        const uint8_t *a = bytes + p->value, *b = (const uint8_t *)t->bytes;
        uint32_t at = 0;
        while (at < t->size && a[at] == b[at]) at++;
        if (at == t->size) return true;
    }
    return false;
}

/* 合えば項の数、合わなければ 0。項の無い規則は何にも合わない。 */
static inline uint32_t n_hal_rule_score(const struct n_hal_rule *r, const struct n_hal_prop *props, uint32_t count,
                                        const uint8_t *bytes) {
    for (uint32_t i = 0; i < r->count; i++)
        if (!n_hal_term_holds(&r->terms[i], props, count, bytes)) return 0;
    return r->count;
}

/* 装置が持つ資源。slot はバスが決める番号で、PCI は BAR の番号、ACPI は _CRS の中の順である。 */
enum n_hal_resource_kind { N_HAL_MEMORY, N_HAL_PORT, N_HAL_GSI };
#define N_HAL_PREFETCHABLE 1u /* N_HAL_MEMORY：書込みをまとめてよい */
struct n_hal_resource {
    uint32_t kind, slot, flags, reserved;
    uint64_t base, size;
};

enum n_hal_state {
    N_HAL_UNBOUND,   /* 持つ実装が無い */
    N_HAL_ATTACHING, /* 実装が立ち上げている */
    N_HAL_RUNNING,   /* 実装が持っている */
    N_HAL_FAILED,    /* 実装が持てなかった、または止まった。理由は reason */
};

/* 実装がこの装置で提供するもの：ディスク、NIC、音の出口、画面など。kind は鍵と同じ形の名前で、label は人に
   見せる短い名である。種類の持ち主は種類の見出しに自分の名前を書く。 */
struct n_hal_offer {
    uint32_t kind, instance;
    char label[40];
};

/* SYS_HAL_LIST の一件。次の件は (char *)this + bytes にある。後ろに n_hal_prop、n_hal_offer、依存先の id
   （uint32_t）、バイトの区画の順に並ぶ。 */
struct n_hal_device {
    uint32_t bytes;
    uint32_t id;     /* 起動の間、二度と使わない番号 */
    uint32_t parent; /* 親の番号。根の子は根を指す。根の parent は 0 */
    uint32_t bus;    /* N_HAL_PROP_BUS と同じ */
    uint32_t state;  /* enum n_hal_state */
    int32_t reason;  /* N_HAL_FAILED の負の N_ERR_* */
    uint32_t prop_count, offer_count, depend_count, byte_count;
    char name[64];     /* 装置が言う名。"": 言わない */
    char location[48]; /* バスの中の場所。バスが書く字 */
    char driver[48];   /* 持っている実装の名。プロセスの実装はプログラムの名 */
} __attribute__((aligned(8)));

static inline const struct n_hal_prop *n_hal_device_props(const struct n_hal_device *d) {
    return (const struct n_hal_prop *)(d + 1);
}
static inline const struct n_hal_offer *n_hal_device_offers(const struct n_hal_device *d) {
    return (const struct n_hal_offer *)(n_hal_device_props(d) + d->prop_count);
}
static inline const uint32_t *n_hal_device_depends(const struct n_hal_device *d) {
    return (const uint32_t *)(n_hal_device_offers(d) + d->offer_count);
}
static inline const uint8_t *n_hal_device_bytes(const struct n_hal_device *d) {
    return (const uint8_t *)(n_hal_device_depends(d) + d->depend_count);
}
static inline const struct n_hal_device *n_hal_device_next(const struct n_hal_device *d) {
    return (const struct n_hal_device *)((const char *)d + d->bytes);
}
/* 鍵の最初の数。無ければ false。 */
static inline bool n_hal_device_number(const struct n_hal_device *d, uint32_t key, uint64_t *value) {
    const struct n_hal_prop *p = n_hal_device_props(d);
    for (uint32_t i = 0; i < d->prop_count; i++)
        if (p[i].key == key && !p[i].size) {
            *value = p[i].value;
            return true;
        }
    return false;
}
static inline uint32_t n_hal_device_rule_score(const struct n_hal_device *d, const struct n_hal_rule *r) {
    return n_hal_rule_score(r, n_hal_device_props(d), d->prop_count, n_hal_device_bytes(d));
}
