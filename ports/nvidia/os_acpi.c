/* NVIDIA RM on Nocturne: RM's ACPI methods (the port of kernel-open/nvidia/nv-acpi.c).
 *
 * RM evaluates a few ACPI objects of the GPU's own device node: _DSM (the GPU's and NVIDIA's
 * platform functions: GPS, JT, NBCI, MXM...), _ROM (the VBIOS image notebooks keep in system
 * firmware), _DOD (the display outputs), _DDC of an internal panel's child device, the MUX
 * methods MXDS/MXDM of a switchable output's child device, and _DSM of the upstream bridge for
 * D3cold. It also listens to the GPU node's Notify events (rm_acpi_notify: display hotplug, GPS
 * and D-notifier events) while the adapter runs, as the original's notifier does (nvrm.c starts
 * and stops it, outside RM's locks).
 *
 * Nocturne's kernel holds the AML interpreter (uACPI, kernel/src/dev/acpi.c); a driver app
 * reaches its claimed function's part of the namespace through the PCI claim: the function's
 * device node and the device nodes under it, and the upstream bridge's node, nothing outside.
 * The evaluation entry (n_pci_acpi_eval) and the notification feed (n_pci_acpi_notify_take) are
 * part of that claim's ABI. Where the firmware has no such method (or no AML at all), the method
 * answers as the original does then, and RM goes on without it.
 *
 * Outside the claim, and so not reachable here: the power source device (_PSR), batteries
 * (_BIX/_BIF), NVIDIA's platform controller (\_SB.NPCF) and the global NVIF/WMMX methods. RM's
 * answers for a platform without them are kept: power source unknown, no battery, no NVIF or
 * WMMX handle.
 */
#include "nvrm_os.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <os-interface.h>

/* The largest argument 3 of a _DSM RM may pass, as the original allows. */
#define NV_MAX_ACPI_DSM_PARAM_SIZE 1024

/* ---------------------------------------------------------------- one evaluation */
/* What an evaluation returned: nodes, with buffers and strings in the same block. */
struct acpi_result {
    void *block;
    const struct n_acpi_value *nodes;
    size_t size;
};

/* Room for the largest result RM asks for: a _ROM chunk (4 KiB), a _DSM result (its parameter
   limit), a 256-byte EDID, and their nodes. */
#define ACPI_RESULT_ROOM (64u * 1024u)

/* Evaluate path (relative to the node: "_DSM", "_ADR") with nargs arguments. */
static NV_STATUS acpi_eval(uint32_t node, const char *path, const struct n_acpi_value *args, uint32_t nargs,
                           const void *arg_data, size_t arg_data_size, struct acpi_result *out) {
    out->block = malloc(ACPI_RESULT_ROOM);
    if (!out->block) return NV_ERR_NO_MEMORY;
    n_result r = n_pci_acpi_eval(nvrm_dev.pci, node, path, args, nargs, arg_data, arg_data_size, out->block,
                                 ACPI_RESULT_ROOM);
    if (r < (n_result)sizeof(struct n_acpi_value)) {
        free(out->block);
        out->block = NULL;
        if (r == -N_ERR_NOT_FOUND) return NV_ERR_OBJECT_NOT_FOUND;
        if (r == -N_ERR_INVALID) return NV_ERR_INVALID_INDEX; /* no such node: past the last child */
        if (r == -N_ERR_UNSUPPORTED) return NV_ERR_NOT_SUPPORTED;
        if (r == -N_ERR_TOO_LARGE) return NV_ERR_BUFFER_TOO_SMALL;
        if (r == -N_ERR_DENIED) return NV_ERR_INSUFFICIENT_PERMISSIONS; /* a method the system keeps */
        return NV_ERR_OPERATING_SYSTEM;
    }
    out->nodes = out->block;
    out->size = (size_t)r;
    return NV_OK;
}

static void acpi_done(struct acpi_result *result) {
    free(result->block);
    result->block = NULL;
}

/* A node's buffer or string bytes, when they lie inside the result. */
static const uint8_t *acpi_bytes(const struct acpi_result *result, const struct n_acpi_value *v) {
    if (v->value > result->size || v->length > result->size - v->value) return NULL;
    return (const uint8_t *)result->block + v->value;
}

