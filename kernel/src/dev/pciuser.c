/* Drivers in apps (docs/nocturne-native-abi.md §9). Only init claims a PCI function; the claim
   is an object whose handle init passes to the driver app it launches. The app reads config space
   (its device, the bridges above it with their devices, the host bridges), writes its own, maps BARs as
   shared objects (syscall.c) and waits for its message-signalled interrupts on the handle.
   The kernel keeps what decides where memory and interrupts go: BAR registers and the MSI and
   MSI-X capabilities are written here only. Where an IOMMU is on (dev/iommu.h) the function's
   DMA reaches only the DMA objects made through its claim. The last handle's close stops the
   function: every mapping of its BARs, in any process, loses the device pages; then no
   interrupts, no bus mastering, and the IOMMU blocks it, after which the app's DMA memory can
   return to the allocator. Without a confirmed IOMMU block, DMA pages that outlive the claim
   stay out of use for good: neither BME nor Transactions Pending proves every posted write
   arrived. Release never waits for a failed device or resets it (syscall.c asks
   pci_user_dma_reusable).
   BAR and DMA objects hold the claim record, so a later claim of the same function is a new
   record and the old objects stay dead.
   Every call comes from any CPU. Claiming and a claim's end go one at a time under claim_mutex,
   which holds every function's reservation (d->claimed) and is taken before pci.c's reset and
   dev/iommu.c's locks. Each claim's irq_lock holds what its interrupt handlers read, on whichever
   CPUs its messages arrive; its notes_lock its ACPI Notify values. */
#include "kernel.h"
#include "abi.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/spin.h"
#include "dev/acpi.h"
#include "dev/fb.h"
#include "dev/fbcon.h"
#include "dev/iommu.h"
#include "dev/pci.h"
#include "dev/pciuser.h"
#include "gui/screenmode.h"
#include "gui/wm.h"
#include "mm/heap.h"
#include "sys/proc.h"
#include "sys/object.h"
#include "sys/process.h"
#include "sys/syscall.h"
#include "sys/thread.h"
#include "sys/wait.h"

#define MSIX_MOST 2048 /* the most entries an MSI-X table has (PCIe 6.0, 7.7.2.2): a function's most messages */
#define CLAIM_GUARDS 32  /* registers with bits only the kernel changes (guards_init) */
#define CLAIM_ACPI_NOTES 32 /* Notify values of the function's node not yet taken */

/* A config dword some of whose bits are the kernel's: a write that would change them is refused. */
struct guard {
    uint16_t offset;
    uint32_t bits;
};

/* A claim's messages as one pci_user_interrupts turned them on: each one's irq context, and a
   pending bit each (message n in word n / 64), set by its handler and cleared by take. A larger
   count makes a new block; the older ones stay with the claim, since an irq a remapping entry
   may still hold keeps its context (interrupts_off). */
struct claim_message {
    struct pci_claim *claim;
    unsigned n;
};
struct claim_vectors {
    struct claim_vectors *older;
    unsigned room;
    uint64_t *pending; /* [(room + 63) / 64], after messages */
    struct claim_message messages[];
};

struct pci_claim {
    struct pci_device *d;
    struct object object; /* what the claim's handle reaches */
    struct wait_queue wq;
    /* Its handlers, pending and vectors as they read them. They wake and notify under it, so once
       vectors is 0 no handler still reaches the claim. Before the wait queue and event locks. */
    struct spinlock irq_lock;
    unsigned vectors;              /* messages on: changed under irq_lock, by one pci_user_interrupts at a time */
    unsigned pending;              /* messages whose bit is set: changed under irq_lock; claim_ready reads it bare */
    struct claim_vectors *vec;     /* the newest block, set under irq_lock and vectors_mutex */
    struct mutex vectors_mutex;    /* pci_user_interrupts of the claim's threads, one at a time */
    unsigned refs;   /* the object and every BAR or DMA object made through it: atomic */
    bool released;   /* the last handle closed: BAR objects are revoked, the function stopped. Atomic */
    bool dma_unsafe; /* the ended claim's IOMMU block was not confirmed: retain its DMA pages. Atomic */
    unsigned screen_holds; /* n_pci_hold_screen nesting, under screen_lock; given back when the claim ends */
    struct guard guards[CLAIM_GUARDS];
    unsigned guard_count;
    uint16_t pmcsr; /* the power management capability's control register, or 0 */
    struct iommu_domain *domain; /* where an IOMMU isolates the function (dev/iommu.h) */
    struct pci_device *reset_port; /* where this claim's attempt sent a Secondary Bus Reset (bus_reset) */
    /* Its function's part of the ACPI namespace (n_pci_acpi_eval): the nodes, looked up at the
       first evaluation under acpi_lock, and the function node's Notify values from the first
       n_pci_acpi_notify_take on, kept until taken (a ring; the oldest give way). */
    struct mutex acpi_lock;
    bool acpi_looked;
    void *acpi_node, *acpi_upstream;
    struct acpi_watch *acpi_watch;
    uint32_t acpi_notes[CLAIM_ACPI_NOTES];
    unsigned acpi_first, acpi_count; /* notes_lock */
    struct spinlock notes_lock;      /* the ring above; before acpi_wq's lock */
    struct wait_queue acpi_wq;
    /* Its display outputs (struct pci_output), under screen_lock; and the flips its screens ask
       and the driver has not taken yet (flips, a slot for each output with buffers: a newer flip
       of an output takes its slot), under flip_lock, which the driver's n_pci_screen_flips waits
       on (flip_wq). */
    struct pci_output **outputs;
    size_t noutputs, outputs_room;
    struct spinlock flip_lock;
    struct wait_queue flip_wq;
    struct n_screen_flip *flips;
    size_t nflips, flips_room;
};

/* Claiming and a claim's end, and every function's d->claimed (dev/pci.h): one at a time, as a
   claim's resets and its group's reservations span sleeps. Before screen_lock, and before
   pci.c's and dev/iommu.c's locks. */
static struct mutex claim_mutex;

static void claim_drop(struct pci_claim *c) {
    unsigned left = __atomic_sub_fetch(&c->refs, 1, __ATOMIC_ACQ_REL);
    ASSERT(left != UINT32_MAX);
    if (left) return;
    if (c->domain) iommu_domain_free(c->domain); /* every DMA object of the claim is gone */
    for (struct claim_vectors *v = c->vec, *older; v; v = older) {
        older = v->older;
        kfree(v);
    }
    kfree(c);
}

/* Message n of the claim's, on the CPU its irq is delivered to. */
static enum irq_result claim_isr(void *context) {
    struct claim_message *m = context;
    struct pci_claim *c = m->claim;
    spin_take(&c->irq_lock);
    if (m->n < c->vectors) {
        uint64_t *word = &c->vec->pending[m->n / 64], bit = 1ull << (m->n % 64);
        if (!(*word & bit)) {
            *word |= bit;
            __atomic_store_n(&c->pending, c->pending + 1, __ATOMIC_RELEASE);
        }
        wq_wake_all(&c->wq);
        object_notify(&c->object);
    }
    spin_give(&c->irq_lock);
    return IRQ_HANDLED;
}

static void interrupts_off(struct pci_claim *c) {
    if (!c->vectors) return;
    uint64_t irq = spin_lock(&c->irq_lock);
    c->vectors = 0;
    memset(c->vec->pending, 0, (c->vec->room + 63) / 64 * sizeof *c->vec->pending);
    __atomic_store_n(&c->pending, 0, __ATOMIC_RELAXED);
    spin_unlock(&c->irq_lock, irq);
    /* irqs a remapping entry the units may still hold could reach stay (pci_irq_disable), their
       handlers given this claim: it stays with them */
    if (!pci_irq_disable(c->d)) __atomic_add_fetch(&c->refs, 1, __ATOMIC_ACQ_REL);
    /* A waiter must observe NOT_READY instead of sleeping forever after the
       driver disables its last interrupt (including an infinite take). */
    wq_wake_all(&c->wq);
    object_notify(&c->object);
}

static uint32_t claim_ready(struct handle *h) {
    return __atomic_load_n(&((struct pci_claim *)h->object->priv)->pending, __ATOMIC_ACQUIRE) ? N_EVENT_RECEIVABLE : 0;
}

/* The memory BAR the boot screen is drawn through, or -1. */
static int screen_bar(const struct pci_device *d) {
    if (!fb.addr) return -1;
    for (unsigned i = 0; i < pci_bar_count(d); i++) {
        uint32_t raw = d->bar[i];
        if (raw & 1) continue;
        uint64_t address = pci_bar_address(d, i), size = pci_bar_size(d, i);
        if (address && size && fb.phys >= address && fb.phys - address < size) return (int)i;
        if (((raw >> 1) & 3) == 2) i++;
    }
    return -1;
}

