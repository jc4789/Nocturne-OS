/* nv.c's part of a client's request (ports/nvidia/os_ioctl.c), the production file with RM's
   entry points replaced by fixtures. No GPU is touched. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ports/nvidia/os_ioctl.c"
#include <nv_escape.h>
#include <nvos.h>

static int failures, external_clients, version_checks;
static NV_STATUS version_result = NV_OK, numa_result = NV_OK;
static NvU32 registers[0x200 / 4];

#define CHECK(expr) do { if (!(expr)) { \
    printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

struct nvrm_device nvrm_dev;
thread_local bool nvrm_thread_admin;

/* Single-threaded fixture. */
int mtx_init(mtx_t *m, int type) { (void)m; (void)type; return thrd_success; }
int mtx_lock(mtx_t *m) { (void)m; return thrd_success; }
int mtx_unlock(mtx_t *m) { (void)m; return thrd_success; }
void call_once(once_flag *flag, void (*init)(void)) { (void)flag; init(); }
void nvrm_log(const char *fmt, ...) { (void)fmt; }

/* RM's own table: one of its escapes, the rest unknown to it. */
NV_STATUS NV_API_CALL rm_validate_ioctls(NvU32 command, NvU32 size) {
    if ((command & 0xFF) == (NV_ESC_RM_FREE & 0xFF))
        return size == sizeof(NVOS00_PARAMETERS) ? NV_OK : NV_ERR_INVALID_ARGUMENT;
    return NV_ERR_INVALID_COMMAND;
}
NV_STATUS NV_API_CALL rm_perform_version_check(nvidia_stack_t *sp, void *data, NvU32 size) {
    (void)sp; (void)data;
    CHECK(size == sizeof(nv_ioctl_rm_api_version_t));
    version_checks++;
    return version_result;
}
NV_STATUS NV_API_CALL rm_get_gpu_numa_info(nvidia_stack_t *sp, nv_state_t *nv, nv_ioctl_numa_info_t *info) {
    (void)sp;
    CHECK(nv == &nvrm_dev.nv);
    info->nid = -1;
    info->status = 99; /* the OS layer, not RM, reports the status */
    return numa_result;
}
NV_STATUS NV_API_CALL rm_set_external_kernel_client_count(nvidia_stack_t *sp, nv_state_t *nv, NvBool more) {
    (void)sp;
    CHECK(nv == &nvrm_dev.nv);
    external_clients += more ? 1 : -1;
    CHECK(external_clients >= 0);
    return NV_OK;
}

static NV_STATUS run(struct nvrm_gpu_attachments *a, uint32_t escape, void *args, uint32_t size, bool *handled) {
    NV_STATUS status = NV_ERR_GENERIC;
    *handled = nvrm_nv_ioctl(a, escape, args, size, &status);
    return status;
}

static void setup(void) {
    nv_state_t *nv = &nvrm_dev.nv;
    memset(&nvrm_dev, 0, sizeof nvrm_dev);
    nv->gpu_id = 0x100;
    nv->pci_info.bus = 1;
    nv->pci_info.slot = 0;
    nv->pci_info.vendor_id = 0x10de;
    nv->pci_info.device_id = 0x2786;
    nv->bars[0] = (nv_aperture_t){ .cpu_address = 0xf6000000, .size = 0x1000000, .map = registers };
    nv->bars[1] = (nv_aperture_t){ .cpu_address = 0xe0000000, .size = 0x10000000 };
    nv->regs = &nv->bars[0];
    nv->fb = &nv->bars[1];
    nvrm_dev.adapter_ready = true;
}