/* A package's element i, when it lies inside the result. */
static const struct n_acpi_value *acpi_element(const struct acpi_result *result, const struct n_acpi_value *package,
                                               uint32_t i) {
    size_t count = result->size / sizeof(struct n_acpi_value);
    if (package->value >= count || i >= count - package->value) return NULL;
    return &result->nodes[package->value + i];
}

static NV_STATUS acpi_eval_integer(uint32_t node, const char *path, NvU64 *value) {
    struct acpi_result result;
    NV_STATUS status = acpi_eval(node, path, NULL, 0, NULL, 0, &result);
    if (status != NV_OK) return status;
    status = result.nodes[0].type == N_ACPI_INTEGER ? NV_OK : NV_ERR_INVALID_DATA;
    if (status == NV_OK) *value = result.nodes[0].value;
    acpi_done(&result);
    return status;
}

/* RM's flattening of a result into its buffer (nv-acpi.c's nv_acpi_extract_*): integers as 4
   bytes (8 when they need it), buffers as their bytes, packages as their elements in order, an
   uninitialised object as nothing. */
static NV_STATUS acpi_extract(const struct acpi_result *result, const struct n_acpi_value *v, uint8_t *buffer,
                              NvU32 buffer_size, NvU32 *data_size, unsigned depth) {
    if (!v || depth > 32) return NV_ERR_INVALID_DATA;
    switch (v->type) {
    case N_ACPI_INTEGER: {
        bool wide = v->value >> 32;
        *data_size = wide ? sizeof(NvU64) : sizeof(NvU32);
        if (buffer_size < *data_size) return NV_ERR_BUFFER_TOO_SMALL;
        memcpy(buffer, &v->value, *data_size); /* little endian, as the original */
        return NV_OK;
    }
    case N_ACPI_BUFFER: {
        const uint8_t *bytes = acpi_bytes(result, v);
        if (!bytes) return NV_ERR_INVALID_DATA;
        *data_size = v->length;
        if (buffer_size < v->length) return NV_ERR_BUFFER_TOO_SMALL;
        memcpy(buffer, bytes, v->length);
        return NV_OK;
    }
    case N_ACPI_PACKAGE: {
        NvU32 total = 0;
        for (uint32_t i = 0; i < v->length; i++) {
            NvU32 element_size = 0;
            NV_STATUS status = acpi_extract(result, acpi_element(result, v, i), buffer + total, buffer_size - total,
                                            &element_size, depth + 1);
            if (status != NV_OK) {
                *data_size = total;
                return status;
            }
            total += element_size;
        }
        *data_size = total;
        return NV_OK;
    }
    case N_ACPI_NONE:
        *data_size = 0;
        return NV_OK;
    default:
        return NV_ERR_NOT_SUPPORTED;
    }
}

/* _DSM(GUID, revision, function, argument 3) on node; the result flattened into out. */
static NV_STATUS acpi_dsm(uint32_t node, const NvU8 *guid, NvU32 revision, NvU32 function, const void *arg3,
                          NvU16 arg3_size, bool arg3_integer, void *out, NvU16 *out_size) {
    uint8_t data[16 + NV_MAX_ACPI_DSM_PARAM_SIZE];
    if (arg3_size > NV_MAX_ACPI_DSM_PARAM_SIZE) return NV_ERR_INVALID_ARGUMENT;
    memcpy(data, guid, 16);
    struct n_acpi_value args[4] = {
        {.type = N_ACPI_BUFFER, .length = 16, .value = 0},
        {.type = N_ACPI_INTEGER, .value = revision},
        {.type = N_ACPI_INTEGER, .value = function},
    };
    size_t data_size = 16;
    if (arg3_integer) {
        NvU32 value;
        memcpy(&value, arg3, sizeof value);
        args[3] = (struct n_acpi_value){.type = N_ACPI_INTEGER, .value = value};
    } else {
        memcpy(data + 16, arg3, arg3_size);
        args[3] = (struct n_acpi_value){.type = N_ACPI_BUFFER, .length = arg3_size, .value = 16};
        data_size += arg3_size;
    }
    struct acpi_result result;
    NV_STATUS status = acpi_eval(node, "_DSM", args, 4, data, data_size, &result);
    if (status != NV_OK) return status == NV_ERR_NOT_SUPPORTED ? status : NV_ERR_OPERATING_SYSTEM;
    NvU32 size = 0;
    status = acpi_extract(&result, &result.nodes[0], out, *out_size, &size, 0);
    *out_size = (NvU16)size;
    if (status != NV_OK) nvrm_log("NVRM: _DSM function 0x%x: the result is not one RM reads", function);
    acpi_done(&result);
    return status;
}

