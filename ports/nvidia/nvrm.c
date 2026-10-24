/* nvrm: NVIDIA's Resource Manager as a user-space driver app (C:\Nocturne\System\nvrm).
 *
 * init claims the GPU's PCI function and starts this app with the N_KIND_PCI right as handle 3
 * (docs/nocturne-native-abi.md, section 9). nvrm brings RM up on it the way NVIDIA's kernel
 * interface layer does when a device is probed and first opened, then serves RM's client API
 * (nvrm_serve.c) to other apps:
 *
 *   options (NVRM_REGISTRY_DWORDS) -> rm_init_rm -> n_pci_info / BAR0 -> rm_is_supported_device
 *   -> rm_init_private_state -> interrupts -> rm_init_adapter (GSP firmware boots here)
 *   -> ACPI notifications (os_acpi.c) -> rm_get_device_name -> NVKMS (nvkms_os.c) and the
 *   screen (display.c) -> UVM (uvm_rm.c) -> nvrm_serve
 *
 * Firmware (gsp_ga10x.bin, and for the log gsp_log_*.bin) comes from
 * C:\Nocturne\Firmware\NVIDIA\615.78.08 (os.c).
 */
#include "nvrm_os.h"
#include "nvkms_os.h"
#include "display.h"
#include "uvm_nvrm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <os-interface.h>           /* os_free_mem */

/* ------------------------------------------------------------------------------------------
 * Interrupts: one thread takes the device's message-signalled interrupts from the system and
 * runs RM's interrupt handler for them, as the kernel interface does in its IRQ handler. RM
 * serialises itself with its own locks (the GPU lock and its spinlocks, which are mutexes
 * here); nothing in nvrm adds a lock around rm_isr.
 * ---------------------------------------------------------------------------------------- */
#define IRQ_WAKE_MS 50        /* how often a waiting interrupt thread looks at the stop flag */
#define IRQ_POLL_MS 1         /* polling mode: how often the GPU's interrupt status is read */

static thrd_t irq_thread_id;
static bool irq_thread_running;
static bool irq_stop;         /* atomic */
static bool irq_polling;      /* no usable vector: the same thread runs rm_isr on a timer */

static uint64_t irq_runs;     /* atomic: times rm_isr ran, for the bring-up progress lines */

/* nvidia_isr's order: RM copies the MMU faults it serves out of the shadow buffer, UVM's top
   half takes the replayable faults and access counters, then RM's interrupt handler. */
static void irq_run_rm(void) {
    __atomic_add_fetch(&irq_runs, 1, __ATOMIC_RELAXED);
    NvU32 need_bottom_half = 0, rm_faults = 0;
    nvrm_in_isr = true;
    rm_gpu_handle_mmu_faults(nvrm_dev.sp, &nvrm_dev.nv, &rm_faults);
    nvrm_uvm_interrupt();
    NvBool handled = rm_isr(nvrm_dev.sp, &nvrm_dev.nv, &need_bottom_half);
    nvrm_in_isr = false;
    if (handled && need_bottom_half) rm_isr_bh(nvrm_dev.sp, &nvrm_dev.nv);
}

static int irq_thread(void *unused) {
    (void)unused;
    while (!__atomic_load_n(&irq_stop, __ATOMIC_ACQUIRE)) {
        if (!irq_polling) {
            n_result r = n_pci_interrupt_take(nvrm_dev.pci, IRQ_WAKE_MS);
            if (r == -N_ERR_TIMEOUT) continue;
            if (r < 0) {
                /* The vector delivery broke: keep the GPU served by reading its interrupt
                   status instead of leaving RM without interrupts. */
                nvrm_log("NVRM: interrupt wait failed (%s); polling the GPU's interrupt status", n_result_text(r));
                irq_polling = true;
                continue;
            }
        } else {
            n_sleep_ms(IRQ_POLL_MS);
        }
        irq_run_rm();
    }
    return 0;
}

/* Interrupts are part of the GPU working: GSP answers arrive through them. With a vector the
   thread waits for it; without one (or when enabling it fails) the same thread reads RM's
   interrupt status every millisecond, which is what rm_isr does on an interrupt anyway. Only a
   thread that cannot be created is a failure. */
