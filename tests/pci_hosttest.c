/* PCI実製品コードのホスト検査。CF8/CFC・IOMMU応答・割込み禁止だけを模擬し、実機I/O・QEMUは実行しない。
   tools/msys64/ucrt64/bin/clang.exe -std=gnu11 -O0 -ffunction-sections -fdata-sections -Wno-format
     -Ikernel/src -Icommon tests/pci_hosttest.c -Wl,--gc-sections -o build/pci_hosttest.exe */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arch/cpu.h"
#include "sys/sched.h"
#undef current_task
static struct task *test_current;
#define current_task test_current
static bool kill_on_irq_save;
static unsigned resets;
/* CF8アクセスもirq_saveを使うため、killはFLR発行後の最初のirq_save（reset_waitの境界）で起こす。 */
static uint64_t test_irq_save(void) {
    if (kill_on_irq_save && resets && test_current) test_current->killed = true;
    return 0;
}
static void test_irq_restore(uint64_t flags) { (void)flags; }
#define irq_save test_irq_save
#define irq_restore test_irq_restore
/* S3復帰の待ちはTSCで数える：実時間ではなく、pause一回で1ms分、sleep_msでも同じだけ進む模擬時計
   （tsc_hzは各検査が決める）。 */
static uint64_t test_tsc = 1;
static uint64_t test_rdtsc(void) { return test_tsc; }
static void test_pause(void) { test_tsc += tsc_hz >= 1000 ? tsc_hz / 1000 : 1; }
#define rdtsc test_rdtsc
#define pause test_pause

static void config_outl(uint16_t port, uint32_t value);
static uint32_t config_inl(uint16_t port);
static void config_outw(uint16_t port, uint16_t value);
static void config_outb(uint16_t port, uint8_t value);
static uint32_t ecam_read32(const volatile void *p);
static void ecam_write32(volatile void *p, uint32_t v);
static void ecam_write16(volatile void *p, uint16_t v);
static void ecam_write8(volatile void *p, uint8_t v);
/* cpu.hは既に読み込んだ。製品のconfig accessorを残し、特権I/OとMMIOだけを置き換える。 */
#define outl config_outl
#define inl config_inl
#define outw config_outw
#define outb config_outb
#define mmio_read32 ecam_read32
#define mmio_write32 ecam_write32
#define mmio_write16 ecam_write16
#define mmio_write8 ecam_write8
#include "../kernel/src/dev/pci.c"
#undef outl
#undef inl
#undef outw
#undef outb
#undef mmio_read32
#undef mmio_write32
#undef mmio_write16
#undef mmio_write8
static struct task *test_process(void) { return test_current; }
#define task_process test_process
#include "../kernel/src/dev/pciuser.c"
#undef task_process
#undef irq_save
#undef irq_restore
#undef rdtsc
#undef pause

uint64_t hhdm_offset;
volatile uint64_t timer_ticks;
static struct task test_task;
static uint32_t config[4096 / sizeof(uint32_t)], selected;
static uint32_t peer_config[4096 / sizeof(uint32_t)];
static uint32_t root_config[4096 / sizeof(uint32_t)];
static uint32_t root2_config[4096 / sizeof(uint32_t)]; /* 二つ目のRoot Port（00:02.0） */
static uint64_t link_up_at[2]; /* 0でなければ、その模擬TSC時刻から各Root PortのDLLLAが立つ */
struct access { bool seen; unsigned off; uint64_t at; };
static struct access first[2]; /* 機能0・1への最初の構成アクセス：offsetと模擬TSC時刻 */
static struct pci_device root_port;
static bool root_model, require_visibility, refuse_visibility, lose_visibility_on_sleep;
static bool root_restored; /* S3復帰：Root Port自身の構成復元を許す */
static unsigned root_writes, root_narrow_writes;
static struct pci_device *watch;  /* claim外の検査対象：FLR中の構成アクセスを監視する */
static uint64_t flr_at;           /* FLRを発行した時刻と、その時のCommand・Transactions Pending */
static uint16_t flr_command;
static bool flr_pending;
static unsigned ones_reads_left;  /* 機能0のVendor ID照会が全ビット1を返す残り回数（S3復帰の模擬） */
static unsigned function_accesses; /* 機能0への構成アクセス回数 */
static unsigned sleeps, failures;
static bool refuse_master_clear, ready_after_reset, cancel_on_sleep, force_master_after_reset;
static bool force_pending_after_reset;
static bool fail_after_reset; /* RRSを見せないRoot Complexが再試行を打ち切った：reset後のVendor IDが全ビット1 */
static unsigned prints;        /* kprintfの回数と最後の一行 */
static char last_print[256];
static bool block_confirmed;
static unsigned blocks, revocations;
static bool claim_model, domain_attached, check_reservation, refuse_peer_master_clear, refuse_irq_clear;
static bool refuse_msi_disable, refuse_msix_disable, refuse_open;
static bool check_pending_access;
static uint64_t pending_clear_at, ready_at, cancel_at;
static unsigned progress_bdf;
static uint32_t progress_id;
static unsigned domain_frees, reset_bdf, msi_clears;
static struct pci_device functions[2];
static struct pci_device *members[2];
static struct iommu_domain group_domain;
static const uint32_t test_id = 0x43211234;
static const uint32_t test_bar = 0x80000000;
enum { TEST_PCIE_CAP = 0x40, TEST_ROOTCTL = 0x1C, TEST_ROOTCAP = 0x1E,
       TEST_RRS_ENABLE = 1u << 4, TEST_ROOT_BDF = 1u << 3, TEST_ROOT2_BDF = 2u << 3 };

#define CHECK(condition, message) do { \
    if (!(condition)) { printf("失敗: %s (行%d)\n", message, __LINE__); failures++; } \
} while (0)

void *kzalloc(size_t bytes) { return calloc(1, bytes); }
void *kmalloc(size_t bytes) { return malloc(bytes); }
void kfree(void *p) { free(p); }
void *vmm_map_mmio(uint64_t phys, size_t bytes) { (void)bytes; return phys_to_virt(phys); }
int kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(last_print, sizeof last_print, fmt, args);
    va_end(args);
    prints++;
    return 0;
}
/* このfixtureが検査しない経路を誤って実行した場合、成功で代用しない。 */
const struct acpi_ecam *acpi_ecams(unsigned *count) { (void)count; abort(); }
void *kappend(void *array, size_t *count, size_t *room, size_t size) {
    (void)array; (void)count; (void)room; (void)size; abort();
}
bool lapic_present(void) { abort(); }
uint32_t lapic_current_id(void) { abort(); }
bool iommu_msi_message(struct pci_device *d, unsigned index, unsigned count, uint8_t vector, uint32_t dest,
                       uint32_t *address, uint32_t *data) {
    (void)d; (void)index; (void)count; (void)vector; (void)dest; (void)address; (void)data; abort();
}
bool iommu_msi_clear(struct pci_device *d) { (void)d; msi_clears++; return !refuse_irq_clear; }
void shared_revoke_claim(const struct pci_claim *c) { CHECK(c->released, "取消しより先にclaimを解放状態にする"); revocations++; }
bool iommu_block(struct iommu_domain *domain) {
    CHECK(domain != NULL, "実domainだけを遮断する"); blocks++;
    if (claim_model) { domain->blocked=block_confirmed; domain_attached=!block_confirmed; }
    return block_confirmed;
}
void iommu_domain_free(struct iommu_domain *domain) {
    if (!claim_model) abort();
    if (!domain->blocked) iommu_block(domain);
    domain_frees++;
}
bool fb_release(void) { abort(); }
void wm_repaint(void) { abort(); }
/* 表示ドライバーの面（n_pci_give_screen）：claim の終わりは画面の持ち主を調べるだけ */
void mutex_lock(struct mutex *m) { (void)m; }
void mutex_unlock(struct mutex *m) { (void)m; }
int64_t fb_scanout_check(const struct n_scanout *d, uint64_t *bytes) { (void)d; (void)bytes; abort(); }
int64_t fb_scanout_take(const uint64_t *frames, size_t pages, uint64_t type, size_t first_byte,
                        const struct n_scanout *d) {
    (void)frames; (void)pages; (void)type; (void)first_byte; (void)d; abort();
}
void fb_scanout_boot(void) { abort(); }
int64_t shared_scanout_open(struct handle *surface, const struct pci_claim *claim, uint64_t offset, uint64_t bytes,
                            struct shared_scanout *out) {
    (void)surface; (void)claim; (void)offset; (void)bytes; (void)out; abort();
}
void shared_scanout_close(struct shared_scanout *s) { (void)s; abort(); }
void wm_vblank(uint64_t count, uint64_t timestamp_ns) { (void)count; (void)timestamp_ns; abort(); }
struct fb_info fb;
int init_pid;
NORETURN void panic(const char *fmt, ...) { (void)fmt; abort(); }
int task_kill(int pid) { (void)pid; abort(); }
uint64_t iommu_dma_limit(const struct iommu_domain *dom) { (void)dom; abort(); }
bool iommu_map(struct iommu_domain *dom, const uint64_t *frames, uint64_t base, size_t count) {
    (void)dom; (void)frames; (void)base; (void)count; abort();
}
bool iommu_unmap(struct iommu_domain *dom, const uint64_t *frames, uint64_t base, size_t count) {
    (void)dom; (void)frames; (void)base; (void)count; abort();
}
bool cmdline_has(const char *word) { (void)word; if (!claim_model) abort(); return false; }
bool cmdline_number(const char *key, uint64_t *out) { (void)key; (void)out; abort(); }
size_t strlcpy(char *d, const char *s, size_t n) {
    size_t len=strlen(s);
    if (n) { size_t copy=len<n?len:n-1; memcpy(d,s,copy); d[copy]=0; }
    return len;
}
static bool check_attach_ready;
int iommu_attach(struct pci_device *d, struct iommu_domain **out) {
    if (!claim_model) abort();
    if (check_attach_ready)
        for (size_t i = 0; i < ndevices; i++)
            CHECK(!devices[i].reset, "groupの形成（ACS読出し）より先に、取り残されたFLRを完了する");
    if (domain_attached) return -N_ERR_BUSY;
    group_domain.d=d; group_domain.blocked=false; domain_attached=true; *out=&group_domain;
    return 0;
}
bool iommu_active(void) { abort(); }
/* 付いているdomainの構成員は、そのclaimの回復が予約している */
bool iommu_in_use(const struct pci_device *d) {
    if (!claim_model || !domain_attached) return false;
    for (unsigned i = 0; i < group_domain.nmembers; i++)
        if (group_domain.members[i] == d) return true;
    return false;
}
struct file *vfs_open_vnode(struct vnode *v, int flags) {
    (void)flags; if (!claim_model) abort();
    if (refuse_open) return NULL;
    struct file *f=calloc(1,sizeof *f); if (f) f->vn=v; return f;
}
void fb_hold(void) { abort(); }
struct task *kthread_create(const char *name, void (*fn)(void *), void *arg) {
    (void)name; (void)fn; (void)arg; abort();
}
void wq_wake_all(struct wait_queue *wq) { (void)wq; abort(); }
int irq_vector_alloc(unsigned count, unsigned align) { (void)count; (void)align; abort(); }
void irq_vector_free(unsigned vector, unsigned count) { (void)vector; (void)count; abort(); }
void vector_register(int vector, void (*handler)(struct regs *)) { (void)vector; (void)handler; abort(); }
void vfs_poll_notify(struct vnode *v) { (void)v; abort(); }
void wq_wait(struct wait_queue *wq) { (void)wq; abort(); }
bool wq_wait_timeout(struct wait_queue *wq, uint64_t ms) { (void)wq; (void)ms; abort(); }
int ksnprintf(char *buf, size_t size, const char *fmt, ...) { (void)buf; (void)size; (void)fmt; abort(); }