/* The first device under the GPU's node whose _ADR passes match. A child without an _ADR the
   original could read (none, or not an integer) is skipped, as there. Any other failure ends the
   search: one of the claim or the system would come again for every number (a failed _ADR
   method answers as they do, NV_ERR_OPERATING_SYSTEM). */
static NV_STATUS acpi_find_child(bool (*match)(NvU64 address, NvU32 wanted), NvU32 wanted, uint32_t *node) {
    for (uint32_t i = 0;; i++) {
        NvU64 address;
        NV_STATUS status = acpi_eval_integer(N_ACPI_CHILD(i), "_ADR", &address);
        if (status == NV_ERR_INVALID_INDEX) return NV_ERR_GENERIC; /* past the last child: none matched */
        if (status != NV_OK && status != NV_ERR_OBJECT_NOT_FOUND && status != NV_ERR_INVALID_DATA) return status;
        if (status == NV_OK && match(address, wanted)) {
            *node = N_ACPI_CHILD(i);
            return NV_OK;
        }
    }
}

/* ---------------------------------------------------------------- notifications */
/* Notify(GPU node, value) to rm_acpi_notify on a thread of its own, from nvrm_acpi_notify_start
   to nvrm_acpi_notify_stop (nvrm_os.h). Not from nv_acpi_methods_init/uninit: RM calls those from
   rm_init_adapter and rm_shutdown_adapter holding its API and GPU locks, which rm_acpi_notify
   takes, so a join there could wait for a thread that waits for them. */
static thrd_t notify_thread;
static bool notify_running;
static atomic_bool notify_stop;

/* The values rm_acpi_notify handles (RM's platform/acpi_common.h): display hotplug
   (ACPI_VIDEO_NOTIFY_PROBE), the GPS status (ACPI_NOTIFY_GPS_STATUS_CHANGE) and the D-notifier's
   power levels (ACPI_NOTIFY_POWER_LEVEL_D1..D5). RM asserts on any other; those go to the log. */
static void notify_rm(n_result value) {
    if (value == 0x81 || value == 0xC0 || (value >= 0xD1 && value <= 0xD5))
        rm_acpi_notify(nvrm_dev.sp, &nvrm_dev.nv, (NvU32)value);
    else
        nvrm_log("NVRM: ACPI Notify 0x%x on the GPU is not one RM handles; dropped", (unsigned)value);
}

static int notify_loop(void *arg) {
    (void)arg;
    nvrm_thread_admin = true;
    while (!atomic_load(&notify_stop)) {
        n_result value = n_pci_acpi_notify_take(nvrm_dev.pci, 250);
        if (value == -N_ERR_TIMEOUT) continue;
        if (value < 0) break;
        notify_rm(value);
    }
    return 0;
}

void nvrm_acpi_notify_start(void) {
    if (notify_running) return;
    /* The first take starts the system's feed, so a Notify sent once this returns is kept (the
       D-notifier's state that rm_request_dnotifier_state asks the firmware for next). */
    n_result first = n_pci_acpi_notify_take(nvrm_dev.pci, 0);
    if (first < 0 && first != -N_ERR_TIMEOUT) {
        nvrm_log("NVRM: no ACPI notifications for the GPU (%s)", n_result_text(first));
        return; /* no node of its own (or no AML): nothing can come */
    }
    if (first >= 0) notify_rm(first);
    atomic_store(&notify_stop, false);
    notify_running = thrd_create(&notify_thread, notify_loop, NULL) == thrd_success;
    if (!notify_running) nvrm_log("NVRM: no thread for the GPU's ACPI notifications");
}

void nvrm_acpi_notify_stop(void) {
    if (!notify_running) return;
    atomic_store(&notify_stop, true);
    thrd_join(notify_thread, NULL);
    notify_running = false;
}

/* RM's "which global ACPI handles exist" (NVIF, WMMX); called when the adapter starts, inside
   RM's locks. */
