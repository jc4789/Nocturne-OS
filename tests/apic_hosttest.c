/* apic.cの実製品コード: IA32_APIC_BASEのmode遷移、IOMMUのためにx2APICを離れる判断、
   firmwareのx2APICロック検出、S3後とAPでのmodeの揃え方、どの状態からでも全レジスターを書く
   初期設定（SVR全体、使わないLVTのmask、その順序、AMDの8bit xAPIC ID）、宛先として使える
   APIC ID、APIC基底の幅（TMEのKeyID bitを含む）。MSR・CPUID・APICレジスター（xAPICの
   MMIOページとx2APICのMSRは同じ記憶）だけを模擬し、#GPになる操作、予約bitへの書込み、
   不正vectorのLVT、maskの外れたLVTを残したままのAPIC有効化は記録する。実機のAPICでの確認は別。 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "arch/cpu.h"

#define BASE_EN (1ull << 11)
#define BASE_X2 (1ull << 10)
#define X2 (BASE_EN | BASE_X2)
#define BASE_ADDR 0xFEE00000ull
#define MODE(base) ((base) & X2)
#define POISON 0x5A5A5A5Au   /* 未定義・firmwareの値: maskされていない（bit16が0）、EOI broadcast抑止（bit12） */
#define ABSENT 0xC0DEC0DEu   /* 実装されていないoffset: 書かれれば残る */
#define REG(off) xapic_page[(off) / 4]
#define INTEL_V 0x00060015u  /* Max LVT Entry 6（Nehalem以降の7 entry） */
#define AMD_V 0x80050010u    /* EAS、Max LVT Entry 5 */
#define AMD_EXT 0x00040007u  /* XLC 4、XAIDC、SNIC、INC */

/* firmware/Limineが渡した状態とCPUの性質。0の欄は既定（Intel、版INTEL_V、基底FEE00000h、39bit）。 */
struct machine {
    uint64_t mode; bool x2_cpu; uint32_t max_leaf, leaf7_d; uint64_t arch_cap, status; uint32_t id;
    bool amd; uint32_t version, ext_feature; bool cmci; uint64_t base; unsigned phys_bits;
    bool apic_bit_fixed; /* CPUID.1:EDX[9]がEN=0でも1のまま（SDM 11.4.3に反する仮想CPU） */
    bool refuses_leave;  /* x2ApicEnを消すと#GP（AMD PPR 56713のMSR0000_001Bの文言どおり） */
    bool no_mca;         /* CPUID.1:EDX[7]（MCE）とEDX[14]（MCA）が無い: IA32_MCG_CAPも無い */
    bool no_width_leaf;  /* CPUID 80000008hが無い */
    bool zero_width;     /* CPUID.80000008H:EAX[7:0]が0（実在のCPUには無い値） */
    uint32_t leaf7_c;    /* CPUID.(EAX=7,ECX=0):ECX。bit 13はIntelのTME（IA32_TME_ACTIVATEがある） */
    uint64_t tme;        /* IA32_TME_ACTIVATE */
    uint32_t signature;  /* CPUID.1:EAX（family、0は既定の0） */
    unsigned nodes;      /* AMDのnorthbridge（bus 0、device 18h〜）の数 */
    uint32_t f68[2];     /* 各nodeのF0x68（bit 18 ApicExtId、bit 17 ApicExtBrdCst） */
};

static struct machine now;
static uint64_t apic_base, disable_status, icr;
static bool gp;              /* #GPになる操作（不正な遷移、無い・使えないMSR、読出し専用への書込み） */
static unsigned base_writes, conflict_calls, refused; /* refused: wrmsr_checkedが#GPを回収した回数 */
static unsigned icr_writes;   /* x2APICのICRへの送信回数 */
static bool illegal_vector;   /* Fixed（配送modeの無いentryを含む）のLVTへvector 0〜15（SDM 11.5.2: maskされていても） */
static bool enabled_unmasked; /* SVRでAPICを有効にした時、maskの外れたLVT entryが残っていた */
static bool reserved_bits;    /* xAPICで予約bitを1にして書いた（x2APICでは#GP） */
static const char *conflict;
static uint32_t xapic_page[1024];
static bool mapped, no_apic;
static uint64_t mapped_at;
static char messages[1024];
static jmp_buf panic_at;
static bool panicked;

static uint32_t version(void) { return now.version ? now.version : INTEL_V; }
static uint64_t base_addr(void) { return now.base ? now.base : BASE_ADDR; }
static bool eas(void) { return now.amd && (version() & (1u << 31)); }

static bool status_listed(void) {
    return now.max_leaf >= 7 && (now.leaf7_d & (1u << 29)) && (now.arch_cap & (1ull << 21));
}

/* このAPICにあるレジスター（SDM表11-6、APM表16-6）。CMCIのLVTはMCG_CAP[10]のIntelだけ
   （AMDではそのbitは予約: APM 9.3.1.1）。 */