/* ---- a driver that holds the screen (n_pci_hold_screen, dev/fb.c) ----
   Only the function the boot screen lies in can hold it, so one claim at most does. Its hold
   ends when the driver gives it back or when the claim ends; claim_release stops the function
   first, so drawing never resumes while the driver can still reach the device. A hold that lasts
   as long as boot option fbhold=<seconds> says is ended the same way: the screen watch thread
   stops the driver's app, and its claim's end gives the screen back. No time ends a hold by
   default: RM's waits follow one another without a bound a limit could be checked against, and
   stopping a driver that is only slow loses its GPU. */
/* The hold the watch thread looks at: screen_holder and the three after it, under
   screen_hold_lock (taken alone, before screen_watch_wq's lock). The hold counts themselves and
   screen_watch_started change under screen_lock (pci_user_hold_screen). */
static struct spinlock screen_hold_lock;
static struct pci_claim *screen_holder;
static int screen_holder_app;
static uint64_t screen_held_since, screen_hold_limit_ns;
static bool screen_holder_stopped, screen_watch_started;
static struct wait_queue screen_watch_wq;

/* Stops the app whose driver holds the screen, once a hold: false when none does. */
static bool screen_holder_stop(const char *why) {
    uint64_t irq = spin_lock(&screen_hold_lock);
    int app = screen_holder && !screen_holder_stopped ? screen_holder_app : 0;
    struct pci_device *d = app ? screen_holder->d : NULL;
    if (app) screen_holder_stopped = true;
    spin_unlock(&screen_hold_lock, irq);
    if (!app) return false;
    kprintf("pci: %02x:%02x.%u: %s; stopping app %d; framebuffer writes resume at release, device recovery may be needed\n", d->bus,
            d->dev, d->func, why, app);
    if (process_kill(app) < 0) kprintf("pci: app %d cannot be stopped; the screen stays held\n", app);
    return true;
}

/* Started with the first hold, only when fbhold= sets a limit. */
static void screen_watch(void *arg) {
    (void)arg;
    char limit[64];
    ksnprintf(limit, sizeof limit, "its driver has held the screen for %lu s (fbhold=)",
              (unsigned long)(screen_hold_limit_ns / NS_PER_S));
    for (;;) {
        uint64_t irq = spin_lock(&screen_hold_lock);
        bool holding = screen_holder && !screen_holder_stopped;
        uint64_t until = holding ? screen_held_since + screen_hold_limit_ns : CLOCK_NEVER;
        if (holding && clock_now_ns() >= until) {
            spin_unlock(&screen_hold_lock, irq);
            screen_holder_stop(limit);
            continue;
        }
        /* a hold that begins or ends takes screen_hold_lock first, then wakes this */
        wq_wait_locked_until(&screen_watch_wq, &screen_hold_lock, until);
        irq_restore(irq);
    }
}

/* screen_lock held. */
static void screen_hold_begin(struct pci_claim *c) {
    if (!screen_watch_started) {
        screen_watch_started = true;
        uint64_t seconds;
        if (cmdline_number("fbhold", &seconds) && seconds) {
            /* at most a century: the hold's end stays a moment the clock reaches */
            screen_hold_limit_ns = MIN(seconds, (uint64_t)100 * 365 * 86400) * NS_PER_S;
            kthread_create("screen hold", screen_watch, NULL);
        }
    }
    /* Said while the screen still shows it: a screen that stops changing next tells why. */
    kprintf("pci: %02x:%02x.%u: app %d holds the screen while its driver starts or stops the device\n",
            c->d->bus, c->d->dev, c->d->func, process_current()->pid);
    int app = process_current()->pid;
    uint64_t irq = spin_lock(&screen_hold_lock);
    screen_holder = c;
    screen_holder_app = app;
    screen_held_since = clock_now_ns();
    screen_holder_stopped = false;
    wq_wake_all(&screen_watch_wq);
    spin_unlock(&screen_hold_lock, irq);
}

static void screen_hold_end(struct pci_claim *c) {
    uint64_t irq = spin_lock(&screen_hold_lock);
    if (screen_holder == c) screen_holder = NULL;
    wq_wake_all(&screen_watch_wq);
    spin_unlock(&screen_hold_lock, irq);
}

/* Whoever draws the screen (the desktop, or the text console when there is none) draws all of it
   again. */
static void screen_repaint(void) {
    wm_repaint();
    fbcon_repaint();
}

/* Gives back one hold. Nothing was drawn while the screen was held, so when the last hold ends
   all of it is drawn again. */
static void screen_release(void) {
    if (fb_release()) screen_repaint();
}

/* ---- a display driver's outputs (n_pci_give_screen, dev/fb.c) ----
   A display function's driver drives outputs, each a monitor. It offers the modes of an output's
   monitor (n_pci_screen_modes; the system chooses one, gui/screenmode.c), sets the one chosen up
   with buffers of its size and gives them to the output's screen (n_pci_give_screen): the screen
   takes that size, its frames are drawn into the buffer the output does not show, and each is
   asked for as a flip (n_pci_screen_flips); the driver says when the flip was shown, with the
   vertical blank the display engine measured (n_pci_screen_vblank). Output 0 of the function the
   boot screen is drawn through is screen 0; every other output of a function has a screen of its
   own, the same one each time a driver of that function drives it (output_screens). An output's
   screen is off when the driver gives its buffers back, and when its claim ends once the
   function is stopped; screen 0 then shows the firmware's framebuffer again, though the display
   engine may no longer show it: the driver that left reprogrammed it and nothing sets it back. */
static struct mutex screen_lock; /* the outputs of every claim, output_screens and the images */
struct output_screen {
    uint8_t bus, dev, func;
    uint32_t output;
    struct screen *screen;
};
static struct output_screen *output_screens;
static size_t noutput_screens, output_screens_room;

struct pci_output {
    struct pci_claim *claim;
    uint32_t output;
    struct screen *screen;
    bool modes;  /* it offers modes */
    bool direct; /* its buffers have N_SCANOUT_DIRECT */
    unsigned count; /* its buffers, 0: none given */
    struct shared_scanout surface[SCREEN_BUFFERS_MAX];
    struct scanout *buf[SCREEN_BUFFERS_MAX];
};

/* The images such a driver may show in an output's buffers' place (n_pci_screen_image): the panic
   screen is drawn into them too (dev/fb.c, fb_image_add), as no driver runs then to show the
   desktop's buffers again. They live while their output's buffers have N_SCANOUT_DIRECT. */
struct screen_image {
    struct screen_image *next;
    struct pci_output *output;
    int64_t number;          /* never 0 or negative */
    struct shared_scanout surface;
    struct scanout *out;
};
static struct screen_image *screen_images;
static int64_t screen_image_last;

static void screen_say(const struct pci_claim *c, uint32_t output, const char *what) {
    kprintf("pci: %02x:%02x.%u: output %u: %s\n", c->d->bus, c->d->dev, c->d->func, output, what);
}

/* Nothing is drawn into it any more, nor mapped or pinned. screen_lock held, out of the list. */
static void screen_image_free(struct screen_image *i) {
    fb_image_remove(i->out);
    fb_buffer_unmap(i->out);
    shared_scanout_close(&i->surface);
    kfree(i);
}

/* Every image of output o: when its buffers are gone or no longer N_SCANOUT_DIRECT. */
static void screen_images_end(struct pci_output *o) {
    for (struct screen_image **at = &screen_images; *at;) {
        struct screen_image *i = *at;
        if (i->output != o) {
            at = &i->next;
            continue;
        }
        *at = i->next;
        screen_image_free(i);
    }
}

/* The screen of output of function d: screen 0 for output 0 of the function the boot screen is
   drawn through, else the one it had before or a new one. screen_lock held; NULL without memory. */
static struct screen *output_screen_of(struct pci_device *d, uint32_t output) {
    if (output == 0 && screen_bar(d) >= 0) return screen_get(0);
    for (size_t i = 0; i < noutput_screens; i++) {
        struct output_screen *e = &output_screens[i];
        if (e->bus == d->bus && e->dev == d->dev && e->func == d->func && e->output == output) return e->screen;
    }
    struct screen *s = screen_new();
    if (!s) return NULL;
    struct output_screen *e = kappend(&output_screens, &noutput_screens, &output_screens_room, sizeof *e);
    if (!e) return NULL; /* the screen stays, off: screens are never freed */
    *e = (struct output_screen){d->bus, d->dev, d->func, output, s};
    return s;
}

/* Claim c's output, made when make (NULL: none, or no memory). screen_lock held. */
static struct pci_output *output_of(struct pci_claim *c, uint32_t output, bool make) {
    for (size_t i = 0; i < c->noutputs; i++)
        if (c->outputs[i]->output == output) return c->outputs[i];
    if (!make) return NULL;
    struct screen *s = output_screen_of(c->d, output);
    struct pci_output *o = s ? kzalloc(sizeof *o) : NULL;
    if (!o) return NULL;
    struct pci_output **at = kappend(&c->outputs, &c->noutputs, &c->outputs_room, sizeof *at);
    if (!at) {
        kfree(o);
        return NULL;
    }
    *o = (struct pci_output){.claim = c, .output = output, .screen = s};
    *at = o;
    return o;
}

