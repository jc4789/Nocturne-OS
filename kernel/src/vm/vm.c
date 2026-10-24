/* Which virtual machine Nocturne runs in, and the one place the kernel enters VM code (vm/vm.h).
   On real hardware every entry returns at once. */
#include "kernel.h"
#include "arch/cpu.h"
#include "limine.h"
#include "vm/vm.h"
#include "vm/devices.h"
#include "vm/hyperv.h"
#include "vm/hv/hv.h"
#include "vm/hv/vmbus.h"

extern volatile struct limine_rsdp_request rsdp_req;

static enum vm_machine machine;

/* The OEM ID of the firmware's ACPI root pointer, or false without a valid one. Limine has
   already supplied the mapped response. */
static bool firmware_oem(char oem[7]) {
    if (!rsdp_req.response || !rsdp_req.response->address) return false;
    const uint8_t *rsdp = rsdp_req.response->address;
    if ((uint64_t)rsdp < hhdm_offset) rsdp = phys_to_virt((uint64_t)rsdp);
    if (memcmp(rsdp, "RSD PTR ", 8)) return false;
    uint8_t sum = 0;
    for (unsigned i = 0; i < 20; i++) sum += rsdp[i];
    if (sum) return false;
    memcpy(oem, rsdp + 9, 6);
    oem[6] = 0;
    return true;
}

void vm_detect(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    bool hypervisor = c & (1u << 31);
    hv_detect(); /* the Hyper-V interface, wherever it is offered (WHPX offers it to QEMU too) */
    char oem[7];
    bool have_oem = firmware_oem(oem);
    /* The emulator's firmware tells whose devices these are: WHPX shows "Microsoft Hv" to a QEMU
       guest whose devices are QEMU's. */
    if (have_oem && !memcmp(oem, "BOCHS ", 6)) machine = VM_QEMU;
    else if (have_oem && !memcmp(oem, "VBOX  ", 6)) machine = VM_VIRTUALBOX;
    else if (hv.present) machine = VM_HYPERV;
    else if (hypervisor) machine = VM_OTHER;
    else machine = VM_NONE;
    static const char *const names[] = {"real hardware", "Hyper-V", "QEMU", "VirtualBox",
                                        "an unknown hypervisor"};
    kprintf("vm: %s\n", names[machine]);
}

enum vm_machine vm_machine(void) { return machine; }

/* ---- clock ---- */
uint64_t vm_timer_hz(uint64_t *tsc_hz) {
    uint64_t hz = hv_apic_hz();
    if (hz) *tsc_hz = hv_tsc_hz();
    return hz;
}

bool vm_clock_read(uint64_t *units_100ns) { return hv_reference_time(units_100ns); }
uint64_t vm_clock_page(void) { return hv_reference_tsc_page(); }
bool vm_clock_owned(void) { return hv.present; }

/* ---- devices ---- */
void vm_init(void) {
    if (!hv.present || !vmbus_init()) return;
    hv_input_init();
    hv_util_init();
    hvsock_init();
}

/* The emulated devices are found by their PCI identity or legacy ports, in any VM; on real
   hardware none of them is looked for. */
void vm_storage_init(void) {
    if (machine == VM_NONE) return;
    ata_init();
    storvsc_init(); /* only with VMBus */
}

bool vm_net_probe(void) {
    if (machine == VM_NONE) return false;
    return netvsc_probe() || e1000_probe() || tulip_probe();
}

void vm_audio_init(void) {
    if (machine != VM_NONE) ac97_init();
}

void vm_display_pm(struct pm_device *display) {
    if (machine != VM_NONE) vbe_display_pm(display);
}

void vm_session_init(void) {
    if (vmbus_ready()) rdp_init();
}

/* ---- quirks ---- */
/* Hyper-V on Windows 11 hosts reports the emulated PS/2 mouse's Y axis upside down (a known
   regression that also hits Windows XP and Haiku guests). QEMU's i8042 does not, even when WHPX
   shows Hyper-V's CPUID. */
bool vm_ps2_flips_y(void) {
    if (machine != VM_HYPERV) return false;
    kprintf("vm: Hyper-V host build %u (%u.%u)\n", hv.build, hv.major, hv.minor);
    return hv.build >= 22000; /* Windows 11 and later */
}

void vm_power_off(void) {
    if (machine == VM_QEMU) {
        outw(0x604, 0x2000);  /* q35 and current "pc" */
        outw(0xB004, 0x2000); /* older "pc" */
    } else if (machine == VM_VIRTUALBOX) {
        outw(0x4004, 0x3400);
    }
}

/* ---- sleep and hibernation ---- */
const char *vm_hibernate_blocker(void) {
    return vmbus_ready() ? "Hyper-V devices cannot come back from it yet" : NULL;
}

void vm_suspend(void) {
    hv_core_suspend(); /* the hypervisor's overlays would swallow the copy's writes to their frames */
    hv_reference_suspend();
}

void vm_resume(void) {
    hv_reference_resume(); /* the hypervisor's overlay pages: reset, or the restoring kernel's */
    hv_core_resume();
}
