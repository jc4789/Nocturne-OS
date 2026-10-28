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
#include <limits.h>
#include "nocturne.h"
#include "display.h"

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

/* The claim's part of the ACPI namespace (n_pci_acpi_eval). The VGA sits on QEMU's root bus, so
   its upstream node is the host bridge (PNP0A08 on q35, whose _PRT is a package of packages);
   whether the VGA has a node of its own depends on QEMU's tables, and when it has one its _ADR
   is its device and function. */
static void acpi_cases(n_handle vga, const struct n_pciinfo *at) {
    static uint64_t out[8192]; /* a root bus's _PRT has 128 routes of 5 values */
    struct n_acpi_value *v = (struct n_acpi_value *)out;
    n_result r = n_pci_acpi_eval(vga, N_ACPI_UPSTREAM, "_HID", NULL, 0, NULL, 0, out, sizeof out);
    bool integer_hid = r >= (n_result)sizeof *v && v[0].type == N_ACPI_INTEGER;
    bool string_hid = r >= (n_result)sizeof *v && v[0].type == N_ACPI_STRING && v[0].length == 7 &&
                      v[0].value + 7 <= (uint64_t)r && !memcmp((char *)out + v[0].value, "PNP0A0", 6);
    CHECK((integer_hid && (v[0].value == 0x080AD041 || v[0].value == 0x030AD041)) || string_hid,
          "the host bridge's _HID: %s", r < 0 ? n_result_text(r) : "not a PCI host bridge");
    r = n_pci_acpi_eval(vga, N_ACPI_UPSTREAM, "_PRT", NULL, 0, NULL, 0, out, sizeof out);
    size_t values = r > 0 ? (size_t)r / sizeof *v : 0;
    bool routes = values > 1 && v[0].type == N_ACPI_PACKAGE && v[0].length > 0 && v[0].value == 1 &&
                  v[1].type == N_ACPI_PACKAGE && v[1].length == 4;
    CHECK(routes, "the host bridge's _PRT is a package of 4-element packages: %s",
          r < 0 ? n_result_text(r) : "another layout");
    CHECK(n_pci_acpi_eval(vga, N_ACPI_UPSTREAM, "_PRT", NULL, 0, NULL, 0, out, sizeof *v) == -N_ERR_TOO_LARGE,
          "a result larger than the room is refused");
    /* QEMU's tables give the host bridge no _ROM (its own names, as "NONE", are refused:
       acpi_refused_cases) */
    CHECK(n_pci_acpi_eval(vga, N_ACPI_UPSTREAM, "_ROM", NULL, 0, NULL, 0, out, sizeof out) == -N_ERR_NOT_FOUND,
          "a missing name");
    CHECK(n_pci_acpi_eval(vga, N_ACPI_UPSTREAM, "\\_SB", NULL, 0, NULL, 0, out, sizeof out) == -N_ERR_INVALID &&
              n_pci_acpi_eval(vga, N_ACPI_UPSTREAM, "_SB_.PCI0", NULL, 0, NULL, 0, out, sizeof out) == -N_ERR_INVALID,
          "paths outside the claim's nodes are refused");
    struct n_acpi_value too_many[8] = {{0}};
    CHECK(n_pci_acpi_eval(vga, N_ACPI_UPSTREAM, "_PRT", too_many, 8, NULL, 0, out, sizeof out) == -N_ERR_INVALID,
          "more than 7 arguments");
    struct n_acpi_value outside = {N_ACPI_BUFFER, 16, 0};
    CHECK(n_pci_acpi_eval(vga, N_ACPI_UPSTREAM, "_PRT", &outside, 1, "short", 5, out, sizeof out) == -N_ERR_INVALID,
          "an argument past its data");
    r = n_pci_acpi_eval(vga, N_ACPI_SELF, "_ADR", NULL, 0, NULL, 0, out, sizeof out);
    if (r == -N_ERR_NOT_FOUND) {
        CHECK(n_pci_acpi_eval(vga, N_ACPI_CHILD(0), "_ADR", NULL, 0, NULL, 0, out, sizeof out) == -N_ERR_INVALID,
              "no children without a node of its own");
        CHECK(n_pci_acpi_notify_take(vga, 0) == -N_ERR_NOT_FOUND, "no notifications without a node");
    } else {
        CHECK(r >= (n_result)sizeof *v && v[0].type == N_ACPI_INTEGER &&
                  v[0].value == ((uint64_t)at->dev << 16 | at->func),
              "the VGA's own _ADR: %s", r < 0 ? n_result_text(r) : "another value");
        CHECK(n_pci_acpi_notify_take(vga, 0) == -N_ERR_TIMEOUT, "no notification waiting");
    }
}

/* The ACPI methods through which the system runs devices and the platform stay the system's, as
   the config bits do: refused on every node before the node is looked up, so edu gets the same
   answer whether QEMU's tables give it a node of its own or not, and a child past the last is
   refused too. On the bridge above it (q35's host bridge, whose _OSC exists: without the refusal
   it would answer N_ERR_IO, the AML wanting 4 arguments) the firmware's own names are refused as
   well. The names a driver uses (RM's) answer as they did. */
static void acpi_refused_cases(n_handle pci, const struct n_pciinfo *at) {
    static uint64_t out[512];
    struct n_acpi_value *v = (struct n_acpi_value *)out;
    static const char *const system_methods[] = {
        "_PS0", "_PS1", "_PS2", "_PS3", "_ON_", "_OFF", "_RST", "_EJ0", "_EJ1", "_EJ2", "_EJ3", "_EJ4", "_LCK", "_DCK",
        "_DIS", "_SRS", "_OSC", "_OST", "_PSW", "_DSW", "_INI", "_REG", "_EVT", "_PTS", "_WAK", "_TTS", "_GTS", "_BFS",
    };
    static const uint32_t nodes[] = {N_ACPI_SELF, N_ACPI_UPSTREAM, N_ACPI_CHILD(0), N_ACPI_CHILD(1000)};
    for (size_t i = 0; i < sizeof system_methods / sizeof *system_methods; i++)
        for (size_t k = 0; k < sizeof nodes / sizeof *nodes; k++) {
            n_result r = n_pci_acpi_eval(pci, nodes[k], system_methods[i], NULL, 0, NULL, 0, out, sizeof out);
            CHECK(r == -N_ERR_DENIED, "%s on node %x is the system's: %s", system_methods[i], nodes[k],
                  n_result_text(r));
        }
    n_result r = n_pci_acpi_eval(pci, N_ACPI_UPSTREAM, "NONE", NULL, 0, NULL, 0, out, sizeof out);
    CHECK(r == -N_ERR_DENIED, "the bridge's own names are the platform's: %s", n_result_text(r));
    r = n_pci_acpi_eval(pci, N_ACPI_UPSTREAM, "\\_SB", NULL, 0, NULL, 0, out, sizeof out);
    CHECK(r == -N_ERR_INVALID, "a malformed name stays malformed on the bridge: %s", n_result_text(r));
    /* as before the refusals */
    r = n_pci_acpi_eval(pci, N_ACPI_SELF, "NONE", NULL, 0, NULL, 0, out, sizeof out);
    CHECK(r == -N_ERR_NOT_FOUND, "the function's own names are not refused: %s", n_result_text(r));
    r = n_pci_acpi_eval(pci, N_ACPI_SELF, "_ROM", NULL, 0, NULL, 0, out, sizeof out);
    CHECK(r == -N_ERR_NOT_FOUND, "edu has no _ROM: %s", n_result_text(r));
    r = n_pci_acpi_eval(pci, N_ACPI_SELF, "_ADR", NULL, 0, NULL, 0, out, sizeof out);
    CHECK(r == -N_ERR_NOT_FOUND || (r >= (n_result)sizeof *v && v[0].type == N_ACPI_INTEGER &&
                                    v[0].value == ((uint64_t)at->dev << 16 | at->func)),
          "edu's own _ADR, when it has a node: %s", r < 0 ? n_result_text(r) : "another value");
    r = n_pci_acpi_eval(pci, N_ACPI_CHILD(0), "_ADR", NULL, 0, NULL, 0, out, sizeof out);
    CHECK(r == -N_ERR_INVALID, "edu has no child devices: %s", n_result_text(r));
    r = n_pci_acpi_eval(pci, N_ACPI_UPSTREAM, "_ROM", NULL, 0, NULL, 0, out, sizeof out);
    CHECK(r == -N_ERR_NOT_FOUND, "a predefined name the host bridge lacks: %s", n_result_text(r));
    /* RM's names on the nodes it uses them on (those that take arguments get none here, so no AML
       of theirs runs: whatever the answer, it is not a refusal) */
    static const struct { uint32_t node; const char *name; } rm[] = {
        {N_ACPI_SELF, "_DSM"}, {N_ACPI_SELF, "_ROM"}, {N_ACPI_SELF, "_DOD"}, {N_ACPI_UPSTREAM, "_DSM"},
        {N_ACPI_CHILD(0), "_ADR"}, {N_ACPI_CHILD(0), "_DDC"}, {N_ACPI_CHILD(0), "MXDS"}, {N_ACPI_CHILD(0), "MXDM"},
    };
    for (size_t i = 0; i < sizeof rm / sizeof *rm; i++) {
        r = n_pci_acpi_eval(pci, rm[i].node, rm[i].name, NULL, 0, NULL, 0, out, sizeof out);
        CHECK(r != -N_ERR_DENIED, "RM's %s on node %x is not refused", rm[i].name, rm[i].node);
    }
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
    acpi_cases(vga, &list[at]);
    n_handle_close(vga);
}