static bool implemented(uint32_t reg, bool write) {
    unsigned max_lvt = (version() >> 16) & 0xFF;
    switch (reg) {
    case 0x20: case 0x30: case 0x390: return !write;
    case 0xB0: return write;
    case 0x80: case 0xF0: case 0x280: case 0x300: case 0x320: case 0x350: case 0x360: case 0x370:
    case 0x310: case 0x380: case 0x3E0: return true; /* 0x310: xAPICのICR上位（x2APICでは0x830の64bit） */
    case 0x2F0: return now.cmci && !now.amd;
    case 0x330: return max_lvt >= 5;
    case 0x340: return max_lvt >= 4;
    case 0x400: return eas() && !write;
    case 0x410: return eas();
    }
    return reg >= 0x100 && reg < 0x280 && !write; /* ISR, TMR, IRR */
}

/* 書込み可能なレジスターを未定義の値に、読出し専用を正しい値に、無いoffsetを印にする */
static void fill_registers(void) {
    for (uint32_t off = 0; off < 0x1000; off += 0x10)
        REG(off) = implemented(off, true) ? POISON : implemented(off, false) ? 0 : ABSENT;
    REG(0x20) = now.id << 24;
    REG(0x30) = version();
    if (eas()) REG(0x400) = now.ext_feature, REG(0x410) = POISON & 7; /* 予約bitは0で読める */
}

/* 書込みで値を持つ欄。LVT: vector、配送mode、LINTのtriggerと極性、timerのmode、mask。読出し
   専用の配送状態・remote IRRは書かない。Intel（SDM図11-8）: timerのmodeは18:17、timerとerrorに
   配送modeは無い。AMD（PPR 56713 MSR0000_0832〜0837）: timerのmodeはbit 17だけ、LINTのbit 13
   （極性）は予約、errorにMsgType 10:8がある。0: 欄の検査をしないレジスター。 */
static uint32_t lvt_fields(uint32_t reg) {
    switch (reg) {
    case 0x320: return now.amd ? 0x300FF : 0x700FF;
    case 0x350: case 0x360: return now.amd ? 0x187FF : 0x1A7FF;
    case 0x370: return now.amd ? 0x107FF : 0x100FF;
    case 0x2F0: case 0x330: case 0x340: return 0x107FF;
    }
    return 0;
}
static bool fields_ok(uint32_t reg, uint32_t v) {
    if (lvt_fields(reg)) return !(v & ~lvt_fields(reg));
    switch (reg) {
    case 0x80: return v <= 0xFF;
    case 0xB0: return v == 0;
    case 0xF0: return !(v & ~(0x3FFu | (version() & (1u << 24) ? 0x1000u : 0))); /* bit 12はEOI broadcast抑止のあるAPICだけ */
    case 0x3E0: return !(v & ~0xBu);
    case 0x410: return v <= 7;
    }
    return true;
}

/* 書ける値として受けた後の記憶への書込み: 無効なAPIC（SVR[8]=0）のLVTはmaskが外れない
   （SDM 11.4.7.2、APM 16.4.7）。APICを有効にする時点のLVTも記録する。 */
static void apic_store(uint32_t reg, uint32_t v) {
    uint32_t fields = lvt_fields(reg);
    if (fields) {
        bool fixed = !(fields & 0x700) || !(v & 0x700);
        if (fixed && (v & 0xFF) < 16) illegal_vector = true;
        if (!(REG(0xF0) & 0x100)) v |= 1u << 16;
    }
    if (reg == 0xF0 && (v & 0x100) && !(REG(0xF0) & 0x100))
        for (uint32_t r = 0x2F0; r <= 0x370; r += 0x10)
            if (lvt_fields(r) && implemented(r, true) && !(REG(r) & (1u << 16))) enabled_unmasked = true;
    REG(reg) = v;
}

static void model_mmio_write32(volatile void *p, uint32_t v) {
    uint32_t reg = (uint32_t)((volatile uint32_t *)p - xapic_page) * 4;
    if (!fields_ok(reg, v)) reserved_bits = true;
    apic_store(reg, v);
}

static bool absent_untouched(void) {
    for (uint32_t off = 0; off < 0x1000; off += 0x10)
        if (!implemented(off, true) && !implemented(off, false) && REG(off) != ABSENT) return false;
    return true;
}

/* 無効状態（EN=0, EXTD=0）を経ずにx2APICからxAPICへ、無効状態から直接x2APICへは不正。
   EN=0, EXTD=1も無効な組合せ。ロック中はx2APICから出られない。x2APICを出ると、IDの他の
   レジスターは保たれない（SDM 11.12.5）。 */
