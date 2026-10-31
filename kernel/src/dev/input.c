/* The keys and pointer moves of every input driver (USB, Hyper-V, a remote viewer), on any CPU. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/spin.h"
#include "dev/input.h"
#include "dev/pm.h"

/* input_lock: the modifiers and caps lock, the drivers' scan code prefix (stream_e0) and the
   sinks, which are called under it (interrupts off): a sink does not sleep or take a mutex, and
   its own locks come after this one (gui/wm.c's input ring). Keys from several drivers reach the
   sink one at a time, each with the modifiers as of it. */
static struct spinlock input_lock;
static key_sink_t key_sink;
static mouse_sink_t mouse_sink;
static uint8_t mods;
static bool caps;
static bool stream_e0;
static bool mouse_ok; /* atomic: any driver may attach a mouse */

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

/* input_lock held. */
static void emit_key(uint16_t key, bool pressed) {
    struct key_event e = {key, mods, pressed};
    if (key_sink) key_sink(&e);
}

/* One key of scan code set 1, input_lock held. */
static void key_set1(uint8_t sc, bool extended) {
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
        case 0x1D: key = KEY_RCTRL; if (pressed) mods |= MOD_CTRL; else mods &= ~MOD_CTRL; break;
        case 0x38: key = KEY_RALT; if (pressed) mods |= MOD_ALT; else mods &= ~MOD_ALT; break;
        case 0x5B: case 0x5C: key = KEY_SUPER; if (pressed) mods |= MOD_SUPER; else mods &= ~MOD_SUPER; break;
        case 0x5D: key = KEY_MENU; break;
        case 0x2A: case 0x37: return; /* print screen fake shifts */
        default: key = KEY_UNKNOWN; break;
        }
        emit_key(key, pressed);
        return;
    }
    switch (code) {
    case 0x2A: key = KEY_LSHIFT; if (pressed) mods |= MOD_SHIFT; else mods &= ~MOD_SHIFT; emit_key(key, pressed); return;
    case 0x36: key = KEY_RSHIFT; if (pressed) mods |= MOD_SHIFT; else mods &= ~MOD_SHIFT; emit_key(key, pressed); return;
    case 0x1D: key = KEY_LCTRL; if (pressed) mods |= MOD_CTRL; else mods &= ~MOD_CTRL; emit_key(key, pressed); return;
    case 0x38: key = KEY_LALT; if (pressed) mods |= MOD_ALT; else mods &= ~MOD_ALT; emit_key(key, pressed); return;
    case 0x3A: if (pressed) caps = !caps; emit_key(KEY_CAPS, pressed); return;
    }
    if (code >= 0x3B && code <= 0x44) key = KEY_F1 + (code - 0x3B);
    else if (code == 0x57) key = KEY_F11;
    else if (code == 0x58) key = KEY_F12;
    else {
        bool shift = mods & MOD_SHIFT;
        char c = map_norm[code];
        if (c >= 'a' && c <= 'z') {
            if (shift ^ caps) c = map_shift[code];
        } else if (shift) {
            c = map_shift[code];
        }
        key = c ? (uint8_t)c : KEY_UNKNOWN;
    }
    emit_key(key, pressed);
}

void input_key_set1(uint8_t sc, bool extended) {
    uint64_t f = spin_lock(&input_lock);
    key_set1(sc, extended);
    spin_unlock(&input_lock, f);
}

void input_scancode(uint8_t sc) {
    uint64_t f = spin_lock(&input_lock);
    if (sc == 0xE0) stream_e0 = true;
    else if (sc != 0xE1) {
        bool extended = stream_e0;
        stream_e0 = false;
        key_set1(sc, extended);
    }
    spin_unlock(&input_lock, f);
}

/* After a sleep the keys held when it began were let go long ago. */
static bool input_resume(void *arg) {
    (void)arg;
    uint64_t f = spin_lock(&input_lock);
    mods = 0;
    stream_e0 = false;
    spin_unlock(&input_lock, f);
    return true;
}

static struct pm_device input_pm = {.name = "input", .stage = PM_STAGE_IO, .resume = input_resume};

void input_init(void) { pm_register(&input_pm); }

void input_set_sinks(key_sink_t k, mouse_sink_t m) {
    uint64_t f = spin_lock(&input_lock);
    key_sink = k;
    mouse_sink = m;
    spin_unlock(&input_lock, f);
}

bool input_mouse_present(void) { return __atomic_load_n(&mouse_ok, __ATOMIC_RELAXED); }

void input_unicode(uint16_t c, bool pressed) {
    if (c == '\r') c = '\n';
    if (c >= 0x80) c = KEY_UNKNOWN; /* the key codes have no room for other characters yet */
    uint64_t f = spin_lock(&input_lock);
    emit_key(c, pressed);
    spin_unlock(&input_lock, f);
}

void input_mouse(const struct mouse_event *e) {
    uint64_t f = spin_lock(&input_lock);
    if (mouse_sink) mouse_sink(e);
    spin_unlock(&input_lock, f);
}

void input_mouse_attach(void) { __atomic_store_n(&mouse_ok, true, __ATOMIC_RELAXED); }
