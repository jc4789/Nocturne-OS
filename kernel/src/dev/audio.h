#pragma once
#include "kernel.h"
#include "audio_abi.h"

/* Sound's way out: the outputs a machine's drivers have, each a way out of a sound device (a jack of a
   codec, a pin to a display, a remote desktop client). This layer holds their list and one open of each:
   the format, the ring of frames, where the device is in it. It mixes nothing and chooses nothing: the
   service audio (user/apps/audio.c), which holds N_CAP_AUDIO_DEVICE, reads the list, opens the output it
   chose in the format it chose, and writes the ring itself (docs/nocturne-native-abi.md §17).

   A device walks its ring of equal blocks without end from the moment it runs, and plays what lies
   there. Each block's end tells the service (audio_output_period). A service that dies has its open
   closed like any handle: the device stops, the driver stays, and the next service opens it again. */

void audio_init(void);
void hda_init(void); /* dev/hda.c: every High Definition Audio controller's outputs */

struct audio_output;
struct audio_open;
struct object;

/* The ring of an open. Going in, what the service would like; coming back, what the device takes, and
   its memory, an object of RAM the service maps, one reference of the driver's until close. */
struct audio_ring {
    struct object *memory;
    uint32_t block_frames, blocks;
    uint32_t flush_bytes; /* n_audio_device_ring */
};

/* The calls into an output, one at a time, in a thread that may sleep, with the layer's mutex held.
   open: the output is routed for this format, one of those it declares, its ring as close to what is
   asked as the device takes, silent, and the device not running; an error when it cannot (another output
   of its controller is open: N_ERR_BUSY).
   close: the device stops, and the output is no longer routed.
   run: true: the device plays from the ring's first frame, and the count of frames begins at 0.
   false: it stops. An error when it cannot now (its link is down): its period says when to look again.
   status: where it is. A power change between two calls stops the device and loses its ring: status
   then says so, with a new generation. */
struct audio_output_ops {
    int (*open)(struct audio_output *output, const struct n_audio_device_format *format, struct audio_ring *ring);
    void (*close)(struct audio_output *output);
    int (*run)(struct audio_output *output, bool run);
    void (*status)(struct audio_output *output, struct n_audio_device_status *status);
};

/* An output, the driver's for as long as it is added. The driver fills everything down to the layer's
   before it adds it; what the pointers reach stays with the driver as long. */
struct audio_output {
    const struct audio_output_ops *ops;
    void *priv;
    enum n_audio_kind kind;
    enum n_audio_jack jack;
    const char *key;
    const uint32_t *rates;
    const struct n_audio_size *sizes;
    uint32_t rate_count, size_count;
    uint32_t channels_low, channels_high;
    const char *name; /* copied when added; from then on the layer's, changed by audio_output_state */
    /* the layer's, under its mutex */
    uint32_t id;
    bool available;
    char *shown;
    struct audio_output *next;
    struct audio_open *opened; /* written with its period lock too */
};

/* In a thread that may sleep. Add: there is one more output. Remove: it is gone, closed first if open,
   and its memory is the driver's again. State: what is plugged in, and the name when not NULL, changed.
   Available false: it cannot play (its device is stopped or gone), and it is closed before this returns.
   Each one tells the watches of SYS_AUDIO_DEVICE_WATCH. */
void audio_output_add(struct audio_output *output);
void audio_output_remove(struct audio_output *output);
void audio_output_state(struct audio_output *output, enum n_audio_jack jack, const char *name);
void audio_output_available(struct audio_output *output, bool available);
/* Any context: a block has played, or something else status would tell has changed. */
void audio_output_period(struct audio_output *output);

/* The system calls: the caps checked here (N_CAP_AUDIO_DEVICE). */
int64_t audio_syscall_list(int cap, void *user_out, uint64_t bytes);
int64_t audio_syscall_open(int cap, uint32_t id, const struct n_audio_device_format *user_format, struct n_audio_device_ring *user_ring);
int64_t audio_syscall_run(int device, bool run);
int64_t audio_syscall_status(int device, struct n_audio_device_status *user_out);
int64_t audio_syscall_watch(int cap);