static void model_wrmsr(uint32_t msr, uint64_t v) {
    if (msr == 0x1B) {
        uint64_t from = MODE(apic_base), to = MODE(v);
        bool locked = status_listed() && (disable_status & 1);
        if (to == BASE_X2 || (from == X2 && to == BASE_EN) || (!from && to == X2) ||
            (locked && to != X2) || (to == X2 && !now.x2_cpu) || (v & ~(X2 | 0x100 | 0xFFFFFFFFFF000ull)) ||
            (now.refuses_leave && from == X2 && to != X2)) {
            gp = true;
            return;
        }
        if (from == X2 && !to) fill_registers();
        apic_base = v;
        base_writes++;
        return;
    }
    uint32_t reg = (msr - 0x800) << 4;
    if (msr < 0x800 || msr > 0x8FF || MODE(apic_base) != X2 || reg == 0x310 || !implemented(reg, true)) {
        gp = true;
        return;
    }
    if (reg == 0x300) {   /* ICR: vector、配送mode、宛先mode、level、trigger、shorthand、宛先 63:32 */
        if (v & ~0xFFFFFFFF000CCFFFull) gp = true;
        else icr = v, icr_writes++;
        return;
    }
    if ((v >> 32) || !fields_ok(reg, (uint32_t)v)) { /* 予約bit（63:32を含む）は#GP（SDM表11-6注2） */
        gp = true;
        return;
    }
    apic_store(reg, (uint32_t)v);
}

static uint64_t model_rdmsr(uint32_t msr) {
    if (msr == 0x1B) return apic_base;
    if (msr == 0x179 && !now.no_mca) return now.cmci ? 1ull << 10 | 6 : 6;  /* IA32_MCG_CAP: MCG_CMCI_P（AMDでは予約bitが1）、bank数 */
    if (msr == 0x982 && !now.amd && now.max_leaf >= 7 && (now.leaf7_c & (1u << 13))) return now.tme;
    if (msr == 0x10A && now.max_leaf >= 7 && (now.leaf7_d & (1u << 29))) return now.arch_cap;
    if (msr == 0xBD && status_listed()) return disable_status;
    uint32_t reg = (msr - 0x800) << 4;
    if (msr >= 0x800 && msr <= 0x8FF && MODE(apic_base) == X2 && reg != 0x310 && implemented(reg, false))
        return reg == 0x20 ? now.id : reg == 0x300 ? icr : REG(reg);
    gp = true;
    return 0;
}

/* 最大より上のleafは全bit（確かめずに読めば誤る）。EN=0ではCPUID.1:EDX[9]が0（SDM 11.4.3）。 */
static void model_cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    (void)sub;
    *a = *b = *c = *d = 0;
    if (leaf >= 0x80000000) {
        uint32_t top = now.no_width_leaf ? 0x80000004 : 0x80000008;
        if (leaf > top) *a = *b = *c = *d = UINT32_MAX;
        if (leaf == 0x80000000) *a = top;
        if (leaf == 0x80000008 && !now.no_width_leaf) *a = now.zero_width ? 0 : now.phys_bits ? now.phys_bits : 39;
        return;
    }
    if (leaf > now.max_leaf) {
        *a = *b = *c = *d = UINT32_MAX;
        return;
    }
    if (leaf == 0) {
        *a = now.max_leaf;
        if (now.amd) *b = 0x68747541, *d = 0x69746E65, *c = 0x444D4163;  /* AuthenticAMD */
        else *b = 0x756E6547, *d = 0x49656E69, *c = 0x6C65746E;          /* GenuineIntel */
    }
    if (leaf == 1) {
        *a = now.signature;
        *c = now.x2_cpu ? 1u << 21 : 0;
        *d = (now.no_mca ? 0 : 1u << 14 | 1u << 7) | (!no_apic && ((apic_base & BASE_EN) || now.apic_bit_fixed) ? 1u << 9 : 0);
    }
    if (leaf == 7) *c = now.leaf7_c, *d = now.leaf7_d;
}

#define rdmsr model_rdmsr
#define wrmsr model_wrmsr
#define cpuid model_cpuid
#define mmio_write32 model_mmio_write32
#include "../kernel/src/arch/apic.c"
#undef rdmsr
#undef wrmsr
#undef cpuid
#undef mmio_write32

static unsigned checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "失敗 %d行: %s\n", __LINE__, #c); } } while (0)

int kprintf(const char *format, ...) {
    size_t used = strlen(messages);
    va_list ap;
    va_start(ap, format);
    vsnprintf(messages + used, sizeof messages - used, format, ap);
    va_end(ap);
    return 0;
}
void panic(const char *format, ...) {
    (void)format;
    panicked = true;
    longjmp(panic_at, 1);
}
/* #GPを例外処理で回収する書込み（isr.asmとisr_dispatch）: 模擬では#GPを記録せず失敗を返す */
bool wrmsr_checked(uint32_t msr, uint64_t v) {
    bool before = gp;
    gp = false;
    model_wrmsr(msr, v);
    bool ok = !gp;
    refused += !ok;
    gp = before;
    return ok;
}
const char *iommu_x2apic_conflict(void) {
    conflict_calls++;
    return conflict;
}
void *vmm_map_mmio(uint64_t phys, uint64_t size) {
    if (size > sizeof xapic_page) return NULL;
    mapped = true;
    mapped_at = phys;
    return xapic_page;
}
/* 構成空間はAMDのnorthbridgeだけ（BKDG 32559 4.1: bus 0、device 18h＋node、function 0） */
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off) {
    if (bus || func || dev < 0x18 || dev - 0x18u >= now.nodes) return UINT32_MAX;
    return off == 0 ? 0x11001022u : off == 0x68 ? now.f68[dev - 0x18] : 0;
}
uint64_t hv_apic_hz(void) { return 0; }
uint64_t hv_tsc_hz(void) { return 0; }

