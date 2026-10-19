/* pcitest: drivers in apps (docs/nocturne-native-abi.md §9) on QEMU's "edu" device (-device edu,dma_mask=0xffffffff,
   1234:11e8): the claim, config space, a BAR as a shared object and its views, MSI, and DMA memory
   the device copies through. Booted with appdrivers=any so this app may claim; compiled inside
   Nocturne with tcc. Prints "pcitest: N failed". */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "nocturne.h"

static int failed;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL: " __VA_ARGS__);                                                                              \
            printf("\n");                                                                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* edu's registers (BAR 0) */
#define EDU_IDENT 0x00
#define EDU_LIVENESS 0x04
#define EDU_IRQ_STATUS 0x24
#define EDU_IRQ_RAISE 0x60
#define EDU_IRQ_ACK 0x64
#define EDU_DMA_SRC 0x80
#define EDU_DMA_DST 0x88
#define EDU_DMA_COUNT 0x90
#define EDU_DMA_CMD 0x98
#define EDU_DMA_BUFFER 0x40000 /* the device's own 4 KiB, reached only by its DMA (not by MMIO) */
/* edu reaches 28 bits unless QEMU widens it: run with -device edu,dma_mask=0xffffffff so the
   N_DMA_32BIT memory below is always in reach. */
#define EDU_DMA_MASK 0xFFFFFFFFull

static volatile uint8_t *regs;
static uint32_t rd32(uint32_t at) { return *(volatile uint32_t *)(regs + at); }
static void wr32(uint32_t at, uint32_t v) { *(volatile uint32_t *)(regs + at) = v; }
static void wr64(uint32_t at, uint64_t v) { *(volatile uint64_t *)(regs + at) = v; }

/* One DMA transfer of `count` bytes; to_ram copies edu's buffer to `ram`, else from it. */
static int dma(n_handle pci, uint64_t ram, uint32_t count, int to_ram) {
    wr64(EDU_DMA_SRC, to_ram ? EDU_DMA_BUFFER : ram);
    wr64(EDU_DMA_DST, to_ram ? ram : EDU_DMA_BUFFER);
    wr64(EDU_DMA_COUNT, count);
    wr64(EDU_DMA_CMD, 1 | (to_ram ? 2 : 0) | 4); /* start, direction, interrupt when done */
    n_result got = n_pci_interrupt_take(pci, 2000);
    uint32_t why = rd32(EDU_IRQ_STATUS);
    wr32(EDU_IRQ_ACK, why);
    return got > 0 && (why & 0x100);
}

