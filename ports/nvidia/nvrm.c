/* /bin/nvrm: NVIDIA's Resource Manager as a user-space driver app.
 *
 * init claims the GPU's PCI function and starts this app with the N_KIND_PCI right as handle 3
 * (docs/nocturne-native-abi.md, section 9). nvrm brings RM up on it the way NVIDIA's kernel
 * interface layer does when a device is probed and first opened, then serves RM's client API
 * (nvrm_serve.c) to other apps:
 *
 *   options (NVRM_REGISTRY_DWORDS) -> rm_init_rm -> n_pci_info / BAR0 -> rm_is_supported_device
 *   -> rm_init_private_state -> interrupts -> rm_init_adapter (GSP firmware boots here)
 *   -> rm_get_device_name -> nvrm_serve
 *
 * Firmware (gsp_ga10x.bin, and for the log gsp_log_*.bin) comes from
 * /usr/share/firmware/nvidia/615.78.08/ or /data/firmware/nvidia/615.78.08/ (os.c).
 */
#include "nvrm_os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

/* ------------------------------------------------------------------------------------------
 * Interrupts: one thread takes the device's message-signalled interrupts from the system and
 * runs RM's interrupt handler for them, as the kernel interface does in its IRQ handler. RM
 * serialises itself with its own locks (the GPU lock and its spinlocks, which are mutexes
 * here); nothing in nvrm adds a lock around rm_isr.
 * ---------------------------------------------------------------------------------------- */
static int irq_thread(void *unused) {
    (void)unused;
    nvidia_stack_t *sp = nvrm_dev.sp;
    nv_state_t *nv = &nvrm_dev.nv;
    for (;;) {
        n_result r = n_pci_interrupt_take(nvrm_dev.pci, -1);
        if (r == -N_ERR_TIMEOUT) continue;
        if (r < 0) {
            nvrm_log("NVRM: interrupt wait ended: %s", n_result_text(r));
            return 0;
        }
        NvU32 need_bottom_half = 0;
        nvrm_in_isr = true;
        NvBool handled = rm_isr(sp, nv, &need_bottom_half);
        nvrm_in_isr = false;
        if (handled && need_bottom_half) rm_isr_bh(sp, nv);
    }
}

int nvrm_irq_thread_start(void) {
    int vectors = nvrm_irq_enable();
    if (vectors <= 0) {
        nvrm_log("NVRM: no interrupt vector for the GPU (%d); GSP messages are polled", vectors);
        return vectors;
    }
    thrd_t t;
    if (thrd_create(&t, irq_thread, NULL) != thrd_success) {
        nvrm_log("NVRM: cannot start the interrupt thread");
        return -1;
    }
    thrd_detach(t);
    return vectors;
}

/* ------------------------------------------------------------------------------------------
 * Bring-up
 * ---------------------------------------------------------------------------------------- */
static const char *status_text(NV_STATUS s) {
    switch (s) {
    case NV_OK: return "ok";
    case NV_ERR_GENERIC: return "generic error";
    case NV_ERR_GPU_IS_LOST: return "GPU lost (does not answer on the bus)";
    case NV_ERR_NOT_SUPPORTED: return "not supported";
    case NV_ERR_INVALID_STATE: return "invalid state";
    case NV_ERR_OPERATING_SYSTEM: return "operating system error";
    case NV_ERR_INSUFFICIENT_RESOURCES: return "insufficient resources";
    default: return "see nvstatuscodes.h";
    }
}

static bool bring_up(void) {
    struct nvrm_device *d = &nvrm_dev;
    nvidia_stack_t *sp;
    nv_state_t *nv = &d->nv;

    nvrm_work_init();
    nvrm_timers_init();

    NV_STATUS st = nvrm_pci_attach();
    if (st != NV_OK) {
        nvrm_log("NVRM: cannot attach to the PCI function: %s (0x%x)", status_text(st), (unsigned)st);
        return false;
    }
    sp = d->sp;
    nvrm_log("NVRM: GPU %04x:%04x at %02x:%02x.%x, BAR0 %llu MiB at 0x%llx",
             (unsigned)d->info.vendor, (unsigned)d->info.device, (unsigned)d->info.bus,
             (unsigned)d->info.dev, (unsigned)d->info.func,
             (unsigned long long)(nv->bars[0].size >> 20), (unsigned long long)nv->bars[0].cpu_address);

    /* RM's options: the equivalent of the module parameters, as "name=value;name=value". */
    const char *options = getenv("NVRM_REGISTRY_DWORDS");
    if (options && *options) {
        nvrm_log("NVRM: registry options: %s", options);
        rm_parse_option_string(sp, options);
    }

    if (!rm_init_rm(sp)) {
        nvrm_log("NVRM: rm_init_rm failed");
        return false;
    }

    st = rm_is_supported_device(sp, nv);
    if (st != NV_OK) {
        nvrm_log("NVRM: this GPU is not supported by RM %s: %s (0x%x)", NVRM_FIRMWARE_VERSION,
                 status_text(st), (unsigned)st);
        return false;
    }

    if (!rm_init_private_state(sp, nv)) {
        nvrm_log("NVRM: rm_init_private_state failed");
        return false;
    }
    if (!rm_init_event_locks(sp, nv)) {
        nvrm_log("NVRM: rm_init_event_locks failed");
        return false;
    }
    rm_set_rm_firmware_requested(sp, nv);

    nvrm_irq_thread_start();

    nvrm_log("NVRM: initialising the GPU (GSP firmware %s)...", NVRM_FIRMWARE_VERSION);
    if (!rm_init_adapter(sp, nv)) {
        nvrm_log("NVRM: rm_init_adapter failed; the GPU did not start");
        return true; /* RM's client API (without a working GPU) is still served for diagnosis */
    }
    d->adapter_ready = true;

    const char *name = rm_get_device_name(d->info.device, d->info.subsystem_vendor, d->info.subsystem_id);
    snprintf(d->name, sizeof d->name, "%s", name ? name : "(unknown)");
    nvrm_log("NVRM: GPU started: %s", d->name);
    return true;
}

static void tear_down(void) {
    struct nvrm_device *d = &nvrm_dev;
    if (d->sp && d->adapter_ready) {
        rm_disable_adapter(d->sp, &d->nv);
        rm_shutdown_adapter(d->sp, &d->nv);
        d->adapter_ready = false;
    }
    if (d->sp) {
        rm_free_private_state(d->sp, &d->nv);
        rm_shutdown_rm(d->sp);
    }
    nvrm_pci_detach();
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    nvrm_log_init();
    nvrm_log("NVRM: Nocturne user-space RM %s", NVRM_FIRMWARE_VERSION);

    if (!bring_up()) {
        tear_down();
        return 1;
    }
    n_result r = nvrm_serve();
    nvrm_log("NVRM: request server ended: %s", n_result_text(r));
    tear_down();
    return 0;
}