static void reset(const struct machine *m) {
    now = *m;
    apic_base = base_addr() | m->mode;
    disable_status = m->status;
    fill_registers();
    gp = mapped = panicked = no_apic = false;
    illegal_vector = enabled_unmasked = reserved_bits = false;
    mapped_at = 0;
    conflict = NULL;
    icr = 0;
    base_writes = conflict_calls = refused = icr_writes = 0;
    messages[0] = 0;
    available = x2_mode = amd = cmci = false;
    xapic_id_max = 0xFE;
    lapic = NULL;
    timer_reload = 0;
}

static bool init(const struct limine_mp_response *mp) {
    if (setjmp(panic_at)) return false;
    apic_init(mp);
    return true;
}

/* lapic_setupの結果: 全レジスターが元の値によらず決まる。無いレジスターには触れない。maskした
   LVTも合法なvectorを持ち、APICはLVTを全部maskしてから有効にし、LINTのmaskはその後に外す
   （無効なAPICでは外れない）。 */
static bool set_up(bool bsp) {
    unsigned max_lvt = (version() >> 16) & 0xFF;
    return REG(0x80) == 0 && REG(0xF0) == 0x1FF && REG(0x320) == 0x100FE &&
           REG(0x350) == (bsp ? 0x700u : 0x100FEu) && REG(0x360) == (bsp ? 0x400u : 0x100FEu) &&
           REG(0x370) == 0x100FE && (max_lvt < 4 || REG(0x340) == 0x100FE) &&
           (max_lvt < 5 || REG(0x330) == 0x100FE) && (!implemented(0x2F0, true) || REG(0x2F0) == 0x100FE) &&
           !illegal_vector && !enabled_unmasked && !reserved_bits && absent_untouched();
}

static const struct machine plain_x2 = {X2, true, 7, 0, 0, 0, 3};

