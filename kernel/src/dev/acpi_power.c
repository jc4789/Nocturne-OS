/* ACPI の装置の電源（hal/hal.h の struct hal_firmware_ops、ACPI 6.5 7.1〜7.3）：装置の D 状態（_PS0、_PS3、_PSC）と、
   装置が D0 で要る電源資源（_PR0 と、資源の _ON、_OFF、_STA）。

   電源資源は機械に一つずつあり、装置の _PR0 が参照する。資源は D0 にある装置の参照を数え、参照が 0 になれば切る
   （7.3：どの装置も参照しない資源は OFF）。数は AML の初期化の後に名前空間の全部の Device から始める
   （acpi_power_init）：_PSC が 0 か、_PSC が無く _PR0 の資源が全部 _STA で ON の装置は D0 で、参照を持つ。実装の無い
   装置の参照も数えるので、共有の資源を切らない。参照の無い ON の資源には初めは触れない（_PR3 や _PRW の資源かも
   しれない）。
   資源は装置ごとの要る資源の中で、_ON を順の値（resourceorder）の小さい方から、_OFF を大きい方から行う（7.2）。
   _ON と _OFF は繰り返してよい（7.2.2、7.2.3）。記録の on と違えば、参照の向きに合わせて呼ぶ。_ON、_OFF、_STA の
   無い資源（7.2.5 の受け身の資源）は呼ばない。
   装置の記録は acpi_power_init か、初めて電源を聞かれた時に作り、消さない。_PSx も _PR0 も無い装置の記録は D0 の
   まま何もしない。
   錠：power_mutex が記録、数、AML の呼出しを一つずつにする（7.3.15 の _IRC も一つずつを求める）。HAL は電源の walk の
   装置ごとの thread と、結ぶ前の thread から並んで呼ぶ。acpi_power_restore だけは錠を取らない：ほかの CPU は止まり、
   walk も結ぶ前の thread も走っていない。 */
#include "kernel.h"
#include "dev/acpi.h"
#include "hal/hal.h"
#include "lib/avl.h"
#include "mm/heap.h"
#include "sys/wait.h"
#include <uacpi/uacpi.h>
#include <uacpi/namespace.h>

struct power_resource {
    struct avl_node in_tree;
    uacpi_namespace_node *node;
    uint16_t order;  /* resourceorder */
    bool passive;    /* _ON、_OFF、_STA が無い */
    bool on;         /* 最後に呼んだか読んだ状態 */
    unsigned refs;   /* D0 にある装置の _PR0 の参照 */
};

struct power_device {
    struct avl_node in_tree;
    uacpi_namespace_node *node;
    struct power_resource **needs; /* _PR0。順の値の小さい順 */
    unsigned need_count;
    bool ps0, ps3; /* _PS0、_PS3 がある */
    bool d0;       /* D0 にあり、needs の参照を持つ */
};

static struct mutex power_mutex;
static struct avl_tree resources, devices;

static struct power_resource *resource_of(const struct avl_node *n) {
    return (struct power_resource *)((char *)n - __builtin_offsetof(struct power_resource, in_tree));
}
static struct power_device *device_of(const struct avl_node *n) {
    return (struct power_device *)((char *)n - __builtin_offsetof(struct power_device, in_tree));
}
static bool resource_before(const struct avl_node *a, const struct avl_node *b) {
    return (uintptr_t)resource_of(a)->node < (uintptr_t)resource_of(b)->node;
}
static bool device_before(const struct avl_node *a, const struct avl_node *b) {
    return (uintptr_t)device_of(a)->node < (uintptr_t)device_of(b)->node;
}
static const struct avl_order resource_order = {.before = resource_before};
static const struct avl_order device_order = {.before = device_before};

static struct power_resource *resource_find(uacpi_namespace_node *node) {
    for (struct avl_node *n = resources.root; n;) {
        struct power_resource *r = resource_of(n);
        if (r->node == node) return r;
        n = (uintptr_t)node < (uintptr_t)r->node ? n->left : n->right;
    }
    return NULL;
}

static struct power_device *device_find(uacpi_namespace_node *node) {
    for (struct avl_node *n = devices.root; n;) {
        struct power_device *p = device_of(n);
        if (p->node == node) return p;
        n = (uintptr_t)node < (uintptr_t)p->node ? n->left : n->right;
    }
    return NULL;
}

