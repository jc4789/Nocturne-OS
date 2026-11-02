#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "input_abi.h"

/* time_ns in both: the moment on the monotonic clock (clock_now_ns) the driver received the
   event from its device, a USB report's completion or a host's message. */
struct key_event {
    uint16_t usage;  /* HID usage (Keyboard page); 0: only a character */
    uint8_t mods;    /* NMOD_* after this key */
    bool pressed;
    uint32_t text;   /* a character the host typed without a key (usage 0), else 0 */
    uint32_t layout; /* the device's enum n_key_layout, a number the kernel does not read */
    uint64_t time_ns;
};

#define MOUSE_ABS_MAX 32767

struct mouse_event {
    int dx, dy, wheel;
    uint8_t buttons; /* bit0 left, bit1 right, bit2 middle */
    bool absolute;   /* x, y are a position in 0..MOUSE_ABS_MAX instead of dx, dy */
    int x, y;
    uint64_t time_ns;
};

typedef void (*key_sink_t)(const struct key_event *e);
typedef void (*mouse_sink_t)(const struct mouse_event *e);

/* One keyboard or pointer, which its driver keeps from input_device_add to input_device_remove:
   the driver fills id, name and kinds. The values in effect (the user's defaults, or the device's own
   choice, N_INPUT_DEVICE) are read on each event without a lock. */
struct input_device {
    struct input_device *next;           /* in the list, under the input lock */
    const char *id, *name;               /* UTF-8; the driver's, for as long as the device is added. name may be "" */
    uint32_t kinds;                      /* N_INPUT_KEYBOARD, N_INPUT_POINTER */
    int32_t speed, accel, natural, repeat_delay_ms, repeat_interval_ms, layout; /* atomic */
    int rest_x, rest_y;                  /* what a slow move has not yet made into a pixel: its driver's */
};

/* Once at boot: the modifiers back to none after each sleep. */
void input_init(void);
/* The sinks get every key and pointer move, called by the driver that has it, in any context
   (an interrupt's handler included) and on several CPUs at once: a sink does not sleep or take a
   mutex, and keeps its own order with its own lock. One driver's events reach it in their order. */
void input_set_sinks(key_sink_t k, mouse_sink_t m);
bool input_mouse_present(void);
/* For the keyboard and mouse drivers (USB, Hyper-V, a remote viewer), any context, any CPU,
   without sleeping; `at` and e->time_ns as in the events above. input_key_set1 is one key of scan
   code set 1 (sc with its release bit, extended when it came after 0xE0). */
void input_key_set1(uint8_t sc, bool extended, uint64_t at);
void input_unicode(uint16_t c, bool pressed, uint64_t at); /* a typed character, independent of layout */
void input_mouse(const struct mouse_event *e);
void input_mouse_attach(void);
/* The monotonic time of the last key or pointer event; 0 before the first. Any context. */
uint64_t input_last_ns(void);

/* For the input drivers, on a thread. */
void input_device_add(struct input_device *d);
void input_device_remove(struct input_device *d);

/* For the input drivers, any context, any CPU, without sleeping; `at` and e->time_ns as in the
   events above. input_key is one key by its HID usage. input_text is a character a host typed
   without a key. input_mouse scales a relative move and turns the wheel as the device's values say. */
void input_key(struct input_device *d, uint16_t usage, bool pressed, uint64_t at);
void input_text(struct input_device *d, uint32_t c, bool pressed, uint64_t at);
void input_mouse(struct input_device *d, const struct mouse_event *e);

/* Caps Lock on now: what the keyboards' lights show. */
bool input_caps_on(void);
/* The held key's repeat on this keyboard, in nanoseconds: before the first, between the others. */
uint64_t input_repeat_delay_ns(const struct input_device *d);
uint64_t input_repeat_interval_ns(const struct input_device *d);
/* Two presses this close and at about the same place are one click more. */
uint64_t input_double_click_ns(void);

/* N_DOMAIN_INPUT (sys/settings.c). */
int input_setting_put(uint32_t key, const void *value);
int input_device_record_put(uint32_t key, const void *record, size_t length);
int64_t input_setting_get(uint32_t key, void *out, size_t cap);
