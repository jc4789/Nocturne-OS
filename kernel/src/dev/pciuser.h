#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
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
   dma_reusable: its driver still owns the live claim, or the IOMMU confirmed the ended claim's
   group is blocked. BME clear alone does not let remaining DMA pages return to the allocator. BSP. */
struct pci_claim *pci_user_hold(struct file *f);
void pci_user_drop(struct pci_claim *c);
bool pci_user_revoked(const struct pci_claim *c);
bool pci_user_dma_reusable(const struct pci_claim *c);
/* Exclusive physical address limit imposed by the attached IOMMU's translation widths. */
uint64_t pci_user_dma_limit(const struct pci_claim *c);
/* DMA pages made through the claim (frames, or count pages from base when frames is NULL): where
   an IOMMU isolates the function, its domain maps them at their physical addresses. unmap is
   false when the IOMMU did not confirm the function can no longer reach them. BSP. */
bool pci_user_dma_map(struct pci_claim *c, const uint64_t *frames, uint64_t base, size_t count);
bool pci_user_dma_unmap(struct pci_claim *c, const uint64_t *frames, uint64_t base, size_t count);
int64_t pci_user_info(struct file *f, struct n_pci_function *out);
int64_t pci_user_config_read(struct file *f, uint32_t target, uint32_t offset, uint32_t width);
int64_t pci_user_config_write(struct file *f, uint32_t offset, uint32_t width, uint32_t value);
/* A memory BAR's bus address and size (page aligned, at least a page). */
int64_t pci_user_bar(struct file *f, uint32_t bar, uint64_t *address, uint64_t *size);
bool pci_user_bar_prefetchable(struct file *f, uint32_t bar);
int64_t pci_user_interrupts(struct file *f, uint32_t count);
int64_t pci_user_interrupt_take(struct file *f, int64_t timeout_ms);
/* n_pci_hold_screen: only for the function whose BAR the boot screen is drawn through. */
int64_t pci_user_hold_screen(struct file *f, bool hold);
/* The user's way out of a stuck driver that holds the screen (Ctrl+Alt+Esc, gui/wm.c and the
   text console in init.c): its app is stopped and the claim's end gives the screen back. False
   when no driver holds it, or its app is already being stopped. Only asks the screen watch thread
   to stop it, so key handlers may call it from an interrupt. */
bool pci_user_screen_take_back(void);
