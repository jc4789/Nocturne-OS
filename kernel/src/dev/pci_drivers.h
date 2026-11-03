/* カーネルの実機の PCI の実装（dev/pci_drivers.c の表）。 */
#pragma once
#include "dev/pci.h"

extern const struct pci_driver rtl8125_driver, xhci_driver, hda_driver, ahci_driver, nvme_driver;