void sleep_ms(uint64_t ms) {
    sleeps++;
    if (check_reservation) {
        CHECK(group_domain.d->claimed && domain_attached, "group回復のsleep中はprimaryとdomainを予約する");
        struct pci_device *peer=members[members[0]==group_domain.d?1:0];
        struct file *other=NULL;
        CHECK(pci_user_claim(peer->bus,peer->dev,peer->func,&other)==-N_ERR_BUSY && !other,
              "group回復中にpeerの別claimを公開しない");
    }
    uint64_t slept = cancel_on_sleep ? 1 : ms;
    timer_ticks += slept;
    test_tsc += slept * (tsc_hz / 1000);
    uint32_t *progress = progress_bdf ? peer_config : config;
    if (pending_clear_at && timer_ticks >= pending_clear_at)
        progress[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] &= ~((uint32_t)PCIE_DEVSTA_PENDING << 16);
    if (lose_visibility_on_sleep) root_config[(TEST_PCIE_CAP + TEST_ROOTCTL) / 4] &= ~TEST_RRS_ENABLE;
    if (ready_at && resets && timer_ticks >= ready_at) progress[0] = progress_id;
    if (cancel_at && timer_ticks >= cancel_at) test_task.killed = true;
    if (cancel_on_sleep) test_task.killed = true;
}

static unsigned config_offset(uint16_t port) { return (selected & 0xFC) + (port - 0xCFC); }
static uint32_t *selected_config(void) {
    unsigned bdf=(selected>>8)&0xFFFF;
    if (root_model && bdf == TEST_ROOT_BDF) return root_config;
    if (root_model && bdf == TEST_ROOT2_BDF) return root2_config;
    CHECK(bdf==0 || bdf==1, "モデルの二機能以外へconfigを送らない");
    if (bdf == 0) function_accesses++;
    if (bdf < 2 && !first[bdf].seen) first[bdf] = (struct access){true, selected & 0xFC, test_tsc};
    struct pci_device *w = claim_model ? (bdf < 2 ? &functions[bdf] : NULL) : bdf == 0 ? watch : NULL;
    if (w && check_pending_access && w->reset && w->reset->issued)
        CHECK(timer_ticks - w->reset->since > w->reset->wait, "FLR完了待ちの間はconfigへアクセスしない");
    uint32_t *r=bdf==1?peer_config:config;
    if (root_model && require_visibility && r[0] == PCI_CONFIG_RRS_ID)
        CHECK(root_config[(TEST_PCIE_CAP + TEST_ROOTCTL) / 4] & TEST_RRS_ENABLE,
              "初期化中の機能への照会より先に実Root Portのvisibilityを再確認する");
    if (resets && (r[0]==PCI_CONFIG_RRS_ID || r[0]==UINT32_MAX))
        CHECK((selected&0xFC)==0, "RRS・失敗読みの間はVendor IDだけを照会し未準備のconfigを復元しない");
    return r;
}
static unsigned rrs_reads_left; /* 機能0のVendor ID照会がRRSを返す残り回数（S3復帰の模擬） */
static uint32_t config_inl(uint16_t port) {
    CHECK(port == 0xCFC, "config読出しport");
    uint32_t *r = selected_config();
    unsigned off = selected & 0xFC;
    if (((selected >> 8) & 0xFFFF) == 0 && off == 0) {
        if (rrs_reads_left) {
            if (root_model && require_visibility)
                CHECK(root_config[(TEST_PCIE_CAP + TEST_ROOTCTL) / 4] & TEST_RRS_ENABLE,
                      "復帰後の準備待ちはRoot PortのRRS visibilityを有効にして照会する");
            rrs_reads_left--;
            return PCI_CONFIG_RRS_ID;
        }
        if (ones_reads_left) {
            ones_reads_left--;
            return UINT32_MAX;
        }
    }
    uint32_t v = r[off / 4];
    int root = r == root_config ? 0 : r == root2_config ? 1 : -1;
    if (root >= 0 && link_up_at[root] && off == TEST_PCIE_CAP + PCIE_LNKCTL) {
        v &= ~((uint32_t)PCIE_LNKSTA_DLL_ACTIVE << 16);
        if (test_tsc >= link_up_at[root]) v |= (uint32_t)PCIE_LNKSTA_DLL_ACTIVE << 16;
    }
    return v;
}
/* FLR・S3のConventional Reset：読出し専用の識別とcapabilityの連鎖だけを残し、構成を初期値へ戻す */
static void reset_function(uint32_t *r) {
    uint32_t caps=r[1]&0x100000, cap_head=r[0x34/4];
    uint32_t msi=r[0x60/4]&~0x710000u, msix=r[0x70/4]&~0xC0000000u, pcie_head=r[TEST_PCIE_CAP/4];
    uint32_t devcap2=r[(TEST_PCIE_CAP + PCIE_DEVCAP2) / 4];
    memset(r, 0, sizeof config);
    r[0] = ready_after_reset ? test_id : fail_after_reset ? UINT32_MAX : PCI_CONFIG_RRS_ID;
    r[1]=caps; r[0x34/4]=cap_head; r[0x60/4]=msi; r[0x70/4]=msix;
    r[TEST_PCIE_CAP / 4] = pcie_head; /* 読出し専用：版数と種別はresetで変わらない */
    r[(TEST_PCIE_CAP + PCIE_DEVCAP) / 4] = PCIE_DEVCAP_FLR;
    r[(TEST_PCIE_CAP + PCIE_DEVCAP2) / 4] = devcap2;
    if (force_pending_after_reset) r[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] |= (uint32_t)PCIE_DEVSTA_PENDING << 16;
}
static void emulate_flr(void) {
    if (require_visibility)
        CHECK(root_config[(TEST_PCIE_CAP + TEST_ROOTCTL) / 4] & TEST_RRS_ENABLE,
              "FLRの発行前に対応Root PortのRRS visibilityを有効化する");
    resets++;
    reset_bdf=(selected>>8)&0xFFFF;
    uint32_t *r=selected_config();
    flr_at = timer_ticks;
    flr_command = r[1] & 0xFFFF;
    flr_pending = (r[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] >> 16) & PCIE_DEVSTA_PENDING;
    reset_function(r);
}
static void config_outl(uint16_t port, uint32_t value) {
    if (port == 0xCF8) { selected = value; return; }
    CHECK(port == 0xCFC, "config書込みport");
    if (root_model && ((selected >> 8) & 0xFFFF) == TEST_ROOT_BDF) root_writes++;
    uint32_t *r=selected_config();
    if (refuse_msi_disable && (selected&0xFC)==0x60) value|=r[0x60/4]&0x10000;
    if (refuse_msix_disable && (selected&0xFC)==0x70) value|=r[0x70/4]&0x80000000;
    r[(selected & 0xFC) / 4] = value;
}
static void config_outw(uint16_t port, uint16_t value) {
    unsigned off = config_offset(port);
    if (root_model && ((selected >> 8) & 0xFFFF) == TEST_ROOT_BDF) {
        root_writes++; root_narrow_writes++;
        CHECK(root_restored || off == TEST_PCIE_CAP + TEST_ROOTCTL, "Root PortへはRoot Controlだけnarrow writeする");
        if (refuse_visibility) return;
    }
    uint32_t shift = (off & 2) * 8, *word = &selected_config()[off / 4];
    if (off == 4 && (refuse_master_clear || (refuse_peer_master_clear && ((selected>>8)&0xFFFF)==1)) &&
        (*word & PCI_COMMAND_MASTER)) value |= PCI_COMMAND_MASTER;
    if (off == 4 && force_master_after_reset && resets) value |= PCI_COMMAND_MASTER;
    *word = (*word & ~(0xFFFFu << shift)) | (uint32_t)value << shift;
    if (off == TEST_PCIE_CAP + PCIE_DEVCTL && (value & PCIE_DEVCTL_FLR)) emulate_flr();
}
static void config_outb(uint16_t port, uint8_t value) {
    if (root_model && ((selected >> 8) & 0xFFFF) == TEST_ROOT_BDF) root_writes++;
    unsigned off = config_offset(port), shift = (off & 3) * 8;
    uint32_t *r=selected_config();
    r[off / 4] = (r[off / 4] & ~(0xFFu << shift)) | (uint32_t)value << shift;
}

