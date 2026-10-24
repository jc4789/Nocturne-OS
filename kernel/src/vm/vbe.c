/* The Bochs VBE display interface (QEMU's standard VGA, VirtualBox's VGA). The firmware set the
   display mode at boot and nothing sets it again after a sleep; this interface alone can be put
   back by its index/data ports, so only with it does the display resume (dev/fb.c). */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/pci.h"
#include "dev/pm.h"
#include "vm/devices.h"

#define VBE_INDEX  0x1CE
#define VBE_DATA   0x1CF
#define VBE_REGS   10 /* id, xres, yres, bpp, enable, bank, virt width/height, x/y offset */
#define VBE_ENABLE 4
static uint16_t vbe_saved[VBE_REGS];

static uint16_t vbe_read(uint16_t reg) {
    outw(VBE_INDEX, reg);
    return inw(VBE_DATA);
}

static void vbe_write(uint16_t reg, uint16_t value) {
    outw(VBE_INDEX, reg);
    outw(VBE_DATA, value);
}

static bool vbe_quiesce(void *arg) {
    (void)arg;
    for (uint16_t r = 1; r < VBE_REGS; r++) vbe_saved[r] = vbe_read(r);
    return true; /* scanout only: no DMA to stop */
}

static bool vbe_resume(void *arg) {
    (void)arg;
    vbe_write(VBE_ENABLE, 0);
    for (uint16_t r = 1; r < VBE_REGS; r++)
        if (r != VBE_ENABLE) vbe_write(r, vbe_saved[r]);
    vbe_write(VBE_ENABLE, vbe_saved[VBE_ENABLE] | 0x80); /* back on, without clearing the memory */
    inb(0x3DA);       /* the attribute controller's flip-flop to "index" */
    outb(0x3C0, 0x20); /* palette address source: the picture is shown again */
    return (vbe_read(VBE_ENABLE) & 0x41) == 0x41 && vbe_read(1) == vbe_saved[1] && vbe_read(2) == vbe_saved[2];
}

bool vbe_display_pm(struct pm_device *display) {
    struct pci_device *d = pci_find(0x1234, 0x1111);
    if (!d) d = pci_find(0x80EE, 0xBEEF);
    uint16_t id = d && d->cls == 0x03 && d->subcls == 0x00 ? vbe_read(0) : 0;
    if (id < 0xB0C0 || id > 0xB0CF || (vbe_read(VBE_ENABLE) & 0x41) != 0x41) return false;
    display->quiesce = vbe_quiesce;
    display->resume = vbe_resume;
    return true;
}