/* The VGA as a display driver uses it to give the system its own scanout (n_pci_give_screen):
   BAR 0 is its memory, the boot screen at its start, and BAR 2 has the Bochs display registers
   (index i at 0x500 + 2i), whose Y offset is the row of that memory the display starts at. The
   driver draws a second buffer past the boot screen, gives it to the system and shows it, then
   DMA memory, then the boot screen again, then windows shown directly and the monitor's modes;
   last a surface its claim's end gives back. A mark of
   pixels the desktop never draws tells where the system drew. linger_s: how long the second
   buffer stays shown, its vertical blanks given at 60 Hz (pcitest screen=<s>, to look at it). */
#define DISPI(i) (0x500 + 2 * (i))
#define DISPI_VIRT_HEIGHT 7
#define DISPI_Y_OFFSET 9
#define MARK 0x00A1B2C3u
static void mark(volatile uint32_t *p) {
    for (int i = 0; i < 64; i++) p[i] = MARK;
    __asm__ volatile("sfence" ::: "memory");
}
static int marks(const volatile uint32_t *p) {
    int k = 0;
    for (int i = 0; i < 64; i++) k += p[i] == MARK;
    return k;
}
static n_result vblanks(n_handle vga, uint64_t *count, unsigned frames) {
    n_result r = 0;
    for (unsigned i = 0; i < frames && !r; i++) {
        r = n_pci_screen_vblank(vga, ++*count, uptime_ms() * 1000000ull);
        n_sleep_ms(16);
    }
    return r;
}
/* The display starts at row (when the Bochs registers are there). */
static void show_row(volatile uint16_t *dispi, uint32_t row) {
    if (dispi) dispi[DISPI(DISPI_Y_OFFSET) / 2] = (uint16_t)row;
}

/* A surface given with N_SCANOUT_DIRECT: the system says which fullscreen window the driver may
   show itself (n_pci_screen_state), and that window hears of it (EV_DIRECT, WIN_STATE_DIRECT). */
static bool screen_app(n_handle vga, struct n_screen_state *st, uint32_t app) {
    for (uint64_t end = uptime_ms() + 2000; st->app != app && uptime_ms() < end;)
        if (n_pci_screen_state(vga, st->serial, st, 200) == -N_ERR_DENIED) return false;
    return st->app == app;
}
static bool direct_heard(window_t *win, int key) {
    struct gui_event e;
    for (uint64_t end = uptime_ms() + 2000; uptime_ms() < end;)
        if (win_event(win, &e, 200) > 0 && e.type == EV_DIRECT && e.key == key) return true;
    return false;
}
/* The images such a driver shows in its scanout's place, the app's own (n_pci_screen_image): the
   panic screen is drawn into them too, as no driver runs then to show the scanout again. They are
   in the format NVIDIA's are, red in the low byte. The last is in the second buffer (when there is
   room), apart from the scanout's own rows; with linger_s (and the Bochs registers) it is shown
   that long, as a driver shows an app's window: a panic meanwhile (QEMU's monitor: nmi) is to be
   seen on it, red and blue swapped, as the display reads XRGB8888, and in its own colors on the
   scanout. */
