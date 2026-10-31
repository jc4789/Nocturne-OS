/* The drivers behind vm/vm.h's device entries: hardware only emulators provide to Nocturne.
   Only vm/vm.c calls them, and only in a virtual machine. */
#pragma once
#include <stdbool.h>

struct pm_device;

void ata_init(void);      /* vm/ata.c: legacy IDE disks (Hyper-V Generation 1, QEMU's "pc") */
void ac97_init(void);     /* vm/ac97.c: AC'97 audio (QEMU, VirtualBox) */
bool e1000_probe(void);   /* vm/e1000.c: Intel 8254x (QEMU, VirtualBox) */
bool tulip_probe(void);   /* vm/tulip.c: DEC 21140/21143 (Hyper-V's legacy adapter, QEMU) */
bool netvsc_probe(void);  /* vm/hv/netvsc.c: Hyper-V's synthetic network adapter */
void rdp_init(void);      /* vm/rdp.c: the enhanced session's RDP server over a Hyper-V socket */
/* vm/vbe.c: the Bochs VBE display (QEMU's standard VGA, VirtualBox's) can set its mode again
   after a sleep; false when it is not there. */
bool vbe_display_pm(struct pm_device *display);