/* ECAM（readiness_time_cases）：機能0の先頭256バイトはCF8と同じ模擬（FLR・RRS・FLR中の監視）を通し、
   拡張構成領域とそれ以外のMMIOは記憶域として読み書きする */
static bool ecam_function0(const volatile void *p, unsigned *off) {
    uintptr_t a = (uintptr_t)p, base = (uintptr_t)config;
    if (a < base || a >= base + 256) return false;
    *off = (unsigned)(a - base);
    selected = 0x80000000u | (*off & 0xFC);
    return true;
}
static uint32_t ecam_read32(const volatile void *p) {
    unsigned off;
    return ecam_function0(p, &off) ? config_inl(0xCFC) : *(const volatile uint32_t *)p;
}
static void ecam_write32(volatile void *p, uint32_t v) {
    unsigned off;
    if (ecam_function0(p, &off)) config_outl(0xCFC, v);
    else *(volatile uint32_t *)p = v;
}
static void ecam_write16(volatile void *p, uint16_t v) {
    unsigned off;
    if (ecam_function0(p, &off)) config_outw(0xCFC + (off & 2), v);
    else *(volatile uint16_t *)p = v;
}
static void ecam_write8(volatile void *p, uint8_t v) {
    unsigned off;
    if (ecam_function0(p, &off)) config_outb(0xCFC + (off & 3), v);
    else *(volatile uint8_t *)p = v;
}

static struct pci_device fixture(void) {
    memset(config, 0, sizeof config);
    memset(&test_task, 0, sizeof test_task);
    current_task = &test_task;
    selected = resets = sleeps = 0;
    timer_ticks = 0;
    refuse_master_clear = cancel_on_sleep = force_master_after_reset = false;
    force_pending_after_reset = fail_after_reset = false;
    ready_after_reset = true;
    memset(link_up_at, 0, sizeof link_up_at);
    memset(first, 0, sizeof first);
    claim_model=domain_attached=check_reservation=refuse_peer_master_clear=refuse_irq_clear=false;
    check_attach_ready=false;
    refuse_msi_disable=refuse_msix_disable=refuse_open=false; msi_clears=domain_frees=reset_bdf=0;
    check_pending_access=false;
    pending_clear_at=ready_at=cancel_at=0; progress_bdf=0; progress_id=test_id;
    kill_on_irq_save=false;
    root_model=require_visibility=refuse_visibility=lose_visibility_on_sleep=root_restored=false;
    root_writes=root_narrow_writes=0;
    watch=NULL; flr_at=0; flr_command=0; flr_pending=false; ones_reads_left=rrs_reads_left=0;
    function_accesses=0;
    ecam_off = true;
    config[0] = test_id;
    config[1] = 7;
    config[0x10 / 4] = test_bar;
    config[TEST_PCIE_CAP / 4] = 0x10 | (2u << 16); /* PCI Express Capability version 2 */
    config[(TEST_PCIE_CAP + PCIE_DEVCAP) / 4] = PCIE_DEVCAP_FLR;
    return (struct pci_device){.vendor = test_id & 0xFFFF, .device = test_id >> 16,
                               .pcie_cap = TEST_PCIE_CAP, .pcie_v2 = true, .needs_reset = true};
}

/* FLR完了（100ms）の後の最初の照会：時刻は整数tickなので、待機はn msを超えてから終わる */
#define FIRST_ACCESS (FLR_DONE_MS + 1)

static void reset_restore_cases(void) {
    struct pci_device d = fixture();
    refuse_master_clear = true;
    config[(TEST_PCIE_CAP + PCIE_DEVCTL2) / 4] = 6; /* 65..210 ms: 表7-34 */
    CHECK(!pci_function_reset(&d) && !resets && !d.reset,
          "BME停止未確認では現CT既知でも新要求を禁止できずFLRしない");
    d = fixture();
    config[(TEST_PCIE_CAP + PCIE_DEVCTL2) / 4] = 14; /* 17..64秒でもTP0の後に追加待機しない。 */
    CHECK(pci_function_reset(&d) && resets == 1 && flr_at == 0 && timer_ticks == FIRST_ACCESS,
          "BMEoffとTP0実測後はCT上限64秒を不要に課さない");
    CHECK(flr_command == PCI_COMMAND_INTX_DISABLE,
          "FLR前にCommandを消し（6.6.2）、Interrupt DisableだけはINTxを止める側へ立てる");
    CHECK((config[1] & 0xFFFF) == (7 & ~PCI_COMMAND_MASTER), "元のCommandをBMEだけ除いて戻す");
    CHECK(config[0x10 / 4] == test_bar && d.reset == NULL, "BAR復元と完了状態の回収");

    /* 止められない機能（BMEが落ちない）はFLRせず、停止のために消したCommandをBMEだけ落として戻す */
    const uint32_t command = 7 | PCI_COMMAND_PARITY | PCI_COMMAND_SERR;
    d = fixture();
    config[1] = command;
    refuse_master_clear = true;
    CHECK(!pci_function_reset(&d) && !resets && !d.reset, "BMEが落ちない機能をFLRしない");
    CHECK((config[1] & 0xFFFF) == command,
          "停止に失敗したらMSE・I/O・SERR・Parityを戻し、Interrupt Disableだけの値で残さない");
    refuse_master_clear = false;
    CHECK(pci_function_reset(&d) && resets == 1 && (config[1] & 0xFFFF) == (command & ~PCI_COMMAND_MASTER),
          "次の試行は元のCommandを保存し、FLR後にBMEだけ除いて戻す");

    /* 版数1のPCIe構造にはRoot Status以降の登録が無い：+28hは別の構造であり保存・復元しない。
       6.0が定めるのは版数2だけで、それ以上の版数も同じ構造として扱ってよい（7.5.3.2）。 */
    for (unsigned version = 1; version <= 3; version++) {
        bool v2 = version >= 2;
        d = fixture();
        config[1] |= 0x10u << 16; /* Status: Capabilities List */
        config[0x34 / 4] = TEST_PCIE_CAP;
        config[TEST_PCIE_CAP / 4] = 0x10 | version << 16 | 4u << 20; /* Device/Port Type 4: Root Port */
        pcie_identify(&d);
        CHECK(d.pcie_cap == TEST_PCIE_CAP && d.pcie_type == 4 && d.pcie_v2 == v2, "PCIe能力の位置・型・版数");
        d.pcie_type = 0;
        config[(TEST_PCIE_CAP + PCIE_DEVCTL2) / 4] = 0xE; /* 版数1では他の構造の内容 */
        config[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] |= (uint32_t)PCIE_DEVSTA_PENDING << 16;
        pending_clear_at = UINT64_MAX;
        CHECK(pci_function_reset(&d) && flr_at == COMPLETION_FLOOR_MS + 1 + (v2 ? 63000 : 0),
              "版数1は+28hをCompletion Timeout値として読まない");
        CHECK(config[(TEST_PCIE_CAP + PCIE_DEVCTL2) / 4] == (v2 ? 0xE : 0),
              "Device Control 2は版数2以降だけ書き戻す");
    }

    d = fixture();
    force_master_after_reset = true;
    CHECK(!pci_function_reset(&d) && d.reset != NULL, "復元後BMEが残る装置を成功としない");
    force_master_after_reset = false;
    CHECK(pci_function_reset(&d) && resets == 1, "BME readback失敗後は二度目のFLRなしで復元再試行");
}

