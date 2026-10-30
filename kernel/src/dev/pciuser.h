#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
/* Drivers in apps (docs/nocturne-native-abi.md §9): the N_KIND_PCI handle. syscall.c owns the
   handle table, user copies and shared objects; this file owns claims, config space and
   interrupts. Every function takes a handle the caller already holds, and is called from any
   CPU in a thread that may sleep, holding no spinlock. */
struct handle;
struct n_pci_function;
struct pci_claim;
int64_t pci_user_claim(uint8_t bus, uint8_t dev, uint8_t func, struct handle **out);
bool pci_user_is_claim(struct handle *h);
/* The claim record behind a handle, held by a BAR or DMA object for its whole life (NULL when h
   is no claim). revoked: its last handle closed, so its BARs are no longer the app's to map.
   dma_reusable: its driver still owns the live claim, or the IOMMU confirmed the ended claim's
   group is blocked. BME clear alone does not let remaining DMA pages return to the allocator.
   hold and the reads take no lock; drop of the last reference frees the claim's IOMMU domain
   (dev/iommu.c locks, the heap). revoked turns true before the claim's end calls
   shared_revoke_claim (syscall.c), which then finds every BAR mapping made before it: making a
   BAR mapping and that revocation must exclude each other under syscall.c's own lock. */
struct pci_claim *pci_user_hold(struct handle *h);
void pci_user_drop(struct pci_claim *c);
bool pci_user_revoked(const struct pci_claim *c);
bool pci_user_dma_reusable(const struct pci_claim *c);
/* Exclusive physical address limit imposed by the attached IOMMU's translation widths. */
uint64_t pci_user_dma_limit(const struct pci_claim *c);
/* DMA pages made through the claim (frames, or count pages from base when frames is NULL): where
   an IOMMU isolates the function, its domain maps them at their physical addresses. unmap is
   false when the IOMMU did not confirm the function can no longer reach them. They take the
   domain's mutex and may sleep (table allocations, invalidation waits). */
bool pci_user_dma_map(struct pci_claim *c, const uint64_t *frames, uint64_t base, size_t count);
bool pci_user_dma_unmap(struct pci_claim *c, const uint64_t *frames, uint64_t base, size_t count);
int64_t pci_user_info(struct handle *h, struct n_pci_function *out);
int64_t pci_user_config_read(struct handle *h, uint32_t target, uint32_t offset, uint32_t width);
int64_t pci_user_config_write(struct handle *h, uint32_t offset, uint32_t width, uint32_t value);
/* A memory BAR's bus address and size (page aligned, at least a page). */
int64_t pci_user_bar(struct handle *h, uint32_t bar, uint64_t *address, uint64_t *size);
bool pci_user_bar_prefetchable(struct handle *h, uint32_t bar);
int64_t pci_user_interrupts(struct handle *h, uint32_t count);
int64_t pci_user_interrupt_take(struct handle *h, int64_t timeout_ms);
/* n_pci_hold_screen: only for the function whose BAR the boot screen is drawn through, or the
   one whose surface the screen is drawn into. */
int64_t pci_user_hold_screen(struct handle *h, bool hold);
/* n_pci_give_screen: a display function's surface (a shared object made through its claim; NULL
   for the boot screen again) becomes where the screen is drawn; desc says how. n_pci_screen_vblank:
   the surface shown had a vertical blank, which paces the desktop's frames (gui/wm.c). */
struct n_scanout;
int64_t pci_user_give_screen(struct handle *h, struct handle *surface, const struct n_scanout *desc);
int64_t pci_user_screen_vblank(struct handle *h, uint64_t count, uint64_t timestamp_ns);
/* n_pci_screen_state: for the claim whose scanout the screen is drawn into or whose monitor's
   modes the system has (gui/wm.c keeps the state). n_pci_screen_modes: a display function's
   monitor and the modes its driver can show there, and what it showed (m: a kernel copy), for
   the system to choose from (gui/screenmode.c); count 0 withdraws them. */
struct n_screen_state;
struct n_screen_modes;
int64_t pci_user_screen_state(struct handle *h, uint64_t seen, struct n_screen_state *out, int64_t timeout_ms);
int64_t pci_user_screen_modes(struct handle *h, const struct n_screen_modes *m);
/* n_pci_screen_image: for that claim, a surface (as n_pci_give_screen's, without N_SCANOUT_DIRECT)
   the display engine may show in its scanout's place, which the panic screen is drawn into too;
   returns its number. n_pci_screen_image_end: that one no longer. The scanout's end, or a scanout
   without N_SCANOUT_DIRECT, ends them all. */
int64_t pci_user_screen_image(struct handle *h, struct handle *surface, const struct n_scanout *desc);
int64_t pci_user_screen_image_end(struct handle *h, int64_t number);
/* n_pci_acpi_eval and n_pci_acpi_notify_take: the function's part of the ACPI namespace
   (dev/acpi.c). which is N_ACPI_SELF, N_ACPI_UPSTREAM or N_ACPI_CHILD(i); the arguments, their
   data and out are kernel copies. The methods through which the system runs devices and the
   platform (power, removal, resources, _OSC, wake, initialisation, sleep), and the upstream
   bridge's own (not predefined) methods, are refused with N_ERR_DENIED. A thread that may block. */
struct n_acpi_value;
int64_t pci_user_acpi_eval(struct handle *h, uint32_t which, const char *name, const struct n_acpi_value *args,
                           uint32_t nargs, const uint8_t *data, size_t data_size, uint8_t *out, size_t out_size);
int64_t pci_user_acpi_notify_take(struct handle *h, int64_t timeout_ms);