static void test_leave_decision(void) {
    struct limine_mp_info small = {.lapic_id = 0xFE}, big = {.lapic_id = 0xFF}, zero = {.lapic_id = 0};
    struct limine_mp_info *two_small[] = {&zero, &small}, *with_big[] = {&zero, &big};
    struct limine_mp_response mp_small = {.cpu_count = 2, .cpus = two_small};
    struct limine_mp_response mp_big = {.cpu_count = 2, .cpus = with_big};
    const uint32_t ac = 1u << 29;              /* CPUID.7.EDX[29]: IA32_ARCH_CAPABILITIES */
    const uint64_t xs = 1ull << 21;            /* IA32_ARCH_CAPABILITIES[21]: IA32_XAPIC_DISABLE_STATUS */
    const struct {
        struct machine m; const char *conflict; const struct limine_mp_response *mp;
        bool x2, asked; const char *message;
    } cases[] = {
        {plain_x2, NULL, &mp_small, true, true, "x2APIC MSR id 3"},
        {plain_x2, "IOMMU", &mp_small, false, true, "leaving x2APIC mode for xAPIC mode, as IOMMU"},
        {plain_x2, "IOMMU", NULL, false, true, "leaving x2APIC"},
        {{X2, true, 7, ac, xs, 1, 3}, "IOMMU", &mp_small, true, true, "firmware locked x2APIC"},
        {{X2, true, 7, ac, xs, 0, 3}, "IOMMU", &mp_small, false, true, "leaving"},  /* 列挙だけ */
        {{X2, true, 7, ac, 0, 1, 3}, "IOMMU", &mp_small, false, true, "leaving"},   /* 0xBDは無い */
        {{X2, true, 7, 0, xs, 1, 3}, "IOMMU", &mp_small, false, true, "leaving"},   /* 0x10Aは無い */
        {{X2, true, 6, ac, xs, 1, 3}, "IOMMU", &mp_small, false, true, "leaving"},  /* leaf 7が無い */
        {{X2, true, 7, 0, 0, 0, 0xFF}, "IOMMU", &mp_small, true, true, "this CPU's APIC ID"},
        {{X2, true, 7, 0, 0, 0, 0x100}, "IOMMU", NULL, true, true, "this CPU's APIC ID"},
        {plain_x2, "IOMMU", &mp_big, true, true, "a CPU's APIC ID"},
        {{BASE_EN, true, 7, 0, 0, 0, 3}, "IOMMU", &mp_small, false, false, "id 3, PIC"},
        {{BASE_EN, false, 7, 0, 0, 0, 3}, NULL, NULL, false, false, "xAPIC at"},
        /* AMD: 8bit IDがあれば0xFEまで、無ければ下位4bitだけ（0x0Fは放送） */
        {{X2, true, 7, 0, 0, 0, 3, true, AMD_V, AMD_EXT}, "IOMMU", &mp_small, false, true, "leaving"},
        {{X2, true, 7, 0, 0, 0, 3, true, AMD_V, 0x00040003}, "IOMMU", &mp_small, true, true, "a CPU's APIC ID"},
        {{X2, true, 7, 0, 0, 0, 0x0F, true, AMD_V, 0x00040003}, "IOMMU", NULL, true, true, "this CPU's APIC ID"},
        {{X2, true, 7, 0, 0, 0, 0x0E, true, AMD_V, 0x00040003}, "IOMMU", NULL, false, true, "(4-bit IDs)"},
        {{X2, true, 7, 0, 0, 0, 0x0E, true, 0x00050010}, "IOMMU", NULL, false, true, "(4-bit IDs)"}, /* EASが無い */
        {{X2, true, 7, 0, 0, 0, 0x0F, true, 0x00050010}, "IOMMU", NULL, true, true, "this CPU's APIC ID"},
    };
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        reset(&cases[i].m);
        conflict = cases[i].conflict;
        CHECK(init(cases[i].mp));
        CHECK(!gp);
        CHECK(lapic_present());
        CHECK(lapic_x2apic() == cases[i].x2);
        CHECK(MODE(apic_base) == (cases[i].x2 ? X2 : BASE_EN));
        CHECK((apic_base & ~X2) == BASE_ADDR);                  /* 基底は変えない */
        CHECK(mapped == !cases[i].x2);                          /* x2APICではMMIOを使わない */
        CHECK(cases[i].x2 || mapped_at == BASE_ADDR);
        CHECK((conflict_calls != 0) == cases[i].asked);
        CHECK(strstr(messages, cases[i].message) != NULL);
        CHECK(lapic_current_id() == now.id);
        CHECK(set_up(true));
        CHECK(base_writes == (cases[i].x2 || cases[i].m.mode == BASE_EN ? 0u : 2u));
        /* AMDのxAPIC modeでは、あるときだけ8bit IDを使う */
        bool wide = !now.amd || (eas() && (now.ext_feature & 4));
        CHECK(cases[i].x2 || !eas() || ((REG(0x410) & 4) != 0) == wide);
        CHECK(lapic_addressable(0x0E) && !lapic_addressable(UINT32_MAX));
        CHECK(lapic_addressable(0x0F) == (cases[i].x2 || wide));
        CHECK(lapic_addressable(0xFE) == (cases[i].x2 || wide));
        CHECK(lapic_addressable(0xFF) == cases[i].x2);
    }

    /* APICが無い、全体で無効: 何も書かず、使わない */
    struct machine off = {0, true, 7, 0, 0, 0, 3};
    reset(&off);
    CHECK(init(NULL) && !lapic_present() && !base_writes && !gp && strstr(messages, "no local APIC"));
    off.apic_bit_fixed = true;
    reset(&off);
    CHECK(init(NULL) && !lapic_present() && !base_writes && !gp && strstr(messages, "globally disabled"));
    reset(&plain_x2);
    no_apic = true;
    CHECK(init(NULL) && !lapic_present() && !base_writes && !gp && strstr(messages, "no local APIC"));
    /* x2APICなのにCPUが対応を示さない: 進めない */
    struct machine odd = plain_x2;
    odd.x2_cpu = false;
    reset(&odd);
    CHECK(!init(NULL) && panicked && !gp);
}

/* 全レジスターの初期設定: SVRは全体を書き（firmwareのEOI broadcast抑止を残さない）、使わない
   LVTはあるものだけをmaskする。CMCIはMCG_CAP[10]のIntelだけ、AMDの拡張空間はEASのときだけ。 */
