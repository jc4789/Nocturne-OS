/* pcitest: drivers in apps (docs/nocturne-native-abi.md §9) on QEMU's "edu" device (-device edu,dma_mask=0xffffffff,
   1234:11e8): the claim, config space, a BAR as a shared object and its views, MSI, and DMA memory
   the device copies through. Booted with appdrivers=any so this app may claim; compiled inside
   Nocturne with tcc. Prints "pcitest: N failed". */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <stdatomic.h>
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
static atomic_int waiter_started;
static n_result waiter_result;
static int interrupt_waiter(void *arg) {
    atomic_store(&waiter_started,1);
    waiter_result=n_pci_interrupt_take((n_handle)(intptr_t)arg,-1);
    return 0;
}
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

static int revoked_reader(void) {
    n_handle view=-1;char signal=0;
    if(n_channel_receive(3,&signal,1,N_CHANNEL_WAIT,&view)!=1 || view<0)return 10;
    void *m;if(n_shared_map(view,4096,N_MEMORY_READ,&m)<0)return 11;
    const volatile uint32_t *p=m;
    if(p[0]!=0x12345678)return 11;
    /* Receiving a view must not confer the issuer's cancellation authority. */
    if(n_shared_revoke(view)!=-N_ERR_DENIED)return 12;
    if(n_channel_send(3,"r",1,-1)!=1)return 13;
    if(n_channel_receive(3,&signal,1,N_CHANNEL_WAIT,NULL)!=1)return 14;
    volatile uint32_t value=p[0]; /* expected access fault after parent revoke */
    return value==0x12345678?15:16;
}
struct map_race {
    n_handle view;size_t bytes;atomic_int ready,go,done;void *result;n_result error;
};
static int mapping_builder(void *arg) {
    struct map_race *r=arg;atomic_store(&r->ready,1);
    while(!atomic_load(&r->go))thrd_yield();
    r->error=n_shared_map(r->view,r->bytes,N_MEMORY_READ,&r->result);
    atomic_store(&r->done,1);return 0;
}
static void construction_cancel_case(void) {
    struct n_sysinfo info={0};
    CHECK(sysinfo(&info)==0,"construction race memory information");
    /* A legal sparse object as large as the actually available RAM gives the
       scheduler work to interleave, without allocating/touching all frames or
       introducing a product/test-only delay or fixed resource ceiling. */
    size_t bytes=(size_t)(info.free_mem&~(uint64_t)4095);
    n_handle root=n_shared_reserve(bytes),view=n_shared_view_access(root,0,bytes,N_CACHE_DEFAULT,N_MEMORY_READ);
    n_handle channel[2]={-1,-1};
    CHECK(root>=0 && view>=0 && n_channel_pair(channel)==0,"construction race resources");
    struct map_race race={.view=view,.bytes=bytes};thrd_t builder;
    int created=thrd_create(&builder,mapping_builder,&race);
    CHECK(created==thrd_success,"construction race builder");
    if(created==thrd_success){
        while(!atomic_load(&race.ready))thrd_yield();
        void *candidate;n_shared_map(root,bytes,N_MEMORY_READ_WRITE,&candidate);
        uint32_t *probe=candidate;
        CHECK(probe!=NULL,"reserve known candidate VA before racing");
        if(probe){probe[0]=0x19384756;CHECK(n_memory_release(probe,bytes)==0,"release candidate VA for the builder");}
        atomic_store(&race.go,1);
        bool observed=false,released=false;
        while(probe && !atomic_load(&race.done)){
            n_result state=n_memory_protect(probe,4096,N_MEMORY_READ);
            if(state==-N_ERR_NOT_READY){
                observed=true;
                n_result release=n_memory_release(probe,bytes);
                /* It may finish between these two syscalls. Either the still
                   constructing range is protected, or a finished map releases. */
                CHECK(release==-N_ERR_NOT_READY || release==0,"release respects constructing or completed map lifetime: %lld",(long long)release);
                released=release==0;break;
            }
            thrd_yield();
        }
        CHECK(observed,"native map yields with a tracked constructing VA");
        CHECK(n_shared_revoke(view)==0,"cancel view while map builds");
        CHECK(!probe || n_channel_send(channel[0],probe,sizeof *probe,-1)==-N_ERR_BAD_ADDRESS,
              "after cancellation returns, even guessed in-flight PTE cannot be copied");
        thrd_join(builder,NULL);
        CHECK(!race.result?race.error==-N_ERR_CLOSED:race.result==probe,
              "builder either closes or had already published the known mapping");
        if(race.result && !released)n_memory_release(race.result,bytes);
    }
    if(channel[0]>=0)n_handle_close(channel[0]);if(channel[1]>=0)n_handle_close(channel[1]);
    if(view>=0)n_handle_close(view);if(root>=0)n_handle_close(root);
}
static void composition_cases(const char *program) {
    const size_t page=4096; /* native x86-64 shared-page granularity */
    n_handle a=n_shared_create(3*page),b=n_shared_reserve(2*page);
    void *am,*joinedm,*bm,*slicem,*exportm,*none;
    n_shared_map(a,3*page,N_MEMORY_READ_WRITE,&am);
    uint32_t *a_map=am;
    CHECK(a>=0 && b>=0 && a_map,"composition backing objects");
    if(a_map){a_map[0]=0x11111111;a_map[2*page/4]=0x33333333;}
    struct n_shared_range ranges[]={
        {.shared=a,.offset=2*page,.bytes=page},
        {.shared=b,.offset=page,.bytes=page},
        {.shared=a,.offset=0,.bytes=page}
    };
    n_handle joined=n_shared_compose(ranges,3,N_MEMORY_READ_WRITE);
    n_shared_map(joined,3*page,N_MEMORY_READ_WRITE,&joinedm);
    uint32_t *mapped=joinedm;
    CHECK(joined>=0 && mapped && mapped[0]==0x33333333 && mapped[page/4]==0 && mapped[2*page/4]==0x11111111,
          "ordered nonadjacent slices and untouched demand page share one VA");
    if(mapped){mapped[0]=0x12345678;mapped[page/4]=0x55555555;}
    n_shared_map(b,2*page,N_MEMORY_READ,&bm);
    uint32_t *b_map=bm;
    CHECK(a_map && b_map && a_map[2*page/4]==0x12345678 && b_map[page/4]==0x55555555,
          "composition aliases backing pages, including first-touch allocation");
    if(a_map)n_memory_release(a_map,3*page);
    if(b_map)n_memory_release(b_map,2*page);
    if(a>=0)n_handle_close(a);if(b>=0)n_handle_close(b);
    CHECK(mapped && mapped[0]==0x12345678 && mapped[page/4]==0x55555555,"source handle close retains backing");
    n_handle slice=n_shared_view(joined,page,2*page,N_CACHE_DEFAULT);
    n_shared_map(slice,2*page,N_MEMORY_READ,&slicem);
    const uint32_t *slice_map=slicem;
    CHECK(slice>=0 && slice_map && slice_map[0]==0x55555555 && slice_map[page/4]==0x11111111,
          "view of composite crosses segment boundaries");
    struct n_shared_range invalid[]={{.shared=joined,.bytes=page},{.shared=-1,.bytes=page}};
    CHECK(n_shared_compose(invalid,2,N_MEMORY_READ)==-N_ERR_NO_HANDLE,"invalid later segment rolls back composition");
    CHECK(n_shared_compose(ranges,SIZE_MAX,N_MEMORY_READ)==-N_ERR_TOO_LARGE,"composition metadata multiplication cannot overflow");
    n_handle ro=n_shared_view_access(joined,0,3*page,N_CACHE_DEFAULT,N_MEMORY_READ);
    struct n_shared_range readonly={.shared=ro,.bytes=3*page};
    CHECK(n_shared_compose(&readonly,1,N_MEMORY_READ_WRITE)==-N_ERR_DENIED,"composition cannot upgrade read-only source");
    n_handle nested=n_shared_compose(&readonly,1,N_MEMORY_READ);
    CHECK(nested>=0 && n_shared_map(nested,3*page,N_MEMORY_READ_WRITE,&none)<0,"read-only composite maximum is immutable");
    CHECK(n_shared_view_access(nested,0,page,N_CACHE_DEFAULT,N_MEMORY_READ_WRITE)==-N_ERR_DENIED,
          "view cannot upgrade read-only composite");
    n_handle derived=n_shared_view(nested,0,page,N_CACHE_DEFAULT);
    struct n_shared_range nested_range={.shared=derived,.bytes=page};
    n_handle exported=n_shared_compose(&nested_range,1,N_MEMORY_READ);
    n_shared_map(exported,page,N_MEMORY_READ,&exportm);
    const uint32_t *export_map=exportm;
    CHECK(export_map && export_map[0]==0x12345678,"nested composition and view retain original order");
    if(nested>=0)n_handle_close(nested);if(derived>=0)n_handle_close(derived);
    n_handle pair[2]={-1,-1},reader=-1;
    if(exported>=0 && n_channel_pair(pair)==0){
        const char *args[]={program,"revoked-reader"};int32_t handles[]={0,1,2,pair[1]};
        struct n_launch launch={.arguments=args,.argument_count=2,.handles=handles,.handle_count=4};
        reader=n_app_launch(program,&launch);n_handle_close(pair[1]);pair[1]=-1;
        CHECK(reader>=0,"launch nested composition reader");
        if(reader>=0){
            char ready=0;
            CHECK(n_channel_send(pair[0],"m",1,exported)==1 &&
                  n_channel_receive(pair[0],&ready,1,N_CHANNEL_WAIT,NULL)==1 && ready=='r',"child reads nested composition before source revoke");
        }
    }else CHECK(0,"nested composition reader channel");
    CHECK(n_shared_revoke(ro)==0,"source view issuer cancels nested compositions");
    if(reader>=0){
        int status=0;CHECK(n_channel_send(pair[0],"x",1,-1)==1,"release nested reader after cancellation");
        n_result result=n_app_result(reader,2000,&status);
        CHECK(result==0 && status==128+14,"nested existing map faults after source revoke: result=%lld status=%d",(long long)result,status);
        if(result<0)n_app_stop(reader,0);n_handle_close(reader);
    }
    if(pair[0]>=0)n_handle_close(pair[0]);
    CHECK(n_shared_map(exported,page,N_MEMORY_READ,&none)<0 &&n_shared_view(exported,0,page,N_CACHE_DEFAULT)==-N_ERR_CLOSED,
          "nested composite cannot escape revoked dependency");
    CHECK(!export_map || n_memory_protect((void *)export_map,page,N_MEMORY_READ)<0,"nested parent mapping also lost its PTE");
    CHECK(mapped && mapped[0]==0x12345678,"source-view cancellation preserves independent composite owner");
    if(export_map)n_memory_release((void *)export_map,page);
    if(slice_map)n_memory_release((void *)slice_map,2*page);
    if(mapped)n_memory_release(mapped,3*page);
    if(exported>=0)n_handle_close(exported);if(ro>=0)n_handle_close(ro);
    if(slice>=0)n_handle_close(slice);if(joined>=0)n_handle_close(joined);
}
/* QEMU's standard VGA (1234:1111, -vga std) draws the boot screen through its prefetchable BAR 0.
   The screen's pages are write-combined for as long as the system runs, and any device page has
   one memory type while something maps it. A driver may hold the screen while it rebuilds how
   the CPU reaches it; the claim's end gives back what it still holds. */