static void pending_resume_cases(void) {
    struct pci_device d = fixture();
    test_task.killed = true;
    CHECK(!pci_function_reset(&d) && resets == 0 && d.reset == NULL, "取消済みの要求から新しいFLRを開始しない");
    test_task.killed = false;
    ready_after_reset = false;
    cancel_at = FIRST_ACCESS + 2;
    CHECK(!pci_function_reset(&d) && d.reset != NULL, "RRS中の装置へ設定を復元しない");
    CHECK(resets == 1 && timer_ticks == cancel_at && test_task.killed,
          "100ms後のRRSはyieldして再照会し、取消しに応答する");
    CHECK(config[0x10 / 4] == 0, "未準備ならBARはまだ復元されない");
    config[0] = test_id;
    cancel_at = 0; test_task.killed = false;
    CHECK(pci_function_reset(&d) && resets == 1, "後の回復はFLRを再発行しない");
    CHECK(config[0x10 / 4] == test_bar && d.reset == NULL, "reset前のBARを保持したまま復元");

    d = fixture();
    cancel_on_sleep = true;
    CHECK(!pci_function_reset(&d) && d.reset != NULL && timer_ticks == 1, "待機中の取消で直ちに戻る");
    CHECK(d.reset && d.reset->state.hdr[4] == test_bar, "保留状態は取消されたappのstackに依存しない");
    cancel_on_sleep = false;
    test_task.killed = false;
    CHECK(pci_function_reset(&d) && resets == 1 && timer_ticks == FIRST_ACCESS,
          "取消後は残りの仕様時間だけ待って復元を再開");
    CHECK(config[0x10 / 4] == test_bar && !(config[1] & PCI_COMMAND_MASTER), "再開後BARとBME");

    /* 取消し中に時間が過ぎ、再開がちょうど100ms目の刻みに来た：その刻みはまだ100ms未満であり得る */
    d = fixture();
    cancel_on_sleep = true;
    CHECK(!pci_function_reset(&d) && d.reset, "FLR完了待ちで取り消す");
    cancel_on_sleep = false; test_task.killed = false;
    timer_ticks = FLR_DONE_MS; watch = &d; check_pending_access = true;
    CHECK(pci_function_reset(&d) && timer_ticks == FIRST_ACCESS,
          "再開が100ms目の刻みでも、次の刻みまで照会しない");

    d = fixture();
    kill_on_irq_save = true;
    CHECK(!pci_function_reset(&d) && d.reset && resets == 1 && !sleeps,
          "killが外側検査とwait登録の境界に来ても長時間sleepへ登録しない");
    CHECK(d.reset && d.reset->state.hdr[4] == test_bar && timer_ticks == 0,
          "境界取消でもFLR前の保存状態と開始時刻を維持する");
    kill_on_irq_save = false; test_task.killed = false;
    CHECK(pci_function_reset(&d) && !d.reset && resets == 1 && timer_ticks == FIRST_ACCESS,
          "境界取消後は再resetせず残りの仕様待機から復元する");

    d = fixture();
    force_pending_after_reset = true;
    CHECK(!pci_function_reset(&d) && d.reset != NULL, "IDが準備済みでもreset後pendingが残れば復元を保留");
    CHECK(config[0x10 / 4] == 0, "pendingの残る機能へBARを復元しない");
    config[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] &= ~((uint32_t)PCIE_DEVSTA_PENDING << 16);
    CHECK(pci_function_reset(&d) && d.reset == NULL && resets == 1,
          "pending消失後は既存の保存状態から復元し二度目のFLRをしない");
}

/* Transactions Pendingが消えない機能：Completionがもう届き得ない時間（現在のCompletion Timeout値、
   機能の対応する最長の範囲、下限1秒）だけ待ち、それでもFLRする（6.6.2）。 */
static void completion_window_cases(void) {
    static const struct { uint16_t value; uint32_t ranges; unsigned window; const char *why; } windows[] = {
        {PCIE_DEVCTL2_CT_DISABLE, 0, COMPLETION_FLOOR_MS, "CT無効は100ms以上の系依存時間：下限1秒"},
        {3, 0, 64000, "予約符号は範囲を選ばない：定義された最長の64秒"},
        {6, 0, COMPLETION_FLOOR_MS, "210msの範囲でも下限1秒"},
        {14, 0, 64000, "範囲Dの64秒"},
        {0, 0xF, 64000, "既定値でも、待ち中に変えられ得る範囲Dの64秒"},
        {0, 1u << 2, 3500, "範囲Cだけを持つ機能は3.5秒"},
        {0, 1u << 1, COMPLETION_FLOOR_MS, "範囲Bだけなら210msでも下限1秒"},
    };
    for (unsigned i = 0; i < sizeof windows / sizeof windows[0]; i++) {
        struct pci_device d = fixture();
        config[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] |= (uint32_t)PCIE_DEVSTA_PENDING << 16;
        config[(TEST_PCIE_CAP + PCIE_DEVCTL2) / 4] = windows[i].value;
        config[(TEST_PCIE_CAP + PCIE_DEVCAP2) / 4] = windows[i].ranges;
        watch = &d; check_pending_access = true;
        CHECK(pci_function_reset(&d) && resets == 1 && flr_pending && flr_at == windows[i].window + 1,
              windows[i].why);
        CHECK(flr_command == PCI_COMMAND_INTX_DISABLE, "TP待機の間も新しい要求を止めたままにする");
        CHECK(timer_ticks == flr_at + FIRST_ACCESS && !d.reset && config[0x10 / 4] == test_bar &&
              !(config[1] & PCI_COMMAND_MASTER), "窓を待ち切ったFLRは100ms後に構成しBMEを戻さない");
        if (d.reset) kfree(d.reset);
    }

    /* 窓の途中での取消：保存状態を公開したまま戻り、再開は同じ静止時刻からの窓を待つ */
    struct pci_device d = fixture();
    config[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] |= (uint32_t)PCIE_DEVSTA_PENDING << 16;
    cancel_on_sleep = true;
    CHECK(!pci_function_reset(&d) && !resets && d.reset && !d.reset->issued && test_task.killed,
          "TP待機中の取消はFLRを発行せず保存状態を保持する");
    CHECK(d.reset && (d.reset->state.hdr[1] & 0xFFFF) == (7 & ~PCI_COMMAND_MASTER) &&
          d.reset->state.hdr[4] == test_bar, "保存はCommand静止より前の構成（BMEだけ除く）");
    CHECK(!pci_requests_done(&d) && sleeps == 1, "解放時pending判定は新たに待機しない");
    cancel_on_sleep = false; test_task.killed = false;
    CHECK(pci_function_reset(&d) && resets == 1 && flr_at == COMPLETION_FLOOR_MS + 1 && !d.reset,
          "再開は最初の静止時刻から窓を数え、状態を取り直さない");
    CHECK(config[0x10 / 4] == test_bar && (config[1] & 0xFFFF) == (7 & ~PCI_COMMAND_MASTER),
          "再開後も静止前の構成を復元する");

    d = fixture();
    refuse_master_clear = true;
    config[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] |= (uint32_t)PCIE_DEVSTA_PENDING << 16;
    CHECK(!pci_function_reset(&d) && !resets && !sleeps && !d.reset,
          "BMEが止まらない機能はTPを待たずFLRもしない");
    d = fixture();
    config[0] = PCI_CONFIG_RRS_ID;
    CHECK(!pci_requests_done(&d), "RRS Vendor IDを正常な装置の完了としない");
}

static void readiness_time_cases(void) {
    struct pci_device d = fixture();
    static const struct acpi_ecam mock_ecam = {.start_bus = 0, .end_bus = 0};
    ecam = &mock_ecam;
    necam = 1;
    ecam_off = false;
    ecam_active[0] = 1;
    hhdm_offset = (uint64_t)(uintptr_t)config;
    config[0x100 / 4] = 0x00010022;
    config[0x104 / 4] = 1u << 31;
    config[0x108 / 4] = 0xA1E;
    CHECK(flr_time_ms(&d) == 1007, "有効なRTR 1.00663296秒をms単位へ切り上げる");
    config[0x108 / 4] = 0xA1F;
    CHECK(flr_time_ms(&d) == 0, "仕様最大値を超すRTR値を根拠にしない");
    config[0x108 / 4] = 0xC00;
    CHECK(flr_time_ms(&d) == 0, "予約の尺度6・7を使わない");
    config[0x108 / 4] = 0x001;
    CHECK(flr_time_ms(&d) == FLR_DONE_MS, "RTRが100msより短くてもFLR完了の100msは待つ");
    config[0x104 / 4] = 0;
    config[0x108 / 4] = 0xA1E;
    CHECK(flr_time_ms(&d) == 0, "Valid=0のRTR値は使わない");

    /* RTR 200ms（190×32^4 ns）：初回照会はその後、RRSの許容もそこまで */
    const unsigned rtr = 200, rtr_field = (4u << 9) | 190;
    d = fixture();
    ecam_off = false;
    config[0x100 / 4] = 0x00010022; config[0x104 / 4] = 1u << 31; config[0x108 / 4] = rtr_field;
    ready_after_reset = false; ready_at = FLR_DONE_MS + 20; watch = &d; check_pending_access = true;
    CHECK(pci_function_reset(&d) && timer_ticks == rtr + 1 && !d.reset,
          "有効なFLR Timeの前には照会せず、その後の最初の照会で復元する");
    d = fixture();
    ecam_off = false;
    config[0x100 / 4] = 0x00010022; config[0x104 / 4] = 1u << 31; config[0x108 / 4] = rtr_field;
    ready_after_reset = false; ready_at = 0;
    CHECK(!pci_function_reset(&d) && d.reset && timer_ticks == rtr + 1,
          "報告したFLR Timeを過ぎてRRSの機能は未準備とし、1秒を待たない");
    if (d.reset) kfree(d.reset);
    ecam_off = true;
}

