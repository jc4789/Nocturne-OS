/* Late kernel initialisation: filesystems, devices, GUI and the first process. */
#include "kernel.h"
#include "dev/entropy.h"
#include "fs/store.h"
#include "dev/input.h"
#include "dev/fbcon.h"
#include "dev/fb.h"
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
#include "sys/sched.h"
#include "sys/stream.h"
#include "sys/keep.h"
#include "gui/screenmode.h"
#include "mm/heap.h"
#include "mm/vmalloc.h"

extern const char *kernel_cmdline;
void acpi_init(void);
void ahci_init(void);
void net_init(void);
void nvme_init(void);

#define INIT_PATH N_FOLDER_SYSTEM "\\init"

/* text-mode keyboard: translate key events into terminal byte sequences */
static void console_key_sink(const struct key_event *e) {
    if (!e->pressed) return;
    uint16_t k = e->key;
    if (k < 0x80) {
        char c = (char)k;
        if ((e->mods & MOD_CTRL) && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c &= 0x1F;
        console_input_char(c);
        return;
    }
    switch (k) {
    case KEY_UP: console_input_str("\x1b[A"); break;
    case KEY_DOWN: console_input_str("\x1b[B"); break;
    case KEY_RIGHT: console_input_str("\x1b[C"); break;
    case KEY_LEFT: console_input_str("\x1b[D"); break;
    case KEY_HOME: console_input_str("\x1b[H"); break;
    case KEY_END: console_input_str("\x1b[F"); break;
    case KEY_DELETE: console_input_str("\x1b[3~"); break;
    case KEY_PGUP: console_input_str("\x1b[5~"); break;
    case KEY_PGDN: console_input_str("\x1b[6~"); break;
    default: break;
    }
}

bool cmdline_has(const char *word) {
    size_t n = strlen(word);
    for (const char *p = kernel_cmdline; *p; p++) {
        if ((p == kernel_cmdline || p[-1] == ' ') && !strncmp(p, word, n) && (p[n] == 0 || p[n] == ' '))
            return true;
    }
    return false;
}

bool cmdline_number(const char *key, uint64_t *out) {
    size_t n = strlen(key);
    bool found = false;
    for (const char *p = kernel_cmdline; *p; p++) {
        if ((p != kernel_cmdline && p[-1] != ' ') || strncmp(p, key, n) || p[n] != '=') continue;
        const char *q = p + n + 1;
        uint64_t value = 0;
        bool valid = *q >= '0' && *q <= '9';
        while (*q >= '0' && *q <= '9') {
            unsigned digit = *q++ - '0';
            if (value > (UINT64_MAX - digit) / 10) valid = false;
            if (valid) value = value * 10 + digit;
        }
        if (valid && (!*q || *q == ' ')) { *out = value; found = true; }
    }
    return found;
}

void kmain_late(void) {
    entropy_init();
    heap_trim_start();
    vmalloc_purge_start();
    acpi_init();
    pm_init(); /* the power thread: every shutdown and restart goes through it */
    vm_init(); /* only in a VM: its own bus and the devices on it */
    pci_init();
    fb_pm_init(); /* the display joins the power changes once its PCI identity is known */
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
    stream_system_log_keep(); /* what this start says is kept on C: */

    bool gui = !cmdline_has("nogui");
    if (gui) wm_init();
    /* Each part's state back from its document on C: before the first program (sys/keep.c): sound
       registered in audio_init, the desktop in wm_init, the screen's modes here. */
    screen_modes_init();
    keep_start();
    vm_session_init();
    if (!gui || !wm_running()) input_set_sinks(console_key_sink, NULL);
    pm_sleep_check(); /* every driver has registered: whether the start menu offers Sleep */

    /* its input, output and log are the console */
    struct handle *con = stream_console_open();
    struct handle *handles[3] = {con, con, con};
    char folder[sizeof "FOLDER=C:\\Users\\" + 255];
    ksnprintf(folder, sizeof folder, "FOLDER=%s", store_user_folder());
    char *argv[] = {INIT_PATH, NULL};
    char *settings[] = {folder, "TERMINAL=0 1 2"};
    struct proc_launch l = {.argc = 1, .argv = argv, .setting_count = 2, .settings = settings,
                            .handles = handles, .handle_count = 3};
    int pid = proc_launch(INIT_PATH, &l);
    if (pid > 0) init_pid = pid;
    if (pid < 0) kprintf("init: failed to start %s (%d)\n", INIT_PATH, pid);
    if (con) handle_close(con);
}
