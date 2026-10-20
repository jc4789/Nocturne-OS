/* nvrmtest: /bin/nvrm's start on a machine without an NVIDIA GPU. QEMU's "edu" device
   (-device edu,dma_mask=0xffffffff, 1234:11e8) stands in for the GPU: nvrm attaches to it, maps
   BAR 0 and runs rm_init_rm (RM's CPU, registry and OS objects, all before the GPU is touched),
   then RM must refuse the device and nvrm end with status 1. A crash there (status 128 + the
   CPU exception, e.g. 141 for "mov %cr4" in user mode) is what real hardware hit first.
   Booted with appdrivers=any so this app may claim; compiled inside Nocturne with tcc. Run it
   once without and once with displaydriver: the display that carries the screen is refused,
   then claimed and held. Prints "nvrmtest: N failed". */
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
    n_handle app = n_app_launch("/bin/nvrm", &l);
    CHECK(app >= 0, "launch /bin/nvrm: %s", n_result_text(app));
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
    if (again >= 0) {
        n_result r = n_pci_hold_screen(again, true);
        CHECK(r == -N_ERR_NOT_FOUND, "holding the screen through a function it is not in: %s", n_result_text(r));
        n_handle_close(again);
    }

    /* The display that carries the screen is refused unless the system booted with displaydriver.
       With it, RM's os_disable_console_access/os_enable_console_access: holds nest, an extra
       release is refused, and a claim that ends while it holds gives the screen back (the
       desktop must be drawn again afterwards). Run once with and once without the option. */
    n_handle screen = -1;
    bool refused = false;
    for (int i = 0; i < n && screen < 0; i++) {
        if (list[i].cls != 0x03) continue;
        n_handle h = n_pci_claim(list[i].bus, list[i].dev, list[i].func);
        struct n_pci_function info;
        if (h == -N_ERR_DENIED) refused = true;
        else if (h >= 0 && n_pci_info(h, &info) >= 0 && info.framebuffer_bar != N_PCI_NONE) screen = h;
        else if (h >= 0) n_handle_close(h);
    }
    got = text ? n_system_log(text, 65535) : -1;
    if (got >= 0) text[got] = 0;
    if (refused) {
        printf("nvrmtest: the display that shows the desktop stays with the system (no displaydriver)\n");
        CHECK(got >= 0 && strstr(text, "carries the screen; it stays with the system"), "the refusal is logged");
    }
    CHECK(screen >= 0 || refused, "claim the display that carries the boot screen");
    if (screen >= 0) {
        n_result r1 = n_pci_hold_screen(screen, true), r2 = n_pci_hold_screen(screen, true);
        n_result r3 = n_pci_hold_screen(screen, false), r4 = n_pci_hold_screen(screen, false);
        n_result r5 = n_pci_hold_screen(screen, false);
        CHECK(r1 == 0 && r2 == 0 && r3 == 0 && r4 == 0, "nested hold/release: %s %s %s %s", n_result_text(r1),
              n_result_text(r2), n_result_text(r3), n_result_text(r4));
        CHECK(r5 == -N_ERR_INVALID, "a release without a hold: %s", n_result_text(r5));
        r1 = n_pci_hold_screen(screen, true);
        CHECK(r1 == 0, "hold before the claim ends: %s", n_result_text(r1));
        n_handle_close(screen);
        /* the claim's BAR sizing pauses and resumes once, then the two holds above */
        got = text ? n_system_log(text, 65535) : -1;
        if (got >= 0) text[got] = 0;
        const char *p = got >= 0 ? text : NULL;
        int pauses = 0;
        while (p && (p = strstr(p, "fb: drawing paused"))) {
            const char *r = strstr(p, "fb: drawing resumed");
            if (!r) break;
            pauses++;
            p = r + 1;
        }
        CHECK(pauses >= 3, "drawing paused and resumed %d times (sizing, nested holds, the claim ending: 3)", pauses);
    }

    printf("nvrmtest: %d failed\n", failed);
    return failed != 0;
}