/* Whether c gives a screen buffers now. screen_lock held. */
static bool claim_shows(struct pci_claim *c) {
    for (size_t i = 0; i < c->noutputs; i++)
        if (c->outputs[i]->count) return true;
    return false;
}

/* The screen's frame asks for a flip (dev/fb.h): into the claim's slot for the output, which a
   flip the driver has not taken yet gives up. In the compositor's thread; the slot is there
   (give_buffers made room for every output with buffers). */
static void output_flip(struct screen *s, void *owner, unsigned buffer, uint64_t serial) {
    (void)s;
    struct pci_output *o = owner;
    struct pci_claim *c = o->claim;
    struct n_screen_flip f = {o->output, buffer, serial};
    uint64_t irq = spin_lock(&c->flip_lock);
    size_t i = 0;
    while (i < c->nflips && c->flips[i].output != o->output) i++;
    if (i == c->nflips && c->nflips < c->flips_room) c->nflips++;
    if (i < c->nflips) c->flips[i] = f;
    spin_unlock(&c->flip_lock, irq);
    wq_wake_all(&c->flip_wq);
}
static const struct screen_ops output_ops = {.flip = output_flip};

/* The output's buffers to its screen (count 0: given back, the screen off or screen 0 the boot
   screen again). The old ones are taken away once no frame writes there, then unmapped and their
   surfaces closed; a flip of them the driver has not taken goes. Returns the serial of the last
   flip of the old buffers (dev/fb.h). screen_lock held. */
static uint64_t output_buffers(struct pci_output *o, const struct screen_mode_desc *desc, struct shared_scanout *surface,
                               struct scanout **buf, unsigned count) {
    struct shared_scanout old_surface[SCREEN_BUFFERS_MAX];
    struct scanout *old_buf[SCREEN_BUFFERS_MAX];
    unsigned old = o->count;
    memcpy(old_surface, o->surface, sizeof old_surface);
    memcpy(old_buf, o->buf, sizeof old_buf);
    memset(o->surface, 0, sizeof o->surface);
    memset(o->buf, 0, sizeof o->buf);
    for (unsigned i = 0; i < count; i++) {
        o->surface[i] = surface[i];
        o->buf[i] = buf[i];
    }
    o->count = count;
    o->direct = count && desc->direct;
    if (!o->direct) screen_images_end(o); /* nothing is shown in the buffers' place now */
    uint64_t last = screen_set_buffers(o->screen, &output_ops, o, desc, buf, count, 0);
    struct pci_claim *c = o->claim;
    uint64_t irq = spin_lock(&c->flip_lock);
    for (size_t i = 0; i < c->nflips; i++)
        if (c->flips[i].output == o->output && c->flips[i].serial <= last) {
            c->flips[i] = c->flips[--c->nflips];
            break;
        }
    spin_unlock(&c->flip_lock, irq);
    for (unsigned i = 0; i < old; i++) {
        fb_buffer_unmap(old_buf[i]);
        shared_scanout_close(&old_surface[i]);
    }
    wm_screen_direct(screen_id(o->screen), o->direct);
    return last;
}

/* Room in the claim's flips for every output with buffers. */
static bool flips_room(struct pci_claim *c) {
    size_t want = c->noutputs;
    if (want <= c->flips_room) return true;
    struct n_screen_flip *room = kmalloc(want * sizeof *room);
    if (!room) return false;
    uint64_t irq = spin_lock(&c->flip_lock);
    memcpy(room, c->flips, c->nflips * sizeof *room);
    struct n_screen_flip *old = c->flips;
    c->flips = room;
    c->flips_room = want;
    spin_unlock(&c->flip_lock, irq);
    kfree(old);
    return true;
}

/* Every output of c given back, its modes withdrawn: its claim ended. screen_lock held. True when
   it gave a screen buffers. */
static bool outputs_end(struct pci_claim *c) {
    bool shown = false;
    for (size_t i = 0; i < c->noutputs; i++) {
        struct pci_output *o = c->outputs[i];
        if (o->count) {
            shown = true;
            output_buffers(o, NULL, NULL, NULL, 0);
            screen_say(c, o->output, "its driver ended while a screen was drawn into its buffers; the screen is off "
                                     "(screen 0 the boot screen again, which the display may not show: nothing sets the "
                                     "display engine back)");
        }
        if (o->modes) screen_modes_gone(screen_id(o->screen));
        kfree(o);
    }
    kfree(c->outputs);
    c->outputs = NULL;
    c->noutputs = c->outputs_room = 0;
    kfree(c->flips);
    c->flips = NULL;
    c->nflips = c->flips_room = 0;
    return shown;
}

/* A driver that is done with its device stops it and then switches bus mastering off: that is
   how it tells the kernel the device will not reach memory again. A device that still masters
   when its claim ends lost its driver in the middle (a crash, or a driver that never stopped
   it), and what runs on it may be running still: a GPU's own firmware keeps the addresses it was
   given, and switching bus mastering off only pauses it until the next driver switches it on.
   Record that recovery is needed, but do not wait for it in process exit. A later non-display
   claim attempts FLR; the boot display cannot be generically reset without losing its scanout.
   Where an IOMMU isolates the function, a confirmed block settles DMA lifetime. Otherwise BME
   clear and no pending non-posted request do not prove that posted requests using other TCs,
   relaxed ordering or ID ordering have arrived (PCIe 6.0, 6.4). Keep the claim's remaining DMA
   pages even after a clean driver close; FLR success later is not proof of their drain either. */
static void claim_release(struct object *o) {
    struct pci_claim *c = o->priv;
    struct pci_device *d = c->d;
    __atomic_store_n(&c->released, true, __ATOMIC_SEQ_CST);
    shared_revoke_claim(c); /* no process reaches the registers any more */
    interrupts_off(c);
    mutex_lock(&claim_mutex); /* no claim forms a group with these functions meanwhile */
    if (c->domain) iommu_guests_stop(c->domain);
    bool left_running;
    bool stopped = pci_disable_busmaster(d, &left_running);
    if (c->domain && c->domain->nmembers > 1) {
        /* P2P lets the old driver program every member. FLR resets only its target
           Function (PCIe 6.0, 6.6.2), not its peers: even a clean primary must be
           recovered if it becomes another owner's peer. Never wait/reset at exit. A peer
           left under reset (claim_recover) masters no more and is its reset's alone. */
        for (unsigned i = 0; i < c->domain->nmembers; i++) {
            struct pci_device *peer = c->domain->members[i];
            peer->needs_reset = true;
            if (peer != d && !peer->reset && !pci_disable_busmaster(peer, NULL))
                kprintf("pci: %02x:%02x.%u: peer bus mastering did not stop; group recovery is required\n",
                        peer->bus, peer->dev, peer->func);
        }
    }
    bool blocked = c->domain && iommu_block(c->domain);
    bool running = left_running || !stopped || !pci_requests_done(d);
    d->needs_reset |= running;
    __atomic_store_n(&c->dma_unsafe, !blocked, __ATOMIC_RELEASE);
    if (running)
        kprintf("pci: %02x:%02x.%u: %s; %s%s\n", d->bus, d->dev, d->func,
                left_running ? "its driver ended without stopping it (bus mastering was on)"
                : !stopped   ? "it does not stop"
                             : "it still has requests in flight",
                blocked ? "the IOMMU keeps it from memory" : "its DMA memory stays out of use",
                "; recovery is deferred to the next driver, not process exit");
    else if (!blocked)
        kprintf("pci: %02x:%02x.%u: bus mastering is off, but posted DMA drain is unconfirmed; remaining DMA memory stays out of use\n",
                d->bus, d->dev, d->func);
    mutex_unlock(&claim_mutex);
    /* Revoke CPU access before returning screen holds. Resuming CPU framebuffer writes is only
       a best-effort redraw: neither BME clear nor IOMMU blocking rebuilds a GPU's BAR/scanout. */
    acpi_node_unwatch(c->acpi_watch); /* no Notify reaches the claim any more */
    c->acpi_watch = NULL;
    mutex_lock(&screen_lock);
    bool given_back = outputs_end(c);
    mutex_unlock(&screen_lock);
    /* the last handle is closed: no pci_user_hold_screen of the claim's runs any more */
    if (c->screen_holds) screen_hold_end(c);
    if (!c->screen_holds && given_back) screen_repaint();
    for (; c->screen_holds; c->screen_holds--)
        screen_release();
    mutex_lock(&claim_mutex);
    d->claimed = false;
    mutex_unlock(&claim_mutex);
    kprintf("pci: %02x:%02x.%u released\n", d->bus, d->dev, d->func);
    claim_drop(c);
}
/* An app's claims close before its other handles and its mappings at its end: its device is
   stopped (and seen to have no DMA in flight) before the DMA objects among them can free RAM. */
