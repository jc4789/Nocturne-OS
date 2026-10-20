#pragma once
#include <stdint.h>
#include <stdbool.h>
/* Drivers in apps (docs/nocturne-native-abi.md §9): the N_KIND_PCI handle. syscall.c owns the
   handle table, user copies and shared objects; this file owns claims, config space and
   interrupts. Every function takes a file the caller already holds. */
struct file;
struct n_pci_function;
struct pci_claim;
int64_t pci_user_claim(uint8_t bus, uint8_t dev, uint8_t func, struct file **out);
bool pci_user_is_claim(struct file *f);
/* The claim record behind a handle, held by a BAR or DMA object for its whole life (NULL when f
   is no claim). revoked: its last handle closed, so its BARs are no longer the app's to map.
   dma_reusable: the function was seen stopped (or still runs under its driver), so DMA pages
   it could reach may go back to the allocator. BSP. */
struct pci_claim *pci_user_hold(struct file *f);
void pci_user_drop(struct pci_claim *c);
bool pci_user_revoked(const struct pci_claim *c);
bool pci_user_dma_reusable(const struct pci_claim *c);
int64_t pci_user_info(struct file *f, struct n_pci_function *out);
int64_t pci_user_config_read(struct file *f, uint32_t target, uint32_t offset, uint32_t width);
int64_t pci_user_config_write(struct file *f, uint32_t offset, uint32_t width, uint32_t value);
/* A memory BAR's bus address and size (page aligned, at least a page). */
int64_t pci_user_bar(struct file *f, uint32_t bar, uint64_t *address, uint64_t *size);
int64_t pci_user_interrupts(struct file *f, uint32_t count);
int64_t pci_user_interrupt_take(struct file *f, int64_t timeout_ms);
