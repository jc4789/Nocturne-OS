/* What nvrm needs from KAPI's objects that KAPI's function table does not give (nvkms_os.h).
 *
 * Compiled with NVKMS's own flags and linked into nv-modeset-kernel.o, because it reads KAPI's
 * private structures (kapi/include/nvkms-kapi-internal.h), which only NVKMS's headers describe.
 *
 * On Linux nvidia-drm hands KAPI memory to another RM client as a file descriptor (KAPI's
 * exportMemory, RM's NV0000_CTRL_CMD_OS_UNIX_EXPORT_OBJECT_TO_FD). nvrm has no file
 * descriptors: display.c duplicates the memory into the app's RM client by its RM handles
 * instead (NV04_DUP_OBJECT), with RM's own access checks.
 */
#include "nvkms_os.h"

#include "nvkms-kapi-internal.h"

bool nvrm_nvkms_memory_handle(const struct NvKmsKapiDevice *device, const struct NvKmsKapiMemory *memory,
                              uint32_t *client, uint32_t *object)
{
    if (!device || !memory || !device->hRmClient || !memory->hRmHandle) return false;
    *client = device->hRmClient;
    *object = memory->hRmHandle;
    return true;
}