static const struct object_ops claim_ops = {
    .kind = N_KIND_PCI, .ready = claim_ready, .release = claim_release, .closes_first = true};

static struct pci_claim *claim_of(struct handle *h) { return handle_is(h, &claim_ops) ? h->object->priv : NULL; }

struct pci_claim *pci_user_hold(struct handle *h) {
    struct pci_claim *c = claim_of(h);
    if (c) __atomic_fetch_add(&c->refs, 1, __ATOMIC_RELAXED); /* h holds one already */
    return c;
}
void pci_user_drop(struct pci_claim *c) { if (c) claim_drop(c); }
bool pci_user_revoked(const struct pci_claim *c) { return c && __atomic_load_n(&c->released, __ATOMIC_SEQ_CST); }
bool pci_user_dma_reusable(const struct pci_claim *c) { return !c || !__atomic_load_n(&c->dma_unsafe, __ATOMIC_ACQUIRE); }
unsigned pci_user_dma_bits(const struct pci_claim *c) { return iommu_dma_bits(c->domain); }
bool pci_user_dma_map(struct pci_claim *c, const uint64_t *frames, uint64_t base, size_t count) {
    return !c->domain || iommu_map(c->domain, frames, base, count);
}
bool pci_user_dma_unmap(struct pci_claim *c, const uint64_t *frames, uint64_t base, size_t count) {
    return !c->domain || iommu_unmap(c->domain, frames, base, count);
}

static void guard(struct pci_claim *c, uint32_t offset, uint32_t bits) {
    ASSERT(!(offset & 3) && c->guard_count < CLAIM_GUARDS);
    c->guards[c->guard_count++] = (struct guard){(uint16_t)offset, bits};
}

/* The registers a driver uses but some of whose bits decide things beyond its device: how the
   function reaches the rest of the system, resets it, or changes what its BARs decode. The
   whole-register ones (BARs, MSI, MSI-X) are kernel_register's. RM tolerates a refused write: it
   does not check what the platform's config accessors return. */
static void guards_init(struct pci_claim *c) {
    struct pci_device *d = c->d;
    /* Command: memory decode, bus mastering and parity response are the driver's. I/O decode
       (apps reach no I/O ports), INTx (apps get no legacy interrupt; one left asserting would
       hold a shared line) and the legacy PCI bits stay as the kernel set them. */
    guard(c, PCI_COMMAND, 0xFFFFu & ~(PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER | PCI_COMMAND_PARITY));
    guard(c, 0x0C, 1u << 30); /* BIST: start */
    guard(c, 0x3C, 0xFF);     /* Interrupt Line: the platform's routing record */
    uint8_t pm = pci_find_cap(d, 0x01);
    if (pm) {
        c->pmcsr = pm + 4;
        guard(c, c->pmcsr, 3); /* power state: sleep belongs to dev/pm.c; waking to D0 is let through */
    }
    if (d->pcie_cap) {
        /* Device Control: Max_Payload_Size must match the whole path to the root, phantom
           functions widen the requester IDs, and Initiate FLR resets the function. */
        guard(c, d->pcie_cap + PCIE_DEVCTL, 7u << 5 | 1u << 9 | PCIE_DEVCTL_FLR);
        guard(c, d->pcie_cap + PCIE_LNKCTL, PCIE_LNKCTL_DISABLE);
        if (d->pcie_v2)
            guard(c, d->pcie_cap + PCIE_LNKCTL2, PCIE_LNKCTL2_COMPLIANCE | PCIE_LNKCTL2_MOD_COMPLIANCE);
    }
    /* Extended capabilities (ECAM only): BAR sizes, virtual functions, and the controls that
       decide how its requests are routed and translated. */
    static const uint16_t sized[] = {0x15, 0x24}; /* Resizable BAR, VF Resizable BAR */
    for (unsigned k = 0; k < 2; k++) {
        uint16_t rebar = pci_find_ext_cap(d, sized[k]);
        if (!rebar) continue;
        unsigned bars = (pci_read32(d->bus, d->dev, d->func, rebar + 8) >> 5) & 7;
        for (unsigned i = 0; i < bars && i < 6; i++) guard(c, rebar + 8 + 8 * i, 0x3F00); /* BAR Size */
    }
    uint16_t sriov = pci_find_ext_cap(d, 0x10);
    if (sriov) {
        guard(c, sriov + 0x08, 0xFFFF); /* SR-IOV Control */
        guard(c, sriov + 0x10, 0xFFFF); /* NumVFs */
    }
    uint16_t acs = pci_find_ext_cap(d, 0x0D), ats = pci_find_ext_cap(d, 0x0F);
    uint16_t pri = pci_find_ext_cap(d, 0x13), pasid = pci_find_ext_cap(d, 0x1B);
    if (acs) guard(c, acs + 4, 0xFFFF0000);   /* ACS Control */
    if (ats) guard(c, ats + 4, 0xFFFF0000);   /* ATS Control */
    if (pri) guard(c, pri + 4, 0x3);          /* Page Request Control: enable, reset */
    if (pasid) guard(c, pasid + 4, 0x70000);  /* PASID Control */
}

/* A write of `width` bytes at `offset` would change a bit only the kernel changes. */
static bool guarded(const struct pci_claim *c, uint32_t offset, uint32_t width, uint32_t value) {
    uint32_t dword = offset & ~3u, shift = (offset & 3) * 8;
    uint32_t written = width == 4 ? ~0u : ((1u << (width * 8)) - 1) << shift;
    for (unsigned i = 0; i < c->guard_count; i++) {
        if (c->guards[i].offset != dword) continue;
        const struct pci_device *d = c->d;
        uint32_t old = pci_read32(d->bus, d->dev, d->func, dword);
        uint32_t now = (old & ~written) | ((value << shift) & written);
        uint32_t bits = c->guards[i].bits;
        if (dword == c->pmcsr && !(now & 3)) bits &= ~3u; /* back to D0 */
        if ((old ^ now) & bits) return true;
    }
    return false;
}

/* A Secondary Bus Reset (dev/pci.h) for m, which its FLR did not bring back, at the narrowest
   Downstream Port above it: once per port in a claim's attempt. Everything below that port is
   reset, so only where all of it is the claim's (its function, m, its domain's members) or in no
   one's use: never a function a kernel driver runs, the one the boot screen is drawn through
   (its scanout would be lost), one another claim holds or one in another domain. The others are
   reserved meanwhile. False when it was not done. */
static bool claim_owns(const struct pci_claim *c, const struct pci_device *m, const struct pci_device *f) {
    return f == c->d || f == m || (c->domain && iommu_member(c->domain, f));
}

/* claim_mutex held. */
static void others_reserve(const struct pci_claim *c, const struct pci_device *m, const struct pci_device *port,
                           bool reserve) {
    for (int i = 0; i < pci_count(); i++) {
        struct pci_device *f = pci_at(i);
        if (pci_below(port, f) && !claim_owns(c, m, f)) f->claimed = reserve;
    }
}

static bool bus_reset(struct pci_claim *c, struct pci_device *m) {
    struct pci_device *port = pci_bus_reset_port(m);
    if (!port || port == c->reset_port || thread_current()->killed) return false;
    for (int i = 0; i < pci_count(); i++) {
        struct pci_device *f = pci_at(i);
        if (!pci_below(port, f)) continue;
        bool ours = claim_owns(c, m, f);
        const char *why = !ours && f->kernel_owned     ? "a kernel driver runs it"
                          : screen_bar(f) >= 0         ? "the boot screen is drawn through it"
                          : !ours && f->claimed        ? "it is claimed"
                          : !ours && iommu_in_use(f)   ? "it is in another claim's domain"
                                                       : NULL;
        if (why) {
            kprintf("pci: %02x:%02x.%u: no Secondary Bus Reset at %02x:%02x.%u: %02x:%02x.%u is below it and %s\n",
                    m->bus, m->dev, m->func, port->bus, port->dev, port->func, f->bus, f->dev, f->func, why);
            return false;
        }
    }
    c->reset_port = port;
    others_reserve(c, m, port, true);
    bool done = pci_bus_reset(port);
    others_reserve(c, m, port, false);
    return done;
}

/* Completes a reset an earlier caller left (cancelled, or the function not ready then), with
   MSI off: the saved configuration may hold an old MSI Enable. A function its FLR did not bring
   back gets a Secondary Bus Reset where one may be done (bus_reset), and its FLR again after it:
   a Conventional Reset does not stop all that an FLR stops (PCIe 6.0, 6.6.2). */
