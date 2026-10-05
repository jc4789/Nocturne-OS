/* Late kernel initialisation: filesystems, devices, GUI and the first process. */
#include "kernel.h"
#include "dev/entropy.h"
#include "limine.h"
#include "fs/vfs.h"
#include "dev/input.h"
#include "dev/fbcon.h"
#include "dev/pci.h"
#include "gui/wm.h"
#include "sys/proc.h"
#include "sys/sched.h"

extern volatile struct limine_module_request module_req;
extern const char *kernel_cmdline;
void acpi_init(void);
void net_init(void);

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

void kmain_late(void) {
    entropy_init();
    vfs_init();
    vfs_mkdir("/home");
    vfs_mkdir("/tmp");
    devfs_init();
    if (module_req.response && module_req.response->module_count) {
        struct limine_file *m = module_req.response->modules[0];
        initrd_load(m->address, m->size);
        kprintf("initrd: %lu KiB loaded from %s\n", m->size / 1024, m->path);
    } else {
        kprintf("initrd: none found!\n");
    }
    acpi_init();
    pci_init();
    net_init();

    bool gui = !cmdline_has("nogui");
    if (gui) wm_init();
    if (!gui || !wm_running()) input_set_sinks(console_key_sink, NULL);

    struct file *con = NULL;
    vfs_open("/dev/console", O_RDWR, &con);
    struct file *fds[3] = {con, con, con};
    char *argv[] = {"/bin/init", NULL};
    int pid = proc_spawn("/bin/init", 1, argv, fds, "/home", NULL);
    if (pid < 0) kprintf("init: failed to start /bin/init (%d)\n", pid);
    if (con) vfs_close(con);
}
