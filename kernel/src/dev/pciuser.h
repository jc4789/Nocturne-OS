/* PCI の claim のバスの持ち分（dev/pciuser.c）。hal_bus_ops の claim に置く。 */
#pragma once
#include "hal/hal.h"

extern const struct hal_claim_ops pci_claim_ops;
