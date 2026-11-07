/* PCI の claim のバスの持ち分（dev/pciuser.c）。io_bus_ops の claim に置く。 */
#pragma once
#include "io/io.h"

extern const struct io_claim_ops pci_claim_ops;
