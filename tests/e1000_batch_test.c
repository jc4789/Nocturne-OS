/* 実e1000.cを使う限定ホスト検査。DMA/MMIOとIRQ状態だけを模擬し、実NICの代替にはしない。 */
#include "../kernel/src/kernel.h"
#include "../kernel/src/arch/cpu.h"

static bool test_irq_enabled = true;
static void (*test_irq_boundary)(void);
static uint64_t test_irq_save(void) {
    uint64_t flags = test_irq_enabled ? 0x200 : 0;
    test_irq_enabled = false;
    if (test_irq_boundary) {
        void (*hook)(void) = test_irq_boundary;
        test_irq_boundary = NULL;
        hook();
    }
    return flags;
}
static void test_irq_restore(uint64_t flags) {
    if (flags & 0x200) test_irq_enabled = true;
}
#define irq_save test_irq_save
#define irq_restore test_irq_restore
#include "../kernel/src/net/e1000.c"
#undef irq_save
#undef irq_restore

extern int printf(const char *fmt, ...);
const char *kernel_cmdline;
uint64_t hhdm_offset;
volatile uint64_t timer_ticks;

/* 未実行のprobe経路のリンク用。検査ではMMIOとringを直接設定する。 */
uint64_t pmm_alloc(void) { return 0; }
uint64_t pmm_alloc_zeroed(void) { return 0; }
void pmm_free(uint64_t phys) { (void)phys; }
void *vmm_map_mmio(uint64_t phys, uint64_t size) { (void)phys; (void)size; return NULL; }
struct pci_device *pci_find(uint16_t vendor, uint16_t device) { (void)vendor; (void)device; return NULL; }
void pci_enable_busmaster(struct pci_device *device) { (void)device; }
void irq_register(int irq, irq_handler_t handler) { (void)irq; (void)handler; }
void pic_unmask(int irq) { (void)irq; }
void net_register(struct netif *iface) { (void)iface; }
void net_delay_ms(uint64_t ms) { (void)ms; }
int kprintf(const char *fmt, ...) { (void)fmt; return 0; }

static uint32_t registers[(REG_RAH + sizeof(uint32_t)) / sizeof(uint32_t)];
static struct rx_desc rx_descriptors[NRX];
static struct tx_desc tx_descriptors[NTX];
static uint8_t rx_pixels[NRX][BUFSZ], tx_pixels[NTX][BUFSZ];
static unsigned failures, checks, notifications, inputs;
static uint32_t expected_tail;

void net_rx_notify(void) { notifications++; }

static void check(bool condition, const char *label) {
    checks++;
    if (!condition) {
        failures++;
        printf("失敗: %s\n", label);
    }
}

static void prepare(void) {
    memset(registers, 0, sizeof registers);
    memset(rx_descriptors, 0, sizeof rx_descriptors);
    memset(tx_descriptors, 0, sizeof tx_descriptors);
    rx = rx_descriptors;
    tx = tx_descriptors;
    mmio = (volatile uint8_t *)registers;
    rx_cur = tx_cur = tx_clean = 0;
    rx_irq_pending = false;
    test_irq_enabled = true;
    test_irq_boundary = NULL;
    notifications = inputs = 0;
    expected_tail = NRX - 1;
    registers[REG_RDT / sizeof(uint32_t)] = expected_tail;
    for (size_t i = 0; i < NRX; i++) rxbuf[i] = rx_pixels[i];
    for (size_t i = 0; i < NTX; i++) {
        txbuf[i] = tx_pixels[i];
        tx[i].status = DESC_DD;
    }
}

static void receive_at(size_t index) {
    rx_pixels[index][0] = (uint8_t)index;
    rx_descriptors[index].len = sizeof(uint32_t);
    rx_descriptors[index].status = DESC_DD | RX_EOP;
}

static void consume(const void *frame, size_t len) {
    check(frame == rxbuf[rx_cur] && len == sizeof(uint32_t), "借用bufferと長さ");
    check(rx[rx_cur].status & DESC_DD, "callbackが終わるまでDMA所有権を返さない");
    check(*(const uint8_t *)frame == (uint8_t)rx_cur, "借用画素の完全性");
    check(rd(REG_RDT) == expected_tail, "callback中にRDTを部分更新しない");
    inputs++;
}

static void arrival_after_empty(void) { receive_at((size_t)rx_cur); }