static void test_setup(void) {
    const struct machine machines[] = {
        {X2, true, 7, 0, 0, 0, 3, false, INTEL_V, 0, true},
        {BASE_EN, true, 7, 0, 0, 0, 3, false, INTEL_V, 0, true},
        {X2, true, 7, 0, 0, 0, 3, false, 0x00050014},             /* Max LVT 5、CMCIなし */
        {BASE_EN, true, 7, 0, 0, 0, 3, false, 0x00030014},        /* Max LVT 3: 性能・熱のLVTもなし */
        {X2, true, 7, 0, 0, 0, 3, true, 0x80060010, AMD_EXT},     /* Max LVT 6でもAMDに0x2F0は無い */
        {X2, true, 7, 0, 0, 0, 3, true, 0x80060010, AMD_EXT, true}, /* AMDの予約bit MCG_CAP[10]が1でも */
        {X2, true, 7, 0, 0, 0, 3, true, 0x00050010},              /* EASなし: 0x840を読まない */
    };
    for (size_t i = 0; i < ARRAY_SIZE(machines); i++)
        for (int leave = 0; leave <= 1; leave++) {
            reset(&machines[i]);
            conflict = leave ? "IOMMU" : NULL;
            CHECK(init(NULL));
            CHECK(!gp);
            CHECK(set_up(true));
            CHECK(lapic_x2apic() == (machines[i].mode == X2 && !leave));
        }

    /* x2ApicEnを消せないCPU: 退出の書込みが#GPになっても進み、x2APICのまま使う */
    struct machine refusing = {X2, true, 7, 0, 0, 0, 3, true, AMD_V, AMD_EXT, false, 0, 0, false, true};
    reset(&refusing);
    conflict = "IOMMU";
    CHECK(init(NULL) && !gp && lapic_x2apic() && MODE(apic_base) == X2 && !base_writes && refused == 1);
    CHECK(strstr(messages, "refused to leave it") != NULL && set_up(true));
    /* MCE・MCAの無いCPUではIA32_MCG_CAPを読まない（読めば#GP） */
    struct machine bare = {X2, true, 7, 0, 0, 0, 3, false, 0x00040014};
    bare.no_mca = true;
    reset(&bare);
    CHECK(init(NULL) && !gp && set_up(true));

    /* xAPICの基底: MAXAPICADDR-1:12（SDM 093版13.4.4）。AMDの51:12（APM 16.3.1）もPhysAddrSize
       まで（PPR 56713 MSR0000_001B: 48bitの機種では63:48が予約）。実機のMSRは幅の外に1を持たない
       が、幅を誤れば別の番地を写すことを見るため、幅の外にも1を置く */
    const uint32_t tme_cpu = 1u << 13;                     /* CPUID.7.0:ECX[13] */
    const uint64_t tdx_keys = 1ull << 36 | 6ull << 32 | 3; /* KeyID 6bit、うちTDX 1bit、有効・lock */
    const struct { struct machine m; uint64_t at; } bases[] = {
        {{.mode = BASE_EN, .max_leaf = 7, .id = 3, .base = 0x7FFEE00000ull, .phys_bits = 39}, 0x7FFEE00000ull},
        {{.mode = BASE_EN, .max_leaf = 7, .id = 3, .base = 0xFFFEE00000ull, .phys_bits = 40}, 0xFFFEE00000ull},
        {{.mode = BASE_EN, .max_leaf = 7, .id = 3, .base = 0xFFFEE00000ull, .phys_bits = 39}, 0x7FFEE00000ull},
        /* 80000008hが無い、またはEAX[7:0]が0か32未満: 36bit */
        {{.mode = BASE_EN, .max_leaf = 7, .id = 3, .base = 0x10FEE00000ull, .no_width_leaf = true}, BASE_ADDR},
        {{.mode = BASE_EN, .max_leaf = 7, .id = 3, .base = 0x10FEE00000ull, .zero_width = true}, BASE_ADDR},
        {{.mode = BASE_EN, .max_leaf = 7, .id = 3, .base = 0x10FEE00000ull, .phys_bits = 20}, BASE_ADDR},
        /* leafの無いCPUの36bitからもKeyIDの2bitを引く: 34bit */
        {{.mode = BASE_EN, .max_leaf = 7, .id = 3, .base = 0x14FEE00000ull, .no_width_leaf = true,
          .leaf7_c = tme_cpu, .tme = 1ull << 36 | 2ull << 32 | 3}, 0xFEE00000ull},
        /* TDXのKeyIDがあればKeyIDの6bitを引いた40bit。無ければ、またはleaf 7が無ければ46bitのまま */
        {{.mode = BASE_EN, .max_leaf = 7, .id = 3, .base = 0x2080FEE00000ull, .phys_bits = 46,
          .leaf7_c = tme_cpu, .tme = tdx_keys}, 0x80FEE00000ull},
        {{.mode = BASE_EN, .max_leaf = 7, .id = 3, .base = 0x2080FEE00000ull, .phys_bits = 46,
          .leaf7_c = tme_cpu, .tme = 6ull << 32 | 3}, 0x2080FEE00000ull},
        {{.mode = BASE_EN, .max_leaf = 6, .id = 3, .base = 0x2080FEE00000ull, .phys_bits = 46,
          .leaf7_c = tme_cpu, .tme = tdx_keys}, 0x2080FEE00000ull},
        /* AMD: PhysAddrSizeの48bitまで。TMEのMSRは読まない（ECX[13]はAMDでは予約） */
        {{.mode = BASE_EN, .max_leaf = 7, .id = 3, .amd = true, .version = AMD_V, .ext_feature = AMD_EXT,
          .base = 1ull << 49 | 0xF0FFEE00000ull, .phys_bits = 48, .leaf7_c = tme_cpu, .tme = tdx_keys}, 0xF0FFEE00000ull},
        {{.mode = BASE_EN, .max_leaf = 7, .id = 3, .amd = true, .version = AMD_V, .ext_feature = AMD_EXT,
          .base = 0x10FEE00000ull, .zero_width = true}, BASE_ADDR},
    };
    for (size_t i = 0; i < ARRAY_SIZE(bases); i++) {
        reset(&bases[i].m);
        CHECK(init(NULL) && !gp && mapped && mapped_at == bases[i].at && set_up(true));
    }

    /* 宛先にできないIDへは送らない: AMDの4bit IDで0x0Fは放送になる。lapic_send_ipiは
       割込み禁止（cli）を使うためホストでは呼べず、同じ判定のINITで確かめる。 */
    tsc_hz = 1000000;
    struct machine narrow = {BASE_EN, true, 7, 0, 0, 0, 3, true, AMD_V, 0x00040003};
    reset(&narrow);
    CHECK(init(NULL));
    REG(0x300) = 0;
    CHECK(!lapic_send_init(0x0F) && REG(0x310) == POISON && REG(0x300) == 0);
    CHECK(lapic_send_init(0x0E) && REG(0x310) == 0x0E000000 && REG(0x300) == 0x4500);
    CHECK(lapic_send_startup(0x0E, 0x9000) && REG(0x310) == 0x0E000000 && REG(0x300) == 0x4609);
    CHECK(!lapic_send_startup(0x0F, 0x9000) && !lapic_send_startup(0x0E, 0x9001) &&
          !lapic_send_startup(0x0E, 0x100000) && REG(0x300) == 0x4609);
    /* xAPICではSTARTUPが届いたCPUが自分のID registerを読む（wake.h）。x2APICでは宛先だけで決まる */
    struct lapic_ident ident;
    lapic_ident(&ident);
    CHECK(!ident.x2 && ident.id_reg == &REG(LAPIC_ID) && ident.id_mask == 0x0F && !ident.xaidn);
    CHECK(ident.base == (BASE_ADDR | BASE_EN) && ident.base_mask == (0x7FFFFFF000ull | BASE_EN));
    reset(&plain_x2);
    CHECK(init(NULL));
    CHECK(!lapic_send_init(UINT32_MAX) && icr == 0);
    CHECK(lapic_send_init(0x100) && icr == (0x100ull << 32 | 0x4500) && !gp);
    icr_writes = 0;
    CHECK(lapic_send_startup(0x100, 0x8000) && icr == (0x100ull << 32 | 0x4608) && icr_writes == 2 && !gp);
    lapic_ident(&ident);
    CHECK(ident.x2 && !ident.id_reg && !ident.xaidn);
}