static bool has(uacpi_namespace_node *node, const char *name) {
    uacpi_namespace_node *found;
    return uacpi_namespace_node_find(node, name, &found) == UACPI_STATUS_OK;
}

static void node_say(uacpi_namespace_node *node, const char *what, const char *why) {
    const uacpi_char *path = uacpi_namespace_node_generate_absolute_path(node);
    kprintf("acpi: %s %s: %s\n", path ? path : "?", what, why);
    if (path) uacpi_free_absolute_path(path);
}

static void complain(uacpi_namespace_node *node, const char *method, uacpi_status st) {
    node_say(node, method, uacpi_status_to_string(st));
}

static bool resource_switch(struct power_resource *r, bool on) {
    uacpi_status st = uacpi_execute_simple(r->node, on ? "_ON" : "_OFF");
    if (st != UACPI_STATUS_OK) {
        complain(r->node, on ? "_ON" : "_OFF", st);
        return false;
    }
    r->on = on;
    return true;
}

/* 節の資源の記録。初めてなら作り、_STA で状態を読む。NULL：資源でないか、記憶が無い。 */
static struct power_resource *resource_get(uacpi_namespace_node *node) {
    struct power_resource *r = resource_find(node);
    if (r) return r;
    uacpi_object *object = NULL;
    uacpi_power_resource_info info;
    bool ok = uacpi_eval(node, UACPI_NULL, UACPI_NULL, &object) == UACPI_STATUS_OK &&
              uacpi_object_get_power_resource_info(object, &info) == UACPI_STATUS_OK;
    if (object) uacpi_object_unref(object);
    if (!ok || !(r = kzalloc(sizeof *r))) return NULL;
    r->node = node;
    r->order = info.resource_order;
    r->passive = !has(node, "_ON") || !has(node, "_OFF") || !has(node, "_STA");
    uacpi_u64 sta = 1;
    if (!r->passive) uacpi_eval_simple_integer(node, "_STA", &sta);
    r->on = sta & 1;
    avl_insert(&resources, &resource_order, &r->in_tree);
    return r;
}

/* _PR0 の資源を順の値の小さい順に。読めない要素はログに書いて飛ばす。 */
static void needs_read(struct power_device *p) {
    uacpi_object *list = NULL;
    uacpi_object_array items;
    if (uacpi_eval_simple_package(p->node, "_PR0", &list) != UACPI_STATUS_OK) return;
    if (uacpi_object_get_package(list, &items) == UACPI_STATUS_OK && items.count &&
        (p->needs = kmalloc(items.count * sizeof *p->needs))) {
        for (uacpi_size i = 0; i < items.count; i++) {
            uacpi_namespace_node *target = NULL;
            struct power_resource *r = NULL;
            if (uacpi_object_is_aml_namepath(items.objects[i]) &&
                uacpi_object_resolve_as_aml_namepath(items.objects[i], p->node, &target) == UACPI_STATUS_OK)
                r = resource_get(target);
            if (!r) {
                node_say(p->node, "_PR0", "an element is no power resource; left out");
                continue;
            }
            unsigned at = p->need_count++;
            for (; at && p->needs[at - 1]->order > r->order; at--) p->needs[at] = p->needs[at - 1];
            p->needs[at] = r;
        }
    }
    uacpi_object_unref(list);
}

/* 節の装置の記録。初めてなら作り、今の状態を読んで、D0 なら参照を数える。NULL：記憶が無い。 */
static struct power_device *device_get(uacpi_namespace_node *node) {
    struct power_device *p = device_find(node);
    if (p) return p;
    if (!(p = kzalloc(sizeof *p))) return NULL;
    p->node = node;
    p->ps0 = has(node, "_PS0");
    p->ps3 = has(node, "_PS3");
    needs_read(p);
    uacpi_u64 psc;
    if (uacpi_eval_simple_integer(node, "_PSC", &psc) == UACPI_STATUS_OK) {
        p->d0 = psc == 0;
    } else {
        p->d0 = true;
        for (unsigned i = 0; i < p->need_count; i++) p->d0 &= p->needs[i]->on;
    }
    if (p->d0)
        for (unsigned i = 0; i < p->need_count; i++) p->needs[i]->refs++;
    avl_insert(&devices, &device_order, &p->in_tree);
    return p;
}