static void recovery_progress_cases(void) {
    const unsigned values[] = {PCIE_DEVCTL2_CT_DISABLE, 3, 0, 6, 14};
    for (unsigned i = 0; i < sizeof values / sizeof values[0]; i++) {
        struct pci_device d = fixture();
        config[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] |= (uint32_t)PCIE_DEVSTA_PENDING << 16;
        config[(TEST_PCIE_CAP + PCIE_DEVCTL2) / 4] = values[i];
        pending_clear_at = 3; /* 模擬装置が既存要求を実際に完了する時刻。製品の期限ではない。 */
        CHECK(pci_function_reset(&d), "全CT値でTP消失を実測したらFLRする");
        CHECK(resets == 1 && flr_at == pending_clear_at && !flr_pending &&
              timer_ticks == pending_clear_at + FIRST_ACCESS && !d.reset,
              "TP実消失の時点でFLRし、窓の残りを待たない");
        CHECK(config[0x10 / 4] == test_bar && !(config[1] & PCI_COMMAND_MASTER),
              "TP待機後もBARを復元しBMEを再開しない");
        if (d.reset) kfree(d.reset);
    }
    struct pci_device d = fixture();
    ready_after_reset = false;
    ready_at = FIRST_ACCESS + 2; /* RTRを持たず100ms後にもRRSを返す装置。 */
    CHECK(pci_function_reset(&d), "RTRなしのRRSも同じclaim内で再照会し準備完了後に復元する");
    CHECK(resets == 1 && timer_ticks == ready_at && !d.reset && config[0x10 / 4] == test_bar,
          "RRS待機に二度目のFLRを追加しない");
    if (d.reset) kfree(d.reset);

    d = fixture();
    ready_after_reset = false; ready_at = 0; /* RTRが無く、いつまでもRRS */
    CHECK(!pci_function_reset(&d) && d.reset && timer_ticks == FLR_READY_MS + 1 && config[0x10 / 4] == 0,
          "RTRなしのRRSは1.0秒（6.6.1・5.10.1）を過ぎたら未準備として保存状態を保持する");
    CHECK(strstr(last_print, "still initializing") != NULL, "RRSのまま上限を過ぎたら初期化中と記録する");
    config[0] = test_id;
    CHECK(pci_function_reset(&d) && resets == 1 && !d.reset && config[0x10 / 4] == test_bar,
          "後で準備できた機能は同じ保存状態から復元する");

    /* RRSを見せないRoot Complexは再試行を打ち切って失敗取引（全ビット1）を返し得る（2.3.2）：
       居ると分かっている機能の全ビット1はRRSと同じく、まだIDを返していないものとして待つ */
    d = fixture();
    ready_after_reset = false; fail_after_reset = true; ready_at = FIRST_ACCESS + 5;
    watch = &d; check_pending_access = true;
    CHECK(pci_function_reset(&d) && resets == 1 && timer_ticks == ready_at && !d.reset &&
          config[0x10 / 4] == test_bar, "FLR後の全ビット1は準備上限まで再照会し、IDが返れば復元する");
    d = fixture();
    ready_after_reset = false; fail_after_reset = true; ready_at = 0;
    CHECK(!pci_function_reset(&d) && d.reset && timer_ticks == FLR_READY_MS + 1 && config[0x10 / 4] == 0,
          "全ビット1のまま1.0秒を過ぎたら未準備として保存状態を保持する");
    CHECK(strstr(last_print, "not answering") != NULL, "失敗読みのまま上限を過ぎたら応答なしと記録する");
    if (d.reset) kfree(d.reset);

    d = fixture();
    ready_after_reset = false; ready_at = FIRST_ACCESS; progress_id = UINT32_MAX;
    CHECK(!pci_function_reset(&d) && d.reset && config[0x10 / 4] == 0,
          "RRSから装置不在へ変わっても成功やBAR復元にしない");
    config[0] = test_id; ready_at = 0;
    CHECK(pci_function_reset(&d) && resets == 1 && !d.reset && config[0x10 / 4] == test_bar,
          "装置が再び応答すれば同じ保存状態から復元する");

    d = fixture();
    config[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] |= (uint32_t)PCIE_DEVSTA_PENDING << 16;
    config[(TEST_PCIE_CAP + PCIE_DEVCTL2) / 4] = PCIE_DEVCTL2_CT_DISABLE;
    cancel_on_sleep = true;
    CHECK(!pci_function_reset(&d) && !resets && d.reset, "TP消失前の取消ではFLRを始めない");
    cancel_on_sleep = false; test_task.killed = false; pending_clear_at = timer_ticks + 2;
    CHECK(pci_function_reset(&d) && resets == 1 && flr_at == pending_clear_at && !d.reset,
          "TP待機を取り消した後も実際の完了を待って回復できる");
}

/* 前のdriverがCTを無効にしてから戻した：その間の要求にはtimerが無い（7.5.3.16）。下限1秒の窓が
   その完了を待ち、窓の中で届けばFLRはその後、届かなければ窓の後にFLRして構成する。 */
static void completion_history_cases(void) {
    struct pci_device d = fixture();
    struct pci_claim c = {.d = &d};
    struct vnode v = {.ops = &claim_ops, .priv = &c};
    struct file f = {.vn = &v};
    guards_init(&c);
    CHECK(pci_user_config_write(&f, TEST_PCIE_CAP + PCIE_DEVCTL2, 2, PCIE_DEVCTL2_CT_DISABLE) == 0,
          "driverの正当なCT設定操作を人工禁止しない");
    config[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] |= (uint32_t)PCIE_DEVSTA_PENDING << 16;
    CHECK(pci_user_config_write(&f, TEST_PCIE_CAP + PCIE_DEVCTL2, 2, 0) == 0,
          "CT Disable clearは既存Requestへtimerを適用する保証ではない");
    pending_clear_at = FLR_DONE_MS + 3; /* 古いuntimed Requestの実Completion到着。 */
    CHECK(pci_function_reset(&d) && resets == 1 && flr_at == pending_clear_at && !flr_pending &&
          timer_ticks == pending_clear_at + FIRST_ACCESS,
          "現CT=defaultの50msを過ぎても、旧untimed Completionの到着まではFLRしない");
    d = fixture();
    config[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] |= (uint32_t)PCIE_DEVSTA_PENDING << 16;
    pending_clear_at = UINT64_MAX;
    CHECK(pci_function_reset(&d) && flr_pending && flr_at == COMPLETION_FLOOR_MS + 1 && !d.reset,
          "届かないuntimed要求は下限1秒の後にFLRし、無期限には待たない");
}

static struct pci_device visibility_fixture(void) {
    struct pci_device d = fixture();
    root_model = true;
    memset(root_config, 0, sizeof root_config);
    root_port = (struct pci_device){.dev = 1, .vendor = test_id & 0xFFFF, .device = test_id >> 16,
                                    .pcie_cap = TEST_PCIE_CAP, .pcie_type = 4, .bridge = true};
    root_config[0] = test_id;
    root_config[TEST_PCIE_CAP / 4] = 0x10 | (2u << 16) | (4u << 20);
    root_config[(TEST_PCIE_CAP + TEST_ROOTCTL) / 4] = (1u << 16) | 0x2B;
    d.upstream = &root_port;
    return d;
}

static uint32_t root_control(void) { return root_config[(TEST_PCIE_CAP + TEST_ROOTCTL) / 4]; }

static void visibility_cases(void) {
    const uint32_t firmware = (1u << 16) | 0x2B; /* Root Capabilities.RRS visibility、Root ControlはRRS bit以外 */
    struct pci_device d = visibility_fixture();
    require_visibility = true; ready_after_reset = false; ready_at = FLR_DONE_MS + 3;
    CHECK(pci_function_reset(&d) && root_writes == 2 && root_narrow_writes == 2,
          "対応Root Portのdefault-off RRS visibilityをFLRからreadinessまでだけnarrow writeで有効にする");
    CHECK(root_control() == firmware && !root_port.rrs_holds,
          "readiness後はRoot Controlを元の値へ戻し、他bitと隣接Root Capabilitiesを保存する");

    d = visibility_fixture(); require_visibility = true; ready_after_reset = false; ready_at = FLR_DONE_MS + 3;
    root_config[(TEST_PCIE_CAP + TEST_ROOTCTL) / 4] |= TEST_RRS_ENABLE;
    CHECK(pci_function_reset(&d) && !root_writes && root_control() == (firmware | TEST_RRS_ENABLE),
          "firmwareが既に有効にしていたvisibilityは書かず、reset後も消さない");

    d = visibility_fixture(); require_visibility = true; cancel_on_sleep = true;
    CHECK(!pci_function_reset(&d) && d.reset && resets == 1 && (root_control() & TEST_RRS_ENABLE),
          "visible FLRの取消では保存状態とvisibilityの保持を残す");
    root_config[(TEST_PCIE_CAP + TEST_ROOTCTL) / 4] &= ~TEST_RRS_ENABLE;
    cancel_on_sleep = false; test_task.killed = false; watch = &d; check_pending_access = true;
    CHECK(pci_function_reset(&d) && !d.reset && resets == 1 && root_control() == firmware && !root_port.rrs_holds,
          "Root Control喪失後のpending resetも照会前にvisibilityを戻し、完了後に解除する");

    d = visibility_fixture(); require_visibility = true; lose_visibility_on_sleep = true;
    CHECK(pci_function_reset(&d) && root_writes == 3 && root_control() == firmware,
          "FLR待機中のRoot Control喪失もreadiness read前に再設定し、完了後に解除する");

    /* 同じRoot Port下の二機能：先のresetが保留中なら、後のresetの完了でvisibilityを消さない */
    d = visibility_fixture(); require_visibility = true;
    memcpy(peer_config, config, sizeof config);
    struct pci_device peer = d;
    peer.func = 1;
    cancel_on_sleep = true;
    CHECK(!pci_function_reset(&d) && d.reset && root_port.rrs_holds == 1, "一機能目のFLRを保留にする");
    cancel_on_sleep = false; test_task.killed = false;
    CHECK(pci_function_reset(&peer) && !peer.reset && root_port.rrs_holds == 1 &&
          (root_control() & TEST_RRS_ENABLE), "他機能の保留resetがある間はvisibilityを保持する");
    CHECK(pci_function_reset(&d) && !d.reset && !root_port.rrs_holds && root_control() == firmware,
          "最後の保留resetの完了でRoot Controlを元へ戻す");

    for (unsigned mode = 0; mode < 2; mode++) {
        d = visibility_fixture();
        if (mode == 0) root_config[(TEST_PCIE_CAP + TEST_ROOTCTL) / 4] &= ~(1u << 16);
        if (mode == 1) root_config[0] = UINT32_MAX;
        CHECK(pci_function_reset(&d) && !root_writes && !root_port.rrs_holds,
              "RRS visibility非対応・応答しないRoot Portへ書かず従来FLRを妨げない");
    }
    d = visibility_fixture(); refuse_visibility = true;
    CHECK(pci_function_reset(&d) && root_control() == firmware && !root_port.rrs_holds,
          "visibility書込拒否でも既存ready装置のFLRを拒否せず、保持を残さない");
    d = visibility_fixture();
    struct pci_device switch_port = {.pcie_type = 6, .pcie_cap = TEST_PCIE_CAP, .upstream = &root_port};
    d.upstream = &switch_port; require_visibility = true;
    CHECK(pci_function_reset(&d) && root_writes == 2 && root_control() == firmware,
          "switch portを誤選択せず実上流Root Portの対応だけを使用する");
}