static int find_vga(const struct n_pciinfo *list, int n) {
    for (int i = 0; i < n; i++)
        if (list[i].vendor == 0x1234 && list[i].device == 0x1111) return i;
    return -1;
}

static void screen_cases(const struct n_pciinfo *list, int n) {
    int at = find_vga(list, n);
    if (at < 0) {
        printf("FAIL: no standard VGA (run QEMU with -vga std)\n");
        failed++;
        return;
    }
    n_handle vga = n_pci_claim(list[at].bus, list[at].dev, list[at].func);
    CHECK(vga >= 0, "claim the VGA: %s", n_result_text(vga));
    if (vga < 0) return;
    struct n_pci_function info;
    CHECK(n_pci_info(vga, &info) == 0, "VGA info");
    CHECK(info.framebuffer_bar == 0 && (info.bar[0].flags & N_PCI_BAR_PREFETCH), "the screen is in prefetchable BAR 0: %u %x",
          info.framebuffer_bar, info.bar[0].flags);
    uint64_t screen = (info.framebuffer_address - info.bar[0].address) & ~4095ull;
    uint64_t past = (info.framebuffer_address - info.bar[0].address + info.framebuffer_size + 4095) & ~4095ull;
    n_handle bar = n_pci_bar(vga, 0);
    CHECK(bar >= 0, "VGA BAR: %s", n_result_text(bar));
    if (bar >= 0 && info.framebuffer_bar == 0) {
        void *none;
        n_handle uc = n_shared_view(bar, screen, 4096, N_CACHE_UNCACHED);
        CHECK(uc >= 0 && n_shared_map(uc, 4096, N_MEMORY_READ, &none) == -N_ERR_DENIED,
              "an uncached mapping of the write-combined screen is refused");
        n_handle wc = n_shared_view(bar, screen, 4096, N_CACHE_WRITE_COMBINE);
        void *wc_map = NULL;
        n_result mapped = wc >= 0 ? n_shared_map(wc, 4096, N_MEMORY_READ, &wc_map) : wc;
        CHECK(mapped >= 0, "the screen maps write-combined: %s", n_result_text(mapped));
        CHECK(n_shared_map(bar, (size_t)info.bar[0].size, N_MEMORY_READ, &none) == -N_ERR_DENIED,
              "the whole BAR uncached would alias the screen");
        if (past + 4096 <= info.bar[0].size) {
            n_handle a = n_shared_view(bar, past, 4096, N_CACHE_UNCACHED);
            n_handle b = n_shared_view(bar, past, 4096, N_CACHE_WRITE_COMBINE);
            void *a_map = NULL, *a_again = NULL, *b_map = NULL;
            mapped = a >= 0 ? n_shared_map(a, 4096, N_MEMORY_READ_WRITE, &a_map) : a;
            CHECK(mapped >= 0, "uncached mapping past the screen: %s", n_result_text(mapped));
            if (a >= 0) n_shared_map(a, 4096, N_MEMORY_READ, &a_again);
            CHECK(a_again != NULL, "a second mapping of the same type");
            CHECK(b >= 0 && n_shared_map(b, 4096, N_MEMORY_READ, &none) == -N_ERR_DENIED,
                  "a write-combined alias of an uncached device page is refused");
            if (a_map) n_memory_release(a_map, 4096);
            CHECK(b >= 0 && n_shared_map(b, 4096, N_MEMORY_READ, &none) < 0, "one mapping of the other type is still there");
            if (a_again) n_memory_release(a_again, 4096);
            mapped = b >= 0 ? n_shared_map(b, 4096, N_MEMORY_READ, &b_map) : b;
            CHECK(mapped >= 0, "once no uncached mapping is left the page takes write-combining: %s", n_result_text(mapped));
            if (b_map) n_memory_release(b_map, 4096);
            if (a >= 0) n_handle_close(a);
            if (b >= 0) n_handle_close(b);
        } else {
            printf("FAIL: no BAR page past the screen (%llu bytes)\n", (unsigned long long)info.bar[0].size);
            failed++;
        }
        if (wc_map) n_memory_release(wc_map, 4096);
        if (uc >= 0) n_handle_close(uc);
        if (wc >= 0) n_handle_close(wc);
    }
    if (bar >= 0) n_handle_close(bar);
    CHECK(n_pci_hold_screen(vga, true) == 0 && n_pci_hold_screen(vga, true) == 0, "holds of the screen nest");
    CHECK(n_pci_hold_screen(vga, false) == 0 && n_pci_hold_screen(vga, false) == 0, "and are released one by one");
    CHECK(n_pci_hold_screen(vga, false) == -N_ERR_INVALID, "a release without a hold");
    CHECK(n_pci_hold_screen(vga, true) == 0, "a hold the claim's end gives back");
    n_handle_close(vga);
}

