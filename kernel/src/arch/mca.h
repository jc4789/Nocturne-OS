/* Machine checks (AMD APM vol. 2 ch. 9, Intel SDM vol. 3B ch. 18): the banks every maker has, here;
   where the makers differ, struct cpu_maker (arch/cpu_maker.h). */
#pragma once
#include <stdint.h>
#include "arch/irq.h"

#define MSR_MCG_CAP 0x179u
#define MCI_VAL   (1ULL << 63)
#define MCI_OVER  (1ULL << 62)
#define MCI_UC    (1ULL << 61)
#define MCI_MISCV (1ULL << 59)
#define MCI_ADDRV (1ULL << 58)
#define MCI_PCC   (1ULL << 57)

/* A bank's registers: first + stride * bank + one of these. limit: the banks the layout reaches. */
enum mca_reg { MCA_CTL, MCA_STATUS, MCA_ADDR, MCA_MISC0 };
struct mca_layout {
    uint32_t first, stride;
    unsigned limit;
};

/* This CPU, from cpu_setup: every bank's reporting on, what the banks hold from before logged and
   cleared, CR4.MCE last. */
void mca_setup(void);
/* This CPU, once its LAPIC is enabled (an LVT entry written before that stays masked, APM 16.4.7):
   its corrected errors are told through an interrupt as they happen, by whichever way its maker
   gives. */
void mca_interrupt_on(void);
/* The #MC handler, on its IST stack: it logs every bank's error, returns where all of them were
   corrected, panics otherwise. */
struct regs;
void mca_machine_check(struct regs *r);

/* For the makers' implementations. */
unsigned mca_banks(void);
uint32_t mca_msr(unsigned bank, enum mca_reg reg);
void mca_report(unsigned bank, uint64_t status, const char *what);
/* The bank's corrected error, if it holds one: logged and cleared. */
void mca_take_corrected(unsigned bank, const char *what);
/* The vector the corrected errors are told on, its handler set once: the first CPU, at boot. */
void mca_threshold_vector(enum irq_result (*handler)(void *context));