static void direct_cases(n_handle vga, n_handle surface, const struct n_scanout *d, n_handle edu_bar,
                         uint64_t second, volatile uint16_t *dispi, unsigned linger_s) {
    struct n_scanout direct = *d, image = *d, bad;
    direct.format |= N_SCANOUT_DIRECT;
    image.format = N_SCANOUT_XBGR8888;
    CHECK(n_pci_screen_image(vga, surface, &image) == -N_ERR_DENIED, "no image while the boot screen is shown");
    n_result r = n_pci_give_screen(vga, surface, &direct);
    CHECK(r == 0, "a surface that can show fullscreen windows directly: %s", n_result_text(r));
    if (r) return;
    struct n_screen_state st = {0};
    r = n_pci_screen_state(vga, 0, &st, 0);
    CHECK(r == 0 && st.serial != 0 && st.app == 0, "no window shown directly yet: %s app %u", n_result_text(r), st.app);
    uint64_t seen = st.serial, t0 = uptime_ms();
    r = n_pci_screen_state(vga, seen, &st, 100);
    CHECK(r == -N_ERR_TIMEOUT && st.serial == seen && uptime_ms() - t0 >= 90, "the state waits for a change: %s",
          n_result_text(r));
    bad = image, bad.format |= N_SCANOUT_DIRECT;
    CHECK(n_pci_screen_image(vga, surface, &bad) == -N_ERR_INVALID, "an image is no scanout of its own");
    bad = image, bad.width = 0;
    CHECK(n_pci_screen_image(vga, surface, &bad) == -N_ERR_INVALID, "an image of no width");
    bad = image, bad.format = 99;
    CHECK(n_pci_screen_image(vga, surface, &bad) == -N_ERR_INVALID, "an image of an unknown format");
    if (edu_bar >= 0)
        CHECK(n_pci_screen_image(vga, edu_bar, &image) == -N_ERR_DENIED, "an image in another claim's BAR");
    n_result one = n_pci_screen_image(vga, surface, &image), two = n_pci_screen_image(vga, surface, &image);
    CHECK(one > 0 && two > 0 && one != two, "two images the panic screen is drawn into: %lld %lld", (long long)one,
          (long long)two);
    r = n_pci_screen_image_end(vga, one);
    CHECK(r == 0 && n_pci_screen_image_end(vga, one) == -N_ERR_INVALID && n_pci_screen_image_end(vga, 0) == -N_ERR_INVALID,
          "an image ended once: %s", n_result_text(r));
    window_t *win = NULL;
    if (!gui_available() || win_open(320, 200, "pcitest", 0, &win) < 0) {
        CHECK(0, "a window to show directly");
    } else {
        /* A window is shown from its first picture on. */
        CHECK(win_update(win) == 0, "the window's first picture");
        CHECK(win_direct(win, true) == 0 && win_fullscreen(win, true) == 0, "a fullscreen window asks for it");
        CHECK(direct_heard(win, 1), "the window hears it is shown directly");
        CHECK(screen_app(vga, &st, (uint32_t)n_app_self()), "the driver is told this app: %u", st.app);
        CHECK((win_state(win) & WIN_STATE_DIRECT) != 0, "WIN_STATE_DIRECT");
        uint32_t hot = st.pointer[st.hot_y * N_SCREEN_POINTER_SIZE + st.hot_x];
        CHECK(st.pointer_shown && st.pointer_serial != 0 && hot >> 24 == 0xFF && st.pointer[0] == 0,
              "the pointer the driver draws: shown %u, %08x at its hot spot", st.pointer_shown, hot);
        CHECK(win_fullscreen(win, false) == 0 && direct_heard(win, 0), "a window no longer fullscreen hears it ends");
        CHECK(screen_app(vga, &st, 0) && !(win_state(win) & WIN_STATE_DIRECT), "the driver is told it ends");
        CHECK(win_fullscreen(win, true) == 0 && direct_heard(win, 1) && screen_app(vga, &st, (uint32_t)n_app_self()),
              "shown directly again");
        win_close(win);
        CHECK(screen_app(vga, &st, 0), "a closed window is no longer shown");
    }
    r = n_pci_give_screen(vga, surface, d);
    CHECK(r == 0 && n_pci_screen_image_end(vga, two) == -N_ERR_INVALID,
          "a scanout without N_SCANOUT_DIRECT ends the images: %s", n_result_text(r));
    r = n_pci_give_screen(vga, surface, &direct);
    if (second) image.offset = second;
    one = r ? r : n_pci_screen_image(vga, surface, &image);
    CHECK(one > 0, "an image again%s: %s", second ? ", in the second buffer" : "", n_result_text(one));
    if (one > 0 && linger_s && dispi && second) {
        show_row(dispi, (uint32_t)(second / d->pitch));
        printf("pcitest: the image in the second buffer is shown for %u s; a panic meanwhile is drawn there too\n",
               linger_s);
        n_sleep_ms(linger_s * 1000);
        show_row(dispi, (uint32_t)(d->offset / d->pitch));
    }
    r = n_pci_give_screen(vga, N_SCANOUT_BOOT, NULL);
    CHECK(r == 0 && n_pci_screen_state(vga, 0, &st, 0) == -N_ERR_DENIED && n_pci_screen_image_end(vga, one) == -N_ERR_INVALID,
          "the boot screen back ends direct showing and its images");
    n_sleep_ms(300);
}

/* The monitor's modes (n_pci_screen_modes): the driver offers what it can show, the system wants
   the monitor's preferred mode, the Settings app tries another (SCREEN_MODE_SET), which goes back
   by itself unless it is kept, and a mode the driver could not show is refused. The driver shows
   each mode wanted: the Bochs registers (when they answer) set to its size, the rows as long as
   the boot screen's, and its surface given at that size, which the desktop takes. linger_s: the
   smaller mode stays shown until its trial ends (15 s) anyway; pcitest screen=<s> says so. */