void NV_API_CALL nv_acpi_methods_init(NvU32 *handlesPresent) {
    if (!handlesPresent) return;
    *handlesPresent = 0; /* NVIF and WMMX live outside the claim */
}

/* The adapter's shutdown, inside RM's locks: no handle was kept, nothing to let go. */
void NV_API_CALL nv_acpi_methods_uninit(void) {}

/* NVIF and WMMX: no handle was found (nv_acpi_methods_init), as the original then answers. */
NV_STATUS NV_API_CALL nv_acpi_method(NvU32 method, NvU32 function, NvU32 sub_function, void *in, NvU16 in_size,
                                     NvU32 *out_status, void *out, NvU16 *out_size) {
    (void)function; (void)sub_function; (void)in; (void)in_size; (void)out_status; (void)out;
    if (method == NV_EVAL_ACPI_METHOD_WMMX && out_size) *out_size = 0;
    return NV_ERR_NOT_SUPPORTED;
}

/* ---------------------------------------------------------------- the GPU's methods */
NV_STATUS NV_API_CALL nv_acpi_dsm_method(nv_state_t *nv, NvU8 *guid, NvU32 revision, NvBool nvpcf, NvU32 function,
                                         void *in, NvU16 in_size, NvU32 *out_status, void *out, NvU16 *out_size) {
    (void)out_status; /* the original does not report one for _DSM either */
    if (nv != &nvrm_dev.nv || !guid) return NV_ERR_INVALID_ARGUMENT;
    if (!in || in_size > NV_MAX_ACPI_DSM_PARAM_SIZE || !out || !out_size) return NV_ERR_INVALID_ARGUMENT;
    /* NVIDIA's platform controller (\_SB.NPCF) is not part of the GPU's namespace */
    if (nvpcf) return NV_ERR_NOT_SUPPORTED;
    return acpi_dsm(N_ACPI_SELF, guid, revision, function, in, in_size, false, out, out_size);
}

NV_STATUS NV_API_CALL nv_acpi_d3cold_dsm_for_upstream_port(nv_state_t *nv, NvU8 *guid, NvU32 revision,
                                                          NvU32 function, NvU32 *data) {
    if (nv != &nvrm_dev.nv || !guid || !data) return NV_ERR_INVALID_ARGUMENT;
    NvU32 result = 0;
    NvU16 size = sizeof result;
    NV_STATUS status = acpi_dsm(N_ACPI_UPSTREAM, guid, revision, function, data, sizeof *data, true, &result, &size);
    if (status == NV_OK) *data = result;
    return status;
}

/* The display output types ACPI gives an internal panel (ACPI 3.0, B.4.2). */
static bool is_panel(NvU64 address, NvU32 wanted) {
    (void)wanted;
    switch (address & 0xffff) {
    case 0x0110: case 0x0118: case 0x0400: case 0xA420: return true;
    default: return false;
    }
}

static bool is_address(NvU64 address, NvU32 wanted) { return address == wanted; }

/* The internal panel's EDID: _DDC(2) for 256 bytes when RM wants more than one block, else (or
   when that fails) _DDC(1) for 128. */
NV_STATUS NV_API_CALL nv_acpi_ddc_method(nv_state_t *nv, void *edid, NvU32 *size, NvBool multi_block) {
    if (nv != &nvrm_dev.nv || !edid || !size) return NV_ERR_INVALID_ARGUMENT;
    uint32_t panel;
    NV_STATUS status = acpi_find_child(is_panel, 0, &panel);
    if (status != NV_OK) return status;
    struct acpi_result result;
    status = NV_ERR_GENERIC;
    for (NvU32 blocks = multi_block ? 2 : 1; blocks >= 1 && status != NV_OK; blocks--) {
        struct n_acpi_value arg = {.type = N_ACPI_INTEGER, .value = blocks};
        status = acpi_eval(panel, "_DDC", &arg, 1, NULL, 0, &result);
    }
    if (status != NV_OK) return NV_ERR_GENERIC;
    const struct n_acpi_value *v = &result.nodes[0];
    const uint8_t *bytes = v->type == N_ACPI_BUFFER && v->length ? acpi_bytes(&result, v) : NULL;
    if (bytes && v->length > *size) status = NV_ERR_BUFFER_TOO_SMALL;
    else if (bytes) {
        memcpy(edid, bytes, v->length);
        *size = v->length;
    }
    acpi_done(&result);
    return status;
}

