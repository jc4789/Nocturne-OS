/* PCI の実装の表。実装を足すのは、ここに一行足すことである。 */
#include "kernel.h"
#include "dev/pci_drivers.h"
#include "vm/vm.h"

/* 表の順に結ぶ。先に合った実装が機能を持つので、機種の行の実装を種類の行の実装より先に置く。起動の待ちが一番長い
   nvme を最後に置く。 */
static const struct pci_driver *const pci_driver_table[] = {
    &rtl8125_driver, &xhci_driver, &hda_driver, &ahci_driver, &nvme_driver,
};

/* 実装の thread が attach の戻りより先に失敗を書くことがあるので、持ったという記録を先に書く。 */
static void pci_bind_driver(const struct pci_driver *driver) {
    for (int i = 0; i < pci_count(); i++) {
        struct pci_device *d = pci_at(i);
        if (d->hal.info.state != N_HAL_UNBOUND) continue;
        bool hit = false;
        for (size_t m = 0; m < driver->match_count && !hit; m++) hit = n_pci_matches(&driver->matches[m], &d->hal.info.at.pci);
        if (!hit) continue;
        hal_bound(&d->hal, driver->name);
        int r = driver->attach(d);
        if (r < 0) hal_failed(&d->hal, driver->name, r);
        else if (r == PCI_NOT_MINE) hal_unbound(&d->hal);
    }
}

void pci_bind_all(void) {
    for (size_t i = 0; i < ARRAY_SIZE(pci_driver_table); i++) pci_bind_driver(pci_driver_table[i]);
    size_t count;
    const struct pci_driver *const *vm = vm_pci_drivers(&count);
    for (size_t i = 0; i < count; i++) pci_bind_driver(vm[i]);
}