#define DISPI_XRES 1
#define DISPI_YRES 2
#define DISPI_BPP 3
#define DISPI_ENABLE 4
#define DISPI_VIRT_WIDTH 6
static void bochs_mode(volatile uint16_t *dispi, uint32_t width, uint32_t height, uint32_t pitch, uint32_t row) {
    if (!dispi) return;
    dispi[DISPI(DISPI_ENABLE) / 2] = 0;
    dispi[DISPI(DISPI_XRES) / 2] = (uint16_t)width;
    dispi[DISPI(DISPI_YRES) / 2] = (uint16_t)height;
    dispi[DISPI(DISPI_BPP) / 2] = 32;
    dispi[DISPI(DISPI_ENABLE) / 2] = 0x01 | 0x40 | 0x80; /* on, linear framebuffer, memory kept */
    dispi[DISPI(DISPI_VIRT_WIDTH) / 2] = (uint16_t)(pitch / 4);
    show_row(dispi, row);
}
static bool same_mode(const struct n_screen_mode *a, const struct n_screen_mode *b) {
    return a->width == b->width && a->height == b->height && a->refresh_mhz == b->refresh_mhz;
}
static n_result modes_get(struct screen_modes *out) {
    return (n_result)__syscall(SYS_SCREEN_INFO, (long)out, sizeof *out, SCREEN_MODES_GET, 0, 0);
}
/* What decides the desktop's size as Settings hears it (SCREEN_SOURCE_*); UINT32_MAX unread. */
static uint32_t screen_source(void) {
    struct screen_modes *s = calloc(1, sizeof *s);
    uint32_t source = s && modes_get(s) == 0 ? s->source : UINT32_MAX;
    free(s);
    return source;
}
static n_result mode_set(uint32_t how, const struct n_screen_mode *m) {
    struct screen_mode_set s = {.how = how};
    if (m) s.mode = *m;
    return (n_result)__syscall(SYS_SCREEN_INFO, (long)&s, sizeof s, SCREEN_MODE_SET, 0, 0);
}
/* The next wish (a mode_serial after *serial) within ms. */
static bool mode_wanted(n_handle vga, struct n_screen_state *st, uint64_t *serial, int64_t ms) {
    for (uint64_t end = uptime_ms() + (uint64_t)ms; st->mode_serial <= *serial && uptime_ms() < end;)
        if (n_pci_screen_state(vga, st->serial, st, 200) == -N_ERR_DENIED) return false;
    if (st->mode_serial <= *serial) return false;
    *serial = st->mode_serial;
    return true;
}
/* As a driver does: shows the mode wanted (or, could_not, says it still shows offer->shown). */
static n_result mode_show(n_handle vga, n_handle surface, const struct n_scanout *d, volatile uint16_t *dispi,
                          struct n_screen_modes *offer, const struct n_screen_state *st, bool could_not) {
    n_result r = 0;
    if (!could_not) {
        struct n_scanout s = *d;
        s.width = st->mode.width, s.height = st->mode.height;
        bochs_mode(dispi, s.width, s.height, d->pitch, (uint32_t)(d->offset / d->pitch));
        r = n_pci_give_screen(vga, surface, &s);
        if (!r) offer->shown = st->mode;
    }
    offer->done = st->mode_serial;
    n_result told = n_pci_screen_modes(vga, offer);
    return r ? r : told;
}
static bool desktop_width(int width) {
    int w = 0, h = 0;
    for (uint64_t end = uptime_ms() + 1000; (screen_size(&w, &h) < 0 || w != width) && uptime_ms() < end;) n_sleep_ms(20);
    return w == width;
}
static void mode_cases(n_handle vga, n_handle edu, n_handle surface, const struct n_scanout *d, volatile uint16_t *dispi,
                       unsigned linger_s) {
    struct n_screen_modes *offer = calloc(1, sizeof *offer);
    struct screen_modes *seen = calloc(1, sizeof *seen);
    if (!offer || !seen) {
        CHECK(0, "memory for the modes");
        free(offer), free(seen);
        return;
    }
    const struct n_screen_mode native = {d->width, d->height, 60000, N_SCREEN_MODE_PREFERRED};
    const struct n_screen_mode wide = {1280, 720, 60000, 0}, small = {1024, 768, 75000, 0};
    const struct n_screen_mode others[] = {wide, {1024, 768, 60000, 0}, small};
    snprintf(offer->monitor, sizeof offer->monitor, "pcitest-vga");
    snprintf(offer->name, sizeof offer->name, "QEMU VGA");
    offer->modes[offer->count++] = native;
    for (unsigned i = 0; i < sizeof others / sizeof *others; i++)
        if (others[i].width != native.width || others[i].height != native.height) offer->modes[offer->count++] = others[i];
    if (offer->count != 4) {
        CHECK(0, "a boot screen that is not 1280x720 or 1024x768 (%ux%u)", d->width, d->height);
        free(offer), free(seen);
        return;
    }
    struct n_screen_state st = {0};
    CHECK(n_pci_screen_state(vga, 0, &st, 0) == -N_ERR_DENIED, "no screen state before modes are offered");
    /* no driver running has offered modes (an earlier run's ended) */
    n_result r = modes_get(seen);
    CHECK(r == 0 && !seen->offered.count &&
              (seen->source == SCREEN_SOURCE_FIRMWARE || seen->source == SCREEN_SOURCE_DRIVER_STOPPED) &&
              seen->offered.shown.width == d->width && seen->offered.shown.height == d->height,
          "without modes Settings sees the boot screen's mode: source %u, %ux%u", seen->source, seen->offered.shown.width,
          seen->offered.shown.height);
    if (edu >= 0) CHECK(n_pci_screen_modes(edu, offer) == -N_ERR_DENIED, "edu offers no modes");
    offer->count = N_SCREEN_MODES_MAX + 1;
    CHECK(n_pci_screen_modes(vga, offer) == -N_ERR_INVALID, "more modes than N_SCREEN_MODES_MAX");
    offer->count = 4;
    offer->modes[3].refresh_mhz = 0;
    CHECK(n_pci_screen_modes(vga, offer) == -N_ERR_INVALID, "a mode without a refresh rate");
    offer->modes[3] = small;

    /* the monitor's preferred mode first */
    r = n_pci_screen_modes(vga, offer);
    CHECK(r == 0, "the VGA offers its monitor's modes: %s", n_result_text(r));
    if (r) {
        free(offer), free(seen);
        return;
    }
    uint64_t serial = 0;
    r = n_pci_screen_state(vga, 0, &st, 0);
    CHECK(r == 0 && same_mode(&st.mode, &native) && mode_wanted(vga, &st, &serial, 0),
          "the system wants the monitor's preferred mode: %ux%u at %u mHz", st.mode.width, st.mode.height, st.mode.refresh_mhz);
    r = mode_show(vga, surface, d, dispi, offer, &st, false);
    CHECK(r == 0, "the preferred mode shown: %s", n_result_text(r));
    CHECK(n_pci_screen_state(vga, st.serial, &st, 100) == -N_ERR_TIMEOUT, "a mode shown as wanted changes nothing");
    r = modes_get(seen);
    CHECK(r == 0 && seen->offered.count == 4 && !strcmp(seen->offered.monitor, "pcitest-vga") &&
              same_mode(&seen->offered.shown, &native) && same_mode(&seen->wanted, &native) && !seen->trial_ms && !seen->refused &&
              seen->source == SCREEN_SOURCE_DRIVER,
          "Settings sees the monitor, its modes and the one the driver shows: %s", n_result_text(r));
    CHECK(mode_set(SCREEN_MODE_KEEP, NULL) == -N_ERR_NOT_FOUND && mode_set(SCREEN_MODE_BACK, NULL) == -N_ERR_NOT_FOUND,
          "nothing on trial to keep or go back from");
    struct n_screen_mode odd = {800, 600, 60000, 0};
    CHECK(mode_set(SCREEN_MODE_TRY, &odd) == -N_ERR_INVALID, "a mode the monitor was not offered");

    /* a mode tried and not kept goes back by itself */
    r = mode_set(SCREEN_MODE_TRY, &small);
    CHECK(r == 0 && mode_wanted(vga, &st, &serial, 1000) && same_mode(&st.mode, &small), "a mode on trial is wanted: %s",
          n_result_text(r));
    r = mode_show(vga, surface, d, dispi, offer, &st, false);
    CHECK(r == 0 && desktop_width(1024), "the mode tried shown, and the desktop takes its size: %s", n_result_text(r));
    r = modes_get(seen);
    CHECK(r == 0 && seen->trial_ms > 0 && seen->trial_ms <= SCREEN_MODE_TRIAL_MS, "Settings sees the trial: %u ms left",
          seen->trial_ms);
    if (linger_s) printf("pcitest: the desktop is 1024x768 until its trial ends\n");
    uint64_t t0 = uptime_ms();
    CHECK(mode_wanted(vga, &st, &serial, SCREEN_MODE_TRIAL_MS + 3000) && same_mode(&st.mode, &native) &&
              uptime_ms() - t0 >= SCREEN_MODE_TRIAL_MS - 2000,
          "the trial not kept goes back after %u ms: %ux%u", (unsigned)(uptime_ms() - t0), st.mode.width, st.mode.height);
    r = mode_show(vga, surface, d, dispi, offer, &st, false);
    CHECK(r == 0 && desktop_width((int)native.width), "the mode before shown again: %s", n_result_text(r));

    /* back at once; then kept, the user's choice for this monitor */
    r = mode_set(SCREEN_MODE_TRY, &wide);
    CHECK(r == 0 && mode_wanted(vga, &st, &serial, 1000) && same_mode(&st.mode, &wide), "another mode on trial: %s", n_result_text(r));
    mode_show(vga, surface, d, dispi, offer, &st, false);
    r = mode_set(SCREEN_MODE_BACK, NULL);
    CHECK(r == 0 && mode_wanted(vga, &st, &serial, 1000) && same_mode(&st.mode, &native), "going back at once: %s", n_result_text(r));
    mode_show(vga, surface, d, dispi, offer, &st, false);
    r = mode_set(SCREEN_MODE_TRY, &small);
    CHECK(r == 0 && mode_wanted(vga, &st, &serial, 1000), "tried again: %s", n_result_text(r));
    mode_show(vga, surface, d, dispi, offer, &st, false);
    r = mode_set(SCREEN_MODE_KEEP, NULL);
    CHECK(r == 0 && n_pci_screen_state(vga, st.serial, &st, 100) == -N_ERR_TIMEOUT && modes_get(seen) == 0 && !seen->trial_ms,
          "kept: the trial ends and the mode stays: %s", n_result_text(r));
    offer->count = 0;
    CHECK(n_pci_screen_modes(vga, offer) == 0 && mode_wanted(vga, &st, &serial, 1000) && st.mode.width == 0,
          "the modes withdrawn: no mode is wanted");
    offer->count = 4;
    offer->done = 0;
    CHECK(n_pci_screen_modes(vga, offer) == 0 && mode_wanted(vga, &st, &serial, 1000) && same_mode(&st.mode, &small),
          "offered again, the monitor shows the mode kept: %ux%u", st.mode.width, st.mode.height);
    mode_show(vga, surface, d, dispi, offer, &st, false);
    CHECK(modes_get(seen) == 0 && same_mode(&seen->chosen, &small), "Settings sees the mode kept for this monitor: %ux%u",
          seen->chosen.width, seen->chosen.height);
    CHECK(__syscall(SYS_SCREEN_INFO, (long)seen, SCREEN_MODES_FIRST_BYTES, SCREEN_MODES_GET, 0, 0) == 0 &&
              seen->offered.count == 4 && same_mode(&seen->wanted, &small),
          "a program built before chosen reads its struct screen_modes");
    r = mode_set(SCREEN_MODE_TRY, &native);
    CHECK(r == 0 && mode_wanted(vga, &st, &serial, 1000) && same_mode(&st.mode, &native), "the preferred mode tried: %s",
          n_result_text(r));
    mode_show(vga, surface, d, dispi, offer, &st, false);
    r = mode_set(SCREEN_MODE_KEEP, NULL);
    CHECK(r == 0 && modes_get(seen) == 0 && same_mode(&seen->chosen, &native), "kept: the user's new choice for this monitor: %s",
          n_result_text(r));
    CHECK(mode_set(4, &native) == -N_ERR_INVALID, "4, once a saved choice put back by Settings, is no operation");

    /* a mode the driver could not show */
    r = mode_set(SCREEN_MODE_TRY, &wide);
    CHECK(r == 0 && mode_wanted(vga, &st, &serial, 1000), "tried: %s", n_result_text(r));
    mode_show(vga, surface, d, dispi, offer, &st, true);
    CHECK(mode_wanted(vga, &st, &serial, 1000) && same_mode(&st.mode, &native) && modes_get(seen) == 0 && seen->refused && !seen->trial_ms,
          "a mode on trial the driver could not show: the one before comes back, and Settings hears it");
    mode_show(vga, surface, d, dispi, offer, &st, false);
    offer->done = 0;
    CHECK(n_pci_screen_modes(vga, offer) == 0 && mode_wanted(vga, &st, &serial, 1000) && same_mode(&st.mode, &native),
          "offered again: the mode shown stays wanted");
    offer->shown = (struct n_screen_mode){0}; /* the driver shows nothing */
    mode_show(vga, surface, d, dispi, offer, &st, true);
    const struct n_screen_mode next = {1024, 768, 60000, 0}; /* 1280x720 was refused above */
    CHECK(mode_wanted(vga, &st, &serial, 1000) && same_mode(&st.mode, &next),
          "the choice it could not show: the largest mode not refused, nearest 60 Hz, instead: %ux%u at %u mHz", st.mode.width,
          st.mode.height, st.mode.refresh_mhz);
    r = mode_show(vga, surface, d, dispi, offer, &st, false);
    CHECK(r == 0 && desktop_width(1024), "shown: %s", n_result_text(r));

    /* withdrawn, and the boot screen back at its size */
    offer->count = 0;
    CHECK(n_pci_screen_modes(vga, offer) == 0 && mode_wanted(vga, &st, &serial, 1000) && st.mode.width == 0 &&
              modes_get(seen) == 0 && seen->offered.count == 0,
          "withdrawn: no modes and none wanted");
    bochs_mode(dispi, native.width, native.height, d->pitch, (uint32_t)(d->offset / d->pitch));
    r = n_pci_give_screen(vga, N_SCANOUT_BOOT, NULL);
    CHECK(r == 0 && desktop_width((int)native.width) && n_pci_screen_state(vga, 0, &st, 0) == -N_ERR_DENIED,
          "the boot screen back at its size: %s", n_result_text(r));
    r = modes_get(seen);
    CHECK(r == 0 && seen->source == SCREEN_SOURCE_HANDED_BACK && !seen->offered.count &&
              seen->offered.shown.width == native.width && seen->offered.shown.height == native.height,
          "Settings hears the screen given back by a driver that runs on: source %u", seen->source);

    /* offered again with the boot screen shown, as a driver that gave the screen back and still
       follows the wishes: Settings lists the modes, and a mode tried asks for the driver's screen */
    offer->count = 4;
    offer->done = 0;
    offer->shown = (struct n_screen_mode){0};
    CHECK(n_pci_screen_modes(vga, offer) == 0 && mode_wanted(vga, &st, &serial, 1000) && modes_get(seen) == 0 &&
              seen->source == SCREEN_SOURCE_HANDED_BACK && seen->offered.count == 4 &&
              seen->offered.shown.width == native.width && seen->offered.shown.height == native.height,
          "modes offered with the boot screen shown: listed, the boot screen's mode shown (source %u)", seen->source);
    r = mode_set(SCREEN_MODE_TRY, &small);
    CHECK(r == 0 && mode_wanted(vga, &st, &serial, 1000) && same_mode(&st.mode, &small),
          "a mode tried with the boot screen shown is wanted: %s", n_result_text(r));
    r = mode_show(vga, surface, d, dispi, offer, &st, false);
    CHECK(r == 0 && desktop_width(1024) && screen_source() == SCREEN_SOURCE_DRIVER,
          "the driver shows its screen again: %s", n_result_text(r));
    r = mode_set(SCREEN_MODE_BACK, NULL);
    CHECK(r == 0 && mode_wanted(vga, &st, &serial, 1000), "back from that trial: %s", n_result_text(r));
    mode_show(vga, surface, d, dispi, offer, &st, false);
    offer->count = 0;
    CHECK(n_pci_screen_modes(vga, offer) == 0 && mode_wanted(vga, &st, &serial, 1000) && st.mode.width == 0,
          "withdrawn again");
    bochs_mode(dispi, native.width, native.height, d->pitch, (uint32_t)(d->offset / d->pitch));
    r = n_pci_give_screen(vga, N_SCANOUT_BOOT, NULL);
    CHECK(r == 0 && desktop_width((int)native.width) && n_pci_screen_state(vga, 0, &st, 0) == -N_ERR_DENIED,
          "the boot screen back again: %s", n_result_text(r));
    n_sleep_ms(300);
    free(offer), free(seen);
}

