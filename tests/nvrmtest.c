/* nvrmtest: C:\Nocturne\System\nvrm's start on a machine without an NVIDIA GPU. QEMU's "edu" device
   (-device edu,dma_mask=0xffffffff, 1234:11e8) stands in for the GPU: nvrm attaches to it, maps
   BAR 0 and runs rm_init_rm (RM's CPU, registry and OS objects, all before the GPU is touched),
   then RM must refuse the device and nvrm end with status 1. A crash there (status 128 + the
   CPU exception, e.g. 141 for "mov %cr4" in user mode) is what real hardware hit first.
   The test claims edu with the N_CAP_HAL_CLAIM capability the test disk gives it and hands the claim to nvrm at
   N_SERVICE_HANDLE_DEVICE, as the service manager does; compiled inside Nocturne with tcc. Prints
   "nvrmtest: N failed". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "nocturne_services.h"

static int failed;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL: " __VA_ARGS__);                                                                              \
            printf("\n");                                                                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* The ledger id of QEMU's edu device, or 0. */
static uint32_t find_edu(void) {
    struct n_hal_snapshot ledger = {0};
    uint32_t id = 0;
    if (n_hal_snapshot(&ledger) < 0) return 0;
    const struct n_hal_device *d = ledger.first;
    for (uint32_t i = 0; i < ledger.count; i++, d = n_hal_device_next(d)) {
        uint64_t vendor, device;
        if (n_hal_device_number(d, N_PROP_PCI_VENDOR, &vendor) && n_hal_device_number(d, N_PROP_PCI_DEVICE, &device) &&
            vendor == 0x1234 && device == 0x11e8)
            id = d->id;
    }
    free((void *)ledger.first);
    return id;
}

int main(void) {
    uint32_t edu = find_edu();
    if (!edu) {
        printf("nvrmtest: no edu device (run QEMU with -device edu)\nnvrmtest: 1 failed\n");
        return 1;
    }
    /* the argument is the device's ledger id, the claim the handle N_SERVICE_HANDLE_DEVICE */
    char where[16];
    snprintf(where, sizeof where, "%x", (unsigned)edu);
    n_handle claim = n_hal_claim(n_cap_find(N_CAP_HAL_CLAIM), edu, "nvrm");
    CHECK(claim >= 0, "claim edu for nvrm: %s", n_result_text(claim));
    n_handle log = n_system_log_open();
    const char *args[2] = {"nvrm", where};
    int32_t handles[N_SERVICE_HANDLE_DEVICE + 1] = {-1, log, log, -1, claim};
    struct n_launch l = {.arguments = args, .handles = handles, .argument_count = 2,
                         .handle_count = N_SERVICE_HANDLE_DEVICE + 1};
    n_handle app = claim >= 0 ? n_app_launch(N_FOLDER_SYSTEM "\\nvrm", &l) : claim;
    CHECK(app >= 0, "launch " N_FOLDER_SYSTEM "\\nvrm: %s", n_result_text(app));
    if (log >= 0) n_handle_close(log);
    if (claim >= 0) n_handle_close(claim); /* nvrm's is the only one left: the claim ends with nvrm */
    if (app >= 0) {
        int status = -1;
        n_result r = n_app_result(app, n_clock_after(60000 * N_NS_PER_MS), &status);
        CHECK(r >= 0, "nvrm did not end within 60 s: %s", n_result_text(r));
        CHECK(status == 1, "nvrm ended with status %d (1 is RM refusing the device; 128 + n is a CPU exception)",
              status);
        n_handle_close(app);
    }

    char *text = malloc(65536);
    n_result got = text ? n_system_log(text, 65535) : -1;
    if (got >= 0) text[got] = 0;
    CHECK(got >= 0 && strstr(text, "NVRM: GPU 1234:11e8"), "nvrm attached to the device");
    CHECK(got >= 0 && strstr(text, "is not supported by RM"), "rm_init_rm ran and RM refused the device");
    CHECK(got >= 0 && !strstr(text, "rm_init_rm failed"), "rm_init_rm succeeded");

    /* the claim ends with nvrm: the device can be claimed again */
    n_handle again = n_hal_claim(n_cap_find(N_CAP_HAL_CLAIM), edu, "nvrmtest");
    CHECK(again >= 0, "claim after nvrm ended: %s", n_result_text(again));
    if (again >= 0) n_handle_close(again);

    printf("nvrmtest: %d failed\n", failed);
    return failed != 0;
}
