/* NVKMS (NVIDIA's mode-setting core, "nvidia-modeset") inside nvrm: the entry points nvrm uses.
 *
 * nvkms_os.c is the OS layer of NVKMS (everything nvidia-modeset-os-interface.h asks for) and
 * its connection to RM, the part nvidia.ko's nv-modeset-interface.c plays on Linux. NVKMS
 * talks to RM through rm_kernel_rmapi_op as a kernel client, so it needs RM up and the GPU
 * started (nvrm_dev.adapter_ready) before it can do anything with a display.
 *
 * Order in nvrm (nvrm.c):
 *
 *   bring_up: ... rm_init_adapter -> adapter_ready = true -> nvrm_nvkms_start()
 *   tear_down: nvrm_nvkms_stop() -> rm_disable_adapter -> ... (before RM stops the GPU, while
 *              RM's timers and work queue still run)
 *
 * NVKMS's own clients in nvrm use its kernel API (KAPI), as nvidia-drm does on Linux:
 *
 *   #include "nvkms-kapi.h"           (src/nvidia-modeset/kapi/interface)
 *   static struct NvKmsKapiFunctionsTable kapi = { .versionString = NV_VERSION_STRING };
 *   if (!nvKmsKapiGetFunctionsTable(&kapi)) ...   (a version mismatch fills in NVKMS's string)
 *   kapi.enumerateGpus(...) -> kapi.allocateDevice(...) -> kapi.grabOwnership(...) -> ...
 *   ... kapi.freeDevice(device) before nvrm_nvkms_stop().
 *
 * nvKmsKapiGetFunctionsTable may be called once nvrm_nvkms_start has returned true; the table
 * stays valid until nvrm_nvkms_stop. NV_VERSION_STRING is in src/common/inc/nvUnixVersion.h.
 * KAPI event callbacks (NvKmsKapiAllocateDeviceParams.eventCallback) run on NVKMS's work
 * thread, the one that also runs NVKMS's timers.
 *
 * Settings: the launch setting NVKMS_OPTIONS, "name=value;name=value", with the names and
 * defaults of the Linux module parameters of nvidia-modeset (README "NVKMS"), for example
 * NVKMS_OPTIONS=hdmi_deepcolor=0;debug=1.
 */
#ifndef NVKMS_OS_H
#define NVKMS_OS_H

#include <stdbool.h>

/* Loads NVKMS (nvKmsModuleLoad, which allocates its RM client) and registers nvrm's GPU with
 * it, so that KAPI's enumerateGpus lists it and allocateDevice can open it. Call after
 * rm_init_adapter succeeded (nvrm_dev.adapter_ready). Returns false, with the reason in the
 * log, when NVKMS cannot run; nvrm then serves RM without a display driver. */
bool nvrm_nvkms_start(void);

/* Takes the GPU away from NVKMS (nvKmsKapiRemove) and unloads it (nvKmsModuleUnload); pending
 * NVKMS timers are cancelled and its work thread is stopped. Every KAPI device must have been
 * freed before, as on Linux. Does nothing when nvrm_nvkms_start did not succeed. */
void nvrm_nvkms_stop(void);

/* Power management, for when nvrm suspends the GPU (nvidia.ko calls nvidia_modeset_suspend
 * before RM's suspend and nvidia_modeset_resume after RM's resume). */
void nvrm_nvkms_suspend(void);
void nvrm_nvkms_resume(void);

#endif
