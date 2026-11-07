/* Late kernel initialisation: filesystems, devices, GUI and the first process. */
#include "kernel.h"
#include "dev/acpi.h"
#include "dev/entropy.h"
#include "fs/store.h"
#include "dev/input.h"
#include "dev/fbcon.h"
#include "dev/fb.h"
#include "dev/pci.h"
#include "hal/hal.h"
#include "dev/iommu.h"
#include "dev/pm.h"
#include "dev/hibernate.h"
#include "dev/audio.h"
#include "dev/usb.h"
#include "gui/wm.h"
#include "vm/vm.h"
#include "sys/object.h"
#include "sys/proc.h"
#include "sys/stream.h"
#include "sys/settings.h"
#include "mm/heap.h"
#include "mm/vmalloc.h"
#include "sys/authority.h"

void acpi_init(void);
void net_init(void);

#define INIT_PATH N_FOLDER_SYSTEM "\\services"

void kmain_late(void) {
    settings_init();
    entropy_init();
    vmalloc_purge_start();
    pci_config_init();
    acpi_init();
    pm_init(); /* the power thread: every shutdown and restart goes through it */
    hal_init();
    fb_publish();
    vm_init(); /* only in a VM: its own bus, its fixed ports and the drivers of its devices */
    acpi_devices_publish(); /* the host bridges first: the root buses' functions go below them */
    pci_init();
    iommu_init(); /* before any driver turns an interrupt on: their messages may be remapped */
    net_init();
    audio_init();
    acpi_devices_publish_below();
    hal_start(); /* the drivers bind, each device on a thread of its own */
    store_init();        /* C:, from the disk the system started from (waiting for it to appear) */
    hibernate_restore(); /* returns only when there is no image to go back to */
    store_start();

    wm_init();
    vm_session_init();
    if (!wm_running()) input_set_sinks(console_key, NULL);
    settings_test_flags();
    pm_sleep_check(); /* 機械の部分（IOMMU、S3、起こしの頁、C: の休止の置き場）が眠れるか */

    /* its input, output and log are the console */
    struct handle *con = stream_console_open();
    /* 根の能力は最初の process だけが受ける。身元はシステム。 */
    _Static_assert(N_INITIAL_ROOT_HANDLE == 3, "the handles below");
    struct handle *root = NULL;
    if (cap_new(N_CAP_ROOT, &root) < 0) kprintf("init: no memory for the root capability\n");
    struct handle *handles[4] = {con, con, con, root};
    char *argv[] = {INIT_PATH, NULL};
    static const int32_t terminals[] = {0, 1, 2};
    struct proc_launch l = {.argc = 1, .argv = argv, .terminals = terminals, .terminal_count = 3,
                            .handles = handles, .handle_count = 4, .identity = identity_kernel};
    int pid = proc_launch(INIT_PATH, &l);
    if (pid < 0) kprintf("init: failed to start %s (%d)\n", INIT_PATH, pid);
    if (con) handle_close(con);
    if (root) handle_close(root);
}
