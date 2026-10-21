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
static uint64_t test_irq_save(void) {
    if (kill_on_irq_save && test_current) test_current->killed = true;
    return 0;
}
static void test_irq_restore(uint64_t flags) { (void)flags; }
#define irq_save test_irq_save
#define irq_restore test_irq_restore

static void config_outl(uint16_t port, uint32_t value);
static uint32_t config_inl(uint16_t port);
static void config_outw(uint16_t port, uint16_t value);
static void config_outb(uint16_t port, uint8_t value);
/* cpu.hは既に読み込んだ。製品のconfig accessorを残し、特権I/Oだけを置き換える。 */
#define outl config_outl
#define inl config_inl
#define outw config_outw
#define outb config_outb
#include "../kernel/src/dev/pci.c"
#undef outl
#undef inl
#undef outw
#undef outb
static struct task *test_process(void) { return test_current; }
#define task_process test_process
#include "../kernel/src/dev/pciuser.c"
#undef task_process
#undef irq_save
#undef irq_restore

uint64_t hhdm_offset;
volatile uint64_t timer_ticks;
static struct task test_task;
static uint32_t config[4096 / sizeof(uint32_t)], selected;
static uint32_t peer_config[4096 / sizeof(uint32_t)];
static uint32_t root_config[4096 / sizeof(uint32_t)];
static struct pci_device root_port;
static bool root_model, require_visibility, refuse_visibility, lose_visibility_on_sleep;
static bool untimed_request;
static unsigned root_writes, root_narrow_writes;
static unsigned resets, sleeps, failures;
static bool refuse_master_clear, ready_after_reset, cancel_on_sleep, force_master_after_reset;
static bool force_pending_after_reset;
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
       TEST_RRS_ENABLE = 1u << 4, TEST_ROOT_BDF = 1u << 3 };

#define CHECK(condition, message) do { \
    if (!(condition)) { printf("失敗: %s (行%d)\n", message, __LINE__); failures++; } \
} while (0)

void *kzalloc(size_t bytes) { return calloc(1, bytes); }
void *kmalloc(size_t bytes) { return malloc(bytes); }
void kfree(void *p) { free(p); }
void *vmm_map_mmio(uint64_t phys, size_t bytes) { (void)bytes; return phys_to_virt(phys); }
int kprintf(const char *fmt, ...) { (void)fmt; return 0; }
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
int iommu_attach(struct pci_device *d, struct iommu_domain **out) {
    if (!claim_model) abort();
    if (domain_attached) return -N_ERR_BUSY;
    group_domain.d=d; group_domain.blocked=false; domain_attached=true; *out=&group_domain;
    return 0;
}
bool iommu_active(void) { abort(); }
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
    timer_ticks += cancel_on_sleep ? 1 : ms;
    uint32_t *progress = progress_bdf ? peer_config : config;
    if (pending_clear_at && timer_ticks >= pending_clear_at)
        progress[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] &= ~((uint32_t)PCIE_DEVSTA_PENDING << 16);
    if (pending_clear_at && timer_ticks >= pending_clear_at) untimed_request = false;
    if (lose_visibility_on_sleep) root_config[(TEST_PCIE_CAP + TEST_ROOTCTL) / 4] &= ~TEST_RRS_ENABLE;
    if (ready_at && resets && timer_ticks >= ready_at) progress[0] = progress_id;
    if (cancel_at && timer_ticks >= cancel_at) test_task.killed = true;
    if (cancel_on_sleep) test_task.killed = true;
}

