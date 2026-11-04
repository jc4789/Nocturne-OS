#include <stdio.h>
#include <stdlib.h>
#include "nocturne.h"
static const char *cls(int c, int s) {
    switch (c) {
    case 1: return s == 1 ? "IDE controller" : s == 6 ? "SATA controller" : "Storage controller";
    case 2: return "Ethernet controller";
    case 3: return "VGA compatible controller";
    case 4: return "Multimedia controller";
    case 6: return s == 0 ? "Host bridge" : s == 1 ? "ISA bridge" : "Bridge";
    case 0xC: return s == 3 ? "USB controller" : "Serial bus controller";
    default: return "Device";
    }
}
static const char *vendor(int v) {
    switch (v) {
    case 0x8086: return "Intel";
    case 0x1234: return "QEMU/Bochs";
    case 0x1414: return "Microsoft";
    case 0x1011: return "DEC";
    case 0x10EC: return "Realtek";
    case 0x1AF4: return "Red Hat (virtio)";
    case 0x15AD: return "VMware";
    case 0x80EE: return "VirtualBox";
    default: return "unknown vendor";
    }
}
int main(void) {
    struct n_hal_snapshot ledger = {0};
    if (n_hal_snapshot(&ledger) < 0) return 1;
    const struct n_hal_device *d = ledger.first;
    for (uint32_t i = 0; i < ledger.count; i++, d = n_hal_device_next(d)) {
        uint64_t vendor_id, device_id, class_code, address;
        if (!n_hal_device_number(d, N_PROP_PCI_VENDOR, &vendor_id) || !n_hal_device_number(d, N_PROP_PCI_DEVICE, &device_id) ||
            !n_hal_device_number(d, N_PROP_PCI_CLASS, &class_code) || !n_hal_device_number(d, N_PROP_PCI_ADDRESS, &address))
            continue;
        printf("%04x:%02x:%02x.%x %s: %s [%04x:%04x]\n", (unsigned)N_PCI_TARGET_SEGMENT(address),
               (unsigned)N_PCI_TARGET_BUS(address), (unsigned)N_PCI_TARGET_DEV(address),
               (unsigned)N_PCI_TARGET_FUNC(address), cls((int)(class_code >> 16), (int)(class_code >> 8 & 0xFF)),
               vendor((int)vendor_id), (unsigned)vendor_id, (unsigned)device_id);
    }
    free((void *)ledger.first);
    return 0;
}
