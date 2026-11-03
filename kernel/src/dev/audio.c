/* Sound's way out: the outputs and their one open each (see audio.h). */
#include "dev/audio.h"
#include "abi.h"
#include "authority_abi.h"
#include "arch/spin.h"
#include "mm/heap.h"
#include "mm/shared.h"
#include "sys/authority.h"
#include "sys/object.h"
#include "sys/syscall.h"
#include "sys/wait.h"
#include "sys/watch.h"
#include "vm/vm.h"

/* The list, the opens, and every call into a driver. */
static struct mutex audio_mutex;
static struct audio_output *outputs;
static uint32_t next_id = 1;
static struct watched list_watched;
/* Between a driver's interrupt, which tells the open of its output, and an open's end. A leaf. */
static struct spinlock period_lock;

/* An open of an output: what its handle reaches. output goes NULL when the output closes under it
   (unavailable, removed), under the mutex. */
struct audio_open {
    struct object object;
    struct audio_output *output;
    struct object *memory;
    /* the periods told and those a status has seen: a look is due when they differ */
    uint64_t told, seen;
};

static uint32_t open_ready(struct handle *h) {
    struct audio_open *o = h->object->priv;
    /* a hint: every change notifies the object after it */
    return __atomic_load_n(&o->told, __ATOMIC_ACQUIRE) != __atomic_load_n(&o->seen, __ATOMIC_ACQUIRE) ? N_EVENT_RECEIVABLE : 0;
}

static void open_tell(struct audio_open *o) {
    __atomic_add_fetch(&o->told, 1, __ATOMIC_ACQ_REL);
    object_notify(&o->object);
}

/* The mutex held: the driver stops, and the handle only tells that it is over. */
static void open_end(struct audio_open *o) {
    struct audio_output *output = o->output;
    if (!output) return;
    output->ops->close(output);
    uint64_t f = spin_lock(&period_lock);
    output->opened = NULL;
    spin_unlock(&period_lock, f);
    o->output = NULL;
    open_tell(o);
}

static void open_close(struct handle *h) {
    mutex_lock(&audio_mutex);
    open_end(h->object->priv);
    mutex_unlock(&audio_mutex);
}

static void open_release(struct object *object) {
    struct audio_open *o = object->priv;
    if (o->memory) object_unref(o->memory);
    kfree(o);
}

static const struct object_ops open_ops = {
    .kind = N_KIND_DEVICE, .ready = open_ready, .close = open_close, .release = open_release,
};

/* The handle, a reference to close, if it is an open. */
static struct handle *open_get(int device, int64_t *error) {
    struct handle *h = handle_get(device);
    *error = !h ? -N_ERR_NO_HANDLE : !handle_is(h, &open_ops) ? -N_ERR_WRONG_KIND : 0;
    if (*error && h) {
        handle_close(h);
        h = NULL;
    }
    return h;
}

void audio_output_period(struct audio_output *output) {
    uint64_t f = spin_lock(&period_lock);
    if (output->opened) open_tell(output->opened);
    spin_unlock(&period_lock, f);
}

void audio_output_add(struct audio_output *output) {
    char *shown = strdup(output->name);
    mutex_lock(&audio_mutex);
    struct audio_output **tail = &outputs;
    while (*tail) tail = &(*tail)->next;
    output->next = NULL;
    output->id = next_id++;
    output->available = true;
    output->shown = shown;
    *tail = output;
    mutex_unlock(&audio_mutex);
    kprintf("audio: output %u \"%s\" (%s)\n", output->id, output->name, output->key);
    watched_changed(&list_watched);
}

void audio_output_remove(struct audio_output *output) {
    mutex_lock(&audio_mutex);
    if (output->opened) open_end(output->opened);
    for (struct audio_output **at = &outputs; *at; at = &(*at)->next)
        if (*at == output) {
            *at = output->next;
            break;
        }
    char *shown = output->shown;
    output->shown = NULL;
    mutex_unlock(&audio_mutex);
    kfree(shown);
    watched_changed(&list_watched);
}

void audio_output_state(struct audio_output *output, enum n_audio_jack jack, const char *name) {
    char *shown = name ? strdup(name) : NULL;
    mutex_lock(&audio_mutex);
    if (shown) {
        char *old = output->shown;
        output->shown = shown;
        shown = old;
    }
    if (jack != output->jack) kprintf("audio: output %u: %s\n", output->id, jack == N_AUDIO_JACK_PLUGGED ? "plugged in" : "unplugged");
    output->jack = jack;
    mutex_unlock(&audio_mutex);
    if (shown) kfree(shown);
    watched_changed(&list_watched);
}

void audio_output_available(struct audio_output *output, bool available) {
    mutex_lock(&audio_mutex);
    output->available = available;
    if (!available && output->opened) open_end(output->opened);
    mutex_unlock(&audio_mutex);
    watched_changed(&list_watched);
}

/* ---- the system calls ---- */

/* The mutex held. The length of an output's record, and with out, the record. */
static uint32_t device_record(const struct audio_output *o, void *out) {
    const char *name = o->shown ? o->shown : "";
    uint32_t name_bytes = (uint32_t)strlen(name) + 1, key_bytes = (uint32_t)strlen(o->key) + 1;
    size_t rates = o->rate_count * sizeof *o->rates, sizes = o->size_count * sizeof *o->sizes;
    uint32_t bytes = (uint32_t)ALIGN_UP(sizeof(struct n_audio_device) + rates + sizes + name_bytes + key_bytes, _Alignof(struct n_audio_device));
    if (!out) return bytes;
    struct n_audio_device *d = out;
    *d = (struct n_audio_device){.bytes = bytes, .id = o->id, .kind = o->kind, .jack = o->jack, .available = o->available,
                                 .channels_low = o->channels_low, .channels_high = o->channels_high, .rate_count = o->rate_count,
                                 .size_count = o->size_count, .name_bytes = name_bytes, .key_bytes = key_bytes};
    memcpy((void *)n_audio_device_rates(d), o->rates, rates);
    memcpy((void *)n_audio_device_sizes(d), o->sizes, sizes);
    memcpy((void *)n_audio_device_name(d), name, name_bytes);
    memcpy((void *)n_audio_device_key(d), o->key, key_bytes);
    return bytes;
}

