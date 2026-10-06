/* Clang's freestanding 128-bit integer helpers (QuickJS BigInt limbs).
   No host compiler runtime or OS compatibility library is linked into Nocturne. */
#include <stdint.h>
#include <stdlib.h>

typedef unsigned __int128 u128;

static u128 divide128(u128 n, u128 d, u128 *remainder) {
    if (!d) abort();
    if (n < d) { if (remainder) *remainder = n; return 0; }
    uint64_t dh = (uint64_t)(d >> 64), dl = (uint64_t)d;
    uint64_t nh = (uint64_t)(n >> 64), nl = (uint64_t)n;
    if (!dh) {
        uint64_t qh = nh / dl, rh = nh % dl, ql, rl;
        /* rh < dl, so the quotient of this instruction fits in 64 bits. */
        __asm__("divq %4" : "=a"(ql), "=d"(rl) : "a"(nl), "d"(rh), "r"(dl) : "cc");
        if (remainder) *remainder = rl;
        return ((u128)qh << 64) | ql;
    }
    int shift = __builtin_clzll(dh) - __builtin_clzll(nh);
    d <<= shift;
    u128 q = 0;
    for (int i = shift; i >= 0; i--) {
        q <<= 1;
        if (n >= d) { n -= d; q |= 1; }
        d >>= 1;
    }
    if (remainder) *remainder = n;
    return q;
}

u128 __udivti3(u128 n, u128 d) { return divide128(n, d, NULL); }
u128 __umodti3(u128 n, u128 d) { u128 r; divide128(n, d, &r); return r; }
