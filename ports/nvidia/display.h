/* nvrm's screen: the part of nvrm that drives the monitors (display.c).
 *
 * On Linux nvidia-drm is the client of NVKMS's kernel API (KAPI) that sets modes and turns the
 * GPU's display engine into the console's and the desktop's output. In nvrm the same job is
 * this file's, without DRM: display.c is KAPI's only client. It offers the system the modes of
 * the display the firmware lit (n_pci_screen_modes), sets the one the system asks for (the
 * monitor's preferred mode until the user chooses another), scans out a surface of its size in
 * video memory, and gives that surface to the system (n_pci_give_screen), which draws the
 * desktop into it from then on.
 * While the system lets an app's fullscreen window be shown directly, the app's own images
 * (NVRM_SCANOUT, include/nvrm_protocol.h) are flipped onto the screen instead, with the pointer
 * as the display engine's cursor over them.
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

#include "nocturne.h"

/* After nvrm_nvkms_start returned true. Returns true when the system draws into nvrm's
   surface, or when the screen was handed back at once (a flip not performed) with the display's
   modes still offered: the system's next wish takes the display again. False (with the reason in
   the log) leaves the boot screen alone. */
bool nvrm_display_start(void);

/* Gives the system the boot screen back, lets NVKMS restore what the firmware showed, and
   frees everything display.c holds in NVKMS. Safe to call when nvrm_display_start failed or
   was never called; before nvrm_nvkms_stop. */
void nvrm_display_stop(void);

/* Serves one NVRM_SCANOUT request (include/nvrm_protocol.h) from the app principal on the
   connection whose RM state is file (nvrm_serve.h); args/size are its struct
   nvrm_scanout_request. Returns 0, the request's status being the NV_STATUS
   (NV_ERR_NOT_SUPPORTED when nvrm shows no screen it can show images on), or a negative n_result
   for a malformed request. Only between nvrm_display_start and nvrm_display_stop. */
n_result nvrm_display_scanout(void *file, uint32_t principal, void *args, uint32_t size);

/* The connection whose RM state is file ended: its images are taken off the screen and freed.
   Before its RM state is freed. */
void nvrm_display_connection_end(void *file);

/* The vertical blanks of the head nvrm lit that went to the system, counted from the first (0
   before any): for nvrm.c's interrupt lines. Any thread. */
uint64_t nvrm_display_vblanks(void);

/* os_pci.c: the bus address that a kernel mapping RM made (os_map_kernel_space) shows at
   address. False when address is in none of them. */
bool nvrm_kernel_mapping_phys(const void *address, uint64_t *phys);

#endif
