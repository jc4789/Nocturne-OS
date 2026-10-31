/* Everything Nocturne does only because it runs in a virtual machine.

   Real hardware is what Nocturne is for. There vm_detect finds no hypervisor and no emulator's
   firmware, and every entry below returns at once without touching a port, a register or a
   device. The rest of the kernel reaches VM code only through these entries; the hypervisor
   interface (vm/hyperv.c), the VMBus devices (vm/hv/), the enhanced session (vm/rdp.c) and the
   drivers of devices that only emulators provide (vm/devices.h) stay behind them. A VM's needs
   never decide how real hardware is driven. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct pm_device;

/* Whose machine Nocturne runs in: its emulated devices and firmware. */
enum vm_machine {
    VM_NONE,       /* real hardware */
    VM_HYPERV,     /* a Hyper-V virtual machine (VMBus; Generation 1's legacy devices) */
    VM_QEMU,       /* QEMU's machine, whatever accelerates it (WHPX also shows Hyper-V's CPUID) */
    VM_VIRTUALBOX,
    VM_OTHER,      /* a hypervisor this code does not know: its devices are found by their identity */
};

/* boot.c, once on CPU 0 before anything asks (and before clock_init): the CPUID hypervisor bit
   and signature, and the ACPI firmware's OEM ID. A hypervisor that states the TSC's rate gives it
   to the clock (clock_register_tsc_hz). */
void vm_detect(void);
enum vm_machine vm_machine(void);
static inline bool vm_hosted(void) { return vm_machine() != VM_NONE; }

/* ---- devices, at the places the kernel looks for each kind ---- */
void vm_init(void);              /* the hypervisor's own bus (VMBus) and what rides on it; before PCI */
void vm_storage_init(void);      /* the emulated disks */
bool vm_net_probe(void);         /* an emulated or synthetic network adapter, when no real one */
void vm_audio_init(void);        /* emulated sound */
void vm_display_pm(struct pm_device *display); /* how the emulated display comes back from sleep */
void vm_session_init(void);      /* the host's enhanced session, once the desktop runs */

/* ---- quirks of the emulated devices ---- */
void vm_power_off(void);         /* the emulator's own power-off ports, after ACPI did not answer */

/* ---- sleep and hibernation ---- */
const char *vm_hibernate_blocker(void); /* NULL, or why the VM's devices cannot hibernate */
void vm_suspend(void);           /* the hypervisor's overlay pages off before an image is copied */
void vm_resume(void);            /* and on again */
