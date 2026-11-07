/* ACPI の名前空間の装置を HAL のバスにする（ACPI 6.5 6.1、6.2、6.5.1）。

   _HID か _CID を持つ Device を全部 hal_publish する。身元は字の性質、資源は _CRS の中身、firmware の節は名前空間の
   節である。親は名前空間で一番近い、木に載った祖先：ACPI の装置か、_ADR で記述された PCI の機能（dev/pci.c が出す）。
   CPU（ACPI0007）は arch/topology.c の台帳に載っている。

   出すのは二度に分ける（dev/acpi.h）。acpi_devices_publish はホストブリッジ（PNP0A03、PNP0A08）の内を除く全部と
   ホストブリッジを出し、acpi_devices_publish_below は PCI の機能が載った後にホストブリッジの内を出す。I2C の HID や
   LPC の EC は、その制御器の機能の下に載る。_DEP は両方が済んでから結ぶ。 */
#include "kernel.h"
#include "arch/ioapic.h"
#include "arch/irq.h"
#include "arch/topology.h"
#include "dev/acpi.h"
#include "hal/hal.h"
#include "mm/heap.h"
#include <uacpi/namespace.h>
#include <uacpi/resources.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#define STA_PRESENT 1u      /* ACPI 6.5 6.3.7 */
#define STA_FUNCTIONING 8u

struct acpi_device {
    struct hal_device hal;
    struct acpi_device *next; /* 出した順の全部 */
    struct irq **irqs;
    unsigned irq_count;
    uint32_t memory_slots, port_slots, gsi_slots;
};

extern const struct hal_claim_ops hal_claim_plain;

static struct acpi_device *of(struct hal_device *d) { return (struct acpi_device *)d; }

static uacpi_namespace_node *node_of(const struct acpi_device *a) { return acpi_namespace_node(a->hal.firmware); }

/* _CRS の GSI を順に count まで。信号は _CRS のとおり（MADT が書いた pin は MADT のとおり、arch/ioapic.h）。
   Exclusive の GSI（ACPI 6.5 6.4.2.1、6.4.3.6）は誰とも線を分けない。一つでも立たなければ、そこで止める。 */
static unsigned acpi_irq_enable(struct hal_device *d, const struct irq_spec *specs, unsigned count, struct irq **irqs) {
    struct acpi_device *a = of(d);
    unsigned most = MIN(count, a->gsi_slots);
    if (!most || a->irqs) return 0;
    if (!(a->irqs = kzalloc(most * sizeof *a->irqs))) return 0;
    unsigned got = 0;
    for (; got < most; got++) {
        const struct n_hal_resource *r = hal_resource(d, N_HAL_GSI, got);
        struct gsi_signal signal = {.level = r->flags & N_HAL_GSI_LEVEL, .active_low = r->flags & N_HAL_GSI_ACTIVE_LOW,
                                    .shared = r->flags & N_HAL_GSI_SHARED};
        struct irq *irq = irq_request_gsi(&specs[got], (unsigned)r->base, signal);
        if (!irq) break;
        a->irqs[got] = irq;
        if (irqs) irqs[got] = irq;
    }
    a->irq_count = got;
    if (!got) {
        kfree(a->irqs);
        a->irqs = NULL;
    }
    return got;
}

static bool acpi_irq_disable(struct hal_device *d) {
    struct acpi_device *a = of(d);
    for (unsigned i = 0; i < a->irq_count; i++) irq_free(a->irqs[i]);
    kfree(a->irqs);
    a->irqs = NULL;
    a->irq_count = 0;
    return true;
}

static unsigned acpi_irq_max(struct hal_device *d) { return of(d)->gsi_slots; }

static const struct hal_bus_ops acpi_bus_ops = {
    .irq_enable = acpi_irq_enable, .irq_disable = acpi_irq_disable, .irq_max = acpi_irq_max, .claim = &hal_claim_plain};