int64_t audio_syscall_list(int cap, void *user_out, uint64_t bytes) {
    int r = cap_check(cap, N_CAP_AUDIO_DEVICE);
    if (r < 0) return r;
    mutex_lock(&audio_mutex);
    uint64_t total = 0;
    for (struct audio_output *o = outputs; o; o = o->next) total += device_record(o, NULL);
    uint8_t *copy = total && total <= bytes ? kmalloc(total) : NULL;
    if (copy) {
        uint64_t at = 0;
        for (struct audio_output *o = outputs; o; o = o->next) at += device_record(o, copy + at);
    }
    mutex_unlock(&audio_mutex);
    if (!copy) return total <= bytes && total ? -N_ERR_NO_MEMORY : (int64_t)total;
    int64_t answer = copy_to_user(user_out, copy, total) ? -N_ERR_BAD_ADDRESS : (int64_t)total;
    kfree(copy);
    return answer;
}

/* One of the formats the output declares. */
static bool format_declared(const struct audio_output *o, const struct n_audio_device_format *f) {
    bool rate = false, size = false;
    for (uint32_t i = 0; i < o->rate_count; i++) rate = rate || o->rates[i] == f->rate;
    for (uint32_t i = 0; i < o->size_count; i++) size = size || (o->sizes[i].bits == f->bits && o->sizes[i].container == f->container);
    return rate && size && f->channels >= o->channels_low && f->channels <= o->channels_high;
}

int64_t audio_syscall_open(int cap, uint32_t id, const struct n_audio_device_format *user_format, struct n_audio_device_ring *user_ring) {
    int64_t r = cap_check(cap, N_CAP_AUDIO_DEVICE);
    if (r < 0) return r;
    struct n_audio_device_format format;
    struct n_audio_device_ring wanted;
    if (copy_from_user(&format, user_format, sizeof format) || copy_from_user(&wanted, user_ring, sizeof wanted)) return -N_ERR_BAD_ADDRESS;
    struct audio_open *o = kzalloc(sizeof *o);
    if (!o) return -N_ERR_NO_MEMORY;
    object_init(&o->object, &open_ops, o);
    struct handle *h = handle_open(&o->object, HANDLE_WRITE);
    if (!h) {
        open_release(&o->object);
        return -N_ERR_NO_MEMORY;
    }
    struct audio_ring ring = {.block_frames = wanted.block_frames, .blocks = wanted.blocks};
    mutex_lock(&audio_mutex);
    struct audio_output *output = outputs;
    while (output && output->id != id) output = output->next;
    if (!output) r = -N_ERR_NOT_FOUND;
    else if (!output->available) r = -N_ERR_NOT_READY;
    else if (output->opened) r = -N_ERR_BUSY;
    else if (!wanted.block_frames || !wanted.blocks || !format_declared(output, &format)) r = -N_ERR_INVALID;
    else r = output->ops->open(output, &format, &ring);
    if (r >= 0) {
        object_ref(ring.memory);
        o->memory = ring.memory;
        o->output = output;
        uint64_t f = spin_lock(&period_lock);
        output->opened = o;
        spin_unlock(&period_lock, f);
    }
    mutex_unlock(&audio_mutex);
    if (r < 0) {
        handle_close(h);
        return r;
    }
    int64_t memory = shared_kernel_handle(ring.memory);
    if (memory < 0) {
        handle_close(h);
        return memory;
    }
    struct n_audio_device_ring answer = {.memory = (int32_t)memory, .block_frames = ring.block_frames,
                                         .blocks = ring.blocks, .flush_bytes = ring.flush_bytes};
    if (copy_to_user(user_ring, &answer, sizeof answer)) {
        handle_close(h);
        return -N_ERR_BAD_ADDRESS;
    }
    return handle_install(h);
}

int64_t audio_syscall_run(int device, bool run) {
    int64_t r;
    struct handle *h = open_get(device, &r);
    if (!h) return r;
    struct audio_open *o = h->object->priv;
    mutex_lock(&audio_mutex);
    r = o->output ? o->output->ops->run(o->output, run) : -N_ERR_CLOSED;
    mutex_unlock(&audio_mutex);
    handle_close(h);
    return r;
}

int64_t audio_syscall_status(int device, struct n_audio_device_status *user_out) {
    int64_t r;
    struct handle *h = open_get(device, &r);
    if (!h) return r;
    struct audio_open *o = h->object->priv;
    struct n_audio_device_status status = {0};
    /* before the look: a period after it is one more to look at */
    __atomic_store_n(&o->seen, __atomic_load_n(&o->told, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    mutex_lock(&audio_mutex);
    if (o->output) {
        o->output->ops->status(o->output, &status);
        r = 0;
    } else r = -N_ERR_CLOSED;
    mutex_unlock(&audio_mutex);
    handle_close(h);
    if (r < 0) return r;
    return copy_to_user(user_out, &status, sizeof status) ? -N_ERR_BAD_ADDRESS : 0;
}

int64_t audio_syscall_watch(int cap) {
    int r = cap_check(cap, N_CAP_AUDIO_DEVICE);
    return r < 0 ? r : watched_open(&list_watched);
}

void audio_init(void) {
    watched_init(&list_watched, NULL); /* before an output is added */
}
