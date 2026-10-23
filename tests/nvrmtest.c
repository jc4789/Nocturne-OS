/* nvrmtest: C:\Nocturne\System\nvrm's start on a machine without an NVIDIA GPU. QEMU's "edu" device
   (-device edu,dma_mask=0xffffffff, 1234:11e8) stands in for the GPU: nvrm attaches to it, maps
   BAR 0 and runs rm_init_rm (RM's CPU, registry and OS objects, all before the GPU is touched),
   then RM must refuse the device and nvrm end with status 1. A crash there (status 128 + the
   CPU exception, e.g. 141 for "mov %cr4" in user mode) is what real hardware hit first.
   Booted with appdrivers=any so this app may claim; compiled inside Nocturne with tcc. Prints
   "nvrmtest: N failed". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

static int failed;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL: " __VA_ARGS__);                                                                              \
            printf("\n");                                                                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

int main(void) {
    struct n_pciinfo list[128];
    int n = pcilist(list, 128), at = -1;
    for (int i = 0; i < n; i++)
        if (list[i].vendor == 0x1234 && list[i].device == 0x11e8) at = i;
    if (at < 0) {
        printf("nvrmtest: no edu device (run QEMU with -device edu)\nnvrmtest: 1 failed\n");
        return 1;
    }
    struct n_pciinfo *e = &list[at];
    n_handle pci = n_pci_claim(e->bus, e->dev, e->func);
    CHECK(pci >= 0, "claim: %s (boot with appdrivers=any)", n_result_text(pci));
    if (pci < 0) {
        printf("nvrmtest: %d failed\n", failed);
        return 1;
    }

    /* the same hand-over as init: arguments "nvrm <where>", the claim as handle 3 */
    char where[16];
    snprintf(where, sizeof where, "%02x:%02x.%u", e->bus, e->dev, e->func);
    n_handle log = n_system_log_open();
    const char *args[2] = {"nvrm", where};
    int32_t handles[4] = {-1, log, log, pci};
    struct n_launch l = {.arguments = args, .handles = handles, .argument_count = 2, .handle_count = 4};
    n_handle app = n_app_launch(N_FOLDER_SYSTEM "\\nvrm", &l);
    CHECK(app >= 0, "launch " N_FOLDER_SYSTEM "\\nvrm: %s", n_result_text(app));
    n_handle_close(pci);
    if (log >= 0) n_handle_close(log);
    if (app >= 0) {
        int status = -1;
        n_result r = n_app_result(app, 60000, &status);
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
    n_handle again = n_pci_claim(e->bus, e->dev, e->func);
    CHECK(again >= 0, "claim after nvrm ended: %s", n_result_text(again));
    if (again >= 0) n_handle_close(again);

    printf("nvrmtest: %d failed\n", failed);
    return failed != 0;
}
