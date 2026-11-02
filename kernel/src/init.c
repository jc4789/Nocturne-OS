/* Late kernel initialisation: filesystems, devices, GUI and the first process. */
#include "kernel.h"
#include "dev/entropy.h"
#include "fs/store.h"
#include "dev/input.h"
#include "dev/fbcon.h"
#include "dev/fb.h"
#include "dev/gpu.h"
#include "dev/pci.h"
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
void ahci_init(void);
void net_init(void);
void nvme_init(void);

#define INIT_PATH N_FOLDER_SYSTEM "\\init"

void kmain_late(void) {
    settings_init();
    entropy_init();
    vmalloc_purge_start();
    acpi_init();
    pm_init(); /* the power thread: every shutdown and restart goes through it */
    vm_init(); /* only in a VM: its own bus and the devices on it */
    pci_init();
    vm_display_init(); /* only in a VM: its emulated displays become screens of their own buffers */
    fb_pm_init(); /* the display joins the power changes once its PCI identity is known */
    gpu_init();   /* the offscreen GPU, which never shows a screen */
    iommu_init(); /* before any driver turns an interrupt on: their messages may be remapped */
    usb_init();
    net_init();
    audio_init();
    ahci_init();
    nvme_init();
    vm_storage_init();
    store_init();        /* C:, from the disk the system started from (waiting for it to appear) */
    hibernate_restore(); /* returns only when there is no image to go back to */
    store_start();

    wm_init();
    vm_session_init();
    if (!wm_running()) input_set_sinks(console_key, NULL);
    settings_test_flags();
    pm_sleep_check(); /* every driver has registered: whether the start menu offers Sleep */

    /* its input, output and log are the console */
    struct handle *con = stream_console_open();
    /* 根の能力は最初の process だけが受ける。身元はシステム。 */
    _Static_assert(N_INITIAL_ROOT_HANDLE == 3, "the handles below");
    struct handle *root = NULL;
    if (cap_new(N_CAP_ROOT, &root) < 0) kprintf("init: no memory for the root capability\n");
    struct handle *handles[4] = {con, con, con, root};
    char folder[sizeof "FOLDER=C:\\Users\\" + 255];
    ksnprintf(folder, sizeof folder, "FOLDER=%s", store_user_folder());
    char *argv[] = {INIT_PATH, NULL};
    char *settings[] = {folder, "TERMINAL=0 1 2"};
    struct proc_launch l = {.argc = 1, .argv = argv, .setting_count = 2, .settings = settings,
                            .handles = handles, .handle_count = 4, .identity = identity_kernel};
    int pid = proc_launch(INIT_PATH, &l);
    if (pid < 0) kprintf("init: failed to start %s (%d)\n", INIT_PATH, pid);
    if (con) handle_close(con);
    if (root) handle_close(root);
}