static bool reset_finish(struct pci_claim *c, struct pci_device *m) {
    bool done = pci_function_reset(m);
    if (!done && pci_reset_stuck(m) && bus_reset(c, m)) done = pci_function_reset(m);
    if (!done || !pci_irq_disable(m)) return false;
    m->needs_reset = false;
    return true;
}

/* claim_mutex held, from here to pci_user_claim.
   The claim's domain (iommu_attach). Forming the group reads its functions' ACS, which no
   access but a reset's own may reach while one is under reset (dev/pci.h): such a reset is
   finished first, its function reserved meanwhile, and the group formed again. Only the
   group's own resets: a claim does not wait on other functions'. One that another claim or
   domain holds, or that does not finish, leaves the claim -N_ERR_NOT_READY; one that does not
   come back is taken as without ACS (dev/iommu.c), which claim_recover settles. */
static int claim_attach(struct pci_claim *c) {
    for (;;) {
        struct pci_device *m;
        int r = iommu_attach(c->d, &c->domain, &m);
        if (r != -N_ERR_NOT_READY || !m) return r;
        bool peer = m != c->d; /* c->d is reserved by the caller */
        if (peer && (m->claimed || iommu_in_use(m))) return r;
        if (peer) m->claimed = true;
        bool done = reset_finish(c, m);
        if (peer) m->claimed = false;
        if (!done && m->reset && !pci_reset_stuck(m)) return r;
    }
}

/* No app has a handle yet. The attached domain reserves every member across FLR sleeps;
   the primary is reserved by the caller too. A fresh peer must stop mastering even when
   it has not belonged to an app. An old app's peer also needs FLR: BME off pauses, not
   erases, its retained DMA state. Only the new primary can delegate missing generic reset
   to its native driver; that driver cannot enumerate/recover a dirty display/FLR-less peer.
   A peer that does not come back from its resets is left where it is: its bus mastering is
   off (cleared before its FLR, or by the reset), whatever it sends stays in the claim's domain,
   and the kernel reaches it only through its reset (dev/pci.h). The primary must come back. */
static bool claim_recover(struct pci_claim *c) {
    struct pci_device *d = c->d;
    unsigned count = c->domain ? c->domain->nmembers : 1;
    if (d->needs_reset && !c->domain) return false;
    for (unsigned i = 0; i < count; i++) {
        struct pci_device *m = c->domain ? c->domain->members[i] : d;
        if (thread_current()->killed) return false;
        /* A reset left unfinished that forming the group did not read (claim_attach). Rejoin
           it before BME/MSI accesses: Requests can be discarded during an FLR (6.6.2), so their
           readback is not a replacement for its remaining wait. */
        if (m->reset && !reset_finish(c, m)) {
            if (m != d && pci_reset_stuck(m)) continue;
            return false;
        }
        if (m != d && !pci_disable_busmaster(m, NULL) && !m->needs_reset) {
            m->needs_reset = true;
            return false;
        }
        if (!m->needs_reset) continue;
        if (!pci_irq_disable(m)) return false; /* do not preserve an old MSI Enable in FLR state */
        bool screen = screen_bar(m) >= 0;
        bool flr = m->pcie_cap && (pci_read32(m->bus, m->dev, m->func, m->pcie_cap + PCIE_DEVCAP) & PCIE_DEVCAP_FLR);
        if (!screen && flr) {
            if (!reset_finish(c, m)) {
                if (m != d && pci_reset_stuck(m)) continue;
                return false;
            }
        } else if (m != d) {
            return false;
        } else {
            if (!pci_disable_busmaster(d, NULL)) return false;
            kprintf("pci: %02x:%02x.%u: isolated, but native driver recovery is still required%s\n",
                    d->bus, d->dev, d->func, screen ? "; boot display is not automatically reset" : "");
        }
    }
    /* A bus reset for one member resets the others below its port too: one it brought back
       still owes its FLR, and one may not have come back from it. */
    for (unsigned i = 0; i < count; i++) {
        struct pci_device *m = c->domain ? c->domain->members[i] : d;
        if (m->reset && !pci_reset_stuck(m) && !reset_finish(c, m) && !pci_reset_stuck(m)) return false;
        if (!m->reset) continue;
        if (m == d) return false;
        m->needs_reset = true;
        kprintf("pci: %02x:%02x.%u does not answer after its resets: left as it is, bus mastering off, what it "
                "sends kept in the domain of %02x:%02x.%u\n", m->bus, m->dev, m->func, d->bus, d->dev, d->func);
    }
    return !thread_current()->killed;
}

/* claim_mutex held: the claim formed, or nothing of it left and d free again. */
static int64_t claim_form(struct pci_claim *c, struct handle **out) {
    struct pci_device *d = c->d;
    d->claimed = true; /* reserved across the resets' waits */
    /* From here the function reaches only the DMA objects made through the claim. */
    int r = claim_attach(c);
    if (r < 0) {
        d->claimed = false;
        return r;
    }
    if (!claim_recover(c)) {
        /* FLR may still be pending on any member: retain its device-owned saved state,
           expose no handle, and re-block the whole group before dropping the reservation. */
        if (c->domain) iommu_domain_free(c->domain);
        d->claimed = false;
        return -N_ERR_NOT_READY;
    }
    guards_init(c); /* pending FLR is complete: capability offsets/control now describe the handle */
    /* BAR sizes come from enumeration (pci_bar_size): measuring them here would switch memory
       decoding off under a running function, and the boot screen may be drawn through it. */
    struct handle *h = handle_open(&c->object, HANDLE_WRITE);
    if (!h) {
        if (c->domain) iommu_domain_free(c->domain);
        d->claimed = false;
        return -N_ERR_NO_MEMORY;
    }
    if (c->domain) iommu_guests_start(c->domain, true);
    *out = h;
    return 0;
}

int64_t pci_user_claim(uint8_t bus, uint8_t dev, uint8_t func, struct handle **out) {
    struct process *t = process_current();
    /* Boot options: noappdrivers keeps every function with the kernel; appdrivers=any lets any
       app claim (tests drive QEMU's edu device from a program they compile) */
    if (!t || cmdline_has("noappdrivers")) return -N_ERR_DENIED;
    if (t->pid != init_pid && !cmdline_has("appdrivers=any")) return -N_ERR_DENIED;
    if (dev > 31 || func > 7) return -N_ERR_INVALID;
    struct pci_device *d = pci_find_function(bus, dev, func);
    if (!d) return -N_ERR_NOT_FOUND;
    /* A bridge's registers route other devices' memory, buses and resets. */
    if (d->cls == 0x06) return -N_ERR_DENIED;
    struct pci_claim *c = kzalloc(sizeof *c);
    if (!c) return -N_ERR_NO_MEMORY;
    object_init(&c->object, &claim_ops, c); /* the claim ends when its last handle closes */
    c->d = d;
    c->refs = 1;
    mutex_lock(&claim_mutex);
    int64_t r = d->kernel_owned || d->claimed ? -N_ERR_BUSY : claim_form(c, out);
    mutex_unlock(&claim_mutex);
    if (r < 0) {
        kfree(c);
        return r;
    }
    kprintf("pci: %02x:%02x.%u (%04x:%04x) claimed by pid %d%s\n", bus, dev, func, d->vendor, d->device, t->pid,
            c->domain ? "" : iommu_active() ? "; the IOMMU cannot isolate it: its DMA reaches all of memory"
                                            : "; no IOMMU isolates it: its DMA reaches all of memory");
    return 0;
}

int64_t pci_user_info(struct handle *h, struct n_pci_function *out) {
    struct pci_claim *c = claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    struct pci_device *d = c->d;
    memset(out, 0, sizeof *out);
    out->bus = d->bus; out->dev = d->dev; out->func = d->func;
    out->revision = pci_read32(d->bus, d->dev, d->func, 0x08) & 0xFF;
    out->vendor = d->vendor; out->device = d->device;
    uint32_t sub = pci_read32(d->bus, d->dev, d->func, 0x2C);
    out->subsystem_vendor = sub & 0xFFFF; out->subsystem_id = sub >> 16;
    out->cls = d->cls; out->subcls = d->subcls; out->progif = d->progif;
    out->interrupt_pin = (pci_read32(d->bus, d->dev, d->func, 0x3C) >> 8) & 0xFF;
    out->msi_vectors = pci_msi_max_vectors(d);
    out->msix_vectors = pci_msix_max_vectors(d);
    struct pci_device *up = pci_upstream(d);
    out->upstream = up ? N_PCI_TARGET(up->bus, up->dev, up->func) : N_PCI_NONE;
    out->flags = c->domain ? N_PCI_ISOLATED : 0;
    if (d->needs_reset) out->flags |= N_PCI_NEEDS_RECOVERY;
    for (unsigned i = 0; i < pci_bar_count(d); i++) {
        uint32_t raw = d->bar[i];
        if (raw & 1) { out->bar[i] = (struct n_pci_bar){.address = raw & ~3u, .flags = N_PCI_BAR_IO}; continue; }
        uint64_t address = pci_bar_address(d, i), size = pci_bar_size(d, i);
        if (!address || !size) continue;
        uint32_t flags = N_PCI_BAR_MEMORY | (raw & 8 ? N_PCI_BAR_PREFETCH : 0);
        bool wide = ((raw >> 1) & 3) == 2;
        if (wide) flags |= N_PCI_BAR_64BIT;
        out->bar[i] = (struct n_pci_bar){.address = address, .size = size, .flags = flags};
        if (wide) i++; /* the upper half has no BAR of its own */
    }
    out->framebuffer_bar = N_PCI_NONE;
    int screen = screen_bar(d);
    if (screen >= 0) {
        out->framebuffer_address = fb.phys;
        out->framebuffer_size = (uint64_t)fb.pitch * fb.height;
        out->framebuffer_bar = (uint32_t)screen;
        out->framebuffer_width = fb.width;
        out->framebuffer_height = fb.height;
        out->framebuffer_pitch = fb.pitch;
        out->framebuffer_bpp = fb.bpp;
    }
    return 0;
}

