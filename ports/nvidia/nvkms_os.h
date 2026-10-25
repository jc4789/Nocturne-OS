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
#include <stdint.h>

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

/* The RM client of a KAPI device and the RM handle of memory KAPI allocated on it
 * (nvkms_kapi_rm.c), for duplicating the memory into another RM client: KAPI's own way, its
 * exportMemory, gives a file descriptor, which nvrm does not have. False when either has no RM
 * object. */
struct NvKmsKapiDevice;
struct NvKmsKapiMemory;
bool nvrm_nvkms_memory_handle(const struct NvKmsKapiDevice *device, const struct NvKmsKapiMemory *memory,
                              uint32_t *client, uint32_t *object);

/* The completion of a flip that puts a surface on a head's layer (nvkms_kapi_rm.c), which KAPI's
 * table does not tell (its getFlipPendingStatus is NULL). nvrm_nvkms_flip_arm goes right before
 * the commit: it clears the completion notifier that commit names. nvrm_nvkms_flip_done is true
 * once the display engine has written that notifier, after a commit that succeeded; a commit
 * that failed or put no surface on the layer names none. Both on the thread that commits.
 * nvrm_nvkms_flip_words, for the log when a flip is not seen to complete: the slot the last
 * commit named (0 or 1) and the raw words of both of the layer's notifiers, word 0 holding the
 * status (NVKMS's nvKmsParseNotifier reads them so), words 2 and 3 the time stamp. */
void nvrm_nvkms_flip_arm(struct NvKmsKapiDevice *device, uint32_t head, uint32_t layer);
bool nvrm_nvkms_flip_done(const struct NvKmsKapiDevice *device, uint32_t head, uint32_t layer);
void nvrm_nvkms_flip_words(const struct NvKmsKapiDevice *device, uint32_t head, uint32_t layer, uint32_t *slot,
                           uint32_t words[2][4]);

#endif