/* linger_s: how long the second buffer is shown. */
static void give_screen_cases(n_handle edu, const struct n_pciinfo *list, int n, unsigned linger_s) {
    int at = find_vga(list, n);
    if (at < 0) {
        printf("FAIL: no standard VGA (run QEMU with -vga std)\n");
        failed++;
        return;
    }
    n_handle vga = n_pci_claim(list[at].bus, list[at].dev, list[at].func);
    CHECK(vga >= 0, "claim the VGA to give the screen: %s", n_result_text(vga));
    if (vga < 0) return;
    struct n_pci_function info;
    if (n_pci_info(vga, &info) != 0 || info.framebuffer_bar != 0 || info.framebuffer_bpp != 32) {
        CHECK(0, "the boot screen is a 32 bpp one in the VGA's BAR 0");
        n_handle_close(vga);
        return;
    }
    uint32_t pitch = info.framebuffer_pitch;
    uint64_t size = info.bar[0].size, boot = info.framebuffer_address - info.bar[0].address;
    uint64_t frame = (uint64_t)pitch * info.framebuffer_height, second = boot + frame;
    struct n_scanout d = {info.framebuffer_width, info.framebuffer_height, pitch, N_SCANOUT_XRGB8888, boot}, bad;
    n_handle bar = n_pci_bar(vga, 0);
    n_handle surface = bar >= 0 ? n_shared_view(bar, 0, size, N_CACHE_WRITE_COMBINE) : bar;
    void *whole = NULL;
    if (surface >= 0) n_shared_map(surface, size, N_MEMORY_READ_WRITE, &whole);
    CHECK(whole != NULL, "the driver maps all of the VGA's memory write-combined");
    if (!whole) {
        if (surface >= 0) n_handle_close(surface);
        if (bar >= 0) n_handle_close(bar);
        n_handle_close(vga);
        return;
    }
    volatile uint32_t *gop = (volatile uint32_t *)((uint8_t *)whole + boot + 100ull * pitch) + 100;
    volatile uint32_t *next = (volatile uint32_t *)((uint8_t *)whole + second + 100ull * pitch) + 100;
    /* The Bochs registers, when their ID answers */
    n_handle regs_bar = info.bar[2].size >= 4096 ? n_pci_bar(vga, 2) : -1;
    void *regs_map = NULL;
    if (regs_bar >= 0) n_shared_map(regs_bar, 4096, N_MEMORY_READ_WRITE, &regs_map);
    volatile uint16_t *dispi = regs_map;
    if (dispi && (dispi[DISPI(0) / 2] & 0xFFF0) != 0xB0C0) dispi = NULL;
    uint32_t boot_row = (uint32_t)(boot / pitch), second_row = (uint32_t)(second / pitch);
    if (dispi && second + frame <= size) {
        dispi[DISPI(DISPI_VIRT_HEIGHT) / 2] = (uint16_t)(second_row + info.framebuffer_height);
        if (dispi[DISPI(DISPI_VIRT_HEIGHT) / 2] < second_row + info.framebuffer_height) dispi = NULL;
    } else dispi = NULL;
    printf("pcitest: the second buffer is %s\n", dispi ? "shown through the Bochs registers" : "not shown");

    /* refused */
    CHECK(n_pci_screen_vblank(vga, 1, 0) == -N_ERR_DENIED, "no vertical blank before a surface is given");
    CHECK(n_pci_give_screen(vga, N_SCANOUT_BOOT, NULL) == -N_ERR_INVALID, "nothing to give back yet");
    n_handle edu_bar = edu >= 0 ? n_pci_bar(edu, 0) : -1;
    if (edu >= 0) {
        CHECK(n_pci_give_screen(edu, edu_bar, &d) == -N_ERR_DENIED, "edu is no display function");
        CHECK(n_pci_give_screen(vga, edu_bar, &d) == -N_ERR_DENIED, "another claim's BAR");
        CHECK(n_pci_screen_image(edu, edu_bar, &d) == -N_ERR_DENIED, "edu shows no images");
    }
    bad = d, bad.width = 0;
    CHECK(n_pci_give_screen(vga, surface, &bad) == -N_ERR_INVALID, "no width");
    bad = d, bad.height = 65536;
    CHECK(n_pci_give_screen(vga, surface, &bad) == -N_ERR_INVALID, "a height no display engine has");
    bad = d, bad.format = 99;
    CHECK(n_pci_give_screen(vga, surface, &bad) == -N_ERR_INVALID, "an unknown format");
    bad = d, bad.format |= 0x200;
    CHECK(n_pci_give_screen(vga, surface, &bad) == -N_ERR_INVALID, "an unknown flag");
    bad = d, bad.pitch = d.width * 4 - 4;
    CHECK(n_pci_give_screen(vga, surface, &bad) == -N_ERR_INVALID, "a pitch shorter than a row");
    bad = d, bad.offset = size - frame + 4;
    CHECK(n_pci_give_screen(vga, surface, &bad) == -N_ERR_INVALID, "a scanout past the end of the surface");
    n_handle readonly = n_shared_view_access(surface, 0, size, N_CACHE_DEFAULT, N_MEMORY_READ);
    CHECK(n_pci_give_screen(vga, readonly, &d) == -N_ERR_DENIED, "a surface the driver may not write");
    size_t frame_pages = (size_t)((frame + 4095) & ~4095ull);
    struct n_scanout at0 = d;
    at0.offset = 0;
    n_handle plain = n_shared_create(frame_pages);
    CHECK(plain >= 0 && n_pci_give_screen(vga, plain, &at0) == -N_ERR_DENIED, "memory that is no device's");
    n_handle dma = n_dma_create(vga, frame_pages, 0);
    CHECK(dma >= 0 && n_pci_give_screen(vga, dma, &at0) == -N_ERR_DENIED, "write-back DMA memory");

    /* the boot screen's own memory */
    mark(gop);
    n_result r = n_pci_give_screen(vga, surface, &d);
    CHECK(r == 0, "the boot screen's memory as the driver's surface: %s", n_result_text(r));
    struct n_screen_state st;
    r = n_pci_screen_state(vga, 0, &st, 0);
    CHECK(r == 0 && st.app == 0 && st.mode.width == 0, "without N_SCANOUT_DIRECT or modes offered, the state shows no window and wants no mode: %s",
          n_result_text(r));
    CHECK(n_pci_screen_image(vga, surface, &d) == -N_ERR_DENIED, "no image without N_SCANOUT_DIRECT");
    n_sleep_ms(300);
    CHECK(marks(gop) == 0, "all of the screen is drawn again: %d marks left", marks(gop));
    uint64_t count = 0;
    r = vblanks(vga, &count, 30);
    CHECK(r == 0, "vertical blanks of the surface shown: %s", n_result_text(r));
    CHECK(n_pci_hold_screen(vga, true) == 0 && n_pci_hold_screen(vga, false) == 0, "the screen held and given back");
    n_sleep_ms(300); /* the release draws all of the screen again: the marks below come after it */

    /* a second buffer past it, which the display then shows */
    if (second + frame <= size) {
        struct n_scanout s2 = d;
        s2.offset = second;
        mark(gop);
        mark(next);
        r = n_pci_give_screen(vga, surface, &s2);
        CHECK(r == 0, "a second buffer past the boot screen: %s", n_result_text(r));
        show_row(dispi, second_row);
        n_sleep_ms(300);
        CHECK(marks(next) == 0 && marks(gop) == 64, "drawn into the second buffer only: %d %d marks", marks(next), marks(gop));
        r = vblanks(vga, &count, 30 + linger_s * 60);
        CHECK(r == 0, "vertical blanks of the second buffer: %s", n_result_text(r));
    } else printf("FAIL: no room for a second buffer in %llu bytes\n", (unsigned long long)size), failed++;

    /* DMA memory, which its write-combining view gives the type the system draws it with */
    n_handle dma_wc = dma >= 0 ? n_shared_view(dma, 0, frame_pages, N_CACHE_WRITE_COMBINE) : dma;
    mark(gop);
    r = n_pci_give_screen(vga, dma_wc, &at0);
    CHECK(r == 0, "DMA memory as the surface: %s", n_result_text(r));
    n_sleep_ms(300);
    void *dma_map = NULL;
    if (dma_wc >= 0) n_shared_map(dma_wc, frame_pages, N_MEMORY_READ, &dma_map);
    const volatile uint32_t *drawn = dma_map;
    int lit = 0;
    for (uint32_t x = 0; drawn && x < d.width; x += 16) lit += drawn[(size_t)(d.height / 2) * (pitch / 4) + x] != 0;
    CHECK(lit > 0 && marks(gop) == 64, "the desktop is in the DMA memory, not the boot screen: %d %d", lit, marks(gop));
    if (dma_map) n_memory_release(dma_map, frame_pages);
    if (dma_wc >= 0) n_handle_close(dma_wc); /* the system keeps what it shows */
    if (dma >= 0) n_handle_close(dma);
    r = n_pci_give_screen(vga, N_SCANOUT_BOOT, NULL);
    CHECK(r == 0, "the boot screen back: %s", n_result_text(r));
    show_row(dispi, boot_row);
    n_sleep_ms(300);
    CHECK(marks(gop) == 0, "the boot screen is drawn again: %d marks left", marks(gop));
    CHECK(n_pci_give_screen(vga, N_SCANOUT_BOOT, NULL) == -N_ERR_INVALID, "given back once");
    CHECK(n_pci_screen_vblank(vga, ++count, 0) == -N_ERR_DENIED, "no vertical blanks of the boot screen");
    CHECK(n_pci_screen_state(vga, 0, &st, 0) == -N_ERR_DENIED, "no screen state of the boot screen");
    direct_cases(vga, surface, &d, edu_bar, second + frame <= size ? second : 0, dispi, linger_s);
    mode_cases(vga, edu, surface, &d, dispi, linger_s);

    /* the claim's end gives the screen back, and ends its images */
    struct n_scanout direct = d;
    direct.format |= N_SCANOUT_DIRECT;
    r = n_pci_give_screen(vga, surface, &direct);
    if (!r) r = n_pci_screen_image(vga, surface, &d);
    CHECK(r > 0, "a surface the claim's end gives back, with an image: %s", n_result_text(r));
    n_sleep_ms(300);
    mark(gop);
    n_memory_release(whole, size);
    if (regs_map) n_memory_release(regs_map, 4096);
    if (regs_bar >= 0) n_handle_close(regs_bar);
    if (plain >= 0) n_handle_close(plain);
    if (readonly >= 0) n_handle_close(readonly);
    if (edu_bar >= 0) n_handle_close(edu_bar);
    n_handle_close(surface);
    if (bar >= 0) n_handle_close(bar);
    n_handle_close(vga);
    vga = n_pci_claim(list[at].bus, list[at].dev, list[at].func);
    CHECK(vga >= 0, "the VGA claimed again: %s", n_result_text(vga));
    if (vga < 0) return;
    bar = n_pci_bar(vga, 0);
    size_t peek = (size_t)(((boot & 4095) + 101ull * pitch + 4095) & ~4095ull); /* the marked row */
    n_handle view = bar >= 0 ? n_shared_view(bar, boot & ~4095ull, peek, N_CACHE_WRITE_COMBINE) : bar;
    void *again = NULL;
    if (view >= 0) n_shared_map(view, peek, N_MEMORY_READ, &again);
    n_sleep_ms(300);
    const volatile uint32_t *seen =
        again ? (const volatile uint32_t *)((uint8_t *)again + (boot & 4095) + 100ull * pitch) + 100 : NULL;
    CHECK(seen && marks(seen) == 0, "the claim's end drew the boot screen again: %d marks left", seen ? marks(seen) : -1);
    CHECK(screen_source() == SCREEN_SOURCE_DRIVER_STOPPED, "Settings hears that the driver which offered modes ended");
    if (again) n_memory_release(again, peek);
    if (view >= 0) n_handle_close(view);
    if (bar >= 0) n_handle_close(bar);
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

/* pcitest flr: release records a crashed driver without waiting for/resetting its device. An
   isolated next claim performs FLR (QEMU: virtio-rng-pci behind a pcie-root-port). Its virtio
   device status reads 0, BARs are retained and BME stays off. A nonisolated next claim is refused:
   resetting the device alone is not permission to reuse the old driver's DMA memory. */
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
    struct n_pci_function info;
    n_result described = n_pci_info(pci, &info);
    CHECK(described == 0, "describe the FLR function: %s", n_result_text(described));
    if (described < 0) { n_handle_close(pci); return; }
    n_result bars[6], cmd = n_pci_config_read(pci, N_PCI_SELF, 4, 2);
    for (int i = 0; i < 6; i++) bars[i] = n_pci_config_read(pci, N_PCI_SELF, 0x10 + 4 * (uint32_t)i, 4);
    CHECK(cmd >= 0 && n_pci_config_write(pci, 4, 2, (uint32_t)cmd | 6) == 0, "command: memory and bus master");
    CHECK(virtio_status(pci, 1) == 1, "the virtio device status takes ACKNOWLEDGE");
    n_handle_close(pci); /* bus mastering still on: the driver did not stop it */
    pci = n_pci_claim(v->bus, v->dev, v->func);
    if (!(info.flags & N_PCI_ISOLATED)) {
        CHECK(pci == -N_ERR_NOT_READY, "a crashed nonisolated function cannot be reopened: %s", n_result_text(pci));
        if (pci >= 0) n_handle_close(pci);
        return;
    }
    CHECK(pci >= 0, "claim it again: %s", n_result_text(pci));
    if (pci < 0) return;
    CHECK(n_pci_info(pci, &info) == 0 && !(info.flags & N_PCI_NEEDS_RECOVERY),
          "completed FLR cleared the recovery requirement");
    cmd = n_pci_config_read(pci, N_PCI_SELF, 4, 2);
    CHECK(cmd >= 0 && !(cmd & 4), "bus mastering is off after the claim ended: command %llx", (long long)cmd);
    for (int i = 0; i < 6; i++)
        CHECK(n_pci_config_read(pci, N_PCI_SELF, 0x10 + 4 * (uint32_t)i, 4) == bars[i], "BAR %d is as it was", i);
    CHECK(cmd >= 0 && n_pci_config_write(pci, 4, 2, (uint32_t)cmd | 2) == 0, "command: memory");
    int status = virtio_status(pci, -1);
    CHECK(status == 0, "the reset cleared the virtio device status: %d", status);
    cmd = n_pci_config_read(pci, N_PCI_SELF, 4, 2);
    CHECK(cmd >= 0 && n_pci_config_write(pci, 4, 2, (uint32_t)cmd & ~4u) == 0,
          "the recovered driver leaves bus mastering off");
    n_handle_close(pci);
}