static void add_memory(struct acpi_device *a, uint64_t base, uint64_t size, uint32_t flags) {
    if (size) hal_resource_add(&a->hal, N_HAL_MEMORY, a->memory_slots++, flags, base, size);
}

static void add_ports(struct acpi_device *a, uint64_t base, uint64_t size) {
    if (size) hal_resource_add(&a->hal, N_HAL_PORT, a->port_slots++, 0, base, size);
}

static void add_gsi(struct acpi_device *a, uint32_t gsi, uint8_t triggering, uint8_t polarity, uint8_t sharing) {
    uint32_t flags = (triggering == UACPI_TRIGGERING_LEVEL ? N_HAL_GSI_LEVEL : 0) |
                     (polarity == UACPI_POLARITY_ACTIVE_LOW ? N_HAL_GSI_ACTIVE_LOW : 0) |
                     (sharing == UACPI_SHARED ? N_HAL_GSI_SHARED : 0);
    hal_resource_add(&a->hal, N_HAL_GSI, a->gsi_slots++, flags, gsi, 1);
}

static void add_address(struct acpi_device *a, const uacpi_resource_address_common *c, uint64_t base, uint64_t size,
                        uint64_t offset) {
    if (c->type == UACPI_RANGE_MEMORY) {
        add_memory(a, base + offset, size, 0);
    } else if (c->type == UACPI_RANGE_IO) {
        add_ports(a, base + offset, size);
    }
}