static const char *format_name(uint32_t format) {
    format &= N_SCANOUT_FORMAT_MASK;
    return format == N_SCANOUT_XRGB2101010 ? "XRGB2101010" : format == N_SCANOUT_XBGR8888 ? "XBGR8888" : "XRGB8888";
}

int64_t pci_user_give_screen(struct handle *h, uint32_t output, const struct n_screen_buffers *b) {
    struct pci_claim *c = claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (c->d->cls != 0x03) return -N_ERR_DENIED;
    if (b && (!b->count || b->count > SCREEN_BUFFERS_MAX)) return -N_ERR_INVALID;
    uint64_t bytes = 0;
    int64_t r = 0;
    for (uint32_t i = 0; b && i < b->count && !r; i++)
        r = fb_buffer_check(b->width, b->height, b->pitch, b->format, b->buffer[i].offset, &bytes);
    if (r) return r;
    /* the surfaces, before screen_lock: a handle's lookup may sleep */
    struct handle *surfaces[SCREEN_BUFFERS_MAX] = {0};
    for (uint32_t i = 0; b && i < b->count && !r; i++)
        if (!(surfaces[i] = handle_get(b->buffer[i].surface))) r = -N_ERR_NO_HANDLE;
    struct shared_scanout surface[SCREEN_BUFFERS_MAX] = {0};
    struct scanout *buf[SCREEN_BUFFERS_MAX] = {0};
    for (uint32_t i = 0; b && i < b->count && !r; i++) {
        r = shared_scanout_open(surfaces[i], c, b->buffer[i].offset, bytes, &surface[i]);
        if (!r && !(buf[i] = fb_buffer_map(surface[i].frames, surface[i].pages, surface[i].type, surface[i].first_byte,
                                           b->width, b->height, b->pitch, b->format)))
            r = -N_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < SCREEN_BUFFERS_MAX; i++) handle_close(surfaces[i]);
    mutex_lock(&screen_lock);
    struct pci_output *o = r ? NULL : output_of(c, output, b != NULL);
    if (!r && b && (!o || !flips_room(c))) r = -N_ERR_NO_MEMORY;
    if (!r && !b && (!o || !o->count)) r = -N_ERR_INVALID;
    if (!r) {
        struct screen_mode_desc desc = {0};
        if (b) {
            desc = (struct screen_mode_desc){.width = b->width, .height = b->height,
                                             .direct = (b->format & N_SCANOUT_DIRECT) != 0};
            screen_modes_shown(screen_id(o->screen), &desc);
        }
        uint64_t last = output_buffers(o, &desc, surface, buf, b ? b->count : 0);
        r = (int64_t)last;
        char line[200];
        if (b)
            ksnprintf(line, sizeof line, "screen %u is drawn into its driver's %u %ux%u %s buffer%s (pitch %u)%s",
                      screen_id(o->screen), b->count, b->width, b->height, format_name(b->format),
                      b->count > 1 ? "s" : "", b->pitch, desc.direct ? ", which can show fullscreen windows directly" : "");
        else
            ksnprintf(line, sizeof line, "its driver gives screen %u back", screen_id(o->screen));
        screen_say(c, output, line);
    }
    mutex_unlock(&screen_lock);
    if (r < 0)
        for (uint32_t i = 0; b && i < b->count; i++) {
            fb_buffer_unmap(buf[i]);
            shared_scanout_close(&surface[i]);
        }
    return r;
}

int64_t pci_user_screen_flips(struct handle *h, struct n_screen_flip *out, size_t max, uint64_t deadline) {
    struct pci_claim *c = claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (!max) return -N_ERR_INVALID;
    uint64_t irq = spin_lock(&c->flip_lock);
    for (;;) {
        if (c->nflips) {
            size_t n = MIN(max, c->nflips);
            memcpy(out, c->flips, n * sizeof *out);
            memmove(c->flips, c->flips + n, (c->nflips - n) * sizeof *out);
            c->nflips -= n;
            spin_unlock(&c->flip_lock, irq);
            return (int64_t)n;
        }
        int64_t r = 1;
        if (thread_current()->killed) r = -N_ERR_STOPPED;
        else if (clock_now_ns() >= deadline) r = 0;
        if (r <= 0) {
            spin_unlock(&c->flip_lock, irq);
            return r;
        }
        /* output_flip takes flip_lock to fill a slot, then wakes flip_wq */
        wq_wait_locked_until(&c->flip_wq, &c->flip_lock, deadline);
        spin_take(&c->flip_lock);
    }
}

int64_t pci_user_screen_vblank(struct handle *h, const struct n_screen_vblank *v) {
    struct pci_claim *c = claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (v->buffer >= SCREEN_BUFFERS_MAX) return -N_ERR_INVALID;
    mutex_lock(&screen_lock);
    struct pci_output *o = output_of(c, v->output, false);
    struct screen *s = o && o->count ? o->screen : NULL;
    mutex_unlock(&screen_lock);
    if (!s) return -N_ERR_DENIED;
    /* the screen outlives the claim (screens are never freed): a blank reported as the output's
       buffers go is a blank of a screen that no longer waits for it */
    if (v->flip_serial) screen_flipped(s, v->buffer, v->flip_serial, v->time_ns);
    screen_vblank(s, v->count, v->time_ns);
    return 0;
}

int64_t pci_user_screen_state(struct handle *h, uint32_t output, uint64_t seen, struct n_screen_state *out,
                              uint64_t deadline) {
    struct pci_claim *c = claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    mutex_lock(&screen_lock);
    struct pci_output *o = output_of(c, output, false);
    int64_t screen = o && (o->count || o->modes) ? (int64_t)screen_id(o->screen) : -1;
    mutex_unlock(&screen_lock);
    if (screen < 0) return -N_ERR_DENIED;
    return wm_screen_state((unsigned)screen, seen, out, deadline);
}

int64_t pci_user_screen_modes(struct handle *h, uint32_t output, const struct n_screen_modes *m) {
    struct pci_claim *c = claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (c->d->cls != 0x03) return -N_ERR_DENIED;
    if (m->count > N_SCREEN_MODES_MAX) return -N_ERR_INVALID;
    for (uint32_t i = 0; i < m->count; i++)
        if (!m->modes[i].width || !m->modes[i].height || m->modes[i].width > UINT16_MAX ||
            m->modes[i].height > UINT16_MAX || !m->modes[i].refresh_mhz)
            return -N_ERR_INVALID;
    int64_t r = 0;
    mutex_lock(&screen_lock);
    struct pci_output *o = output_of(c, output, m->count != 0);
    if (!o) r = m->count ? -N_ERR_NO_MEMORY : 0;
    else {
        o->modes = m->count != 0;
        screen_modes_offer(screen_id(o->screen), m);
    }
    mutex_unlock(&screen_lock);
    return r;
}

int64_t pci_user_screen_image(struct handle *h, uint32_t output, struct handle *surface, const struct n_scanout *desc) {
    struct pci_claim *c = claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    uint64_t bytes = 0;
    int64_t r = desc->format & N_SCANOUT_DIRECT
                    ? -N_ERR_INVALID
                    : fb_buffer_check(desc->width, desc->height, desc->pitch, desc->format, desc->offset, &bytes);
    if (r) return r;
    struct screen_image *i = kzalloc(sizeof *i);
    if (!i) return -N_ERR_NO_MEMORY;
    mutex_lock(&screen_lock);
    struct pci_output *o = output_of(c, output, false);
    if (!o || !o->direct) r = -N_ERR_DENIED;
    if (!r) r = shared_scanout_open(surface, c, desc->offset, bytes, &i->surface);
    if (!r && !(i->out = fb_buffer_map(i->surface.frames, i->surface.pages, i->surface.type, i->surface.first_byte,
                                       desc->width, desc->height, desc->pitch, desc->format))) {
        shared_scanout_close(&i->surface);
        r = -N_ERR_NO_MEMORY;
    }
    if (!r) {
        fb_image_add(i->out);
        i->output = o;
        i->number = r = ++screen_image_last;
        i->next = screen_images;
        screen_images = i;
    }
    mutex_unlock(&screen_lock);
    if (r < 0) kfree(i);
    return r;
}

