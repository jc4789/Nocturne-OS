/* The extended state (XSTATE). The first CPU fixes the components and the area's layout once;
   every other CPU has to match it. The layout is the compacted form (SDM Vol. 1 13.4.3): the
   512-byte legacy region, the 64-byte header, then each enabled component from 2 up at its own
   size, 64-byte aligned where CPUID leaf 0xD says so. It is computed from those sizes, not read
   from the leaf's size of what is enabled now, which a hypervisor may report from stale state
   (QEMU's WHPX gives 0xD.0 EBX for the XCR0 before the last XSETBV). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/fpu.h"

/* x87, SSE, AVX, AVX-512's opmask, ZMM_Hi256 and Hi16_ZMM: what Nocturne enables (its floor has
   them all). Components the CPU has beyond these (PKRU, AMX) stay off. */
#define USER_COMPONENTS 0xE7UL
#define LEGACY_BYTES 512u
#define HEADER_BYTES 64u
#define XCOMP_COMPACTED (1ULL << 63)

struct fpu_state { uint8_t bytes[1]; }; /* fpu_state_bytes of them, 64-byte aligned */

static uint32_t area_bytes;
static uint32_t offset[64]; /* each enabled component's place in the compacted area */
static struct fpu_state *initial;
static uint8_t initial_storage[LEGACY_BYTES + HEADER_BYTES + 63] __attribute__((aligned(64)));

size_t fpu_storage_bytes(void) { return area_bytes + 63; }
struct fpu_state *fpu_state_in(void *storage) { return (struct fpu_state *)ALIGN_UP((uintptr_t)storage, 64); }

/* No supervisor component: IA32_XSS 0 (SDM vol. 1 13.1). CR0 and CR4 are cpu_setup's. */
#define MSR_XSS 0xDA0u

bool fpu_cpu_setup(void) {
    if (!area_bytes) return false;
    xsetbv0(USER_COMPONENTS);
    wrmsr(MSR_XSS, 0);
    if (xgetbv0() != USER_COMPONENTS) return false;
    fpu_reset();
    return true;
}

void fpu_init(void) {
    uint32_t a, b, c, d;
    cpuid(0xD, 0, &a, &b, &c, &d);
    uint64_t has = a | (uint64_t)d << 32;
    if ((has & USER_COMPONENTS) != USER_COMPONENTS) panic("fpu: XSAVE lacks components %lx", USER_COMPONENTS & ~has);
    uint32_t at = LEGACY_BYTES + HEADER_BYTES;
    for (unsigned i = 2; i < 64; i++) {
        if (!(USER_COMPONENTS & (1ULL << i))) continue;
        cpuid(0xD, i, &a, &b, &c, &d);
        if (c & 2) at = ALIGN_UP(at, 64);
        offset[i] = at;
        at += a;
    }
    area_bytes = at;
    /* The initial area: every component in its initial state (XSTATE_BV 0), which XRSTORS loads
       as the architecture's initial values; FCW and MXCSR are written too, so the area also reads
       as them. It is the header and the legacy region only: XRSTORS reads nothing past the header
       for a component whose XSTATE_BV bit is 0. */
    initial = fpu_state_in(initial_storage);
    memset(initial, 0, LEGACY_BYTES + HEADER_BYTES);
    *(uint16_t *)(initial->bytes + 0) = 0x037F;
    *(uint32_t *)(initial->bytes + 24) = 0x1F80;
    *(uint64_t *)(initial->bytes + LEGACY_BYTES + 8) = XCOMP_COMPACTED | USER_COMPONENTS;
    kprintf("fpu: XSAVES, components %lx, %u bytes a task\n", USER_COMPONENTS, area_bytes);
}

void fpu_state_init(struct fpu_state *state) {
    memset(state, 0, area_bytes);
    memcpy(state, initial, LEGACY_BYTES + HEADER_BYTES);
}

void fpu_save(struct fpu_state *state) {
    __asm__ volatile("xsaves64 (%0)" : : "r"(state), "a"((uint32_t)USER_COMPONENTS), "d"(0u) : "memory");
}

void fpu_restore(const struct fpu_state *state) {
    __asm__ volatile("xrstors64 (%0)" : : "r"(state), "a"((uint32_t)USER_COMPONENTS), "d"(0u) : "memory");
}

void fpu_reset(void) { fpu_restore(initial); }

const uint8_t *fpu_state_component(const struct fpu_state *state, unsigned component) {
    if (component < 2) return state->bytes;
    if (component >= 64 || !(USER_COMPONENTS & (1ULL << component))) return NULL;
    uint64_t present = *(const uint64_t *)(state->bytes + LEGACY_BYTES);
    return present & (1ULL << component) ? state->bytes + offset[component] : NULL;
}