static void size_cases(void) {
    struct nvrm_gpu_attachments a = {0};
    bool handled;
    NVOS00_PARAMETERS free_args = {0};
    /* RM's escape of the right size goes on to RM; of another size it is refused here */
    CHECK(run(&a, NV_ESC_RM_FREE, &free_args, sizeof free_args, &handled) == NV_OK && !handled);
    CHECK(run(&a, NV_ESC_RM_FREE, &free_args, sizeof free_args - 1, &handled) == NV_ERR_INVALID_ARGUMENT && handled);
    /* an escape nobody knows, and one past the 8-bit numbers, never reach RM */
    CHECK(run(&a, 0, NULL, 0, &handled) == NV_ERR_INVALID_ARGUMENT && handled);
    CHECK(run(&a, NV_ESC_RM_FREE | 0x100, &free_args, sizeof free_args, &handled) == NV_ERR_INVALID_ARGUMENT &&
          handled);
    nv_ioctl_card_info_t cards[3];
    CHECK(run(&a, NV_ESC_CARD_INFO, cards, sizeof cards[0] + 1, &handled) == NV_ERR_INVALID_ARGUMENT);
    CHECK(run(&a, NV_ESC_CARD_INFO, cards, 0, &handled) == NV_ERR_INVALID_ARGUMENT);
    nv_ioctl_sys_params_t sys = {0};
    CHECK(run(&a, NV_ESC_SYS_PARAMS, &sys, sizeof sys * 2, &handled) == NV_ERR_INVALID_ARGUMENT);
    nv_ioctl_xfer_t xfer = {0};
    CHECK(run(&a, NV_ESC_IOCTL_XFER_CMD, &xfer, sizeof xfer, &handled) == NV_ERR_NOT_SUPPORTED && handled);
}

static void card_cases(void) {
    struct nvrm_gpu_attachments a = {0};
    bool handled;
    nv_ioctl_card_info_t cards[3];
    memset(cards, 0xAA, sizeof cards);
    CHECK(run(&a, NV_ESC_CARD_INFO, cards, sizeof cards, &handled) == NV_OK && handled);
    CHECK(cards[0].valid && cards[0].gpu_id == 0x100 && cards[0].pci_info.bus == 1 &&
          cards[0].pci_info.device_id == 0x2786 && cards[0].reg_address == 0xf6000000 &&
          cards[0].reg_size == 0x1000000 && cards[0].fb_address == 0xe0000000 && cards[0].fb_size == 0x10000000);
    CHECK(!cards[1].valid && !cards[2].valid && !cards[2].gpu_id && !cards[1].dev_name[0]);
    nvrm_dev.nv.flags |= NV_FLAG_EXCLUDE;
    CHECK(run(&a, NV_ESC_CARD_INFO, cards, sizeof cards, &handled) == NV_OK && !cards[0].valid);
    nvrm_dev.nv.flags &= ~NV_FLAG_EXCLUDE;
}

static void attach_cases(void) {
    struct nvrm_gpu_attachments a = {0};
    bool handled;
    NvU32 ids[3] = { 0, 0x100, 0x100 };
    CHECK(run(&a, NV_ESC_ATTACH_GPUS_TO_FD, ids, sizeof ids, &handled) == NV_OK && handled);
    CHECK(a.count == 3 && external_clients == 2);
    /* once per file */
    CHECK(run(&a, NV_ESC_ATTACH_GPUS_TO_FD, ids, sizeof ids, &handled) == NV_ERR_INVALID_ARGUMENT);
    CHECK(external_clients == 2);
    nvrm_nv_file_close(&a);
    CHECK(!a.count && !a.ids && external_clients == 0);
    /* a GPU nvrm does not serve undoes the ones before it */
    struct nvrm_gpu_attachments b = {0};
    NvU32 wrong[3] = { 0x100, 0x100, 0x200 };
    CHECK(run(&b, NV_ESC_ATTACH_GPUS_TO_FD, wrong, sizeof wrong, &handled) == NV_ERR_INVALID_ARGUMENT);
    CHECK(!b.count && external_clients == 0);
    /* nor is a GPU that did not start attached */
    nvrm_dev.adapter_ready = false;
    CHECK(run(&b, NV_ESC_ATTACH_GPUS_TO_FD, ids, sizeof ids, &handled) == NV_ERR_INVALID_ARGUMENT);
    CHECK(!b.count && external_clients == 0);
    nvrm_dev.adapter_ready = true;
    /* the shared counts NVKMS and UVM use */
    CHECK(nvrm_gpu_get(0x100, NV_TRUE) == NV_OK && external_clients == 0);
    nvrm_gpu_put(0x100, NV_TRUE);
    CHECK(nvrm_gpu_get(0, NV_FALSE) == NV_ERR_INVALID_DEVICE);
    nvrm_gpu_put(0x100, NV_FALSE); /* nothing open: no count goes below zero */
    CHECK(external_clients == 0);
}

