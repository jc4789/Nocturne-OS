#include "kernel.h"
#include "arch/cpu.h"
#include "arch/fpu.h"

#define CR4_OSXSAVE (1ULL << 18)
#define LEGACY_MASK 3u
#define AVX_MASK 7u
#define AVX_OFFSET 576u
#define AVX_BYTES 256u

/* BSPで一度だけ選び、AP publication/task生成後には変更しない。 */
static uint32_t xstate_mask, state_bytes = 512;
static bool policy_ready;
static struct fpu_state initial_state;

const uint8_t *fpu_state_data(const struct fpu_state *state) {
    return (const uint8_t *)ALIGN_UP(state->storage, 64);
}

bool fpu_has_avx(void) { return xstate_mask == AVX_MASK; }

static bool legacy_supported(uint32_t *features) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    *features = c;
    return (d & ((1u << 0) | (1u << 24) | (1u << 25) | (1u << 26))) ==
                ((1u << 0) | (1u << 24) | (1u << 25) | (1u << 26));
}

static void legacy_setup(void) {
    write_cr0((read_cr0() & ~((1ULL << 2) | (1ULL << 3))) | (1ULL << 1) | (1ULL << 5));
    write_cr4((read_cr4() & ~CR4_OSXSAVE) | (1ULL << 9) | (1ULL << 10));
}

static bool xsave_supported(uint32_t features, uint32_t mask) {
    uint32_t a, b, c, d;
    if (!(features & (1u << 26))) return false;
    cpuid(0, 0, &a, &b, &c, &d);
    if (a < 0xD) return false;
    cpuid(0xD, 0, &a, &b, &c, &d);
    if ((a & mask) != mask || c < 576) return false;
    if (mask == AVX_MASK) {
        if (!(features & (1u << 28))) return false;
        cpuid(0xD, 2, &a, &b, &c, &d);
        /* 標準形式のYMM上半部。未知layout/XSS/XFDは採用しない。 */
        if (a != AVX_BYTES || b != AVX_OFFSET || (c & 5u)) return false;
    }
    return true;
}

static bool xsave_setup(uint32_t mask, uint32_t expected_bytes) {
    uint32_t a, b, c, d;
    write_cr4(read_cr4() | CR4_OSXSAVE);
    /* CPUID mask検査後だけXSETBV、OSXSAVE確認後だけXGETBVを実行する。 */
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(c & (1u << 27))) goto fail;
    xsetbv0(mask);
    if (xgetbv0() != mask) goto fail;
    cpuid(0xD, 0, &a, &b, &c, &d);
    if (b != expected_bytes || b > FPU_STATE_BYTES) goto fail;
    return true;
fail:
    /* XGETBVもFXSAVE modeで呼ばない。AVXはOSXSAVE clearで使用不可。 */
    write_cr4(read_cr4() & ~CR4_OSXSAVE);
    return false;
}

void fpu_save(struct fpu_state *state) {
    uint8_t *data = (uint8_t *)fpu_state_data(state);
    if (xstate_mask) {
        /* XSAVEはheader reserved/XCOMP_BVを初期化しない。標準形式だけを保持。 */
        memset(data + 512, 0, 64);
        __asm__ volatile("xsave64 (%0)" : : "r"(data), "a"(xstate_mask), "d"(0u) : "memory");
    } else {
        __asm__ volatile("fxsave64 (%0)" : : "r"(data) : "memory");
    }
}

void fpu_restore(const struct fpu_state *state) {
    const uint8_t *data = fpu_state_data(state);
    if (xstate_mask)
        __asm__ volatile("xrstor64 (%0)" : : "r"(data), "a"(xstate_mask), "d"(0u) : "memory");
    else
        __asm__ volatile("fxrstor64 (%0)" : : "r"(data) : "memory");
}

void fpu_state_init(struct fpu_state *state) {
    memset(state->storage, 0, sizeof state->storage);
    memcpy((void *)fpu_state_data(state), fpu_state_data(&initial_state), FPU_STATE_BYTES);
}

void fpu_reset(void) { fpu_restore(&initial_state); }

void fpu_init(void) {
    uint32_t features;
    ASSERT(!policy_ready);
    if (!legacy_supported(&features)) panic("cpu: x87/FXSR/SSE2 required by Nocturne");
    legacy_setup();
    if (!cmdline_has("noxsave") && xsave_supported(features, LEGACY_MASK)) {
        uint32_t wanted = !cmdline_has("noavx") && xsave_supported(features, AVX_MASK) ? AVX_MASK : LEGACY_MASK;
        uint32_t bytes = wanted == AVX_MASK ? AVX_OFFSET + AVX_BYTES : 576;
        if (xsave_setup(wanted, bytes)) {
            xstate_mask = wanted;
            state_bytes = bytes;
        }
    }
    uint8_t *data = (uint8_t *)fpu_state_data(&initial_state);
    memset(data, 0, FPU_STATE_BYTES);
    *(uint16_t *)(data + 0) = 0x037F; /* FXSAVE fallbackのx87 control、tag=empty */
    *(uint32_t *)(data + 24) = 0x1F80; /* XRSTORもMXCSRはmemoryから復帰する */
    /* XSAVE header XSTATE_BV=0で全許可componentをinitial状態へ。XMM/YMMも零。 */
    fpu_reset();
    policy_ready = true;
    kprintf("fpu: %s bytes=%u mask=%x AVX=%s\n", xstate_mask ? "XSAVE64" : "FXSAVE64",
            state_bytes, xstate_mask, fpu_has_avx() ? "enabled" : "disabled");
}

bool fpu_init_ap(void) {
    uint32_t features;
    if (!policy_ready || !legacy_supported(&features)) return false;
    legacy_setup();
    if (xstate_mask && (!xsave_supported(features, xstate_mask) || !xsave_setup(xstate_mask, state_bytes)))
        return false;
    fpu_reset();
    return true;
}