int main(void) {
    struct n_pciinfo list[128];
    int n = pcilist(list, 128), at = -1;
    for (int i = 0; i < n; i++)
        if (list[i].vendor == 0x1234 && list[i].device == 0x11e8) at = i;
    if (at < 0) {
        printf("pcitest: no edu device (run QEMU with -device edu)\npcitest: 1 failed\n");
        return 1;
    }
    struct n_pciinfo *e = &list[at];
    n_handle pci = n_pci_claim(e->bus, e->dev, e->func);
    CHECK(pci >= 0, "claim: %s (boot with appdrivers=any)", n_result_text(pci));
    if (pci < 0) {
        printf("pcitest: %d failed\n", failed);
        return 1;
    }
    CHECK(n_handle_kind(pci) == N_KIND_PCI, "kind %d", (int)n_handle_kind(pci));
    n_handle again = n_pci_claim(e->bus, e->dev, e->func);
    CHECK(again == -N_ERR_BUSY, "second claim: %s", n_result_text(again));
    if (again >= 0) n_handle_close(again);

    struct n_pci_function info;
    CHECK(n_pci_info(pci, &info) == 0, "info");
    CHECK(info.vendor == 0x1234 && info.device == 0x11e8, "ids %04x:%04x", info.vendor, info.device);
    CHECK((info.bar[0].flags & N_PCI_BAR_MEMORY) && info.bar[0].size == 1 << 20, "bar 0: flags %x size %llu",
          info.bar[0].flags, (unsigned long long)info.bar[0].size);
    CHECK(info.msi_vectors >= 1, "msi vectors %u", info.msi_vectors);
    CHECK(info.framebuffer_bar == N_PCI_NONE, "edu has no screen");

    n_result id = n_pci_config_read(pci, N_PCI_SELF, 0, 4);
    CHECK(id == 0x11e81234, "config id %llx", (long long)id);
    CHECK(n_pci_config_read(pci, N_PCI_SELF, 2, 2) == 0x11e8, "config 16-bit read");
    CHECK(n_pci_config_read(pci, N_PCI_SELF, 1, 2) == -N_ERR_INVALID, "unaligned read is refused");
    CHECK(n_pci_config_write(pci, 0x10, 4, 0) == -N_ERR_DENIED, "BAR write is refused");
    n_result cmd = n_pci_config_read(pci, N_PCI_SELF, 4, 2);
    CHECK(cmd >= 0 && n_pci_config_write(pci, 4, 2, (uint32_t)cmd | 6) == 0, "command: memory and bus master");
    CHECK((n_pci_config_read(pci, N_PCI_SELF, 4, 2) & 6) == 6, "command reads back");
    /* a function on bus 0 that is neither this device nor a bridge above it */
    for (int i = 0; i < n; i++)
        if (i != at && list[i].bus == e->bus && list[i].dev != e->dev && list[i].cls != 0x06) {
            n_result r = n_pci_config_read(pci, N_PCI_TARGET(list[i].bus, list[i].dev, list[i].func), 0, 4);
            CHECK(r == -N_ERR_DENIED, "unrelated config read: %lld", (long long)r);
            break;
        }

    n_handle bar = n_pci_bar(pci, 0);
    CHECK(bar >= 0 && n_handle_kind(bar) == N_KIND_SHARED, "bar: %s", n_result_text(bar));
    regs = bar >= 0 ? n_shared_map(bar, 1 << 20, N_MEMORY_READ_WRITE) : NULL;
    CHECK(regs != NULL, "map bar");
    if (regs) {
        CHECK(rd32(EDU_IDENT) >> 16 == 0x0100 && (rd32(EDU_IDENT) & 0xFF) == 0xed, "ident %08x", rd32(EDU_IDENT));
        wr32(EDU_LIVENESS, 0x12345678);
        CHECK(rd32(EDU_LIVENESS) == ~0x12345678u, "liveness %08x", rd32(EDU_LIVENESS));
    }
    n_handle wc = n_shared_view(bar, 0, 4096, N_CACHE_WRITE_COMBINE);
    CHECK(wc >= 0, "write-combining view: %s", n_result_text(wc));
    n_handle wb = n_shared_view(bar, 0, 4096, N_CACHE_WRITE_BACK);
    CHECK(wb == -N_ERR_DENIED, "write-back view of device memory: %s", n_result_text(wb));
    CHECK(n_shared_view(bar, 100, 4096, N_CACHE_UNCACHED) == -N_ERR_INVALID, "unaligned view");
    if (wc >= 0) {
        volatile uint32_t *buf = n_shared_map(wc, 4096, N_MEMORY_READ_WRITE);
        CHECK(buf != NULL, "map view");
        if (buf && regs) {
            /* the liveness register through the write-combining view, read back through the BAR */
            buf[EDU_LIVENESS / 4] = 0xCAFEF00D;
            __asm__ volatile("sfence" ::: "memory");
            CHECK(rd32(EDU_LIVENESS) == ~0xCAFEF00Du, "view and BAR see the same registers: %08x", rd32(EDU_LIVENESS));
            n_memory_release((void *)buf, 4096);
        }
        n_handle_close(wc);
    }

    n_result vectors = n_pci_interrupts(pci, 4);
    CHECK(vectors >= 1, "interrupts: %s", n_result_text(vectors));
    CHECK(n_pci_interrupt_take(pci, 0) == -N_ERR_TIMEOUT, "nothing pending yet");
    if (regs && vectors >= 1) {
        wr32(EDU_IRQ_RAISE, 0x5);
        n_result got = n_pci_interrupt_take(pci, 2000);
        CHECK(got == 1, "interrupt bits %lld", (long long)got);
        CHECK(rd32(EDU_IRQ_STATUS) == 0x5, "irq status %x", rd32(EDU_IRQ_STATUS));
        wr32(EDU_IRQ_ACK, 0x5);
    }

    n_handle mem = n_dma_create(pci, 8192, N_DMA_CONTIGUOUS | N_DMA_32BIT);
    CHECK(mem >= 0, "dma: %s", n_result_text(mem));
    uint64_t pages[2] = {0, 0};
    CHECK(mem >= 0 && n_dma_addresses(mem, 0, pages, 4) == 2, "dma addresses");
    CHECK(pages[1] == pages[0] + 4096 && pages[0] && pages[0] < (1ull << 32), "contiguous below 4 GiB: %llx %llx",
          (unsigned long long)pages[0], (unsigned long long)pages[1]);
    uint8_t *ram = mem >= 0 ? n_shared_map(mem, 8192, N_MEMORY_READ_WRITE) : NULL;
    CHECK(ram != NULL, "map dma");
    if (ram) CHECK(ram[0] == 0 && ram[8191] == 0, "dma memory starts zeroed");
    if (ram && regs && vectors >= 1) {
        if (pages[0] + 8192 - 1 > EDU_DMA_MASK) {
            CHECK(0, "32-bit dma memory at %llx is above 4 GiB", (unsigned long long)pages[0]);
        } else {
            for (int i = 0; i < 4096; i++) ram[i] = (uint8_t)(i * 7 + 3);
            CHECK(dma(pci, pages[0], 4096, 0), "dma to the device");
            CHECK(dma(pci, pages[0] + 4096, 4096, 1), "dma back to RAM");
            CHECK(!memcmp(ram, ram + 4096, 4096), "the copy came back intact");
        }
    }
    n_handle low = mem >= 0 ? n_shared_view(mem, 4096, 4096, N_CACHE_UNCACHED) : -1;
    CHECK(low >= 0, "uncached view of dma memory: %s", n_result_text(low));
    uint64_t through = 0;
    CHECK(low >= 0 && n_dma_addresses(low, 0, &through, 1) == 1 && through == pages[1], "addresses through a view");
    n_handle plain = n_shared_create(4096);
    CHECK(n_shared_view(plain, 0, 4096, N_CACHE_UNCACHED) == -N_ERR_DENIED, "ordinary memory keeps its caching");
    CHECK(n_dma_addresses(plain, 0, &through, 1) == -N_ERR_WRONG_KIND, "no addresses of ordinary memory");
    n_handle_close(plain);
    if (low >= 0) n_handle_close(low);
    if (ram) n_memory_release(ram, 8192);
    if (mem >= 0) n_handle_close(mem);

    if (regs) n_memory_release((void *)regs, 1 << 20);
    if (bar >= 0) n_handle_close(bar);
    n_handle_close(pci);
    n_handle back = n_pci_claim(e->bus, e->dev, e->func);
    CHECK(back >= 0, "claim after release: %s", n_result_text(back));
    if (back >= 0) {
        CHECK((n_pci_config_read(back, N_PCI_SELF, 4, 2) & 4) == 0, "bus mastering stopped at release");
        n_handle_close(back);
    }
    printf("pcitest: %d failed\n", failed);
    return failed != 0;
}