/* AMDのxAPICで使えるIDの幅。0Fh・10hは全nodeのnorthbridge（F0x68のApicExtIdとApicExtBrdCst）
   だけで8bit（10hのExtApicIdEnは任意）、15hはそれとExtApicIdEn（ExtApicIdCapのあるとき、BKDG
   42300 APIC410）、他の機種はAPMのXAIDCとXAIDN。XAIDNはINITで消えるのでトランポリンにも渡す。
   4bitでは、ID registerのbit 7:4は予約（読んだ値を使わない）。 */
static void test_amd_ids(void) {
    const uint32_t both = 3u << 17, narrow_ext = 0x00040003; /* narrow_ext: XAIDCなし */
    const struct { uint32_t signature, ext; unsigned nodes; uint32_t f68_0, f68_1; bool wide, xaidn; } cases[] = {
        {0x00000F00, 0, 2, both, both, true, false},                /* 0Fh: northbridgeだけ */
        {0x00000F00, AMD_EXT, 2, both, 1u << 18, false, false},     /* 一つのnodeにApicExtBrdCstが無い */
        {0x00000F00, AMD_EXT, 0, 0, 0, false, false},               /* northbridgeが見えない */
        {0x00100F00, narrow_ext, 1, both, 0, true, false},          /* 10h: F0x68だけで8bit */
        {0x00100F00, AMD_EXT, 1, 1u << 17, 0, false, false},
        {0x00600F00, AMD_EXT, 2, both, both, true, true},           /* 15h: F0x68とExtApicIdEn */
        {0x00600F00, narrow_ext, 2, both, both, false, false},      /* ExtApicIdCapが無い */
        {0x00600F00, AMD_EXT, 2, both, 0, false, false},            /* F0x68が一部のnodeだけ */
        {0x00600F00, AMD_EXT, 0, 0, 0, false, false},
        {0x00700F00, AMD_EXT, 0, 0, 0, true, true},                 /* 16h: APMどおり */
        {0x00700F00, narrow_ext, 0, 0, 0, false, false},
        {0x00800F00, AMD_EXT, 0, 0, 0, true, true},                 /* 17h */
        {0x00A00F00, AMD_EXT, 2, 0, 0, true, true},                 /* 19h: northbridgeは見ない */
        {0x00A00F00, narrow_ext, 2, both, both, false, false},
    };
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        struct machine m = {BASE_EN, true, 7, 0, 0, 0, 3, true, AMD_V, cases[i].ext};
        m.signature = cases[i].signature;
        m.nodes = cases[i].nodes;
        m.f68[0] = cases[i].f68_0;
        m.f68[1] = cases[i].f68_1;
        reset(&m);
        CHECK(init(NULL) && !gp && set_up(true));
        CHECK(lapic_addressable(0x0E) && lapic_addressable(0xFE) == cases[i].wide);
        CHECK(((REG(0x410) & 4) != 0) == cases[i].xaidn);
        struct lapic_ident ident;
        lapic_ident(&ident);
        CHECK(!ident.x2 && ident.id_reg == &REG(0x20) && ident.xaidn == cases[i].xaidn);
        CHECK(ident.id_mask == (cases[i].wide ? 0xFFu : 0x0Fu));
        REG(0x20) = 0xA3u << 24;
        CHECK(lapic_current_id() == (cases[i].wide ? 0xA3u : 0x03u));
    }
    /* x2APICから出る判断も同じ幅で: 15hでF0x68が無ければID 0x0Fは使えない */
    struct machine m = {X2, true, 7, 0, 0, 0, 0x0F, true, AMD_V, AMD_EXT};
    m.signature = 0x00600F00;
    reset(&m);
    conflict = "IOMMU";
    CHECK(init(NULL) && lapic_x2apic() && strstr(messages, "this CPU's APIC ID"));
    m.nodes = 1;
    m.f68[0] = both;
    reset(&m);
    conflict = "IOMMU";
    CHECK(init(NULL) && !lapic_x2apic() && (REG(0x410) & 4) && lapic_current_id() == 0x0F);
}