/* A driver that holds the screen and never gives it back (run by stuck_holder_cases). */
static int stuck_holder(void) {
    struct n_pciinfo list[128];
    int n = pcilist(list, 128), at = find_vga(list, n);
    n_handle vga = at < 0 ? -1 : n_pci_claim(list[at].bus, list[at].dev, list[at].func);
    if (vga < 0 || n_pci_hold_screen(vga, true) != 0) return 2;
    for (;;) n_sleep_ms(60000);
}

/* Booted with fbhold=<seconds>: the system stops a driver that holds the screen that long, and
   the end of its claim gives the screen back. Before the limit the hold stays, however long. */
static void stuck_holder_cases(const char *program, const struct n_pciinfo *list, int n, uint64_t seconds) {
    int at = find_vga(list, n);
    if (at < 0) {
        printf("FAIL: no standard VGA (run QEMU with -vga std)\n");
        failed++;
        return;
    }
    const char *args[] = {program, "stuck-holder"};
    int32_t handles[3];
    for (int i = 0; i < 3; i++) handles[i] = n_handle_kind(i) >= 0 ? i : -1; /* the console it has */
    struct n_launch launch = {.arguments = args, .argument_count = 2, .handles = handles, .handle_count = 3};
    n_handle holder = n_app_launch(program, &launch);
    CHECK(holder >= 0, "launch the stuck holder: %s", n_result_text(holder));
    if (holder < 0) return;
    int status = 0;
    n_result early = n_app_result(holder, (int64_t)seconds * 500, &status);
    CHECK(early == -N_ERR_TIMEOUT, "the holder keeps the screen before the limit: %s status %d", n_result_text(early),
          status);
    n_handle busy = n_pci_claim(list[at].bus, list[at].dev, list[at].func);
    CHECK(busy == -N_ERR_BUSY, "the VGA is still the holder's: %s", n_result_text(busy));
    if (busy >= 0) n_handle_close(busy);
    n_result ended = n_app_result(holder, (int64_t)seconds * 1000 + 10000, &status);
    CHECK(ended == 0 && status != 0 && status != 2, "the system stopped the holder past the limit: %s status %d",
          n_result_text(ended), status);
    if (ended < 0) n_app_stop(holder, 0);
    n_handle_close(holder);
    n_handle vga = n_pci_claim(list[at].bus, list[at].dev, list[at].func);
    CHECK(vga >= 0, "the VGA can be claimed again: %s", n_result_text(vga));
    if (vga < 0) return;
    CHECK(n_pci_hold_screen(vga, true) == 0 && n_pci_hold_screen(vga, false) == 0, "and held and given back");
    n_handle_close(vga);
}

