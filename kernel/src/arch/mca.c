/* The machine-check banks (arch/mca.h). AMD APM vol. 2 9.3: MCG_CAP gives their count (bits 7:0) and
   whether MCG_CTL is there (bit 8). A bank's registers are at 400h + 4i (CTL, STATUS, ADDR, MISC0),
   which reach banks 0 to 31 (400h-47Fh; 9.3.2, as Intel SDM vol. 3B's IA32_MCi_CTL). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/cpu_maker.h"
#include "arch/mca.h"
#include "arch/spin.h"

#define MSR_MCG_STATUS 0x17Au
#define MSR_MCG_CTL    0x17Bu
#define MCG_CTL_P      (1ULL << 8)
#define MCG_RIPV       (1ULL << 0) /* the interrupted code may go on at the saved rip */

static const struct mca_layout both_manuals = {0x400, 4, 32};

static const struct mca_layout *mca_bank_layout(void) {
    const struct cpu_maker *maker = cpu_features.maker;
    const struct mca_layout *l = maker->mca_layout ? maker->mca_layout() : NULL;
    return l ? l : &both_manuals;
}

uint32_t mca_msr(unsigned bank, enum mca_reg reg) {
    const struct mca_layout *l = mca_bank_layout();
    return l->first + l->stride * bank + reg;
}

unsigned mca_banks(void) {
    unsigned count = (unsigned)(rdmsr(MSR_MCG_CAP) & 0xFF);
    return MIN(count, mca_bank_layout()->limit);
}

void mca_report(unsigned bank, uint64_t status, const char *what) {
    uint64_t addr = status & MCI_ADDRV ? rdmsr(mca_msr(bank, MCA_ADDR)) : 0;
    uint64_t misc = status & MCI_MISCV ? rdmsr(mca_msr(bank, MCA_MISC0)) : 0;
    kprintf("cpu%u: machine check bank %u %s: status %016lx%s%s, address %lx, misc %lx\n",
            cpu_current_index(), bank, what, status, status & MCI_UC ? " uncorrected" : " corrected",
            status & MCI_OVER ? ", errors lost" : "", addr, misc);
}

void mca_take_corrected(unsigned bank, const char *what) {
    uint64_t status = rdmsr(mca_msr(bank, MCA_STATUS));
    if (!(status & MCI_VAL) || (status & (MCI_UC | MCI_PCC))) return;
    mca_report(bank, status, what);
    wrmsr_checked(mca_msr(bank, MCA_STATUS), 0);
}

/* APM 9.4. A bank the CPU (or a hypervisor) refuses a write to is left as it is. */
void mca_setup(void) {
    const struct cpu_maker *maker = cpu_features.maker;
    unsigned banks = mca_banks();
    for (unsigned i = 0; i < banks; i++) {
        if (maker->mca_bank_on) maker->mca_bank_on(i);
        wrmsr_checked(mca_msr(i, MCA_CTL), ~0ULL);
        uint64_t status = rdmsr(mca_msr(i, MCA_STATUS));
        if (!(status & MCI_VAL)) continue;
        mca_report(i, status, "from before");
        wrmsr_checked(mca_msr(i, MCA_STATUS), 0);
    }
    if (rdmsr(MSR_MCG_CAP) & MCG_CTL_P) wrmsr_checked(MSR_MCG_CTL, ~0ULL);
    write_cr4(read_cr4() | CR4_MCE);
}

void mca_threshold_vector(enum irq_result (*handler)(void *context)) {
    static struct irq *threshold_irq;
    if (!threshold_irq &&
        !(threshold_irq = irq_request_system(
              &(struct irq_spec){.name = "machine-check threshold", .handler = handler, .node = -1},
              VEC_MCE_THRESHOLD)))
        panic("cpu: VEC_MCE_THRESHOLD is taken");
}

void mca_interrupt_on(void) {
    const struct cpu_maker *maker = cpu_features.maker;
    if (maker->mca_interrupt_on) maker->mca_interrupt_on();
}

void mca_machine_check(struct regs *r) {
    uint64_t mcg = rdmsr(MSR_MCG_STATUS);
    bool fatal = !(mcg & MCG_RIPV), logged = spin_none_here();
    unsigned banks = mca_banks(), uncorrected = 0;
    for (unsigned i = 0; i < banks; i++) {
        uint64_t status = rdmsr(mca_msr(i, MCA_STATUS));
        if (!(status & MCI_VAL)) continue;
        bool bad = (status & (MCI_UC | MCI_PCC)) != 0;
        if (bad) uncorrected++;
        if (bad || logged) mca_report(i, status, "in a machine check");
        if (!bad) wrmsr_checked(mca_msr(i, MCA_STATUS), 0);
    }
    if (fatal || uncorrected)
        panic("cpu%u: machine check at %lx: %u uncorrected errors%s", cpu_current_index(), r->rip, uncorrected,
              fatal ? ", and the interrupted code cannot go on" : "");
    wrmsr(MSR_MCG_STATUS, 0); /* MCIP: another machine check may come again */
}