/* S3からの復帰: IntelはxAPIC（EN=1）、AMDは無効（APM 16.10）で戻り、firmwareがx2APICにして
   返すこともある。どれからでもmodeを揃え、全レジスターを書き直す。 */
static void test_resume(void) {
    const uint64_t woke[] = {BASE_EN, X2, 0};
    for (int keep = 0; keep <= 1; keep++)
        for (size_t w = 0; w < ARRAY_SIZE(woke); w++) {
            reset(&plain_x2);
            conflict = keep ? NULL : "IOMMU";
            CHECK(init(NULL));
            apic_base = BASE_ADDR | woke[w];
            fill_registers();
            gp = false;
            if (!setjmp(panic_at)) lapic_resume();
            CHECK(!panicked && !gp);
            CHECK(MODE(apic_base) == (keep ? X2 : BASE_EN));
            CHECK(set_up(true));
        }
}

/* APはLimineが起こせばLimineのmode（x2APIC）、INITはmodeを保ち、リセット後はxAPICか無効
   （EN=0ではCPUID.1:EDX[9]も0）。BSPのmodeへ揃える。ロックされたAPは出さずに失敗とする。 */
static void test_worker(void) {
    const uint64_t states[] = {X2, BASE_EN, 0};
    for (int keep = 0; keep <= 1; keep++)
        for (size_t s = 0; s < ARRAY_SIZE(states); s++) {
            reset(&plain_x2);
            conflict = keep ? NULL : "IOMMU";
            CHECK(init(NULL));
            apic_base = BASE_ADDR | states[s];
            fill_registers();
            gp = false;
            CHECK(lapic_worker_init());
            CHECK(!gp);
            CHECK(MODE(apic_base) == (keep ? X2 : BASE_EN));
            CHECK(set_up(false)); /* LINT0とLINT1はBSPだけ */
        }
    /* BSPはxAPICへ戻ったが、このAPはx2APICにロックされている */
    reset(&plain_x2);
    conflict = "IOMMU";
    CHECK(init(NULL) && !lapic_x2apic());
    now.leaf7_d = 1u << 29; now.arch_cap = 1ull << 21; disable_status = 1;
    apic_base = BASE_ADDR | X2;
    gp = false;
    base_writes = 0;
    CHECK(!lapic_worker_init());
    CHECK(!gp && !base_writes && !refused && MODE(apic_base) == X2); /* ロックは書く前に読む */
    /* x2ApicEnを消せないAP: 失敗として残し、#GPで止まらない */
    reset(&plain_x2);
    conflict = "IOMMU";
    CHECK(init(NULL) && !lapic_x2apic());
    now.refuses_leave = true;
    apic_base = BASE_ADDR | X2;
    gp = false;
    base_writes = 0;
    CHECK(!lapic_worker_init());
    CHECK(!gp && !base_writes && refused == 1 && MODE(apic_base) == X2);
}

int main(void) {
    test_leave_decision();
    test_setup();
    test_amd_ids();
    test_resume();
    test_worker();
    printf("APIC mode検査: %u判定、失敗%u（MSR・CPUIDは模擬、実機のAPICは未検証）\n", checks, failed);
    return failed != 0;
}