static unsigned config_offset(uint16_t port) { return (selected & 0xFC) + (port - 0xCFC); }
static uint32_t *selected_config(void) {
    unsigned bdf=(selected>>8)&0xFFFF;
    if (root_model && bdf == TEST_ROOT_BDF) return root_config;
    CHECK(bdf==0 || bdf==1, "モデルの二機能以外へconfigを送らない");
    if (root_model && require_visibility && (resets || untimed_request))
        CHECK(root_config[(TEST_PCIE_CAP + TEST_ROOTCTL) / 4] & TEST_RRS_ENABLE,
              "保留FLR再開とRRS照会より先に実Root Portのvisibilityを再確認する");
    if (bdf<2 && claim_model && check_pending_access && functions[bdf].reset) {
        struct pci_reset *r=functions[bdf].reset;
        CHECK(timer_ticks-r->since>=r->wait, "pending FLRの残waitより先にconfigへアクセスしない");
    }
    uint32_t *r=bdf==1?peer_config:config;
    if (resets && r[0]==PCI_CONFIG_RRS_ID)
        CHECK((selected&0xFC)==0, "RRS中はVendor IDだけを照会し未準備のconfigを復元しない");
    return r;
}
static uint32_t config_inl(uint16_t port) {
    CHECK(port == 0xCFC, "config読出しport");
    return selected_config()[(selected & 0xFC) / 4];
}
static void emulate_flr(void) {
    CHECK(!untimed_request, "CTを再有効化しても古いuntimed Requestが残る間はFLRしない");
    if (require_visibility)
        CHECK(root_config[(TEST_PCIE_CAP + TEST_ROOTCTL) / 4] & TEST_RRS_ENABLE,
              "FLRの発行前に対応Root PortのRRS visibilityを有効化する");
    resets++;
    reset_bdf=(selected>>8)&0xFFFF;
    uint32_t *r=selected_config(), caps=r[1]&0x100000, cap_head=r[0x34/4];
    uint32_t msi=r[0x60/4]&~0x710000u, msix=r[0x70/4]&~0xC0000000u;
    memset(r, 0, sizeof config);
    r[0] = ready_after_reset ? test_id : PCI_CONFIG_RRS_ID;
    r[1]=caps; r[0x34/4]=cap_head; r[0x60/4]=msi; r[0x70/4]=msix;
    r[TEST_PCIE_CAP / 4] = 0x10;
    r[(TEST_PCIE_CAP + PCIE_DEVCAP) / 4] = PCIE_DEVCAP_FLR;
    if (force_pending_after_reset) r[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] |= (uint32_t)PCIE_DEVSTA_PENDING << 16;
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
        CHECK(off == TEST_PCIE_CAP + TEST_ROOTCTL, "Root PortへはRoot Controlだけnarrow writeする");
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

static struct pci_device fixture(void) {
    memset(config, 0, sizeof config);
    memset(&test_task, 0, sizeof test_task);
    current_task = &test_task;
    selected = resets = sleeps = 0;
    timer_ticks = 0;
    refuse_master_clear = cancel_on_sleep = force_master_after_reset = false;
    force_pending_after_reset = false;
    ready_after_reset = true;
    claim_model=domain_attached=check_reservation=refuse_peer_master_clear=refuse_irq_clear=false;
    refuse_msi_disable=refuse_msix_disable=refuse_open=false; msi_clears=domain_frees=reset_bdf=0;
    check_pending_access=false;
    pending_clear_at=ready_at=cancel_at=0; progress_bdf=0; progress_id=test_id;
    kill_on_irq_save=false;
    root_model=require_visibility=refuse_visibility=lose_visibility_on_sleep=untimed_request=false;
    root_writes=root_narrow_writes=0;
    ecam_off = true;
    config[0] = test_id;
    config[1] = 7;
    config[0x10 / 4] = test_bar;
    config[TEST_PCIE_CAP / 4] = 0x10;
    config[(TEST_PCIE_CAP + PCIE_DEVCAP) / 4] = PCIE_DEVCAP_FLR;
    return (struct pci_device){.vendor = test_id & 0xFFFF, .device = test_id >> 16,
                               .pcie_cap = TEST_PCIE_CAP, .needs_reset = true};
}

static void reset_restore_cases(void) {
    struct pci_device d = fixture();
    refuse_master_clear = true;
    config[(TEST_PCIE_CAP + PCIE_DEVCTL2) / 4] = 6; /* 65..210 ms: 表7-34 */
    CHECK(!pci_function_reset(&d) && !resets && !d.reset,
          "BME停止未確認では現CT既知でも新要求を禁止できずFLRしない");
    d = fixture();
    config[(TEST_PCIE_CAP + PCIE_DEVCTL2) / 4] = 14; /* 17..64秒でもTP0の後に追加待機しない。 */
    CHECK(pci_function_reset(&d) && resets == 1 && timer_ticks == FLR_DONE_MS,
          "BMEoffとTP0実測後はCT上限64秒を不要に課さない");
    CHECK(!(config[1] & PCI_COMMAND_MASTER), "保存時BME=1を復元して再enableしない");
    CHECK(config[0x10 / 4] == test_bar && d.reset == NULL, "BAR復元と完了状態の回収");

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
    cancel_at = FLR_DONE_MS + 1;
    CHECK(!pci_function_reset(&d) && d.reset != NULL, "RRS中の装置へ設定を復元しない");
    CHECK(resets == 1 && timer_ticks == cancel_at && test_task.killed,
          "100ms後のRRSはyieldし架空の1秒期限なしで取消しに応答する");
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
    CHECK(pci_function_reset(&d) && resets == 1 && timer_ticks == FLR_DONE_MS,
          "取消後は残りの仕様時間だけ待って復元を再開");
    CHECK(config[0x10 / 4] == test_bar && !(config[1] & PCI_COMMAND_MASTER), "再開後BARとBME");

    d = fixture();
    kill_on_irq_save = true;
    CHECK(!pci_function_reset(&d) && d.reset && resets == 1 && !sleeps,
          "killが外側検査とwait登録の境界に来ても長時間sleepへ登録しない");
    CHECK(d.reset && d.reset->state.hdr[4] == test_bar && timer_ticks == 0,
          "境界取消でもFLR前の保存状態と開始時刻を維持する");
    kill_on_irq_save = false; test_task.killed = false;
    CHECK(pci_function_reset(&d) && !d.reset && resets == 1 && timer_ticks == FLR_DONE_MS,
          "境界取消後は再resetせず残りの仕様待機から復元する");

    d = fixture();
    force_pending_after_reset = true;
    CHECK(!pci_function_reset(&d) && d.reset != NULL, "IDが準備済みでもreset後pendingが残れば復元を保留");
    CHECK(config[0x10 / 4] == 0, "pendingの残る機能へBARを復元しない");
    config[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] &= ~((uint32_t)PCIE_DEVSTA_PENDING << 16);
    CHECK(pci_function_reset(&d) && d.reset == NULL && resets == 1,
          "pending消失後は既存の保存状態から復元し二度目のFLRをしない");
}

static void unknown_completion_cases(void) {
    const unsigned unknown[] = {PCIE_DEVCTL2_CT_DISABLE, 3}; /* 無効、予約符号 */
    for (unsigned i = 0; i < sizeof unknown / sizeof unknown[0]; i++) {
        struct pci_device d = fixture();
        config[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] |= (uint32_t)PCIE_DEVSTA_PENDING << 16;
        config[(TEST_PCIE_CAP + PCIE_DEVCTL2) / 4] = unknown[i];
        cancel_on_sleep = true;
        CHECK(!pci_function_reset(&d), "pending要求の完了境界が不明ならFLRを発行しない");
        CHECK(resets == 0 && sleeps == 1 && d.reset == NULL && test_task.killed,
              "TPが残る間はyield後の取消で戻り100ms/64秒を捏造しない");
        CHECK(!pci_requests_done(&d, 0) && sleeps == 1, "解放時pending判定は新たに待機しない");
        test_task.killed = false;
        cancel_on_sleep = false;
        refuse_master_clear = true;
        config[1] |= PCI_COMMAND_MASTER;
        unsigned before = sleeps;
        CHECK(!pci_function_reset(&d) && !resets && sleeps == before && !d.reset,
              "BME stuckかつCT不明ならTPの瞬間値を安全根拠にせずFLRを拒否する");
    }
    struct pci_device d = fixture();
    config[0] = PCI_CONFIG_RRS_ID;
    CHECK(!pci_requests_done(&d, 0), "RRS Vendor IDを正常な装置の完了としない");
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
    CHECK(flr_ready_ms(&d) == 1007, "有効なRTR 1.00663296秒をms単位へ切り上げる");
    config[0x108 / 4] = 0xA1F;
    CHECK(flr_ready_ms(&d) == FLR_DONE_MS, "仕様最大値を超すRTR値を根拠にしない");
    config[0x104 / 4] = 0;
    CHECK(flr_ready_ms(&d) == FLR_DONE_MS, "Valid=0のRTR値は使わない");
    ecam_off = true;
}

static void recovery_progress_cases(void) {
    const unsigned unknown[] = {PCIE_DEVCTL2_CT_DISABLE, 3, 0, 6, 14};
    for (unsigned i = 0; i < sizeof unknown / sizeof unknown[0]; i++) {
        struct pci_device d = fixture();
        config[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] |= (uint32_t)PCIE_DEVSTA_PENDING << 16;
        config[(TEST_PCIE_CAP + PCIE_DEVCTL2) / 4] = unknown[i];
        pending_clear_at = 3; /* 模擬装置が既存要求を実際に完了する時刻。製品の期限ではない。 */
        CHECK(pci_function_reset(&d), "全CT値でBMEoff後にTP消失を実測してからFLRする");
        CHECK(resets == 1 && timer_ticks == pending_clear_at + FLR_DONE_MS && !d.reset,
              "CTの推測期限を使わずTP実消失後にFLRの仕様時間を待つ");
        CHECK(config[0x10 / 4] == test_bar && !(config[1] & PCI_COMMAND_MASTER),
              "TP待機後もBARを復元しBMEを再開しない");
        if (d.reset) kfree(d.reset);
    }
    struct pci_device d = fixture();
    ready_after_reset = false;
    ready_at = FLR_DONE_MS + 3; /* RTRを持たず100ms後にもRRSを返す装置。 */
    CHECK(pci_function_reset(&d), "RTRなしのRRSも同じclaim内で再照会し準備完了後に復元する");
    CHECK(resets == 1 && timer_ticks == ready_at && !d.reset && config[0x10 / 4] == test_bar,
          "RRS待機に架空の1秒期限も二度目のFLRも追加しない");
    if (d.reset) kfree(d.reset);

    d = fixture();
    ready_after_reset = false; ready_at = FLR_DONE_MS + 1; progress_id = UINT32_MAX;
    CHECK(!pci_function_reset(&d) && d.reset && config[0x10 / 4] == 0,
          "RRSから装置不在へ変わっても成功やBAR復元にしない");
    config[0] = test_id; ready_at = 0;
    CHECK(pci_function_reset(&d) && resets == 1 && !d.reset && config[0x10 / 4] == test_bar,
          "装置が再び応答すれば同じ保存状態から復元する");

    d = fixture();
    config[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] |= (uint32_t)PCIE_DEVSTA_PENDING << 16;
    config[(TEST_PCIE_CAP + PCIE_DEVCTL2) / 4] = PCIE_DEVCTL2_CT_DISABLE;
    cancel_on_sleep = true;
    CHECK(!pci_function_reset(&d) && !resets && !d.reset, "TP消失前の取消ではFLRを始めない");
    cancel_on_sleep = false; test_task.killed = false; pending_clear_at = timer_ticks + 2;
    CHECK(pci_function_reset(&d) && resets == 1 && !d.reset,
          "TP待機を取り消した後も実際の完了を待って回復できる");
}

static void completion_history_cases(void) {
    struct pci_device d = fixture();
    struct pci_claim c = {.d = &d};
    struct vnode v = {.ops = &claim_ops, .priv = &c};
    struct file f = {.vn = &v};
    guards_init(&c);
    CHECK(pci_user_config_write(&f, TEST_PCIE_CAP + PCIE_DEVCTL2, 2, PCIE_DEVCTL2_CT_DISABLE) == 0,
          "driverの正当なCT設定操作を人工禁止しない");
    config[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] |= (uint32_t)PCIE_DEVSTA_PENDING << 16;
    untimed_request = true;
    CHECK(pci_user_config_write(&f, TEST_PCIE_CAP + PCIE_DEVCTL2, 2, 0) == 0,
          "CT Disable clearは既存Requestへtimerを適用する保証ではない");
    pending_clear_at = FLR_DONE_MS + 3; /* 古いuntimed Requestの実Completion到着。 */
    CHECK(pci_function_reset(&d) && !untimed_request && resets == 1 &&
          timer_ticks == pending_clear_at + FLR_DONE_MS,
          "現CT=enabled/defaultでも旧untimed Completion到着前にreset/復元しない");
    d = fixture();
    config[(TEST_PCIE_CAP + PCIE_DEVSTA) / 4] |= (uint32_t)PCIE_DEVSTA_PENDING << 16;
    cancel_on_sleep = true;
    CHECK(!pci_function_reset(&d) && !resets && !d.reset && sleeps == 1 && test_task.killed,
          "現CT既知でもTPが残る間の取消でFLR前に戻る");
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

static void visibility_cases(void) {
    struct pci_device d = visibility_fixture();
    require_visibility = true; ready_after_reset = false; ready_at = FLR_DONE_MS + 3;
    CHECK(pci_function_reset(&d) && root_writes == 1 && root_narrow_writes == 1,
          "対応Root Portのdefault-off RRS visibilityを一度narrow writeして進行する");
    CHECK(root_config[(TEST_PCIE_CAP + TEST_ROOTCTL) / 4] == ((1u << 16) | 0x2B | TEST_RRS_ENABLE),
          "Root Controlの他bitと隣接Root Capabilitiesを保存する");

    d = visibility_fixture(); require_visibility = true; cancel_on_sleep = true;
    CHECK(!pci_function_reset(&d) && d.reset && resets == 1, "visible FLRの取消でも保存状態を保持する");
    root_config[(TEST_PCIE_CAP + TEST_ROOTCTL) / 4] &= ~TEST_RRS_ENABLE;
    cancel_on_sleep = false; test_task.killed = false; check_pending_access = true;
    CHECK(pci_function_reset(&d) && !d.reset && resets == 1 && root_writes == 2,
          "Root Control喪失後のpending resetも対象configより前にvisibilityを再確認する");

    d = visibility_fixture(); require_visibility = true; lose_visibility_on_sleep = true;
    CHECK(pci_function_reset(&d) && root_writes == 2,
          "FLR待機中のRoot Control喪失もreadiness read前に再設定する");

    for (unsigned mode = 0; mode < 6; mode++) {
        d = visibility_fixture();
        if (mode == 0) root_config[(TEST_PCIE_CAP + TEST_ROOTCTL) / 4] &= ~(1u << 16);
        if (mode == 1) root_config[0] = UINT32_MAX;
        if (mode == 2) root_config[TEST_PCIE_CAP / 4] = 0x10 | (6u << 20);
        if (mode == 3) root_config[TEST_PCIE_CAP / 4] = 0x05 | (4u << 20);
        if (mode == 4) root_port.dev = 32;
        if (mode == 5) root_port.pcie_cap = 0xFC;
        CHECK(pci_function_reset(&d) && !root_writes,
              "非対応/不在/別cap/type/不正BDF/短いcapのRoot Portへ書かず従来FLRを妨げない");
    }
    d = visibility_fixture(); refuse_visibility = true;
    CHECK(pci_function_reset(&d) && !(root_config[(TEST_PCIE_CAP + TEST_ROOTCTL) / 4] & TEST_RRS_ENABLE),
          "visibility書込拒否を可視化成功と偽らず既存ready装置のFLRも拒否しない");
    CHECK(!reset_rrs_visible(&d), "visibilityのEnable readback拒否をhelperも成功と報告しない");
    d = visibility_fixture();
    struct pci_device switch_port = {.pcie_type = 6, .pcie_cap = TEST_PCIE_CAP, .upstream = &root_port};
    d.upstream = &switch_port; require_visibility = true;
    CHECK(pci_function_reset(&d) && root_writes == 1,
          "switch portを誤選択せず実上流Root Portの対応だけを使用する");
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
    cancel_at=FLR_DONE_MS+1; group_failure(0);
    CHECK(resets==1 && functions[1].reset && functions[1].needs_reset,
          "peer FLR未準備の保存状態とdirtyを維持する");
    peer_config[0]=test_id; ready_after_reset=true; cancel_at=0; test_task.killed=false;
    f=group_claim(0);
    CHECK(resets==1 && !functions[1].reset && !functions[1].needs_reset,
          "peer FLR再試行は保存状態から再開し二重resetしない");
    group_close(f);
    group_fixture(0); functions[1].needs_reset=true; cancel_on_sleep=true; group_failure(0);
    CHECK(resets==1 && functions[1].reset && functions[1].needs_reset,
          "peer FLR取消しで保存状態を失わない");
    cancel_on_sleep=false; test_task.killed=false;
    check_pending_access=true;
    f=group_claim(0);
    CHECK(resets==1 && !functions[1].reset, "取消後peer FLRを再発行せず残り時間だけ待つ");
    group_close(f);
    group_fixture(0); group_domain.nmembers=1; functions[0].needs_reset=true;
    cancel_on_sleep=true; group_failure(0);
    CHECK(functions[0].reset && timer_ticks==1, "primaryの保留FLRも取消時点を維持する");
    cancel_on_sleep=false; test_task.killed=false; check_pending_access=true;
    f=group_claim(0);
    CHECK(resets==1 && !functions[0].reset && timer_ticks==FLR_DONE_MS,
          "primaryのguard/MSI取得より先に保存FLRの残waitを再開する");
    group_close(f);

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
    CHECK(resets==1 && !functions[1].needs_reset && timer_ticks==pending_clear_at+FLR_DONE_MS,
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
    unknown_completion_cases();
    readiness_time_cases();
    recovery_progress_cases();
    completion_history_cases();
    visibility_cases();
    claim_release_cases();
    group_recovery_cases();
    printf("PCIホスト検査: %u件失敗（実機DMA・画面・QEMUは未検証）\n", failures);
    return failures != 0;
}