bool nvrm_irq_thread_start(void) {
    int vectors = nvrm_irq_enable();
    if (vectors <= 0) {
        nvrm_log("NVRM: no interrupt vector for the GPU (%d); polling its interrupt status every %d ms", vectors,
                 IRQ_POLL_MS);
        irq_polling = true;
    }
    __atomic_store_n(&irq_stop, false, __ATOMIC_RELEASE);
    if (thrd_create(&irq_thread_id, irq_thread, NULL) != thrd_success) {
        nvrm_log("NVRM: cannot start the interrupt thread");
        return false;
    }
    irq_thread_running = true;
    return true;
}

void nvrm_irq_thread_stop(void) {
    if (!irq_thread_running) return;
    __atomic_store_n(&irq_stop, true, __ATOMIC_RELEASE);
    thrd_join(irq_thread_id, NULL);
    irq_thread_running = false;
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

static bool events_ready;   /* rm_init_event_locks ran on the control device */

/* While rm_init_adapter runs (GSP boot prints nothing when it goes well), a line every
   PROGRESS_MS tells a stuck start from a slow one, and says whether interrupts arrive. */
#define PROGRESS_MS 10000
static mtx_t progress_lock;
static cnd_t progress_wake;
static bool progress_done;

static int progress_thread(void *start_ns) {
    uint64_t start = *(const uint64_t *)start_ns;
    mtx_lock(&progress_lock);
    while (!progress_done) {
        struct timespec until;
        timespec_get(&until, TIME_UTC);
        until.tv_sec += PROGRESS_MS / 1000;
        if (cnd_timedwait(&progress_wake, &progress_lock, &until) != thrd_timedout || progress_done) continue;
        nvrm_log("NVRM: still initialising the GPU after %llu s (%llu interrupts handled%s)",
                 (unsigned long long)((nvrm_now_ns() - start) / 1000000000ull),
                 (unsigned long long)__atomic_load_n(&irq_runs, __ATOMIC_RELAXED),
                 irq_polling ? ", polling" : "");
    }
    mtx_unlock(&progress_lock);
    return 0;
}

static bool init_adapter(nvidia_stack_t *sp, nv_state_t *nv) {
    uint64_t start = nvrm_now_ns();
    thrd_t watch;
    progress_done = false;
    bool watching = mtx_init(&progress_lock, mtx_plain) == thrd_success &&
                    cnd_init(&progress_wake) == thrd_success &&
                    thrd_create(&watch, progress_thread, &start) == thrd_success;
    bool ok = rm_init_adapter(sp, nv);
    if (watching) {
        mtx_lock(&progress_lock);
        progress_done = true;
        cnd_signal(&progress_wake);
        mtx_unlock(&progress_lock);
        thrd_join(watch, NULL);
        cnd_destroy(&progress_wake);
        mtx_destroy(&progress_lock);
    }
    nvrm_log("NVRM: rm_init_adapter %s after %llu ms", ok ? "finished" : "failed",
             (unsigned long long)((nvrm_now_ns() - start) / 1000000ull));
    return ok;
}

/* nv_check_and_exclude_gpu: whether the GPU stays unstarted because the options exclude it. */
static bool excluded(nvidia_stack_t *sp, nv_state_t *nv) {
    if (NVreg_ExcludeAllGpus) {
        nvrm_log("NVRM: excluding all GPUs as requested");
        nv->flags |= NV_FLAG_EXCLUDE;
        return true;
    }
    char *uuid = rm_get_gpu_uuid(sp, nv);
    if (!uuid) {
        nvrm_log("NVRM: unable to read the GPU UUID");
        return false;
    }
    bool out = false;
    if (nvrm_gpu_excluded(uuid)) {
        NV_STATUS st = rm_exclude_adapter(sp, nv);
        if (st != NV_OK) {
            nvrm_log("NVRM: failed to exclude GPU %s: %s (0x%x)", uuid, status_text(st), (unsigned)st);
        } else {
            nv->flags |= NV_FLAG_EXCLUDE;
            nvrm_log("NVRM: GPU %s is excluded (NVreg_ExcludedGpus); it is not started", uuid);
            out = true;
        }
    }
    os_free_mem(uuid);
    return out;
}

static bool bring_up(void) {
    struct nvrm_device *d = &nvrm_dev;
    nvidia_stack_t *sp;
    nv_state_t *nv = &d->nv;

    nvrm_work_init();
    nvrm_timers_init();
    if (!nvrm_timers_start()) {
        nvrm_log("NVRM: cannot start the timer thread");
        return false;
    }

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
    /* Client events (allocate/free/post) take the control device's lock, not the GPU's. */
    if (!rm_init_event_locks(sp, &nvrm_ctl_state)) {
        nvrm_log("NVRM: rm_init_event_locks failed");
        return false;
    }
    events_ready = true;
    /* The rest of the original's probe: the UUID, the per-device options, the firmware, and
       the exclusion lists (NVreg_ExcludeAllGpus, NVreg_ExcludedGpus), which keep the GPU from
       being started. */
    rm_get_gpu_uuid_raw(sp, nv);
    st = nvrm_registry_per_device(sp, nv);
    if (st != NV_OK)
        nvrm_log("NVRM: the per-device options are not valid: %s (0x%x)", status_text(st), (unsigned)st);
    rm_set_rm_firmware_requested(sp, nv);
    if (excluded(sp, nv)) return false;

    if (!nvrm_irq_thread_start()) return false;

    if (!nvrm_pci_bus_master(true)) return false;
    nvrm_log("NVRM: initialising the GPU (GSP firmware %s)...", NVRM_FIRMWARE_VERSION);
    if (!init_adapter(sp, nv)) {
        /* RmInitAdapter shut down what it had started (RmShutdownAdapter) before it failed. */
        nvrm_pci_bus_master(false);
        nvrm_log("NVRM: the GPU did not start");
        return true; /* RM's client API (without a working GPU) is still served for diagnosis */
    }
    d->adapter_ready = true;
    /* The GPU node's ACPI notifications, then RM's request for the D-notifier's state (on
       notebooks the firmware answers it with a Notify), as nv_start_device. */
    nvrm_acpi_notify_start();
    rm_request_dnotifier_state(sp, nv);

    const char *name = rm_get_device_name(d->info.device, d->info.subsystem_vendor, d->info.subsystem_id);
    snprintf(d->name, sizeof d->name, "%s", name ? name : "(unknown)");
    nvrm_log("NVRM: GPU started: %s", d->name);
    nvrm_log_gpu_information();

    /* The screen: NVKMS sets the mode and the system draws into a GPU surface (display.c).
       Either can fail with the reason in the log; the boot screen then stays as the firmware
       set it, and RM is served to clients all the same. */
    if (nvrm_nvkms_start()) nvrm_display_start();
    /* UVM (nvidia-uvm): unified memory for compute clients. Without it RM and the screen are
       served all the same, and NVRM_UVM requests are refused. */
    nvrm_uvm_start();
    return true;
}

/* Everything that enters RM from its own threads (ACPI notifications, interrupts, timers, work
   items) is stopped and joined before the state it uses is freed: the notifications, stop the
   GPU, then the interrupt thread, then RM's shutdown of the adapter (which still needs timers
   and work), then timers and workers. Each join happens here, holding none of RM's locks. */
static void tear_down(void) {
    struct nvrm_device *d = &nvrm_dev;
    /* UVM (every client's UVM state ended with the request server), the screen back to the
       firmware's framebuffer, then NVKMS out, while RM still runs. */
    nvrm_uvm_stop();
    nvrm_display_stop();
    nvrm_nvkms_stop();
    nvrm_acpi_notify_stop(); /* before the adapter goes, as nv_stop_device */
    if (d->sp && d->adapter_ready) rm_disable_adapter(d->sp, &d->nv);
    nvrm_irq_thread_stop();
    if (d->sp && d->adapter_ready) {
        rm_shutdown_adapter(d->sp, &d->nv);
        d->adapter_ready = false;
        nvrm_pci_bus_master(false); /* stopped: the system may reuse the DMA memory */
    }
    nvrm_timers_stop();
    nvrm_work_stop();
    if (d->sp) {
        rm_free_private_state(d->sp, &d->nv);
        if (events_ready) rm_destroy_event_locks(d->sp, &nvrm_ctl_state);
        events_ready = false;
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
    n_result r = nvrm_serve(N_PCI_TARGET(nvrm_dev.info.bus, nvrm_dev.info.dev, nvrm_dev.info.func));
    nvrm_log("NVRM: request server ended: %s", n_result_text(r));
    tear_down();
    return 0;
}