int64_t pci_user_screen_image_end(struct handle *h, int64_t number) {
    struct pci_claim *c = claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    int64_t r = -N_ERR_INVALID;
    mutex_lock(&screen_lock);
    for (struct screen_image **at = &screen_images; *at; at = &(*at)->next)
        if ((*at)->number == number && (*at)->output->claim == c) {
            struct screen_image *i = *at;
            *at = i->next;
            screen_image_free(i);
            r = 0;
            break;
        }
    mutex_unlock(&screen_lock);
    return r;
}

/* ---- the function's part of the ACPI namespace (dev/acpi.c) ---- */

/* The function's node and the one above it, once. acpi_lock held. */
static void acpi_look(struct pci_claim *c) {
    if (c->acpi_looked) return;
    c->acpi_looked = true;
    c->acpi_node = acpi_pci_node(c->d);
    c->acpi_upstream = c->d->upstream ? acpi_pci_node(c->d->upstream) : acpi_pci_host(c->d);
}

/* The predefined methods through which the system itself runs devices and the platform, as the
   kernel runs its config bits (guards_init): power and reset, removal and docking, resources, the
   handshake that decides what the firmware or the system controls, wake, initialisation and
   handlers (operation regions, GPIO events), system sleep. A driver's evaluation would change
   behind the kernel what it keeps track of (dev/pm.c, dev/pci.c), and on the bridge above it
   every function below that bridge. None is part of a driver's own protocol with its firmware
   (RM's: _DSM, _ROM, _DOD, _DDC, _ADR, MXDS, MXDM). */
static const char acpi_system_methods[][5] = {
    "_PS0", "_PS1", "_PS2", "_PS3", "_ON_", "_OFF", "_RST", /* power, reset */
    "_EJ0", "_EJ1", "_EJ2", "_EJ3", "_EJ4", "_LCK", "_DCK", /* removal, docking */
    "_DIS", "_SRS",                                         /* resources */
    "_OSC", "_OST",                                         /* the system and firmware's handshake */
    "_PSW", "_DSW",                                         /* wake */
    "_INI", "_REG", "_EVT",                                 /* initialisation, handlers */
    "_PTS", "_WAK", "_TTS", "_GTS", "_BFS",                 /* system sleep */
};

/* Whether a driver may evaluate name (a name segment) on node which: none of the methods above,
   and on the bridge above it predefined names only: the bridge's own methods (the firmware's
   power and link control of its slot) belong to the platform and every function below it, as
   its registers do (a bridge is never claimed). Decided before the node is looked up, so a
   function with a node and one without get the same answer. */
static bool acpi_name_allowed(uint32_t which, const char *name) {
    if (which == N_ACPI_UPSTREAM && name[0] != '_') return false;
    for (size_t i = 0; i < ARRAY_SIZE(acpi_system_methods); i++)
        if (!memcmp(name, acpi_system_methods[i], 4)) return false;
    return true;
}

int64_t pci_user_acpi_eval(struct handle *h, uint32_t which, const char *name, const struct n_acpi_value *args,
                           uint32_t nargs, const uint8_t *data, size_t data_size, uint8_t *out, size_t out_size) {
    struct pci_claim *c = claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (!acpi_name_segment(name)) return -N_ERR_INVALID;
    if (!acpi_name_allowed(which, name)) return -N_ERR_DENIED;
    if (!acpi_aml_ready()) return -N_ERR_UNSUPPORTED;
    mutex_lock(&c->acpi_lock);
    acpi_look(c);
    void *node = which == N_ACPI_SELF ? c->acpi_node : which == N_ACPI_UPSTREAM ? c->acpi_upstream : NULL;
    int64_t r = 0;
    if (which != N_ACPI_SELF && which != N_ACPI_UPSTREAM) {
        node = c->acpi_node ? acpi_node_child(c->acpi_node, which) : NULL;
        if (!node) r = -N_ERR_INVALID; /* past the last child (a function without a node has none) */
    }
    mutex_unlock(&c->acpi_lock);
    if (r) return r;
    if (!node) return -N_ERR_NOT_FOUND;
    return acpi_node_eval(node, name, args, nargs, data, data_size, out, out_size);
}

/* Notify(function node, value), on the acpi thread (any CPU). */
static void acpi_noted(void *context, uint64_t value) {
    struct pci_claim *c = context;
    uint64_t irq = spin_lock(&c->notes_lock);
    if (c->acpi_count == CLAIM_ACPI_NOTES) {
        c->acpi_first = (c->acpi_first + 1) % CLAIM_ACPI_NOTES;
        c->acpi_count--;
    }
    c->acpi_notes[(c->acpi_first + c->acpi_count++) % CLAIM_ACPI_NOTES] = (uint32_t)value;
    wq_wake_all(&c->acpi_wq);
    spin_unlock(&c->notes_lock, irq);
}

int64_t pci_user_acpi_notify_take(struct handle *h, uint64_t deadline) {
    struct pci_claim *c = claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (!acpi_aml_ready()) return -N_ERR_UNSUPPORTED;
    mutex_lock(&c->acpi_lock);
    acpi_look(c);
    if (c->acpi_node && !c->acpi_watch) c->acpi_watch = acpi_node_watch(c->acpi_node, acpi_noted, c);
    bool watched = c->acpi_watch != NULL;
    mutex_unlock(&c->acpi_lock);
    if (!watched) return c->acpi_node ? -N_ERR_NO_MEMORY : -N_ERR_NOT_FOUND;
    uint64_t irq = spin_lock(&c->notes_lock);
    for (;;) {
        if (c->acpi_count) {
            uint32_t value = c->acpi_notes[c->acpi_first];
            c->acpi_first = (c->acpi_first + 1) % CLAIM_ACPI_NOTES;
            c->acpi_count--;
            spin_unlock(&c->notes_lock, irq);
            return value;
        }
        if (thread_current()->killed) { spin_unlock(&c->notes_lock, irq); return -N_ERR_STOPPED; }
        if (clock_now_ns() >= deadline) { spin_unlock(&c->notes_lock, irq); return -N_ERR_TIMEOUT; }
        /* acpi_noted takes notes_lock to add a value, then wakes acpi_wq */
        wq_wait_locked_until(&c->acpi_wq, &c->notes_lock, deadline);
        spin_take(&c->notes_lock); /* a value that came at the time limit is still taken */
    }
}

/* The claim's threads one at a time under screen_lock: a count and the hold it begins or ends
   change together. The redraw after the last hold comes after it. */
int64_t pci_user_hold_screen(struct handle *h, bool hold) {
    struct pci_claim *c = claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    int64_t r = 0;
    mutex_lock(&screen_lock);
    if (screen_bar(c->d) < 0 && !claim_shows(c)) r = -N_ERR_NOT_FOUND;
    else if (hold) {
        if (c->screen_holds == UINT32_MAX) r = -N_ERR_BUSY;
        else {
            if (!c->screen_holds++) screen_hold_begin(c);
            fb_hold();
        }
    } else if (!c->screen_holds) {
        r = -N_ERR_INVALID;
    } else if (!--c->screen_holds) {
        screen_hold_end(c);
    }
    mutex_unlock(&screen_lock);
    if (!hold && !r) screen_release();
    return r;
}

static bool config_access_ok(uint32_t offset, uint32_t width) {
    return (width == 1 || width == 2 || width == 4) && offset < 4096 && !(offset & (width - 1));
}
/* Reads reach the path from the host to the claimed function, but no unrelated device:
   - the function and the other functions of its device (an audio function on a graphics card);
   - each bridge above it with the other functions of that bridge's device. A driver scans a bus
     device by device and gives up on a device whose function 0 does not answer, and a root port
     is often function 1 or later beside a host-bridge function 0;
   - the host bridges, which tell the driver what chipset the path starts at. */
static bool is_host_bridge(const struct pci_device *d) { return d->cls == 0x06 && d->subcls == 0x00; }
static bool may_read(const struct pci_device *d, const struct pci_device *target) {
    if (target->bus == d->bus && target->dev == d->dev) return true;
    for (const struct pci_device *b = pci_upstream(d); b; b = pci_upstream(b))
        if (target->bus == b->bus && target->dev == b->dev) return true;
    return is_host_bridge(target);
}