/* A virtio function's common configuration (virtio 1.x, vendor capability type 1): its BAR and
   the offset in it, false when it has none. */
static int virtio_common(n_handle pci, uint32_t *bar, uint32_t *offset) {
    for (n_result at = n_pci_config_read(pci, N_PCI_SELF, 0x34, 1) & 0xFC, hops = 0; at && hops < 48; hops++) {
        n_result head = n_pci_config_read(pci, N_PCI_SELF, (uint32_t)at, 4);
        if (head < 0) return 0;
        if ((head & 0xFF) == 0x09 && ((head >> 24) & 0xFF) == 1) {
            *bar = (uint32_t)n_pci_config_read(pci, N_PCI_SELF, (uint32_t)at + 4, 1);
            *offset = (uint32_t)n_pci_config_read(pci, N_PCI_SELF, (uint32_t)at + 8, 4);
            return 1;
        }
        at = (head >> 8) & 0xFC;
    }
    return 0;
}

/* The virtio device status byte (common configuration + 0x14) through the BAR; -1 when it cannot
   be reached. A write of `set` first when set >= 0. */
static int virtio_status(n_handle pci, int set) {
    uint32_t bar, offset;
    struct n_pci_function info;
    if (!virtio_common(pci, &bar, &offset) || bar > 5 || n_pci_info(pci, &info) != 0 || !info.bar[bar].size) return -1;
    n_handle h = n_pci_bar(pci, bar);
    void *whole = NULL;
    if (h >= 0) n_shared_map(h, info.bar[bar].size, N_MEMORY_READ_WRITE, &whole);
    volatile uint8_t *m = whole;
    int status = -1;
    if (m) {
        if (set >= 0) m[offset + 0x14] = (uint8_t)set;
        status = m[offset + 0x14];
        n_memory_release((void *)m, info.bar[bar].size);
    }
    if (h >= 0) n_handle_close(h);
    return status;
}

/* pcitest flr: a driver that ends with bus mastering on leaves a running device; one with
   Function Level Reset is reset when its claim ends (QEMU: -device virtio-rng-pci behind a
   pcie-root-port, which gives it FLR). Its virtio device status reads 0 again, its BARs read as
   they were and bus mastering is off. */
static void reset_cases(const struct n_pciinfo *list, int n) {
    int at = -1;
    for (int i = 0; i < n && at < 0; i++)
        if (list[i].vendor == 0x1af4 && list[i].cls != 0x06) at = i;
    if (at < 0) {
        printf("FAIL: no virtio function (run QEMU with -device virtio-rng-pci behind a pcie-root-port)\n");
        failed++;
        return;
    }
    const struct n_pciinfo *v = &list[at];
    n_handle pci = n_pci_claim(v->bus, v->dev, v->func);
    CHECK(pci >= 0, "claim the virtio function %02x:%02x.%u: %s", v->bus, v->dev, v->func, n_result_text(pci));
    if (pci < 0) return;
    n_result bars[6], cmd = n_pci_config_read(pci, N_PCI_SELF, 4, 2);
    for (int i = 0; i < 6; i++) bars[i] = n_pci_config_read(pci, N_PCI_SELF, 0x10 + 4 * (uint32_t)i, 4);
    CHECK(cmd >= 0 && n_pci_config_write(pci, 4, 2, (uint32_t)cmd | 6) == 0, "command: memory and bus master");
    CHECK(virtio_status(pci, 1) == 1, "the virtio device status takes ACKNOWLEDGE");
    n_handle_close(pci); /* bus mastering still on: the driver did not stop it */
    pci = n_pci_claim(v->bus, v->dev, v->func);
    CHECK(pci >= 0, "claim it again: %s", n_result_text(pci));
    if (pci < 0) return;
    cmd = n_pci_config_read(pci, N_PCI_SELF, 4, 2);
    CHECK(cmd >= 0 && !(cmd & 4), "bus mastering is off after the claim ended: command %llx", (long long)cmd);
    for (int i = 0; i < 6; i++)
        CHECK(n_pci_config_read(pci, N_PCI_SELF, 0x10 + 4 * (uint32_t)i, 4) == bars[i], "BAR %d is as it was", i);
    CHECK(cmd >= 0 && n_pci_config_write(pci, 4, 2, (uint32_t)cmd | 2) == 0, "command: memory");
    int status = virtio_status(pci, -1);
    CHECK(status == 0, "the reset cleared the virtio device status: %d", status);
    n_handle_close(pci);
}

