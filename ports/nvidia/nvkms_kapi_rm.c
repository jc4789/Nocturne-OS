/* What nvrm needs from KAPI's objects that KAPI's function table does not give (nvkms_os.h).
 *
 * Compiled with NVKMS's own flags and linked into nv-modeset-kernel.o, because it reads KAPI's
 * private structures (kapi/include/nvkms-kapi-internal.h), which only NVKMS's headers describe.
 *
 * KAPI hands its memory to another RM client only as a file descriptor (exportMemory, RM's
 * NV0000_CTRL_CMD_OS_UNIX_EXPORT_OBJECT_TO_FD). nvrm has no file descriptors: display.c
 * duplicates the memory into the app's RM client by its RM handles instead (NV04_DUP_OBJECT),
 * with RM's own access checks.
 *
 * Nor does KAPI say when a flip has completed: its table has a getFlipPendingStatus entry that
 * nvKmsKapiGetFunctionsTable leaves NULL. Every commit that puts a surface on a layer names one
 * of the layer's two completion notifiers in KAPI's notifier surface (nvkms-kapi.c: the slot
 * after layerState[head][layer].currFlipNotifierIndex), which the display engine writes
 * "finished" when the flip has completed. KAPI never clears a slot before naming it again, so
 * the slot still holds the write of the flip two before; nvrm clears it before each such commit
 * and reads it afterwards.
 */
#include "nvkms_os.h"

#include "nvkms-kapi-internal.h"
#include "nvkms-kapi-notifiers.h"
#include "nvkms-sync.h"

bool nvrm_nvkms_memory_handle(const struct NvKmsKapiDevice *device, const struct NvKmsKapiMemory *memory,
                              uint32_t *client, uint32_t *object)
{
    if (!device || !memory || !device->hRmClient || !memory->hRmHandle) return false;
    *client = device->hRmClient;
    *object = memory->hRmHandle;
    return true;
}

static bool flip_notifiers(const struct NvKmsKapiDevice *device, uint32_t head, uint32_t layer)
{
    return device && device->notifier.pLinearAddress && head < device->numHeads && head < NVKMS_KAPI_MAX_HEADS &&
           layer < NVKMS_MAX_LAYERS_PER_HEAD;
}

void nvrm_nvkms_flip_arm(struct NvKmsKapiDevice *device, uint32_t head, uint32_t layer)
{
    if (!flip_notifiers(device, head, layer)) return;
    NvU32 next = NVKMS_KAPI_INC_NOTIFIER_INDEX(device->layerState[head][layer].currFlipNotifierIndex);
    nvKmsResetNotifier(NVKMS_KAPI_NOTIFIER_INDEX(head, layer, next), device->notifier.pLinearAddress);
    /* Cleared before the commit's methods reach the display engine, through any CPU mapping. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

bool nvrm_nvkms_flip_done(const struct NvKmsKapiDevice *device, uint32_t head, uint32_t layer)
{
    if (!flip_notifiers(device, head, layer)) return false;
    struct nvKmsParsedNotifier parsed = { 0 };
    nvKmsParseNotifier(NVKMS_KAPI_NOTIFIER_INDEX(head, layer, device->layerState[head][layer].currFlipNotifierIndex),
                       device->notifier.pLinearAddress, &parsed);
    return parsed.status == NVKMS_NOTIFIER_STATUS_FINISHED;
}

void nvrm_nvkms_flip_words(const struct NvKmsKapiDevice *device, uint32_t head, uint32_t layer, uint32_t *slot,
                           uint32_t words[2][4])
{
    *slot = 0;
    for (NvU32 i = 0; i < NVKMS_KAPI_MAX_NOTIFERS_PER_LAYER; i++)
        for (NvU32 w = 0; w < 4; w++) words[i][w] = 0;
    if (!flip_notifiers(device, head, layer)) return;
    *slot = device->layerState[head][layer].currFlipNotifierIndex;
    for (NvU32 i = 0; i < NVKMS_KAPI_MAX_NOTIFERS_PER_LAYER; i++) {
        const volatile NvU32 *n = (const volatile NvU32 *)((const char *)device->notifier.pLinearAddress +
                                                           NVKMS_KAPI_NOTIFIER_OFFSET(head, layer, i));
        for (NvU32 w = 0; w < 4; w++) words[i][w] = n[w];
    }
}