/* S3復帰：Conventional Reset後の初期化中（RRS）の機能を不在と判定せず、1秒の猶予内で復元する */
uint64_t tsc_hz;
static void resume_cases(void) {
    struct pci_device d = fixture();
    struct pci_saved s = {0};
    devices = &d; ndevices = 1; saved = &s;
    CHECK(pci_suspend() == NULL && saved_valid, "保存できる");
    config[0x10 / 4] = 0; config[1] = 0; /* 復帰後のreset状態 */
    tsc_hz = 1000000000ull; rrs_reads_left = 3;
    pci_resume();
    CHECK(!rrs_reads_left && config[0x10 / 4] == test_bar && (config[1] & 7) == 7,
          "RRSを返す間は待ち、準備後に構成を復元する");
    config[0x10 / 4] = 0; config[1] = 0;
    tsc_hz = 1000; rrs_reads_left = UINT32_MAX; /* 期限を短くし、永久にRRSを返す機能 */
    pci_resume();
    CHECK(config[0x10 / 4] == 0 && rrs_reads_left != UINT32_MAX,
          "期限を過ぎてもRRSなら復元せず不在扱いにする");
    rrs_reads_left = 0;

    /* RRS Software Visibilityの無い経路では初期化中の読出しが失敗（全ビット1）に見え得る：既知の機能は1秒待つ */
    tsc_hz = 1000000000ull; ones_reads_left = 3;
    pci_resume();
    CHECK(!ones_reads_left && config[0x10 / 4] == test_bar, "既知の機能の全ビット1は1秒の猶予内で再照会する");
    config[0x10 / 4] = 0;
    tsc_hz = 1000; ones_reads_left = UINT32_MAX;
    pci_resume();
    CHECK(config[0x10 / 4] == 0, "1秒を過ぎても応答しない機能へ構成を書かない");
    ones_reads_left = 0;

    /* FLR中の機能：その構成はFLRの所有物で、照会は捨てられ得る（6.6.2）。眠りの前後とも触れない。
       S3のConventional ResetがFLRの代わりを果たす（6.6.1）：取り残しは復帰時点で発行済みとなり、次の
       回復は最初のアクセスをVendor IDの読み（2.3.1）にして、FLRを重ねず保存構成を戻す。
       FLRの発行前（TP待ち）・発行後（完了待ち）のどちらで取り残されても同じ */
    for (unsigned issued = 0; issued < 2; issued++) {
        struct pci_device under = fixture();
        struct pci_saved us = {0};
        devices = &under; ndevices = 1; saved = &us; saved_valid = false;
        if (!issued) config[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] |= (uint32_t)PCIE_DEVSTA_PENDING << 16;
        cancel_on_sleep = true;
        CHECK(!pci_function_reset(&under) && under.reset && under.reset->issued == issued && resets == issued,
              "FLRを取り残したまま眠りに入る");
        cancel_on_sleep = false; test_task.killed = false;
        function_accesses = 0;
        CHECK(pci_suspend() == NULL && !function_accesses, "FLR中の機能を保存のために読まない");
        timer_ticks += 5000;    /* 眠っていた間 */
        reset_function(config); /* S3のConventional Reset：TPも消え、構成は初期値 */
        tsc_hz = 1000000000ull; rrs_reads_left = 2; /* 復帰後しばらく初期化中 */
        memset(first, 0, sizeof first);
        pci_resume();
        CHECK(!function_accesses && under.reset && under.reset->issued && under.reset->since == timer_ticks &&
              under.reset->ready == FLR_READY_MS, "復帰では読まず、取り残しFLRを復帰時点で発行済みにする");
        CHECK(pci_function_reset(&under) && resets == issued && !under.reset && !rrs_reads_left,
              "次の回復はFLRを重ねず、初期化中は待って保存構成を戻す");
        CHECK(first[0].seen && first[0].off == 0, "Conventional Reset後の最初のアクセスはVendor IDの読み");
        CHECK(config[0x10 / 4] == test_bar && (config[1] & 0xFFFF) == (7 & ~PCI_COMMAND_MASTER),
              "停止前の構成をBMEだけ除いて戻す");
        if (under.reset) kfree(under.reset);
    }

    /* Root Port配下：5 GT/s超の口はLink訓練完了（DLLLA）から100ms、その間は配下へ照会しない（6.6.1） */
    for (unsigned up = 0; up < 2; up++) {
        struct pci_device pair[2];
        struct pci_saved ps[2] = {{0}};
        pair[1] = visibility_fixture(); pair[0] = root_port; pair[1].upstream = &pair[0];
        root_restored = true; require_visibility = true;
        root_config[(TEST_PCIE_CAP + PCIE_LNKCAP) / 4] = 3 | PCIE_LNKCAP_DLL_REPORTING; /* 8 GT/s */
        root_config[(TEST_PCIE_CAP + PCIE_LNKCTL) / 4] = (uint32_t)(up ? PCIE_LNKSTA_DLL_ACTIVE : 0) << 16;
        devices = pair; ndevices = 2; saved = ps; saved_valid = false;
        CHECK(pci_suspend() == NULL, "Root Portと配下の機能を保存する");
        config[0x10 / 4] = 0; config[1] = 0; function_accesses = 0;
        tsc_hz = up ? 1000000000ull : 1000; rrs_reads_left = up ? 2 : 0;
        pci_resume();
        if (up)
            CHECK(!rrs_reads_left && config[0x10 / 4] == test_bar && !pair[0].rrs_holds &&
                  root_control() == ((1u << 16) | 0x2B), "Link活性後に照会し、RRSの間はRoot PortのRRS可視化を保持する");
        else
            CHECK(!function_accesses && config[0x10 / 4] == 0, "1秒経ってもLinkが活性にならない口の配下へ照会しない");
        root_model = require_visibility = root_restored = false;
    }
    devices = NULL; ndevices = 0; saved = NULL; saved_valid = false;
}

/* 二つのRoot Port（00:01.0・00:02.0、8 GT/s、DLL報告あり）と、それぞれの下の機能（00:00.0・00:00.1） */
static struct pci_device two_ports[4];
static struct pci_saved two_ports_saved[4];
static void two_ports_fixture(void) {
    struct pci_device f = visibility_fixture();
    memcpy(peer_config, config, sizeof config);
    root_config[(TEST_PCIE_CAP + PCIE_LNKCAP) / 4] = 3 | PCIE_LNKCAP_DLL_REPORTING;
    memcpy(root2_config, root_config, sizeof root_config);
    two_ports[0] = two_ports[1] = root_port;
    two_ports[1].dev = 2;
    two_ports[2] = two_ports[3] = f;
    two_ports[3].func = 1;
    two_ports[2].upstream = &two_ports[0];
    two_ports[3].upstream = &two_ports[1];
    memset(two_ports_saved, 0, sizeof two_ports_saved);
    devices = two_ports; ndevices = 4; saved = two_ports_saved; saved_valid = false;
    root_restored = true;
    tsc_hz = 1000000000ull;
}

/* S3復帰：5 GT/s超の口の100msはその口のLink訓練完了から（6.6.1）で、口ごとに独立している。
   全Root PortのLinkを同時に見て、待ちを口の数だけ積み上げない */