/* 任意サイズの非連続DMA確保を実native経路で観測する診断。時間の合否閾値や
   GPU性能の主張は設けない。通常のpcitestの操作・受入条件とは独立している。 */
static int dma_allocation_case(const char *text) {
    const size_t page = 4096; /* Nocturne x86-64のページ粒度 */
    size_t bytes = 0;
    bool valid = *text != 0;
    for (const char *p = text; valid && *p; p++) {
        if (*p < '0' || *p > '9') { valid = false; break; }
        unsigned digit = (unsigned)(*p - '0');
        if (bytes > (SIZE_MAX - digit) / 10) { valid = false; break; }
        bytes = bytes * 10 + digit;
    }
    CHECK(valid && bytes && bytes <= SIZE_MAX - (page - 1),
          "dma=<10進バイト数>が不正: %s", n_result_text(-N_ERR_INVALID));
    if (!valid || !bytes || bytes > SIZE_MAX - (page - 1)) return 1;
    size_t rounded = (bytes + page - 1) & ~(page - 1), pages = rounded / page;
    struct n_pciinfo edu = {0};
    bool found = false;
    /* pcilistは書き込んだ件数を返す。固定一覧上限を足さず、実一覧に合わせて拡張する。 */
    for (int capacity = 1; !found;) {
        if ((size_t)capacity > SIZE_MAX / sizeof(struct n_pciinfo)) {
            CHECK(0, "PCI一覧サイズ: %s", n_result_text(-N_ERR_TOO_LARGE)); return 1;
        }
        struct n_pciinfo *list = malloc((size_t)capacity * sizeof *list);
        if (!list) { CHECK(0, "PCI一覧確保: %s", n_result_text(-N_ERR_NO_MEMORY)); return 1; }
        n_result n = pcilist(list, capacity);
        for (n_result i = 0; i < n && i < capacity; i++)
            if (list[i].vendor == 0x1234 && list[i].device == 0x11e8) { edu = list[i]; found = true; break; }
        free(list);
        if (found) break;
        if (n < 0 || n < capacity) {
            CHECK(0, "EDU一覧取得: %s", n_result_text(n < 0 ? n : -N_ERR_NOT_FOUND)); return 1;
        }
        if (capacity > INT_MAX / 2) {
            CHECK(0, "PCI一覧のnative int範囲: %s", n_result_text(-N_ERR_TOO_LARGE)); return 1;
        }
        capacity *= 2;
    }
    n_handle pci = n_pci_claim(edu.bus, edu.dev, edu.func), mem = -1, views[2] = {-1, -1};
    void *maps[2] = {NULL, NULL};
    uint64_t *addresses = NULL;
    CHECK(pci >= 0, "DMA診断のEDU claim: %s", n_result_text(pci));
    if (pci < 0) return 1;
    struct n_pci_function info = {0};
    n_result result = n_pci_info(pci, &info);
    CHECK(result == 0 && (info.flags & N_PCI_ISOLATED), "DMA診断には隔離済みEDUが必要: %s flags=%x",
          n_result_text(result), info.flags);
    if (result < 0 || !(info.flags & N_PCI_ISOLATED)) goto done;
    uint64_t began = uptime_ms();
    mem = n_dma_create(pci, bytes, 0); /* CONTIGUOUS/32BITのどちらも指定しない。 */
    uint64_t elapsed = uptime_ms() - began;
    printf("pcitest dma: 要求=%llu 丸め=%llu ページ=%llu flags=0 確保=%llums 結果=%s\n",
           (unsigned long long)bytes, (unsigned long long)rounded, (unsigned long long)pages,
           (unsigned long long)elapsed, n_result_text(mem < 0 ? mem : 0));
    CHECK(mem >= 0, "非連続DMA確保: %s", n_result_text(mem));
    if (mem < 0) goto done;
    if (pages > SIZE_MAX / sizeof *addresses || !(addresses = malloc(pages * sizeof *addresses))) {
        CHECK(0, "DMAアドレス一覧確保: %s", n_result_text(-N_ERR_NO_MEMORY)); goto done;
    }
    result = n_dma_addresses(mem, 0, addresses, pages);
    CHECK(result == (n_result)pages, "全DMAアドレスの件数: %lld / %llu", (long long)result,
          (unsigned long long)pages);
    if (result != (n_result)pages) goto done;
    bool aligned = true;
    for (size_t i = 0; i < pages; i++)
        if (!addresses[i] || (addresses[i] & (page - 1))) { aligned = false; break; }
    CHECK(aligned, "全DMAページの非zero・ページ整列");
    uint64_t past;
    CHECK(n_dma_addresses(mem, pages, &past, 1) == -N_ERR_INVALID, "DMA一覧の終端を確認");
    printf("pcitest dma: 全ページ=%llu 先頭=%llx 末尾=%llx\n", (unsigned long long)pages,
           (unsigned long long)addresses[0], (unsigned long long)addresses[pages - 1]);
    for (unsigned i = 0; i < 2; i++) {
        views[i] = n_shared_view(mem, i ? rounded - page : 0, page, N_CACHE_DEFAULT);
        result = views[i] < 0 ? views[i] : n_shared_map(views[i], page, N_MEMORY_READ_WRITE, &maps[i]);
        CHECK(result == 0 && maps[i], "DMA先頭/末尾ページの写像%u: %s", i, n_result_text(result));
    }
    if (maps[0] && maps[1]) {
        volatile uint8_t *first = maps[0], *last = maps[1];
        CHECK(first[0] == 0 && last[page - 1] == 0, "DMA先頭/末尾はzero初期化済み");
        first[0] = 0x5A; last[page - 1] = 0xA5;
        CHECK(first[0] == 0x5A && last[page - 1] == 0xA5, "DMA先頭/末尾へCPU読書き");
    }
done:
    for (unsigned i = 0; i < 2; i++) {
        if (maps[i]) CHECK(n_memory_release(maps[i], page) == 0, "DMA診断写像%uの解放", i);
        if (views[i] >= 0) CHECK(n_handle_close(views[i]) == 0, "DMA診断view%uの解放", i);
    }
    free(addresses);
    if (mem >= 0) CHECK(n_handle_close(mem) == 0, "DMA診断objectの解放");
    CHECK(n_handle_close(pci) == 0, "DMA診断claimの解放");
    printf("pcitest dma: %d件失敗（実GPU性能は未検証）\n", failed);
    return failed != 0;
}

