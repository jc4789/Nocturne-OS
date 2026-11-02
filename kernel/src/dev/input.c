/* The keys and pointer moves of every input driver (USB, Hyper-V, a remote viewer), on any CPU.
   No lock: the modifiers and caps lock are words changed atomically, and the sinks are called by
   the driver directly, so a key waits for nothing here. */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/input.h"
#include "dev/pm.h"
#include "sys/clock.h"

static key_sink_t key_sink;     /* atomic */
static mouse_sink_t mouse_sink; /* atomic */
static uint8_t mods;            /* atomic: MOD_* held now */
static uint8_t caps;            /* atomic: caps lock on (1) */
static bool mouse_ok;           /* atomic: any driver may attach a mouse */

static const char map_norm[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0, 0, '\\', 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const char map_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0, 0, '|', 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static uint64_t last_input; /* atomic */

uint64_t input_last_ns(void) { return __atomic_load_n(&last_input, __ATOMIC_RELAXED); }

static void emit_key(uint16_t key, bool pressed, uint64_t at) {
    __atomic_store_n(&last_input, clock_now_ns(), __ATOMIC_RELAXED);
    struct key_event e = {key, __atomic_load_n(&mods, __ATOMIC_ACQUIRE), pressed, at};
    key_sink_t sink = __atomic_load_n(&key_sink, __ATOMIC_ACQUIRE);
    if (sink) sink(&e);
}

/* A modifier key: held or let go, then the key itself with the modifiers as they are after it. */
static void modifier(uint16_t key, uint8_t bit, bool pressed, uint64_t at) {
    if (pressed) __atomic_fetch_or(&mods, bit, __ATOMIC_ACQ_REL);
    else __atomic_fetch_and(&mods, (uint8_t)~bit, __ATOMIC_ACQ_REL);
    emit_key(key, pressed, at);
}

void input_key_set1(uint8_t sc, bool extended, uint64_t at) {
    bool pressed = !(sc & 0x80);
    uint8_t code = sc & 0x7F;
    uint16_t key = 0;
    if (extended) {
        switch (code) {
        case 0x48: key = KEY_UP; break;
        case 0x50: key = KEY_DOWN; break;
        case 0x4B: key = KEY_LEFT; break;
        case 0x4D: key = KEY_RIGHT; break;
        case 0x47: key = KEY_HOME; break;
        case 0x4F: key = KEY_END; break;
        case 0x49: key = KEY_PGUP; break;
        case 0x51: key = KEY_PGDN; break;
        case 0x52: key = KEY_INSERT; break;
        case 0x53: key = KEY_DELETE; break;
        case 0x1C: key = '\n'; break;
        case 0x35: key = '/'; break;
        case 0x1D: modifier(KEY_RCTRL, MOD_CTRL, pressed, at); return;
        case 0x38: modifier(KEY_RALT, MOD_ALT, pressed, at); return;
        case 0x5B: case 0x5C: modifier(KEY_SUPER, MOD_SUPER, pressed, at); return;
        case 0x5D: key = KEY_MENU; break;
        case 0x2A: case 0x37: return; /* print screen fake shifts */
        default: key = KEY_UNKNOWN; break;
        }
        emit_key(key, pressed, at);
        return;
    }
    switch (code) {
    case 0x2A: modifier(KEY_LSHIFT, MOD_SHIFT, pressed, at); return;
    case 0x36: modifier(KEY_RSHIFT, MOD_SHIFT, pressed, at); return;
    case 0x1D: modifier(KEY_LCTRL, MOD_CTRL, pressed, at); return;
    case 0x38: modifier(KEY_LALT, MOD_ALT, pressed, at); return;
    case 0x3A:
        if (pressed) __atomic_xor_fetch(&caps, 1, __ATOMIC_ACQ_REL);
        emit_key(KEY_CAPS, pressed, at);
        return;
    }
    if (code >= 0x3B && code <= 0x44) key = KEY_F1 + (code - 0x3B);
    else if (code == 0x57) key = KEY_F11;
    else if (code == 0x58) key = KEY_F12;
    else {
        bool shift = __atomic_load_n(&mods, __ATOMIC_ACQUIRE) & MOD_SHIFT;
        char c = map_norm[code];
        if (c >= 'a' && c <= 'z') {
            if (shift ^ (bool)__atomic_load_n(&caps, __ATOMIC_ACQUIRE)) c = map_shift[code];
        } else if (shift) {
            c = map_shift[code];
        }
        key = c ? (uint8_t)c : KEY_UNKNOWN;
    }
    emit_key(key, pressed, at);
}

/* After a sleep the keys held when it began were let go long ago. */
static bool input_resume(void *arg) {
    (void)arg;
    __atomic_store_n(&mods, 0, __ATOMIC_RELEASE);
    return true;
}

static struct pm_device input_pm = {.name = "input", .stage = PM_STAGE_IO, .resume = input_resume};

void input_init(void) { pm_register(&input_pm); }

void input_set_sinks(key_sink_t k, mouse_sink_t m) {
    __atomic_store_n(&key_sink, k, __ATOMIC_RELEASE);
    __atomic_store_n(&mouse_sink, m, __ATOMIC_RELEASE);
}

bool input_mouse_present(void) { return __atomic_load_n(&mouse_ok, __ATOMIC_RELAXED); }

void input_unicode(uint16_t c, bool pressed, uint64_t at) {
    if (c == '\r') c = '\n';
    if (c >= 0x80) c = KEY_UNKNOWN; /* the key codes have no room for other characters yet */
    emit_key(c, pressed, at);
}

void input_mouse(const struct mouse_event *e) {
    __atomic_store_n(&last_input, clock_now_ns(), __ATOMIC_RELAXED);
    mouse_sink_t sink = __atomic_load_n(&mouse_sink, __ATOMIC_ACQUIRE);
    if (sink) sink(e);
}

void input_mouse_attach(void) { __atomic_store_n(&mouse_ok, true, __ATOMIC_RELAXED); }