static void other_cases(void) {
    struct nvrm_gpu_attachments a = {0};
    bool handled;
    nv_ioctl_rm_api_version_t version = {0};
    CHECK(run(&a, NV_ESC_CHECK_VERSION_STR, &version, sizeof version, &handled) == NV_OK && version_checks == 1);
    version_result = NV_ERR_GENERIC;
    CHECK(run(&a, NV_ESC_CHECK_VERSION_STR, &version, sizeof version, &handled) == NV_ERR_INVALID_ARGUMENT);

    registers[NV_RM_DEVICE_INTR_ADDRESS / 4] = 0x12345678;
    nv_ioctl_query_device_intr_t intr = { .status = NV_ERR_GENERIC };
    CHECK(run(&a, NV_ESC_QUERY_DEVICE_INTR, &intr, sizeof intr, &handled) == NV_OK);
    CHECK(intr.intrStatus == 0x12345678 && intr.status == NV_OK);

    nv_ioctl_sys_params_t sys = { .memblock_size = 0x8000000 };
    CHECK(run(&a, NV_ESC_SYS_PARAMS, &sys, sizeof sys, &handled) == NV_OK);
    CHECK(run(&a, NV_ESC_SYS_PARAMS, &sys, sizeof sys, &handled) == NV_OK);
    sys.memblock_size = 0x10000000;
    CHECK(run(&a, NV_ESC_SYS_PARAMS, &sys, sizeof sys, &handled) == NV_ERR_IN_USE);

    nv_ioctl_numa_info_t numa;
    memset(&numa, 0xAA, sizeof numa);
    CHECK(run(&a, NV_ESC_NUMA_INFO, &numa, sizeof numa, &handled) == NV_OK);
    CHECK(numa.nid == -1 && numa.status == NV_IOCTL_NUMA_STATUS_DISABLED && !numa.use_auto_online &&
          numa.memblock_size == 0x8000000);
    numa_result = NV_ERR_GENERIC;
    CHECK(run(&a, NV_ESC_NUMA_INFO, &numa, sizeof numa, &handled) == NV_ERR_IN_USE);

    nv_ioctl_set_numa_status_t set = { .status = NV_IOCTL_NUMA_STATUS_DISABLED };
    nvrm_thread_admin = false;
    CHECK(run(&a, NV_ESC_SET_NUMA_STATUS, &set, sizeof set, &handled) == NV_ERR_INSUFFICIENT_PERMISSIONS);
    nvrm_thread_admin = true;
    CHECK(run(&a, NV_ESC_SET_NUMA_STATUS, &set, sizeof set, &handled) == NV_OK);
    set.status = NV_IOCTL_NUMA_STATUS_ONLINE;
    CHECK(run(&a, NV_ESC_SET_NUMA_STATUS, &set, sizeof set, &handled) == NV_ERR_INVALID_ARGUMENT);

    nv_ioctl_wait_open_complete_t open = { .rc = 5, .adapterStatus = 5 };
    CHECK(run(&a, NV_ESC_WAIT_OPEN_COMPLETE, &open, sizeof open, &handled) == NV_OK);
    CHECK(open.rc == 0 && open.adapterStatus == NV_OK);
    nvrm_dev.adapter_ready = false;
    CHECK(run(&a, NV_ESC_WAIT_OPEN_COMPLETE, &open, sizeof open, &handled) == NV_OK);
    CHECK(open.rc != 0 && open.adapterStatus != NV_OK);
    nvrm_dev.adapter_ready = true;

    static nv_ioctl_export_to_dma_buf_fd_t export;
    export.status = NV_OK;
    CHECK(run(&a, NV_ESC_EXPORT_TO_DMABUF_FD, &export, sizeof export, &handled) == NV_OK);
    CHECK(export.status == NV_ERR_NOT_SUPPORTED);

    /* a GPU that left the bus answers nothing, RM's escapes included */
    nvrm_dev.nv.flags |= NV_FLAG_IN_SURPRISE_REMOVAL;
    NVOS00_PARAMETERS free_args = {0};
    CHECK(run(&a, NV_ESC_RM_FREE, &free_args, sizeof free_args, &handled) == NV_ERR_GPU_IS_LOST && handled);
    nvrm_dev.nv.flags &= ~NV_FLAG_IN_SURPRISE_REMOVAL;
}

int main(void) {
    setup();
    size_cases();
    card_cases();
    attach_cases();
    other_cases();
    printf("nvrm_ioctl_hosttest: %d failed\n", failures);
    return failures != 0;
}
