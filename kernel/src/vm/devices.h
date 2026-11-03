/* The drivers behind vm/vm.h's device entries: hardware only emulators provide to Nocturne.
   Only vm/vm.c calls them, and only in a virtual machine. */
#pragma once
#include <stdbool.h>

struct pm_device;
struct pci_driver;

void ata_init(void);      /* vm/ata.c: legacy IDE disks (Hyper-V Generation 1, QEMU's "pc") */
extern const struct pci_driver ac97_driver;       /* vm/ac97.c: AC'97 audio (QEMU, VirtualBox) */
extern const struct pci_driver e1000_driver;      /* vm/e1000.c: Intel 8254x (QEMU, VirtualBox) */
extern const struct pci_driver tulip_driver;      /* vm/tulip.c: DEC 21140/21143 (Hyper-V's legacy adapter, QEMU) */
extern const struct pci_driver virtio_gpu_driver; /* vm/virtio_gpu.c: the offscreen GPU, which never shows a screen */
bool netvsc_probe(void);  /* vm/hv/netvsc.c: Hyper-V's synthetic network adapter */
void rdp_init(void);      /* vm/rdp.c: the enhanced session's RDP server over a Hyper-V socket */
/* vm/vbe.c: the Bochs VBE displays (QEMU's standard VGA, VirtualBox's): each a screen with its
   own buffers, and their modes set again after a sleep (false when there is none). */
extern const struct pci_driver vbe_driver;
bool vbe_display_pm(struct pm_device *display);
