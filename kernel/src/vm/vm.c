/* Which virtual machine Nocturne runs in, and the one place the kernel enters VM code (vm/vm.h).
   On real hardware every entry returns at once. */
#include "kernel.h"
#include "arch/cpu.h"
#include "limine.h"
#include "vm/vm.h"
#include "vm/hyperv.h"
#include "vm/hv/hv.h"
#include "vm/hv/vmbus.h"
#include "vm/ata.h"
#include "vm/rdp.h"
#include "hal/hal.h"

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

bool vm_cpu_guest(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    return c & (1u << 31);
}

void vm_detect(void) {
    bool hypervisor = vm_cpu_guest();
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

/* ---- devices ---- */

/* The drivers of the devices only a virtual machine has. Each binds to a device by its rule like any
   other driver; here they are added only where such devices can be. */
extern const struct hal_driver e1000_driver, tulip_driver, ac97_driver, vbe_driver, virtio_gpu_driver;
extern const struct hal_driver netvsc_driver, hv_keyboard_driver, hv_mouse_driver, hv_util_driver;

/* The legacy IDE ports, which no bus announces: a Generation 1 Hyper-V machine and QEMU's "pc" have
   them at the places the PC architecture fixes (vm/ata.c is the driver). */
#define IDE_PRIMARY 0x1F0
#define IDE_PRIMARY_CONTROL 0x3F6
#define IDE_SECONDARY 0x170
#define IDE_SECONDARY_CONTROL 0x376
static struct hal_device ide_ports;

static void publish_ide_ports(void) {
    hal_device_init(&ide_ports, N_BUS_FIXED, NULL);
    hal_prop_text(&ide_ports, N_PROP_FIXED_NAME, "ide-ports");
    hal_resource_add(&ide_ports, N_HAL_PORT, 0, 0, IDE_PRIMARY, 8);
    hal_resource_add(&ide_ports, N_HAL_PORT, 1, 0, IDE_PRIMARY_CONTROL, 1);
    hal_resource_add(&ide_ports, N_HAL_PORT, 2, 0, IDE_SECONDARY, 8);
    hal_resource_add(&ide_ports, N_HAL_PORT, 3, 0, IDE_SECONDARY_CONTROL, 1);
    hal_location(&ide_ports, "IDE ports");
    hal_publish(NULL, &ide_ports);
}

void vm_init(void) {
    if (machine != VM_NONE) {
        hal_driver_add(&vbe_driver);
        hal_driver_add(&virtio_gpu_driver);
        hal_driver_add(&e1000_driver);
        hal_driver_add(&tulip_driver);
        hal_driver_add(&ac97_driver);
        hal_driver_add(&ata_driver);
        publish_ide_ports();
    }
    if (!hv.present) return;
    hal_driver_add(&netvsc_driver);
    hal_driver_add(&hv_keyboard_driver);
    hal_driver_add(&hv_mouse_driver);
    hal_driver_add(&hv_util_driver);
    if (vmbus_init()) hvsock_init(); /* the storage driver (storvsc) is in the ledger's list like any other */
}

void vm_session_init(void) {
    if (vmbus_ready()) rdp_init();
}

/* ---- quirks ---- */
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
}

void vm_resume(void) { hv_core_resume(); }