/* in[0] offset, in[1] length: that part of the VBIOS image the firmware keeps (_ROM). */
NV_STATUS NV_API_CALL nv_acpi_rom_method(nv_state_t *nv, NvU32 *in, NvU32 *out) {
    if (nv != &nvrm_dev.nv || !in || !out) return NV_ERR_INVALID_ARGUMENT;
    struct n_acpi_value args[2] = {
        {.type = N_ACPI_INTEGER, .value = in[0]},
        {.type = N_ACPI_INTEGER, .value = in[1]},
    };
    struct acpi_result result;
    if (acpi_eval(N_ACPI_SELF, "_ROM", args, 2, NULL, 0, &result) != NV_OK) return NV_ERR_GENERIC;
    const struct n_acpi_value *v = &result.nodes[0];
    const uint8_t *bytes = v->type == N_ACPI_BUFFER ? acpi_bytes(&result, v) : NULL;
    NV_STATUS status = NV_ERR_GENERIC;
    if (bytes && v->length >= in[1]) {
        memcpy(out, bytes, in[1]);
        status = NV_OK;
    } else {
        nvrm_log("NVRM: _ROM gave no image part of the asked length");
    }
    acpi_done(&result);
    return status;
}

/* The display output identifiers (_DOD): a package of integers, at most *size / 4 of them. */
NV_STATUS NV_API_CALL nv_acpi_dod_method(nv_state_t *nv, NvU32 *out, NvU32 *size) {
    if (nv != &nvrm_dev.nv || !out || !size) return NV_ERR_INVALID_ARGUMENT;
    NvU32 room = *size / sizeof(NvU32);
    struct acpi_result result;
    if (acpi_eval(N_ACPI_SELF, "_DOD", NULL, 0, NULL, 0, &result) != NV_OK) return NV_ERR_GENERIC;
    const struct n_acpi_value *v = &result.nodes[0];
    NV_STATUS status = NV_OK;
    *size = 0;
    if (v->type != N_ACPI_PACKAGE || v->length > room) {
        status = NV_ERR_GENERIC;
    } else {
        for (uint32_t i = 0; i < v->length; i++) {
            const struct n_acpi_value *e = acpi_element(&result, v, i);
            if (!e || e->type != N_ACPI_INTEGER) {
                status = NV_ERR_GENERIC;
                break;
            }
            out[i] = (NvU32)e->value;
            *size += sizeof(NvU32);
        }
    }
    acpi_done(&result);
    return status;
}

/* MXDS (the mux state) or MXDM (the mux mode) of the output device whose _ADR is mux_id. */
NV_STATUS NV_API_CALL nv_acpi_mux_method(nv_state_t *nv, NvU32 *in_out, NvU32 mux_id, const char *method) {
    if (!method || (strcmp(method, "MXDS") && strcmp(method, "MXDM"))) return NV_ERR_NOT_SUPPORTED;
    if (nv != &nvrm_dev.nv || !in_out) return NV_ERR_INVALID_ARGUMENT;
    uint32_t mux;
    NV_STATUS status = acpi_find_child(is_address, mux_id, &mux);
    if (status != NV_OK) return status;
    struct n_acpi_value arg = {.type = N_ACPI_INTEGER, .value = *in_out};
    struct acpi_result result;
    if (acpi_eval(mux, method, &arg, 1, NULL, 0, &result) != NV_OK) return NV_ERR_GENERIC;
    status = result.nodes[0].type == N_ACPI_INTEGER ? NV_OK : NV_ERR_GENERIC;
    if (status == NV_OK) *in_out = (NvU32)result.nodes[0].value;
    acpi_done(&result);
    return status;
}

/* ---------------------------------------------------------------- platform state outside the claim */
/* The AC adapter's _PSR: not part of the GPU's namespace. RM then treats the power source as
   unknown (it acts only on NV_OK). */
NV_STATUS NV_API_CALL nv_acpi_get_powersource(NvU32 *ac_plugged) {
    if (!ac_plugged) return NV_ERR_INVALID_ARGUMENT;
    return NV_ERR_NOT_SUPPORTED;
}

/* Batteries are not part of the GPU's namespace; the original answers false when it finds none. */
NvBool NV_API_CALL nv_acpi_is_battery_present(void) { return NV_FALSE; }
