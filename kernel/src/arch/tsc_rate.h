/* What each maker's manual says a CPU's registers state of its TSC's rate: nominal, the crystal's
   tolerance and any spread spectrum not in it, so the clock holds it to a reference where there is
   one (sys/clock.c). arch/cpu.c reads the registers; here is only what they mean, so that
   tests/check_tsc_rate.py can run the manuals' cases on the host.

   A rate is num / den counts a second, as the manual's arithmetic gives it, unrounded. num 0 means
   these registers state none, and `by` says why. A family no manual at hand covers states none:
   the clock measures it. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct tsc_rate {
    uint64_t num, den;
    const char *by;
};

static inline void tsc_rate_none(struct tsc_rate *r, const char *why) {
    r->num = 0;
    r->den = 1;
    r->by = why;
}

/* ---- AMD ----
   The TSC counts at the P0 P-state's frequency: HWCR[TscFreqSel] (MSR C001_0015 bit 24, read-only),
   "1=The TSC increments at the P0 frequency". PStateDef[0] (MSR C001_0064) defines P0, PstateEn (bit
   63) saying the definition is valid. Its frequency fields are the same on every core of the
   coherent fabric, and HWCR[LockTscToCurrentP0] 0 keeps the TSC at P0's frequency, which nothing
   here changes.

   The fields differ by family:
   - Family 19h (PPR 56713, Model 61h; PPR 55898, Model 01h): CpuFid bits 7:0 (valid 10h-FFh),
     CpuDfsId bits 13:8 (valid 08h-1Ah and the even ones 1Ch-2Ch), the glossary's CoreCOF = FID / DID
     * 200 MHz. The register's own line in PPR 56713, "COF is defined as CpuFid * 5MHz", reaches
     1275 MHz at most and agrees with neither that glossary nor PPR 55898.
   - Family 1Ah (PPR 57238, Model 02h): CpuFid bits 11:0 (valid 010h-FFFh), CoreCOF = CpuFid * 5 MHz.
     No divisor. The other models of the family have no PPR at hand: a reference shows one that
     differs in its first window. */
#define TSC_AMD_TSC_FREQ_SEL (1ull << 24)
#define TSC_AMD_PSTATE_EN (1ull << 63)

/* Whether a manual at hand gives this family's P-state fields (and so both MSRs are read). */
static inline bool tsc_amd_family_known(unsigned family) { return family == 0x19 || family == 0x1A; }

static inline bool tsc_amd_did_valid(unsigned did) {
    return (did >= 0x08 && did <= 0x1A) || (did >= 0x1C && did <= 0x2C && !(did & 1));
}

/* hwcr and pstate_def0: the two MSRs of a family tsc_amd_family_known names. */
static inline void tsc_rate_amd(unsigned family, uint64_t hwcr, uint64_t pstate_def0, struct tsc_rate *r) {
    if (!(hwcr & TSC_AMD_TSC_FREQ_SEL)) return tsc_rate_none(r, "HWCR[TscFreqSel] is 0: the TSC does not count at P0");
    if (!(pstate_def0 & TSC_AMD_PSTATE_EN)) return tsc_rate_none(r, "PStateDef[0] is not valid (PstateEn 0)");
    if (family == 0x1A) {
        unsigned fid = (unsigned)pstate_def0 & 0xFFF;
        if (fid < 0x010) return tsc_rate_none(r, "PStateDef[0]'s CpuFid is reserved");
        r->num = 5000000ull * fid;
        r->den = 1;
    } else {
        unsigned fid = (unsigned)pstate_def0 & 0xFF, did = (unsigned)(pstate_def0 >> 8) & 0x3F;
        if (fid < 0x10 || !tsc_amd_did_valid(did)) return tsc_rate_none(r, "PStateDef[0]'s CpuFid or CpuDfsId is reserved");
        r->num = 200000000ull * fid;
        r->den = did;
    }
    r->by = "the P0 P-state";
}

/* ---- Intel ----
   CPUID leaf 15H (SDM vol. 2A, Table 3-8's leaf; vol. 1 22.7.3): EAX the denominator and EBX the
   numerator of the TSC / core crystal clock ratio, EBX 0 when it is not enumerated; ECX the nominal
   crystal frequency in Hz, 0 when it is not. Nominal TSC frequency = ECX * EBX / EAX. The models the
   SDM's Table 22-95 gives a crystal for lie below what Nocturne requires: one that enumerates no
   crystal is measured. */
static inline void tsc_rate_intel(uint32_t max_leaf, uint32_t eax, uint32_t ebx, uint32_t ecx, struct tsc_rate *r) {
    if (max_leaf < 0x15) return tsc_rate_none(r, "CPUID has no leaf 15H");
    if (!eax || !ebx) return tsc_rate_none(r, "CPUID leaf 15H enumerates no TSC / crystal ratio");
    if (!ecx) return tsc_rate_none(r, "CPUID leaf 15H enumerates no crystal frequency");
    r->num = (uint64_t)ecx * ebx;
    r->den = eax;
    r->by = "CPUID leaf 15H";
}