int main(int argc,char **argv) {
    if(argc>1 && !strcmp(argv[1],"revoked-reader"))return revoked_reader();
    if(argc>1 && !strcmp(argv[1],"stuck-holder"))return stuck_holder();
    if(argc>1 && !strcmp(argv[1],"flr")){
        struct n_pciinfo list[128];
        reset_cases(list, pcilist(list, 128));
        printf("pcitest flr: %d failed\n", failed);
        return failed != 0;
    }
    /* pcitest fbhold=<seconds>: only the stopped-driver case, for a boot with that option */
    if(argc>1 && !strncmp(argv[1],"fbhold=",7)){
        struct n_pciinfo list[128];
        int n = pcilist(list, 128);
        uint64_t seconds = strtoull(argv[1] + 7, NULL, 10);
        if (seconds) stuck_holder_cases(argv[0], list, n, seconds);
        else { printf("FAIL: fbhold=<seconds> needs a number above 0\n"); failed++; }
        printf("pcitest fbhold: %d failed\n", failed);
        return failed != 0;
    }
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
    printf("pcitest: isolated by an IOMMU: %s\n", info.flags & N_PCI_ISOLATED ? "yes" : "no");

    n_result id = n_pci_config_read(pci, N_PCI_SELF, 0, 4);
    CHECK(id == 0x11e81234, "config id %llx", (long long)id);
    CHECK(n_pci_config_read(pci, N_PCI_SELF, 2, 2) == 0x11e8, "config 16-bit read");
    CHECK(n_pci_config_read(pci, N_PCI_SELF, 1, 2) == -N_ERR_INVALID, "unaligned read is refused");
    CHECK(n_pci_config_write(pci, 0x10, 4, 0) == -N_ERR_DENIED, "BAR write is refused");
    n_result bar0 = n_pci_config_read(pci, N_PCI_SELF, 0x10, 4);
    CHECK(bar0 > 0 && n_pci_config_write(pci, 0x10, 4, (uint32_t)bar0) == 0, "the BAR's own address can be put back");
    CHECK(n_pci_config_write(pci, 0x10, 4, (uint32_t)bar0 ^ 0x100000u) == -N_ERR_DENIED, "moving the BAR is refused");
    n_result cmd = n_pci_config_read(pci, N_PCI_SELF, 4, 2);
    CHECK(cmd >= 0 && n_pci_config_write(pci, 4, 2, (uint32_t)cmd | 6) == 0, "command: memory and bus master");
    CHECK((n_pci_config_read(pci, N_PCI_SELF, 4, 2) & 6) == 6, "command reads back");
    /* The bits that route, power or reset the function stay the system's: a write changing one is
       refused, a read-modify-write that keeps them goes through at its own width. */
    cmd = n_pci_config_read(pci, N_PCI_SELF, 4, 2);
    CHECK(n_pci_config_write(pci, 4, 2, (uint32_t)cmd ^ 1) == -N_ERR_DENIED, "command: I/O decode is the system's");
    CHECK(n_pci_config_write(pci, 4, 2, (uint32_t)cmd ^ 0x400) == -N_ERR_DENIED, "command: INTx disable is the system's");
    CHECK(n_pci_config_write(pci, 4, 2, (uint32_t)cmd) == 0, "command: unchanged guarded bits pass");
    CHECK(n_pci_config_write(pci, 0x0F, 1, 0x40) == -N_ERR_DENIED, "BIST cannot be started");
    n_result line = n_pci_config_read(pci, N_PCI_SELF, 0x3C, 1);
    CHECK(line >= 0 && n_pci_config_write(pci, 0x3C, 1, (uint32_t)line ^ 0xFF) == -N_ERR_DENIED, "interrupt line is the system's");
    CHECK(line >= 0 && n_pci_config_write(pci, 0x3C, 1, (uint32_t)line) == 0, "interrupt line written as it is");
    if (n_pci_config_read(pci, N_PCI_SELF, 6, 2) & 0x10) /* a capability list: power management, if any */
        for (n_result at = n_pci_config_read(pci, N_PCI_SELF, 0x34, 1) & 0xFC, hops = 0; at && hops < 48; hops++) {
            n_result head = n_pci_config_read(pci, N_PCI_SELF, (uint32_t)at, 2);
            if (head < 0) break;
            if ((head & 0xFF) == 0x01) {
                n_result pmcsr = n_pci_config_read(pci, N_PCI_SELF, (uint32_t)at + 4, 2);
                CHECK(pmcsr >= 0 && n_pci_config_write(pci, (uint32_t)at + 4, 2, ((uint32_t)pmcsr & ~3u) | 3) == -N_ERR_DENIED,
                      "the function cannot be put into D3");
                break;
            }
            at = (head >> 8) & 0xFC;
        }
    for (int i = 0; i < n; i++)
        if (list[i].cls == 0x06) {
            n_handle bridge = n_pci_claim(list[i].bus, list[i].dev, list[i].func);
            CHECK(bridge == -N_ERR_DENIED, "bridge %02x:%02x.%u cannot be claimed: %s", list[i].bus, list[i].dev,
                  list[i].func, n_result_text(bridge));
            if (bridge >= 0) n_handle_close(bridge);
            break;
        }
    CHECK(n_pci_hold_screen(pci, true) == -N_ERR_NOT_FOUND, "edu has no screen to hold");
    /* a function on bus 0 that is neither this device nor a bridge above it */
    for (int i = 0; i < n; i++)
        if (i != at && list[i].bus == e->bus && list[i].dev != e->dev && list[i].cls != 0x06) {
            n_result r = n_pci_config_read(pci, N_PCI_TARGET(list[i].bus, list[i].dev, list[i].func), 0, 4);
            CHECK(r == -N_ERR_DENIED, "unrelated config read: %lld", (long long)r);
            break;
        }
    /* behind a root port (QEMU -M q35 with edu on a pcie-root-port at function 1 of a
       multifunction slot): the port and function 0 of its device are readable */
    if (info.upstream != N_PCI_NONE) {
        uint32_t up = info.upstream;
        n_result r = n_pci_config_read(pci, up, 0x18, 4);
        CHECK(r >= 0 && ((r >> 8) & 0xFF) == e->bus, "upstream bridge read: %lld", (long long)r);
        r = n_pci_config_read(pci, up & ~7u, 0, 4);
        CHECK(r >= 0 && (r & 0xFFFF) != 0xFFFF, "function 0 beside the upstream bridge: %lld", (long long)r);
    }
    /* a host bridge names the chipset: readable */
    for (int i = 0; i < n; i++)
        if (list[i].cls == 0x06 && list[i].subcls == 0x00) {
            n_result r = n_pci_config_read(pci, N_PCI_TARGET(list[i].bus, list[i].dev, list[i].func), 0, 4);
            CHECK(r == (n_result)((uint32_t)list[i].device << 16 | list[i].vendor), "host bridge config read: %lld",
                  (long long)r);
            break;
        }

    n_handle bar = n_pci_bar(pci, 0);
    CHECK(bar >= 0 && n_handle_kind(bar) == N_KIND_SHARED, "bar: %s", n_result_text(bar));
    void *bar_map = NULL;
    if (bar >= 0) n_shared_map(bar, 1 << 20, N_MEMORY_READ_WRITE, &bar_map);
    regs = bar_map;
    CHECK(regs != NULL, "map bar");
    if (regs) {
        CHECK(rd32(EDU_IDENT) >> 16 == 0x0100 && (rd32(EDU_IDENT) & 0xFF) == 0xed, "ident %08x", rd32(EDU_IDENT));
        wr32(EDU_LIVENESS, 0x12345678);
        CHECK(rd32(EDU_LIVENESS) == ~0x12345678u, "liveness %08x", rd32(EDU_LIVENESS));
    }
    /* Two disjoint BAR pages plus RAM: per-segment types and claim lifetime
       must survive composition. The register page intentionally comes second. */
    n_handle mixed_ram=n_shared_create(4096);
    struct n_shared_range mixed_ranges[]={
        {.shared=bar,.offset=8192,.bytes=4096},
        {.shared=bar,.offset=0,.bytes=4096},
        {.shared=mixed_ram,.offset=0,.bytes=4096}
    };
    n_handle mixed=n_shared_compose(mixed_ranges,3,N_MEMORY_READ_WRITE);
    void *mixed_whole=NULL;n_shared_map(mixed,3*4096,N_MEMORY_READ_WRITE,&mixed_whole);
    volatile uint32_t *mixed_map=mixed_whole;
    CHECK(mixed>=0 && mixed_map && regs && mixed_map[4096/4]==rd32(EDU_IDENT),"noncontiguous BAR slices compose with RAM");
    if(mixed_map)mixed_map[2*4096/4]=0x51525354;
    CHECK(n_shared_view(mixed,0,3*4096,N_CACHE_WRITE_BACK)==-N_ERR_DENIED,"explicit cache cannot collapse heterogeneous segments");
    n_handle mixed_view=n_shared_view(mixed,4096,2*4096,N_CACHE_DEFAULT);
    CHECK(mixed_view>=0,"default composite view preserves heterogeneous cache attributes");
    n_handle bar_slice=n_shared_view(mixed,4096,4096,N_CACHE_UNCACHED);
    n_handle ram_slice=n_shared_view(mixed_view,4096,4096,N_CACHE_WRITE_BACK);
    void *bar_slice_whole=NULL,*ram_slice_whole=NULL;
    n_shared_map(bar_slice,4096,N_MEMORY_READ,&bar_slice_whole);
    n_shared_map(ram_slice,4096,N_MEMORY_READ,&ram_slice_whole);
    const volatile uint32_t *bar_slice_map=bar_slice_whole;
    const uint32_t *ram_slice_map=ram_slice_whole;
    CHECK(bar_slice>=0 && bar_slice_map && regs && bar_slice_map[0]==rd32(EDU_IDENT),"explicit UC on only the BAR portion of a mixed object");
    CHECK(ram_slice>=0 && ram_slice_map && ram_slice_map[0]==0x51525354,"nested partial view selects only WB RAM, not preceding UC BAR");
    if(bar_slice_map)n_memory_release((void *)bar_slice_map,4096);
    if(ram_slice_map)n_memory_release((void *)ram_slice_map,4096);
    if(bar_slice>=0)n_handle_close(bar_slice);if(ram_slice>=0)n_handle_close(ram_slice);
    if(mixed_view>=0)n_handle_close(mixed_view);
    if(mixed_ram>=0)n_handle_close(mixed_ram);
    n_handle wc = n_shared_view(bar, 0, 4096, N_CACHE_WRITE_COMBINE);
    CHECK(wc == -N_ERR_DENIED, "non-prefetchable register BAR refuses write-combining: %s", n_result_text(wc));
    n_handle wb = n_shared_view(bar, 0, 4096, N_CACHE_WRITE_BACK);
    CHECK(wb == -N_ERR_DENIED, "write-back view of device memory: %s", n_result_text(wb));
    CHECK(n_shared_view(bar, 100, 4096, N_CACHE_UNCACHED) == -N_ERR_INVALID, "unaligned view");
    if (wc >= 0) {
        void *wc_map = NULL;
        n_shared_map(wc, 4096, N_MEMORY_READ_WRITE, &wc_map);
        volatile uint32_t *buf = wc_map;
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
    if(vectors>=1){
        thrd_t waiter;
        int created=thrd_create(&waiter,interrupt_waiter,(void *)(intptr_t)pci);
        CHECK(created==thrd_success,"interrupt waiter thread");
        if(created==thrd_success){
            while(!atomic_load(&waiter_started))thrd_yield();
            n_sleep_ms(1); /* let the native take enter its blocking continuation */
            CHECK(n_pci_interrupts(pci,0)==0,"disable interrupts while take waits");
            thrd_join(waiter,NULL);
            CHECK(waiter_result==-N_ERR_NOT_READY,"disabled IRQ wakes infinite take: %s",n_result_text(waiter_result));
            vectors=n_pci_interrupts(pci,4);
            CHECK(vectors>=1,"interrupt re-enable");
        }
    }

    n_handle mem = n_dma_create(pci, 8192, N_DMA_CONTIGUOUS | N_DMA_32BIT);
    CHECK(mem >= 0, "dma: %s", n_result_text(mem));
    uint64_t pages[2] = {0, 0};
    CHECK(mem >= 0 && n_dma_addresses(mem, 0, pages, 4) == 2, "dma addresses");
    CHECK(pages[1] == pages[0] + 4096 && pages[0] && pages[0] < (1ull << 32), "contiguous below 4 GiB: %llx %llx",
          (unsigned long long)pages[0], (unsigned long long)pages[1]);
    void *mem_map = NULL;
    if (mem >= 0) n_shared_map(mem, 8192, N_MEMORY_READ_WRITE, &mem_map);
    uint8_t *ram = mem_map;
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
            /* Isolated, the function's DMA reaches nothing but the claim's DMA objects: a write
               aimed at its own liveness register (which keeps the complement of what is
               written) through the bus does not arrive. */
            if (info.flags & N_PCI_ISOLATED) {
                wr32(EDU_LIVENESS, 0x11111111);
                memset(ram, 0x22, 4);
                CHECK(dma(pci, pages[0], 4, 0), "a word to the device");
                dma(pci, info.bar[0].address + EDU_LIVENESS, 4, 1);
                CHECK(rd32(EDU_LIVENESS) == ~0x11111111u, "DMA outside the claim's memory arrived: %08x",
                      rd32(EDU_LIVENESS));
            }
        }
    }
    /* One memory type per page: the write-back mapping above fixes this object's type while it
       stays, and once it is gone an uncached view sets the type for the whole object. */
    CHECK(mem < 0 || n_shared_view(mem, 4096, 4096, N_CACHE_UNCACHED) == -N_ERR_DENIED,
          "a mapped write-back dma object keeps its type");
    if (ram) n_memory_release(ram, 8192);
    ram = NULL;
    n_handle low = mem >= 0 ? n_shared_view(mem, 4096, 4096, N_CACHE_UNCACHED) : -1;
    CHECK(low >= 0, "uncached view of dma memory: %s", n_result_text(low));
    CHECK(mem < 0 || n_shared_view(mem, 0, 4096, N_CACHE_WRITE_BACK) == -N_ERR_DENIED,
          "no write-back view of uncached dma memory");
    uint64_t through = 0;
    CHECK(low >= 0 && n_dma_addresses(low, 0, &through, 1) == 1 && through == pages[1], "addresses through a view");
    void *low_map = NULL;
    if (low >= 0) n_shared_map(low, 4096, N_MEMORY_READ_WRITE, &low_map);
    volatile uint8_t *uc = low_map;
    CHECK(uc != NULL, "map the uncached view");
    if (uc) {
        CHECK(uc[0] == (uint8_t)3 || !regs || vectors < 1, "the uncached view sees the copy: %u", uc[0]);
        n_memory_release((void *)uc, 4096);
    }
    n_handle plain = n_shared_create(4096);
    void *plain_whole=NULL,*ro_whole=NULL,*child_whole=NULL,*refused;
    n_shared_map(plain,4096,N_MEMORY_READ_WRITE,&plain_whole);
    uint32_t *plain_map=plain_whole;
    if(plain_map)plain_map[0]=0x12345678;
    n_handle ro=n_shared_view_access(plain,0,4096,N_CACHE_DEFAULT,N_MEMORY_READ);
    CHECK(ro>=0,"read-only maximum view");
    CHECK(n_shared_map(ro,4096,N_MEMORY_READ_WRITE,&refused)<0,"read-only view refuses writable map");
    n_shared_map(ro,4096,N_MEMORY_READ,&ro_whole);
    const uint32_t *ro_map=ro_whole;
    CHECK(ro_map && ro_map[0]==0x12345678,"read-only view maps original bytes");
    CHECK(!ro_map || n_memory_protect((void *)ro_map,4096,N_MEMORY_READ_WRITE)==-N_ERR_DENIED,"read-only page cannot be upgraded");
    CHECK(n_shared_view_access(ro,0,4096,N_CACHE_DEFAULT,N_MEMORY_READ_WRITE)==-N_ERR_DENIED,"derived view cannot enlarge maximum access");
    n_handle child=n_shared_view(ro,0,4096,N_CACHE_DEFAULT);
    n_shared_map(child,4096,N_MEMORY_READ,&child_whole);
    const uint32_t *child_map=child_whole;
    CHECK(child>=0 && child_map && child_map[0]==0x12345678,"derived read-only view");
    n_handle channel[2]={-1,-1},reader=-1;
    if(n_channel_pair(channel)==0){
        const char *args[]={argv[0],"revoked-reader"};
        int32_t handles[]={0,1,2,channel[1]};
        struct n_launch launch={.arguments=args,.argument_count=2,.handles=handles,.handle_count=4};
        reader=n_app_launch(argv[0],&launch);
        CHECK(reader>=0,"launch independent mapping reader");
        n_handle_close(channel[1]);channel[1]=-1;
        if(reader>=0){
            char ready=0;
            CHECK(n_channel_send(channel[0],"m",1,child)==1,"send descendant view to child app");
            CHECK(n_channel_receive(channel[0],&ready,1,N_CHANNEL_WAIT,NULL)==1 && ready=='r',"child mapped and read before revoke");
        }
    }else CHECK(0,"mapping reader channel");
    CHECK(n_shared_revoke(ro)==0,"issuer revokes exported view");
    if(reader>=0){
        int status=0;
        CHECK(n_channel_send(channel[0],"x",1,-1)==1,"release child to read revoked mapping");
        n_result result=n_app_result(reader,2000,&status);
        CHECK(result==0 && status==128+14,"revoked existing mapping faults in child: result=%lld status=%d",(long long)result,status);
        if(result<0)n_app_stop(reader,0);
        n_handle_close(reader);
    }
    if(channel[0]>=0)n_handle_close(channel[0]);
    CHECK(n_shared_map(child,4096,N_MEMORY_READ,&refused)<0,"revoked child cannot remap");
    CHECK(n_shared_view(child,0,4096,N_CACHE_DEFAULT)==-N_ERR_CLOSED,"revoked descendant cannot escape through view");
    CHECK(!child_map || n_memory_protect((void *)child_map,4096,N_MEMORY_READ)<0,"existing descendant mapping lost its PTE");
    CHECK(plain_map && plain_map[0]==0x12345678,"independent owner mapping survives view revoke");
    if(ro_map)n_memory_release((void *)ro_map,4096);
    if(child_map)n_memory_release((void *)child_map,4096);
    if(plain_map)n_memory_release(plain_map,4096);
    if(child>=0)n_handle_close(child);
    if(ro>=0)n_handle_close(ro);
    CHECK(n_shared_view(plain, 0, 4096, N_CACHE_UNCACHED) == -N_ERR_DENIED, "ordinary memory keeps its caching");
    CHECK(n_dma_addresses(plain, 0, &through, 1) == -N_ERR_WRONG_KIND, "no addresses of ordinary memory");
    n_handle_close(plain);
    composition_cases(argv[0]);
    construction_cancel_case();
    if (low >= 0) n_handle_close(low);
    if (ram) n_memory_release(ram, 8192);
    if (mem >= 0) n_handle_close(mem);

    /* The BAR mapping outlives the claim: when the last handle goes, the device pages leave and the
       range reads all-ones, as a function gone from the bus. A driver stops the function's bus
       mastering before it lets go. */
    if (bar >= 0) n_handle_close(bar);
    cmd = n_pci_config_read(pci, N_PCI_SELF, 4, 2);
    CHECK(cmd >= 0 && n_pci_config_write(pci, 4, 2, (uint32_t)cmd & ~4u) == 0, "bus mastering off before release");
    n_handle_close(pci);
    if(mixed_map){
        CHECK(mixed_map[0]==0xFFFFFFFFu && mixed_map[4096/4]==0xFFFFFFFFu,"all composed BAR pages lose their claim");
        CHECK(mixed_map[2*4096/4]==0x51525354,"claim loss does not destroy unrelated composed RAM");
        n_memory_release((void *)mixed_map,3*4096);
    }
    void *revoked;
    CHECK(n_shared_map(mixed,3*4096,N_MEMORY_READ,&revoked)<0,"composed revoked BAR cannot be remapped");
    if(mixed>=0)n_handle_close(mixed);
    if (regs) {
        CHECK(rd32(EDU_IDENT) == 0xFFFFFFFFu, "the BAR mapping is revoked with the claim: %08x", rd32(EDU_IDENT));
        n_memory_release((void *)regs, 1 << 20);
    }
    n_handle back = n_pci_claim(e->bus, e->dev, e->func);
    CHECK(back >= 0, "claim after release: %s", n_result_text(back));
    if (back >= 0) {
        CHECK((n_pci_config_read(back, N_PCI_SELF, 4, 2) & 4) == 0, "bus mastering stopped at release");
        /* A driver that ends with bus mastering on is taken as crashed: the system stops the
           function and keeps the DMA memory it had out of use (the system log says so). */
        cmd = n_pci_config_read(back, N_PCI_SELF, 4, 2);
        CHECK(cmd >= 0 && n_pci_config_write(back, 4, 2, (uint32_t)cmd | 6) == 0, "bus mastering on again");
        n_handle left = n_dma_create(back, 4096, 0);
        CHECK(left >= 0, "dma memory of the crashed driver: %s", n_result_text(left));
        n_handle_close(back);
        if (left >= 0) n_handle_close(left);
        back = n_pci_claim(e->bus, e->dev, e->func);
        CHECK(back >= 0 && (n_pci_config_read(back, N_PCI_SELF, 4, 2) & 4) == 0, "a crashed driver's function is stopped");
        if (back >= 0) n_handle_close(back);
    }
    screen_cases(list, n);
    printf("pcitest: %d failed\n", failed);
    return failed != 0;
}