static bool managed(const struct power_device *p) { return p->ps0 || p->ps3 || p->need_count; }

/* _PS3 の後に参照を放し、参照の無くなった資源を順の値の大きい方から切る（7.3.11）。 */
static bool device_off(struct power_device *p, bool *cold) {
    bool ok = true;
    if (p->ps3) {
        uacpi_status st = uacpi_execute_simple(p->node, "_PS3");
        if (st != UACPI_STATUS_OK) {
            complain(p->node, "_PS3", st);
            ok = false;
        }
        *cold = true;
    }
    p->d0 = false;
    for (unsigned i = p->need_count; i-- > 0;) {
        struct power_resource *r = p->needs[i];
        r->refs--;
        if (r->refs || !r->on || r->passive) continue;
        ok &= resource_switch(r, false);
        *cold = true;
    }
    return ok;
}

/* 参照を取り、切れている資源を順の値の小さい方から入れてから _PS0（7.3.8）。 */
static bool device_on(struct power_device *p, bool *cold) {
    bool ok = true;
    for (unsigned i = 0; i < p->need_count; i++) {
        struct power_resource *r = p->needs[i];
        r->refs++;
        if (r->on || r->passive) continue;
        ok &= resource_switch(r, true);
    }
    if (p->ps0) {
        uacpi_status st = uacpi_execute_simple(p->node, "_PS0");
        if (st != UACPI_STATUS_OK) {
            complain(p->node, "_PS0", st);
            ok = false;
        }
    }
    p->d0 = true;
    *cold = true;
    return ok;
}

static bool firmware_power(struct firmware_node *node, enum hal_power_state to, bool *cold) {
    *cold = false;
    mutex_lock(&power_mutex);
    struct power_device *p = device_get(acpi_namespace_node(node));
    bool ok = p != NULL;
    if (!p) kprintf("acpi: no memory to keep a device's power\n");
    else if (!managed(p) || p->d0 == (to == HAL_D0)) ok = true;
    else if (to == HAL_D0) ok = device_on(p, cold);
    else ok = device_off(p, cold);
    mutex_unlock(&power_mutex);
    return ok;
}

static const struct hal_firmware_ops acpi_power_ops = {.power = firmware_power};

static uacpi_iteration_decision device_found(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)user;
    (void)depth;
    if (!device_get(node)) kprintf("acpi: no memory to keep a device's power\n");
    return UACPI_ITERATION_DECISION_CONTINUE;
}

void acpi_power_init(void) {
    mutex_lock(&power_mutex);
    uacpi_namespace_for_each_child(uacpi_namespace_root(), device_found, UACPI_NULL, UACPI_OBJECT_DEVICE_BIT,
                                   UACPI_MAX_DEPTH_ANY, UACPI_NULL);
    mutex_unlock(&power_mutex);
    hal_firmware_set(&acpi_power_ops);
}

static void resources_walk(struct avl_node *n, uint32_t *next, uint32_t order) {
    if (!n) return;
    resources_walk(n->left, next, order);
    struct power_resource *r = resource_of(n);
    if (!r->passive && r->refs) {
        if (r->order == order) resource_switch(r, true);
        else if (r->order > order && r->order < *next) *next = r->order;
    }
    resources_walk(n->right, next, order);
}

static void resources_read(struct avl_node *n) {
    if (!n) return;
    resources_read(n->left);
    struct power_resource *r = resource_of(n);
    uacpi_u64 sta;
    if (!r->passive && !r->refs && uacpi_eval_simple_integer(r->node, "_STA", &sta) == UACPI_STATUS_OK) r->on = sta & 1;
    resources_read(n->right);
}

void acpi_power_restore(void) {
    /* 参照のある資源を、順の値の段ごとに小さい方から入れ直す。参照の無い資源は今の状態を読み直す（S3 は system level
       の低い資源を切る、7.4.2.4） */
    for (uint32_t order = 0; order <= UINT16_MAX;) {
        uint32_t next = UINT16_MAX + 1;
        resources_walk(resources.root, &next, order);
        order = next;
    }
    resources_read(resources.root);
}