int main(int argc,char **argv) {
    char program[4096]; /* argv[0] is as it was typed; an app is launched by its complete path */
    if(n_path_complete(argv[0],program,sizeof program)>=0)argv[0]=program;
    if(argc>1 && !strcmp(argv[1],"revoked-reader"))return revoked_reader();
    if(argc>1 && !strcmp(argv[1],"stuck-holder"))return stuck_holder();
    if(argc>1 && !strncmp(argv[1],"dma=",4))return dma_allocation_case(argv[1]+4);
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
    /* pcitest screen=<seconds>: only the given scanout, its second buffer shown that long */
    if(argc>1 && !strncmp(argv[1],"screen=",7)){
        struct n_pciinfo list[128];
        int n = pcilist(list, 128);
        give_screen_cases(-1, list, n, (unsigned)strtoul(argv[1] + 7, NULL, 10));
        printf("pcitest screen: %d failed\n", failed);
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
    acpi_refused_cases(pci, e);
    CHECK(n_pci_hold_screen(pci, true) == -N_ERR_NOT_FOUND, "edu has no screen to hold");
    give_screen_cases(pci, list, n, 0);
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
        if (info.flags & N_PCI_ISOLATED) {
            CHECK(back >= 0 && (n_pci_config_read(back, N_PCI_SELF, 4, 2) & 4) == 0,
                  "an isolated crashed driver's function has bus mastering off");
            struct n_pci_function recovery;
            CHECK(back >= 0 && n_pci_info(back, &recovery) == 0 && (recovery.flags & N_PCI_NEEDS_RECOVERY),
                  "EDU has no generic FLR: native recovery remains explicit");
        } else {
            CHECK(back == -N_ERR_NOT_READY, "a crashed nonisolated function is quarantined: %s", n_result_text(back));
        }
        if (back >= 0) n_handle_close(back);
    }
    screen_cases(list, n);
    printf("pcitest: %d failed\n", failed);
    return failed != 0;
}