static void receive_cases(void) {
    prepare();
    receive_at(0); receive_at(1); receive_at(2);
    check(e1000_poll_rx(consume) == 3 && inputs == 3, "3packetを同じbatchで消費");
    check(rd(REG_RDT) == 2 && rx_cur == 3, "batch末尾のRDTとhead");
    check(!e1000_rx_pending() && test_irq_enabled, "消費後にIRQ状態と待機条件を復元");
    check(!rx[0].status && !rx[1].status && !rx[2].status, "消費済みdescriptorだけを解放");

    prepare();
    rx_cur = NRX - 2;
    receive_at(NRX - 2); receive_at(NRX - 1); receive_at(0);
    check(e1000_poll_rx(consume) == 3 && inputs == 3, "RX ring wrap");
    check(rd(REG_RDT) == 0 && rx_cur == 1, "wrap後のRDTとhead");

    prepare();
    receive_at(0);
    registers[REG_ICR / sizeof(uint32_t)] = ICR_RXT0;
    e1000_irq(NULL);
    check(inputs == 0 && rd(REG_RDT) == expected_tail && (rx[0].status & DESC_DD), "IRQはpacketをコピーも解放もしない");
    check(notifications == 1 && e1000_rx_pending() && rd(REG_IMC) == RX_IRQS, "IRQは通知してRXをmask");
    check(e1000_poll_rx(consume) == 1, "IRQ通知後の直接消費");

    prepare();
    receive_at(0);
    rx[0].len = BUFSZ + 1;
    receive_at(1);
    rx[1].errors = 1;
    check(e1000_poll_rx(consume) == 2 && inputs == 0, "不正DMA長と受信errorを破棄してringは返す");

    prepare();
    receive_at(0);
    test_irq_boundary = arrival_after_empty;
    check(e1000_poll_rx(consume) == 1 && inputs == 1, "空判定直後の新着は今回のborrowに混ぜない");
    check(e1000_rx_pending() && notifications == 1 && rd(REG_IMC) == RX_IRQS, "再armの隙間の新着を失わない");
    expected_tail = 0;
    check(e1000_poll_rx(consume) == 1 && inputs == 2, "隙間の新着を次batchで消費");
}

static void transmit_cases(void) {
    prepare();
    uint8_t frame[60];
    memset(frame, 0xA5, sizeof frame);
    bool all_sent = true;
    for (size_t i = 0; i < NTX - 1; i++) all_sent &= e1000_send(frame, sizeof frame) == 0;
    check(all_sent && tx_cur == (int)NTX - 1 && tx_clean == 0, "TX ringの利用可能descriptorを使う");
    check(e1000_send(frame, sizeof frame) == -EAGAIN && tx_cur == (int)NTX - 1, "TX満杯はspinせず成功偽装もしない");
    check(rd(REG_TDT) == NTX - 1, "TX満杯でheadとtailを同値にしない");
    check(!memcmp(tx_pixels[0], frame, sizeof frame) && tx[0].len == sizeof frame, "TX frameを完全に保存");
    tx[0].status = DESC_DD;
    registers[REG_ICR / sizeof(uint32_t)] = ICR_TXDW;
    e1000_irq(NULL);
    check(notifications == 1 && e1000_rx_pending(), "TX completionがnet threadを起こす");
    check(e1000_send(frame, sizeof frame) == 0 && tx_cur == 0 && tx_clean == 1, "completion後のretryとTX wrap");
    check(e1000_send(frame, sizeof frame) == -EAGAIN, "残りcompletion前は再び資源待ち");
    check(test_irq_enabled, "TX資源待ちでもIRQ状態を復元");
    check(e1000_send(frame, BUFSZ + 1) == -EINVAL, "TX buffer範囲外を拒否");
}

static void itr_cases(void) {
    kernel_cmdline = NULL;
    check(e1000_itr_ticks() == ITR_DEFAULT_TICKS, "ITR未指定はIntel推奨値");
    kernel_cmdline = "e1000-itr-us=0";
    check(e1000_itr_ticks() == 0, "ITRを明示無効化");
    kernel_cmdline = "abc e1000-itr-us=1 def";
    check(e1000_itr_ticks() == 4, "microsecondを256ns単位へ切上げ");
    kernel_cmdline = "e1000-itr-us=16776";
    check(e1000_itr_ticks() == 65532, "ITRレジスター上限近傍");
    kernel_cmdline = "e1000-itr-us=16777";
    check(e1000_itr_ticks() == ITR_DEFAULT_TICKS, "16bitレジスター溢れを拒否");
    kernel_cmdline = "e1000-itr-us=18446744073709551616";
    check(e1000_itr_ticks() == ITR_DEFAULT_TICKS, "数値parse overflowを拒否");
    kernel_cmdline = "xe1000-itr-us=0 e1000-itr-us=1x e1000-itr-us=";
    check(e1000_itr_ticks() == ITR_DEFAULT_TICKS, "token境界と不正値を拒否");
}

int main(void) {
    receive_cases();
    transmit_cases();
    itr_cases();
    printf("e1000限定ホスト検査: %u確認、%u失敗\n", checks, failures);
    return failures != 0;
}
