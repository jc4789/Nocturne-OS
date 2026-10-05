#include <stdio.h>
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
    struct n_pciinfo p[64];
    int n = pcilist(p, 64);
    for (int i = 0; i < n; i++)
        printf("%02x:%02x.%x %s: %s [%04x:%04x]\n", p[i].bus, p[i].dev, p[i].func, cls(p[i].cls, p[i].subcls),
               vendor(p[i].vendor), p[i].vendor, p[i].device);
    return 0;
}