static void resume_link_cases(void) {
    const uint64_t ms = 1000000; /* tsc_hz 1e9 の1ms */
    two_ports_fixture();
    CHECK(pci_suspend() == NULL, "二つのRoot Portと配下の機能を保存する");
    config[0x10 / 4] = peer_config[0x10 / 4] = 0; /* 復帰後のreset状態 */
    link_up_at[0] = test_tsc + 10 * ms;
    link_up_at[1] = test_tsc + 30 * ms;
    memset(first, 0, sizeof first);
    pci_resume();
    CHECK(first[0].seen && first[0].at >= link_up_at[0] + 100 * ms && first[1].seen &&
          first[1].at >= link_up_at[1] + 100 * ms, "各口の配下へはその口のLink訓練完了から100ms後まで照会しない");
    CHECK(first[1].seen && first[1].at <= link_up_at[1] + 101 * ms,
          "二つ目の口の100msを一つ目の口の待ちの後へ積み上げない");
    CHECK(config[0x10 / 4] == test_bar && peer_config[0x10 / 4] == test_bar, "両方の口の配下の構成を戻す");

    /* 機能の無い口（空きスロット）：そのLinkは活性にならないが、1秒待たない */
    two_ports_fixture();
    ndevices = 3;
    CHECK(pci_suspend() == NULL, "機能の無い口を含めて保存する");
    uint64_t start = test_tsc;
    link_up_at[0] = start + 10 * ms;
    pci_resume();
    CHECK(test_tsc - start <= 111 * ms && config[0x10 / 4] == test_bar, "機能の無い口のLinkを待たない");

    /* 00:00.1のFLRを取り残したまま眠る（発行前のTP待ち・発行後の完了待ち）：その口も前処理で同時に
       見て、その口の100msは待つが機能は読まない。Root PortのRRS可視化は保持を保つか、新たに取る。
       次の回復の最初のアクセスはLink訓練完了から100ms後のVendor IDの読み */
    for (unsigned issued = 0; issued < 2; issued++) {
        two_ports_fixture();
        struct pci_device *under = &two_ports[3], *port = &two_ports[1];
        if (!issued) peer_config[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] |= (uint32_t)PCIE_DEVSTA_PENDING << 16;
        cancel_on_sleep = true;
        CHECK(!pci_function_reset(under) && under->reset && under->reset->issued == issued && resets == issued &&
              port->rrs_holds == issued, "FLRを取り残す（発行後ならRoot PortにRRS可視化を保持させたまま）");
        cancel_on_sleep = false; test_task.killed = false;
        CHECK(pci_suspend() == NULL, "取り残しFLRを含めて保存する");
        timer_ticks += 5000;
        reset_function(config);
        if (issued) { ready_after_reset = false; progress_bdf = 1; ready_at = timer_ticks + 3; } /* しばらく初期化中 */
        reset_function(peer_config);
        root2_config[(TEST_PCIE_CAP + TEST_ROOTCTL) / 4] &= ~TEST_RRS_ENABLE; /* 復帰ファームウェアのRoot Control */
        link_up_at[0] = test_tsc + 10 * ms;
        link_up_at[1] = test_tsc + 30 * ms;
        memset(first, 0, sizeof first);
        pci_resume();
        CHECK(!first[1].seen && under->reset && under->reset->issued && under->reset->since == timer_ticks,
              "復帰では取り残しFLRの機能を読まず、復帰時点で発行済みにする");
        CHECK(test_tsc >= link_up_at[1] + 100 * ms && test_tsc <= link_up_at[1] + 101 * ms,
              "取り残しFLRの口も他の口と同時に見て、その口の100msまで待つ");
        CHECK(port->rrs_holds == 1 && (root2_config[(TEST_PCIE_CAP + TEST_ROOTCTL) / 4] & TEST_RRS_ENABLE),
              "初期化を待つ間、Root PortのRRS可視化を保つか新たに取る");
        CHECK(config[0x10 / 4] == test_bar, "他の口の配下は通常どおり戻す");
        CHECK(pci_function_reset(under) && resets == issued && !under->reset && peer_config[0x10 / 4] == test_bar,
              "次の回復はFLRを重ねず、RRSの間は待ってから保存構成を戻す");
        CHECK(first[1].seen && first[1].off == 0 && first[1].at >= link_up_at[1] + 100 * ms,
              "最初のアクセスはLink訓練完了から100ms後のVendor IDの読み");
        CHECK(!port->rrs_holds && !(root2_config[(TEST_PCIE_CAP + TEST_ROOTCTL) / 4] & TEST_RRS_ENABLE),
              "準備後にRoot Controlをファームウェアの値へ戻す");
        if (under->reset) kfree(under->reset);
    }

    root_model = root_restored = false;
    devices = NULL; ndevices = 0; saved = NULL; saved_valid = false;
}

/* AML（PCI_Config）の構成アクセス：FLR中の機能へは発行しない（6.6.2の手順1）。ログはresetごとに一度 */
static void config_block_cases(void) {
    struct pci_device d = fixture();
    devices = &d; ndevices = 1;
    CHECK(!pci_config_blocked(0, 0, 0), "FLR中でない機能への構成アクセスは通す");
    cancel_on_sleep = true;
    CHECK(!pci_function_reset(&d) && d.reset, "FLR完了待ちのまま残す");
    cancel_on_sleep = false; test_task.killed = false;
    prints = 0;
    CHECK(pci_config_blocked(0, 0, 0) && pci_config_blocked(0, 0, 0) && prints == 1,
          "FLR中の機能への構成アクセスは発行させず、AMLのポーリングでもログは一度だけ");
    CHECK(!pci_config_blocked(0, 0, 1) && !pci_config_blocked(1, 0, 0), "列挙に無い機能は止めない");
    CHECK(pci_function_reset(&d) && !d.reset && !pci_config_blocked(0, 0, 0), "FLR完了後は通す");
    cancel_on_sleep = true;
    CHECK(!pci_function_reset(&d) && d.reset, "次のFLRを取り残す");
    cancel_on_sleep = false; test_task.killed = false;
    prints = 0;
    CHECK(pci_config_blocked(0, 0, 0) && prints == 1, "次のresetでも最初の拒否を記録する");
    kfree(d.reset);
    devices = NULL; ndevices = 0;
}

static void claim_release_cases(void) {
    for (unsigned mode = 0; mode < 3; mode++) {
        struct pci_device d = fixture();
        d.claimed = true;
        d.needs_reset = false;
        config[1] &= ~PCI_COMMAND_MASTER;
        struct iommu_domain domain = {0};
        struct pci_claim *c = calloc(1, sizeof *c);
        struct vnode *v = calloc(1, sizeof *v);
        if (!c || !v) abort();
        c->d = &d;
        c->v = v;
        c->refs = 2; /* vnodeに加え、claim解放後も残るDMAオブジェクト */
        c->domain = mode ? &domain : NULL;
        v->priv = c;
        blocks = revocations = 0;
        block_confirmed = mode == 2;
        claim_release(v);
        CHECK(revocations == 1 && c->released && !d.claimed, "clean closeでもBAR取消しとclaim返却を行う");
        CHECK(sleeps == 0 && resets == 0 && !d.needs_reset, "clean closeにFLRや待機を追加しない");
        CHECK(blocks == (mode != 0), "domainがある場合だけIOMMU遮断を依頼する");
        CHECK(pci_user_dma_reusable(c) == block_confirmed,
              "BME=0・pending=0でも確認済みIOMMU遮断なしに残存DMAページを再利用しない");
        /* 最後の参照を返す際のdomain解放はこのfixtureの対象外。stackの模擬domainを渡さない。 */
        c->domain = NULL;
        pci_user_drop(c);
    }
}

