/* Late kernel initialisation: filesystems, devices, GUI and the first process. */
#include "kernel.h"
#include "dev/entropy.h"
#include "limine.h"
#include "fs/ramfs.h"
#include "fs/nos.h"
#include "dev/input.h"
#include "dev/fbcon.h"
#include "dev/fb.h"
#include "dev/pci.h"
#include "dev/pciuser.h"
#include "dev/iommu.h"
#include "dev/pm.h"
#include "dev/hibernate.h"
#include "dev/ata.h"
#include "dev/audio.h"
#include "dev/usb.h"
#include "gui/wm.h"
#include "hv/vmbus.h"
#include "sys/proc.h"
#include "sys/sched.h"
#include "sys/prof.h"
#include "mm/pmm.h"
#include "mm/heap.h"

extern volatile struct limine_module_request module_req;
extern const char *kernel_cmdline;
void acpi_init(void);
void net_init(void);
void nvme_init(void);
void rdp_init(void);

/* text-mode keyboard: translate key events into terminal byte sequences */
static void console_key_sink(const struct key_event *e) {
    if (!e->pressed) return;
    uint16_t k = e->key;
    /* while an app's driver holds the screen: stop it, the screen comes back (dev/pciuser.c) */
    if (k == KEY_ESC && (e->mods & (MOD_CTRL | MOD_ALT)) == (MOD_CTRL | MOD_ALT) && pci_user_screen_take_back())
        return;
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

/* GPU firmware is large and only the driver of a present GPU reads it: the
   base keeps it only when such a display controller is on the bus (dropped
   from the base, not removed by the user: docs/nosfs.md §8). */
static void drop_absent_firmware(void) {
    for (int i = 0; i < pci_count(); i++) {
        struct pci_device *d = pci_at(i);
        if (d && d->vendor == 0x10DE && d->cls == 0x03) return;
    }
    const char *path = "/usr/share/firmware/nvidia";
    if (!ramfs_base_find(path)) return;
    uint64_t heap = heap_used_bytes(), free = pmm_free_pages();
    ramfs_base_remove(path);
    /* files copied into the heap shrink it, files read in place give archive pages back */
    int64_t gone = (int64_t)(heap - heap_used_bytes()) + (int64_t)((pmm_free_pages() - free) * PAGE_SIZE);
    kprintf("initrd: no NVIDIA display controller, firmware dropped (%ld MiB)\n", (long)(gone / 1048576));
}

void kmain_late(void) {
    entropy_init();
    prof_init();
    heap_trim_start();
    vfs_init();
    struct vnode *tmp = ramfs_new_root(); /* emptied by every restart: never on the volume */
    if (!tmp || vfs_mount("/tmp", tmp) < 0) panic("vfs: cannot mount /tmp");
    devfs_init();
    if (module_req.response && module_req.response->module_count) {
        struct limine_file *m = module_req.response->modules[0];
        initrd_load(m->address, m->size);
        kprintf("initrd: %lu KiB loaded from %s\n", m->size / 1024, m->path);
        uint64_t phys, pages;
        if (initrd_range(&phys, &pages)) {
            kprintf("initrd: files read in place from the archive\n");
        } else { /* every file was copied into ramfs; the archive itself is dead weight */
            uint64_t before = pmm_free_pages();
            pmm_release_range(hhdm_virt_to_phys(m->address), m->size);
            kprintf("initrd: archive released, %lu MiB returned\n", (unsigned long)((pmm_free_pages() - before) * PAGE_SIZE / 1048576));
        }
    } else {
        kprintf("initrd: none found!\n");
    }
    acpi_init();
    pm_init(); /* the power thread: every shutdown and restart goes through it */
    if (vmbus_init()) {
        hv_input_init();
        hv_util_init();
        hvsock_init();
    }
    pci_init();
    fb_pm_init(); /* the display joins the power changes once its PCI identity is known */
    iommu_init(); /* before any driver turns an interrupt on: their messages may be remapped */
    drop_absent_firmware();
    usb_init();
    net_init();
    audio_init();
    ata_init();
    storvsc_init();
    nvme_init();
    usb_wait_settled(8000); /* USB disks present at boot can hold /data */
    bool persistent = nos_attach(); /* the volume / is on, when one of Nocturne's disks has it */
    fat_mount_data(persistent ? "/media/NOCTDATA" : "/data");
    hibernate_restore(); /* returns only when there is no image to go back to; nothing was committed */
    if (!vfs_lookup("/data")) vfs_mkdir("/data");
    if (!vfs_lookup("/home")) vfs_mkdir("/home");

    bool gui = !cmdline_has("nogui");
    if (gui) wm_init();
    rdp_init();
    if (!gui || !wm_running()) input_set_sinks(console_key_sink, NULL);
    pm_sleep_check(); /* every driver has registered: whether the start menu offers Sleep */

    struct file *con = NULL;
    vfs_open("/dev/console", O_RDWR, &con);
    struct file *fds[3] = {con, con, con};
    char *argv[] = {"/bin/init", NULL};
    /* its input, output and log are the console */
    char *settings[] = {"FOLDER=/home", "TERMINAL=0 1 2"};
    struct proc_launch l = {.argc = 1, .argv = argv, .setting_count = 2, .settings = settings,
                            .handles = fds, .handle_count = 3};
    int pid = proc_launch("/bin/init", &l);
    if (pid > 0) init_pid = pid;
    if (pid < 0) kprintf("init: failed to start /bin/init (%d)\n", pid);
    if (con) vfs_close(con);
}
