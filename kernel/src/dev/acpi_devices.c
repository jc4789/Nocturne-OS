/* ACPI の名前空間の装置を HAL のバスにする（ACPI 6.5 6.1、6.2、6.5.1）。

   _HID か _CID を持つ Device を全部 hal_publish する。身元は字の性質で、資源は _CRS の中身である。PCI の機能は _HID を
   持たず（_ADR）、dev/pci.c が出す。CPU（ACPI0007）は arch/topology.c の台帳に載っている。 */
#include "kernel.h"
#include "arch/ioapic.h"
#include "arch/irq.h"
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
    uacpi_namespace_node *node;
    struct acpi_device *up;   /* 名前空間で一番近い、出した祖先 */
    struct acpi_device *next; /* 出した順の全部 */
    struct irq **irqs;
    unsigned irq_count;
    uint32_t memory_slots, port_slots, gsi_slots;
};

extern const struct hal_claim_ops hal_claim_plain;

static struct acpi_device *of(struct hal_device *d) { return (struct acpi_device *)d; }

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
    bool any = false;
    uacpi_id_string *hid = NULL;
    if (uacpi_eval_hid(a->node, &hid) == UACPI_STATUS_OK) {
        hal_prop_text(&a->hal, N_PROP_ACPI_ID, hid->value);
        any = true;
        uacpi_free_id_string(hid);
    }
    uacpi_pnp_id_list *cids = NULL;
    if (uacpi_eval_cid(a->node, &cids) == UACPI_STATUS_OK) {
        for (uint32_t i = 0; i < cids->num_ids; i++) hal_prop_text(&a->hal, N_PROP_ACPI_ID, cids->ids[i].value);
        any = any || cids->num_ids;
        uacpi_free_pnp_id_list(cids);
    }
    uacpi_id_string *uid = NULL;
    if (uacpi_eval_uid(a->node, &uid) == UACPI_STATUS_OK) {
        hal_prop_text(&a->hal, N_PROP_ACPI_UID, uid->value);
        uacpi_free_id_string(uid);
    }
    return any;
}

static bool is_processor(uacpi_namespace_node *node) {
    static const char *const ids[] = {"ACPI0007", NULL};
    return uacpi_device_matches_pnp_id(node, ids);
}

static struct acpi_device *current, *published;

static uacpi_iteration_decision walk_down(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)user;
    (void)depth;
    uacpi_u32 sta = STA_PRESENT | STA_FUNCTIONING;
    if (uacpi_eval_sta(node, &sta) != UACPI_STATUS_OK) sta = STA_PRESENT | STA_FUNCTIONING;
    /* ACPI 6.5 6.3.7：どちらでもなければ、子も無いものとする */
    if (!(sta & (STA_PRESENT | STA_FUNCTIONING))) return UACPI_ITERATION_DECISION_NEXT_PEER;
    if (!(sta & STA_PRESENT) || is_processor(node)) return UACPI_ITERATION_DECISION_CONTINUE;
    struct acpi_device *a = kzalloc(sizeof *a);
    if (!a) {
        kprintf("acpi: no memory for a device\n");
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    hal_device_init(&a->hal, N_BUS_ACPI, &acpi_bus_ops);
    a->node = node;
    if (!add_identity(a)) {
        kfree(a);
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    uacpi_for_each_device_resource(node, "_CRS", found_resource, a);
    const char *path = uacpi_namespace_node_generate_absolute_path(node);
    if (path) {
        hal_location(&a->hal, path);
        uacpi_free_absolute_path(path);
    }
    a->up = current;
    a->next = published;
    published = a;
    hal_publish(current ? &current->hal : NULL, &a->hal);
    current = a;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision walk_up(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)user;
    (void)depth;
    if (current && current->node == node) current = current->up;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* _DEP（ACPI 6.5 6.5.8）：名前の並び。 */
static void add_depends(struct acpi_device *a) {
    uacpi_object *result = NULL;
    if (uacpi_eval_typed(a->node, "_DEP", NULL, UACPI_OBJECT_PACKAGE_BIT, &result) != UACPI_STATUS_OK) return;
    uacpi_object_array list;
    if (uacpi_object_get_package(result, &list) == UACPI_STATUS_OK)
        for (uacpi_size i = 0; i < list.count; i++) {
            uacpi_namespace_node *on = NULL;
            if (uacpi_object_resolve_as_aml_namepath(list.objects[i], a->node, &on) != UACPI_STATUS_OK) continue;
            for (struct acpi_device *o = published; o; o = o->next)
                if (o->node == on && o != a) hal_depends(&a->hal, &o->hal);
        }
    uacpi_object_unref(result);
}

void acpi_devices_publish(void) {
    if (!acpi_aml_ready()) return;
    uacpi_namespace_for_each_child(uacpi_namespace_root(), walk_down, walk_up, UACPI_OBJECT_DEVICE_BIT, UACPI_MAX_DEPTH_ANY,
                                   NULL);
    for (struct acpi_device *a = published; a; a = a->next) add_depends(a);
}