int64_t pci_user_config_read(struct handle *h, uint32_t target, uint32_t offset, uint32_t width) {
    struct pci_claim *c = claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (!config_access_ok(offset, width)) return -N_ERR_INVALID;
    struct pci_device *t = c->d;
    if (target != N_PCI_SELF) {
        if (target > 0xFFFF) return -N_ERR_INVALID;
        t = pci_find_function(target >> 8, (target >> 3) & 31, target & 7);
        if (!t) return -N_ERR_NOT_FOUND;
        if (!may_read(c->d, t)) return -N_ERR_DENIED;
    }
    uint32_t v;
    if (t == c->d) v = pci_read32(t->bus, t->dev, t->func, offset & ~3u);
    /* under an FLR (dev/pci.h): another claim's, or one an ended claim left for a later claim to
       finish; checked and read in one step against a reset beginning on another CPU */
    else if (!pci_config_read_unless_reset(t, (uint16_t)offset, &v)) return -N_ERR_NOT_READY;
    v >>= (offset & 3) * 8;
    return width == 4 ? v : v & ((1u << (width * 8)) - 1);
}

/* Registers only the kernel writes: the BARs, the expansion ROM BAR and the MSI and MSI-X
   capabilities, which decide where memory and interrupt messages go. */
static bool kernel_register(const struct pci_device *d, uint32_t offset, uint32_t width) {
    uint32_t end = offset + width;
    if (offset < 0x28 && end > 0x10) return true;
    if (offset < 0x34 && end > 0x30) return true;
    uint8_t msi = pci_find_cap(d, 0x05), msix = pci_find_cap(d, 0x11);
    if (msi && offset < msi + 24u && end > msi) return true;
    if (msix && offset < msix + 12u && end > msix) return true;
    return false;
}

/* A reset (the GPU fell off the bus and came back, or left a power state) clears the BARs, and
   the driver puts back the addresses it saw at boot one dword at a time. That write is let
   through when it is exactly the address the kernel found there (d->bar): a BAR can be put back,
   never moved. Half way through a 64-bit BAR it decodes at a mixed address, but only requests
   inside the bridge window above it reach it, so that touches no other device. */
static bool bar_restore(const struct pci_device *d, uint32_t offset, uint32_t width, uint32_t value) {
    return width == 4 && offset >= 0x10 && offset < 0x10 + 4 * pci_bar_count(d) &&
           value == d->bar[(offset - 0x10) / 4];
}

int64_t pci_user_config_write(struct handle *h, uint32_t offset, uint32_t width, uint32_t value) {
    struct pci_claim *c = claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (!config_access_ok(offset, width)) return -N_ERR_INVALID;
    struct pci_device *d = c->d;
    if (kernel_register(d, offset, width) && !bar_restore(d, offset, width, value)) return -N_ERR_DENIED;
    if (guarded(c, offset, width, value)) return -N_ERR_DENIED;
    if (width == 4) pci_write32(d->bus, d->dev, d->func, offset, value);
    else if (width == 2) pci_write16(d->bus, d->dev, d->func, offset, value);
    else pci_write8(d->bus, d->dev, d->func, offset, value);
    return 0;
}

int64_t pci_user_bar(struct handle *h, uint32_t bar, uint64_t *address, uint64_t *size) {
    struct pci_claim *c = claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (bar > 5) return -N_ERR_INVALID;
    if (c->d->bar[bar] & 1) return -N_ERR_UNSUPPORTED; /* I/O ports */
    uint64_t a = pci_bar_address(c->d, bar), s = pci_bar_size(c->d, bar);
    if (!a || !s) return -N_ERR_NOT_FOUND;
    /* A smaller BAR shares its page with whatever lies beside it. */
    if ((a | s) & (PAGE_SIZE - 1)) return -N_ERR_UNSUPPORTED;
    *address = a;
    *size = s;
    return 0;
}
bool pci_user_bar_prefetchable(struct handle *h, uint32_t bar) {
    struct pci_claim *c=claim_of(h);
    return c && bar<6 && !(c->d->bar[bar]&1) && (c->d->bar[bar]&8);
}

static int64_t interrupts_on(struct pci_claim *c, uint32_t count);

int64_t pci_user_interrupts(struct handle *h, uint32_t count) {
    struct pci_claim *c = claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    mutex_lock(&c->vectors_mutex);
    int64_t r = interrupts_on(c, count);
    mutex_unlock(&c->vectors_mutex);
    return r;
}

/* vectors_mutex held. */
static int64_t interrupts_on(struct pci_claim *c, uint32_t count) {
    struct pci_device *d = c->d;
    interrupts_off(c);
    if (!count) return 0;
    unsigned most = MAX(pci_msix_max_vectors(d), pci_msi_max_vectors(d));
    if (!most) return -N_ERR_UNSUPPORTED; /* no legacy INTx for apps */
    count = MIN(count, most);
    struct claim_vectors *v = c->vec;
    if (!v || v->room < count) {
        size_t words = (count + 63) / 64;
        v = kzalloc(sizeof *v + count * sizeof *v->messages + words * sizeof *v->pending);
        if (!v) return -N_ERR_NO_MEMORY;
        v->room = count;
        v->pending = (uint64_t *)&v->messages[count];
        for (unsigned i = 0; i < count; i++) v->messages[i] = (struct claim_message){c, i};
        v->older = c->vec;
    }
    struct irq_spec *specs = kmalloc(count * sizeof *specs);
    if (!specs) {
        if (v != c->vec) kfree(v);
        return -N_ERR_NO_MEMORY;
    }
    for (unsigned i = 0; i < count; i++)
        specs[i] = (struct irq_spec){.name = "app driver", .handler = claim_isr, .context = &v->messages[i], .node = d->node};
    /* on before the messages are: one may come as soon as its entry is written */
    uint64_t irq = spin_lock(&c->irq_lock);
    c->vec = v;
    c->vectors = count;
    spin_unlock(&c->irq_lock, irq);
    unsigned n = pci_irq_enable(d, specs, count, NULL);
    kfree(specs);
    irq = spin_lock(&c->irq_lock);
    c->vectors = n;
    spin_unlock(&c->irq_lock, irq);
    if (!n) { /* no CPU it may name has a vector free, or its messages could not be made */
        wq_wake_all(&c->wq);
        object_notify(&c->object);
        return -N_ERR_BUSY;
    }
    return n;
}

/* irq_lock held: the pending bits of the first `words` words into got, cleared there; how many
   words that is (the claim's, at most). */
static unsigned pending_take(struct pci_claim *c, uint64_t *got, unsigned words) {
    unsigned n = MIN(words, (c->vectors + 63) / 64), taken = 0;
    for (unsigned i = 0; i < n; i++) {
        got[i] = c->vec->pending[i];
        c->vec->pending[i] = 0;
        taken += (unsigned)__builtin_popcountll(got[i]);
    }
    __atomic_store_n(&c->pending, c->pending - taken, __ATOMIC_RELEASE);
    return n;
}

int64_t pci_user_interrupt_take(struct handle *h, uint64_t deadline, uint64_t *ubits, uint32_t words) {
    struct pci_claim *c = claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (!words) return -N_ERR_INVALID;
    uint64_t got[MSIX_MOST / 64];
    words = MIN(words, (uint32_t)(MSIX_MOST / 64));
    uint64_t irq = spin_lock(&c->irq_lock);
    for (;;) {
        /* a message among the first words' that arrived */
        bool any = false;
        for (unsigned i = 0; c->pending && !any && i < MIN(words, (c->vectors + 63) / 64); i++) any = c->vec->pending[i];
        if (any) {
            unsigned n = pending_take(c, got, words);
            spin_unlock(&c->irq_lock, irq);
            if (!copy_to_user(ubits, got, n * sizeof *got)) return n;
            /* not delivered: the bits stay for the next take */
            irq = spin_lock(&c->irq_lock);
            for (unsigned i = 0; i < n && c->vectors; i++)
                for (uint64_t m = got[i] & ~c->vec->pending[i]; m; m &= m - 1) {
                    c->vec->pending[i] |= m & -m;
                    c->pending++;
                }
            spin_unlock(&c->irq_lock, irq);
            return -N_ERR_BAD_ADDRESS;
        }
        int64_t r = 0;
        if (thread_current()->killed) r = -N_ERR_STOPPED;
        else if (!c->vectors) r = -N_ERR_NOT_READY;
        else if (clock_now_ns() >= deadline) r = -N_ERR_TIMEOUT;
        if (r) { spin_unlock(&c->irq_lock, irq); return r; }
        /* the interrupt handlers and interrupts_off take irq_lock, then wake c->wq */
        wq_wait_locked_until(&c->wq, &c->irq_lock, deadline);
        spin_take(&c->irq_lock); /* a message that came at the time limit is still taken */
    }
}