static void group_fixture(unsigned primary) {
    functions[0]=fixture(); functions[0].needs_reset=false;
    functions[1]=functions[0]; functions[1].func=1;
    memcpy(peer_config,config,sizeof config);
    config[1]&=~PCI_COMMAND_MASTER;
    devices=functions; ndevices=2; init_pid=test_task.pid;
    members[0]=&functions[primary]; members[1]=&functions[primary^1];
    group_domain=(struct iommu_domain){.members=members,.nmembers=2};
    block_confirmed=claim_model=true;
    fb=(struct fb_info){0};
}
static struct file *group_claim(unsigned primary) {
    struct file *f=NULL;
    CHECK(pci_user_claim(0,0,primary,&f)==0 && f, "回復済みgroupのclaimを公開する");
    return f;
}
static void group_close(struct file *f) { if (f) { claim_release(f->vn); free(f); } }
static void group_failure(unsigned primary) {
    struct file *f=NULL;
    CHECK(pci_user_claim(0,0,primary,&f)==-N_ERR_NOT_READY && !f,
          "未回復peerを持つgroupからhandleを公開しない");
    if (f) group_close(f);
    CHECK(!functions[primary].claimed && !domain_attached && domain_frees,
          "回復失敗は予約を戻しdomainを再遮断する");
}
static void group_recovery_cases(void) {
    group_fixture(0);
    struct file *f=group_claim(0);
    CHECK(!(peer_config[1]&PCI_COMMAND_MASTER) && !resets,
          "fresh peerもBMEoffを確認しclean singleton外のFLRを捏造しない");
    group_close(f);
    CHECK(functions[0].needs_reset && functions[1].needs_reset,
          "primaryがcleanでも旧groupの全機能を回復対象にする");
    CHECK(!sleeps && !resets, "group解放に待機やFLRを追加しない");
    members[0]=&functions[1]; members[1]=&functions[0];
    check_reservation=true;
    f=group_claim(1);
    CHECK(resets==2 && !functions[0].needs_reset && !functions[1].needs_reset,
          "primary交代でも旧primaryをpeerとしてFLRする");
    group_close(f);

    group_fixture(0); functions[1].needs_reset=true; check_reservation=true;
    peer_config[1]|=0x100000;
    peer_config[0x34/4]=0x60; peer_config[0x60/4]=0x00017005; peer_config[0x70/4]=0x80000011;
    f=group_claim(0);
    CHECK(resets==1 && reset_bdf==1 && msi_clears && !(peer_config[0x60/4]&0x10000) &&
          !(peer_config[0x70/4]&0x80000000), "dirty非primaryだけFLRし旧MSI Enableを復元しない");
    group_close(f);

    group_fixture(0); refuse_peer_master_clear=true; group_failure(0);
    CHECK(!resets, "fresh peer BMEoff未確認ではFLRもhandle公開もしない");
    group_failure(0);
    CHECK(!resets && functions[1].needs_reset,
          "dirty peerもBMEoff未確認のまま現CT値だけで汎用FLRを開始しない");
    group_fixture(0); functions[1].needs_reset=true; functions[1].pcie_cap=0; group_failure(0);
    CHECK(!resets, "FLR不能peerをBMEoffだけで回復済みにしない");
    group_fixture(0); functions[1].needs_reset=true;
    functions[1].bar[0]=test_bar; functions[1].bar_size[0]=PAGE_SIZE;
    fb.addr=(void *)1; fb.phys=test_bar; group_failure(0);
    CHECK(!resets, "dirty boot display peerを汎用FLRしない");
    group_fixture(0); functions[1].needs_reset=true; refuse_irq_clear=true; group_failure(0);
    CHECK(!resets, "peer interrupt invalidation未確認ではFLRしない");
    group_fixture(0); functions[1].needs_reset=true;
    peer_config[1]|=0x100000; peer_config[0x34/4]=0x60; peer_config[0x60/4]=0x10005;
    refuse_msi_disable=true; group_failure(0);
    CHECK(!resets, "peer MSI Enable停止readback失敗ではFLRしない");
    group_fixture(0); functions[1].needs_reset=true;
    peer_config[1]|=0x100000; peer_config[0x34/4]=0x70; peer_config[0x70/4]=0x80000011;
    refuse_msix_disable=true; group_failure(0);
    CHECK(!resets, "peer MSI-X Enable停止readback失敗ではFLRしない");
    group_fixture(0); functions[0].needs_reset=true;
    config[1]|=0x100000; config[0x34/4]=0x60; config[0x60/4]=0x10005;
    refuse_msi_disable=true; group_failure(0);
    CHECK(!resets, "dirty primaryもMSI Enable停止readback失敗ではFLRしない");
    group_fixture(0); functions[1].needs_reset=true; ready_after_reset=false;
    cancel_at=FIRST_ACCESS; group_failure(0);
    CHECK(resets==1 && functions[1].reset && functions[1].needs_reset,
          "peer FLR未準備の保存状態とdirtyを維持する");
    peer_config[0]=test_id; ready_after_reset=true; cancel_at=0; test_task.killed=false;
    check_attach_ready=true;
    f=group_claim(0);
    CHECK(resets==1 && !functions[1].reset && !functions[1].needs_reset,
          "peer FLR再試行は保存状態から再開し二重resetしない");
    group_close(f);
    group_fixture(0); functions[1].needs_reset=true; cancel_on_sleep=true; group_failure(0);
    CHECK(resets==1 && functions[1].reset && functions[1].needs_reset,
          "peer FLR取消しで保存状態を失わない");
    cancel_on_sleep=false; test_task.killed=false;
    check_pending_access=true; check_attach_ready=true;
    f=group_claim(0);
    CHECK(resets==1 && !functions[1].reset && timer_ticks==FIRST_ACCESS,
          "取消後peer FLRを再発行せず残り時間だけ待つ");
    group_close(f);
    group_fixture(0); group_domain.nmembers=1; functions[0].needs_reset=true;
    cancel_on_sleep=true; group_failure(0);
    CHECK(functions[0].reset && timer_ticks==1, "primaryの保留FLRも取消時点を維持する");
    cancel_on_sleep=false; test_task.killed=false; check_pending_access=true;
    f=group_claim(0);
    CHECK(resets==1 && !functions[0].reset && timer_ticks==FIRST_ACCESS,
          "primaryのguard/MSI取得より先に保存FLRの残waitを再開する");
    group_close(f);

    /* 準備できないまま残った他機能のFLR：そのclaimは公開しても、FLR中の機能の構成は読ませない（6.6.2） */
    group_fixture(0); group_domain.nmembers=1; ready_after_reset=false; cancel_on_sleep=true;
    CHECK(!pci_function_reset(&functions[1]) && functions[1].reset, "一機能のFLRを取り残す");
    cancel_on_sleep=false; test_task.killed=false;
    f=group_claim(0);
    CHECK(functions[1].reset && timer_ticks==FLR_READY_MS+1,
          "取り残しの完了を一度試み、1秒で準備できなければ保存状態のまま残す");
    CHECK(f && pci_user_config_read(f,N_PCI_TARGET(0,0,1),0,4)==-N_ERR_NOT_READY,
          "FLR中の他機能の構成読出しは未準備として拒否する");
    group_close(f);
    kfree(functions[1].reset); functions[1].reset=NULL;

    group_fixture(0);
    f=group_claim(0);
    struct pci_claim *c=f?pci_user_hold(f):NULL;
    peer_config[1]|=PCI_COMMAND_MASTER; refuse_peer_master_clear=true;
    group_close(f);
    CHECK(c && functions[0].needs_reset && functions[1].needs_reset && !sleeps && !resets,
          "peer停止拒否でもexitは待たずgroup全体をdirtyにする");
    CHECK(c && pci_user_dma_reusable(c), "peer BMEが残っても確認済みIOMMU遮断がRAM回収条件");
    if (c) pci_user_drop(c);

    group_fixture(0); group_domain.nmembers=1; functions[0].needs_reset=true;
    functions[0].pcie_cap=0; config[1]|=PCI_COMMAND_MASTER; refuse_master_clear=true;
    group_failure(0);
    CHECK(!resets, "無FLR primaryのnative例外でもBME停止未確認ならhandleを出さない");
    group_fixture(0); group_domain.nmembers=1; functions[0].needs_reset=true;
    config[1]|=PCI_COMMAND_MASTER; refuse_master_clear=true;
    group_failure(0);
    CHECK(!resets && functions[0].needs_reset,
          "FLR可能primaryでもBMEclear拒否時は現CT値を安全根拠にしない");
    group_fixture(0); group_domain.nmembers=1;
    f=group_claim(0); group_close(f);
    CHECK(!functions[0].needs_reset && !resets && !sleeps && !msi_clears,
          "clean singletonへ不要な回復やIRQ操作を追加しない");
    group_fixture(0); group_domain.nmembers=1; functions[0].needs_reset=true;
    functions[0].bar[0]=test_bar; functions[0].bar_size[0]=PAGE_SIZE;
    fb.addr=(void *)1; fb.phys=test_bar;
    f=group_claim(0);
    struct n_pci_function info;
    CHECK(f && pci_user_info(f,&info)==0 && (info.flags&N_PCI_NEEDS_RECOVERY) && !resets,
          "boot display primaryのnative回復責務を維持しgeneric FLRしない");
    group_close(f);
    group_fixture(0); test_task.killed=true; group_failure(0);
    CHECK(!resets, "取消済みgroup claimは新たなFLRを始めない");
    group_fixture(0); refuse_open=true;
    f=NULL;
    CHECK(pci_user_claim(0,0,0,&f)==-N_ERR_NO_MEMORY && !f && !functions[0].claimed &&
          !domain_attached && domain_frees, "file確保失敗でも予約を解除しgroupを遮断する");

    group_fixture(0); functions[1].needs_reset=true; check_reservation=true;
    peer_config[(TEST_PCIE_CAP + PCIE_DEVSTA)/4]|=(uint32_t)PCIE_DEVSTA_PENDING<<16;
    peer_config[(TEST_PCIE_CAP + PCIE_DEVCTL2)/4]=PCIE_DEVCTL2_CT_DISABLE;
    progress_bdf=1; pending_clear_at=3;
    f=group_claim(0);
    CHECK(resets==1 && !functions[1].needs_reset && timer_ticks==pending_clear_at+FIRST_ACCESS,
          "group予約を保持したままCT disabled peerの実TP完了を待って再claimする");
    group_close(f);

    group_fixture(0); functions[1].needs_reset=true; check_reservation=true;
    ready_after_reset=false; progress_bdf=1; ready_at=FLR_DONE_MS+3;
    f=group_claim(0);
    CHECK(resets==1 && !functions[1].needs_reset && !functions[1].reset && timer_ticks==ready_at,
          "group予約を保持したままpeerのRRSから回復し同一claimを公開する");
    CHECK(f && pci_user_config_write(f,TEST_PCIE_CAP+PCIE_LNKCTL,2,PCIE_LNKCTL_DISABLE)==-N_ERR_DENIED,
          "named Link Control guardでもLink Disableを許さない");
    CHECK(f && pci_user_config_write(f,TEST_PCIE_CAP+PCIE_LNKCTL2,2,PCIE_LNKCTL2_COMPLIANCE)==-N_ERR_DENIED &&
          pci_user_config_write(f,TEST_PCIE_CAP+PCIE_LNKCTL2,2,PCIE_LNKCTL2_MOD_COMPLIANCE)==-N_ERR_DENIED,
          "named Link Control 2 guardでも両Compliance変更を許さない");
    group_close(f);
    claim_model=false;
}

int main(void) {
    reset_restore_cases();
    pending_resume_cases();
    completion_window_cases();
    readiness_time_cases();
    recovery_progress_cases();
    completion_history_cases();
    visibility_cases();
    resume_cases();
    resume_link_cases();
    config_block_cases();
    claim_release_cases();
    group_recovery_cases();
    printf("PCIホスト検査: %u件失敗（実機DMA・画面・QEMUは未検証）\n", failures);
    return failures != 0;
}
