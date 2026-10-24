/* nvrm's screen: the part of nvrm that drives the monitors (display.c).
 *
 * On Linux nvidia-drm is the client of NVKMS's kernel API (KAPI) that sets modes and turns the
 * GPU's display engine into the console's and the desktop's output. In nvrm the same job is
 * this file's, without DRM: display.c is KAPI's only client. It sets the mode the firmware
 * (GOP) had on the display the firmware lit, scans out a surface in video memory, and gives that
 * surface to the system (n_pci_give_screen), which draws the desktop into it from then on.
 *
 * Order in nvrm (nvrm.c):
 *
 *   bring_up: ... adapter_ready -> nvrm_nvkms_start() -> nvrm_display_start()
 *   tear_down: nvrm_display_stop() -> nvrm_nvkms_stop() -> rm_disable_adapter -> ...
 *
 * A screen that cannot be taken over is not a failure of nvrm: the reason is logged, the boot
 * screen stays as the firmware set it, and RM is served to clients as before.
 */
#ifndef NVRM_DISPLAY_H
#define NVRM_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

/* After nvrm_nvkms_start returned true. Returns true when the system draws into nvrm's
   surface; false (with the reason in the log) leaves the boot screen alone. */
bool nvrm_display_start(void);

/* Gives the system the boot screen back, lets NVKMS restore what the firmware showed, and
   frees everything display.c holds in NVKMS. Safe to call when nvrm_display_start failed or
   was never called; before nvrm_nvkms_stop. */
void nvrm_display_stop(void);

/* os_pci.c: the bus address that a kernel mapping RM made (os_map_kernel_space) shows at
   address. False when address is in none of them. */
bool nvrm_kernel_mapping_phys(const void *address, uint64_t *phys);

#endif