static uacpi_iteration_decision found_resource(void *user, uacpi_resource *r) {
    struct acpi_device *a = user;
    switch (r->type) {
    case UACPI_RESOURCE_TYPE_IRQ:
        for (unsigned i = 0; i < r->irq.num_irqs; i++)
            add_gsi(a, ioapic_isa_gsi(r->irq.irqs[i]), r->irq.triggering, r->irq.polarity, r->irq.sharing);
        break;
    case UACPI_RESOURCE_TYPE_EXTENDED_IRQ:
        /* with a ResourceSource the numbers are that device's interrupts, not GSIs (6.4.3.6) */
        if (r->extended_irq.source.length) break;
        for (unsigned i = 0; i < r->extended_irq.num_irqs; i++)
            add_gsi(a, r->extended_irq.irqs[i], r->extended_irq.triggering, r->extended_irq.polarity,
                    r->extended_irq.sharing);
        break;
    case UACPI_RESOURCE_TYPE_IO: add_ports(a, r->io.minimum, r->io.length); break;
    case UACPI_RESOURCE_TYPE_FIXED_IO: add_ports(a, r->fixed_io.address, r->fixed_io.length); break;
    case UACPI_RESOURCE_TYPE_MEMORY24: add_memory(a, (uint64_t)r->memory24.minimum << 8, (uint64_t)r->memory24.length << 8, 0); break;
    case UACPI_RESOURCE_TYPE_MEMORY32: add_memory(a, r->memory32.minimum, r->memory32.length, 0); break;
    case UACPI_RESOURCE_TYPE_FIXED_MEMORY32: add_memory(a, r->fixed_memory32.address, r->fixed_memory32.length, 0); break;
    case UACPI_RESOURCE_TYPE_ADDRESS16:
        add_address(a, &r->address16.common, r->address16.minimum, r->address16.address_length, r->address16.translation_offset);
        break;
    case UACPI_RESOURCE_TYPE_ADDRESS32:
        add_address(a, &r->address32.common, r->address32.minimum, r->address32.address_length, r->address32.translation_offset);
        break;
    case UACPI_RESOURCE_TYPE_ADDRESS64:
        add_address(a, &r->address64.common, r->address64.minimum, r->address64.address_length, r->address64.translation_offset);
        break;
    default: break; /* GPIO、I2C、SPI の接続などは、それを読む実装が _CRS を読む */
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* _HID と _CID を性質にする。一つも無ければ false。 */
static bool add_identity(struct acpi_device *a) {
    uacpi_namespace_node *node = node_of(a);
    bool any = false;
    uacpi_id_string *hid = NULL;
    if (uacpi_eval_hid(node, &hid) == UACPI_STATUS_OK) {
        hal_prop_text(&a->hal, N_PROP_ACPI_ID, hid->value);
        any = true;
        uacpi_free_id_string(hid);
    }
    uacpi_pnp_id_list *cids = NULL;
    if (uacpi_eval_cid(node, &cids) == UACPI_STATUS_OK) {
        for (uint32_t i = 0; i < cids->num_ids; i++) hal_prop_text(&a->hal, N_PROP_ACPI_ID, cids->ids[i].value);
        any = any || cids->num_ids;
        uacpi_free_pnp_id_list(cids);
    }
    uacpi_id_string *uid = NULL;
    if (uacpi_eval_uid(node, &uid) == UACPI_STATUS_OK) {
        hal_prop_text(&a->hal, N_PROP_ACPI_UID, uid->value);
        uacpi_free_id_string(uid);
    }
    return any;
}

static bool is_processor(uacpi_namespace_node *node) {
    static const char *const ids[] = {"ACPI0007", NULL};
    return uacpi_device_matches_pnp_id(node, ids);
}

static bool is_pci_host(uacpi_namespace_node *node) {
    static const char *const ids[] = {"PNP0A03", "PNP0A08", NULL};
    return uacpi_device_matches_pnp_id(node, ids);
}

/* 巡りの途中の、名前空間の節から木の装置への道。一番上が、次に出す装置の親である。hold は hal_find_firmware で
   見つけた装置（巡りの間 hold する）。 */
struct frame {
    uacpi_namespace_node *node;
    struct hal_device *hal;
    bool hold;
};

struct walk {
    bool below;                  /* acpi_devices_publish_below：ホストブリッジの内 */
    struct frame *frames;
    unsigned depth, room;
};

static struct acpi_device *published;
static struct acpi_device **hosts; /* 出したホストブリッジ。acpi_devices_publish_below がその内を巡る */
static unsigned host_count;

static void frame_push(struct walk *w, uacpi_namespace_node *node, struct hal_device *hal, bool hold) {
    if (w->depth == w->room) {
        unsigned room = w->room ? w->room * 2 : 8;
        struct frame *grown = krealloc(w->frames, room * sizeof *grown);
        if (!grown) {
            /* 下の装置は、この節を飛ばして一つ上の装置の下に載る */
            kprintf("acpi: no memory to walk below %.4s\n", uacpi_namespace_node_name(node).text);
            if (hold) hal_put(hal);
            return;
        }
        w->frames = grown;
        w->room = room;
    }
    w->frames[w->depth++] = (struct frame){node, hal, hold};
}

static struct hal_device *frame_parent(const struct walk *w) { return w->depth ? w->frames[w->depth - 1].hal : NULL; }

static void host_remember(struct acpi_device *a) {
    struct acpi_device **grown = krealloc(hosts, (host_count + 1) * sizeof *grown);
    if (!grown) {
        kprintf("acpi: no memory to walk below host bridge %s\n", a->hal.location);
        return;
    }
    hosts = grown;
    hosts[host_count++] = a;
}

/* 節が _HID か _CID を持てば、その装置を出して true。 */
static bool device_publish(struct walk *w, uacpi_namespace_node *node) {
    struct acpi_device *a = kzalloc(sizeof *a);
    if (!a) {
        kprintf("acpi: no memory for a device\n");
        return false;
    }
    hal_device_init(&a->hal, N_BUS_ACPI, &acpi_bus_ops);
    a->hal.firmware = acpi_firmware(node);
    if (!add_identity(a)) {
        hal_device_discard(&a->hal);
        kfree(a);
        return false;
    }
    uacpi_for_each_device_resource(node, "_CRS", found_resource, a);
    const char *path = uacpi_namespace_node_generate_absolute_path(node);
    if (path) {
        hal_location(&a->hal, path);
        uacpi_free_absolute_path(path);
    }
    uint32_t domain;
    if (topology.nodes > 1 && acpi_proximity(a->hal.firmware, &domain)) a->hal.node = topology_node_of_domain(domain);
    a->next = published;
    published = a;
    hal_publish(frame_parent(w), &a->hal);
    frame_push(w, node, &a->hal, false);
    return true;
}

static uacpi_iteration_decision walk_down(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)depth;
    struct walk *w = user;
    uacpi_u32 sta = STA_PRESENT | STA_FUNCTIONING;
    if (uacpi_eval_sta(node, &sta) != UACPI_STATUS_OK) sta = STA_PRESENT | STA_FUNCTIONING;
    /* ACPI 6.5 6.3.7：どちらでもなければ、子も無いものとする */
    if (!(sta & (STA_PRESENT | STA_FUNCTIONING))) return UACPI_ITERATION_DECISION_NEXT_PEER;
    if (!(sta & STA_PRESENT) || is_processor(node)) return UACPI_ITERATION_DECISION_CONTINUE;
    if (device_publish(w, node)) {
        if (w->below || !is_pci_host(node)) return UACPI_ITERATION_DECISION_CONTINUE;
        host_remember(published);
        return UACPI_ITERATION_DECISION_NEXT_PEER; /* 内は PCI の機能が載ってから */
    }
    /* 身元の無い節：_ADR で記述された PCI の機能なら、下の装置はその機能の下に載る */
    struct hal_device *h = w->below ? hal_find_firmware(acpi_firmware(node)) : NULL;
    if (h) frame_push(w, node, h, true);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision walk_up(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)depth;
    struct walk *w = user;
    if (!w->depth || w->frames[w->depth - 1].node != node) return UACPI_ITERATION_DECISION_CONTINUE;
    struct frame *f = &w->frames[--w->depth];
    if (f->hold) hal_put(f->hal);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* _DEP（ACPI 6.5 6.5.8）：名前の並び。依存先は木の上のどの装置でもよい（ACPI の装置、PCI の機能）。 */
static void add_depends(struct acpi_device *a) {
    uacpi_object *result = NULL;
    if (uacpi_eval_typed(node_of(a), "_DEP", NULL, UACPI_OBJECT_PACKAGE_BIT, &result) != UACPI_STATUS_OK) return;
    uacpi_object_array list;
    if (uacpi_object_get_package(result, &list) == UACPI_STATUS_OK)
        for (uacpi_size i = 0; i < list.count; i++) {
            uacpi_namespace_node *on = NULL;
            if (uacpi_object_resolve_as_aml_namepath(list.objects[i], node_of(a), &on) != UACPI_STATUS_OK) continue;
            struct hal_device *o = hal_find_firmware(acpi_firmware(on));
            if (!o) continue;
            if (o != &a->hal) hal_depends(&a->hal, o);
            hal_put(o);
        }
    uacpi_object_unref(result);
}

void acpi_devices_publish(void) {
    if (!acpi_aml_ready()) return;
    struct walk w = {0};
    uacpi_namespace_for_each_child(uacpi_namespace_root(), walk_down, walk_up, UACPI_OBJECT_DEVICE_BIT,
                                   UACPI_MAX_DEPTH_ANY, &w);
    ASSERT(!w.depth);
    kfree(w.frames);
}

void acpi_devices_publish_below(void) {
    if (!acpi_aml_ready()) return;
    struct walk w = {.below = true};
    for (unsigned i = 0; i < host_count; i++) {
        frame_push(&w, node_of(hosts[i]), &hosts[i]->hal, false);
        uacpi_namespace_for_each_child(node_of(hosts[i]), walk_down, walk_up, UACPI_OBJECT_DEVICE_BIT,
                                       UACPI_MAX_DEPTH_ANY, &w);
        w.depth = 0;
    }
    kfree(w.frames);
    kfree(hosts);
    hosts = NULL;
    host_count = 0;
    for (struct acpi_device *a = published; a; a = a->next) add_depends(a);
}
