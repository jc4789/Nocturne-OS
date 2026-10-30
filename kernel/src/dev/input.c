/* i8042 PS/2 keyboard (scancode set 1 via translation) and mouse, and the keys and pointer moves
   of every other input driver (USB, Hyper-V, a remote viewer), on any CPU. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/spin.h"
#include "dev/input.h"
#include "dev/pm.h"
#include "sys/clock.h"
#include "vm/vm.h"

/* Locks, in the order they are taken:
   - ps2_lock: the i8042's ports and the PS/2 bytes' state (ps2_e0, the mouse packet, ps2_quiet).
     The IRQ handlers (CPU 0) and a resume on any CPU take it.
   - input_lock: the modifiers and caps lock, the other drivers' scan code prefix (stream_e0) and
     the sinks, which are called under it (interrupts off): a sink does not sleep or take a mutex,
     and its own locks come after this one (gui/wm.c's input ring). Keys from several drivers
     reach the sink one at a time, each with the modifiers as of it. */
static struct spinlock ps2_lock, input_lock;
static key_sink_t key_sink;
static mouse_sink_t mouse_sink;
static uint8_t mods;
static bool caps;
static bool stream_e0;
static bool mouse_ok; /* atomic: any driver may attach a mouse */
static bool mouse_wheel;
static bool ps2_e0;
/* ps2_setup talks to the controller: the IRQ handlers leave the ports alone meanwhile (it waits
   on the device far too long to hold ps2_lock). */
static bool ps2_quiet;

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

static void kbd_irq(struct regs *r) {
    (void)r;
    uint64_t f = spin_lock(&ps2_lock);
    uint8_t st = ps2_quiet ? 0 : inb(0x64);
    /* nothing, or mouse data on the keyboard's IRQ: IRQ12's handler takes it */
    if (!(st & 1) || (st & 0x20)) {
        spin_unlock(&ps2_lock, f);
        return;
    }
    uint8_t sc = inb(0x60);
    bool extended = ps2_e0, key = sc != 0xE0 && sc != 0xE1;
    if (sc == 0xE0) ps2_e0 = true;
    else if (key) ps2_e0 = false;
    spin_unlock(&ps2_lock, f);
    if (key) input_key_set1(sc, extended);
}

/* ---- mouse ---- */
static uint8_t packet[4]; /* under ps2_lock, with pidx */
static int pidx;
static bool flip_y; /* the Y axis comes upside down (detect_flip_y), set before the IRQs come */

static void mouse_irq(struct regs *r) {
    (void)r;
    uint64_t f = spin_lock(&ps2_lock);
    uint8_t st = ps2_quiet ? 0 : inb(0x64);
    if (!(st & 1) || !(st & 0x20)) { /* nothing, or a keyboard byte: leave it for IRQ1 */
        spin_unlock(&ps2_lock, f);
        return;
    }
    uint8_t b = inb(0x60);
    bool whole = false;
    uint8_t p[4];
    if (pidx || (b & 0x08)) { /* else a resync */
        packet[pidx++] = b;
        if (pidx == (mouse_wheel ? 4 : 3)) {
            pidx = 0;
            whole = !(packet[0] & 0xC0); /* else an overflow */
            memcpy(p, packet, sizeof p);
        }
    }
    bool wheel = mouse_wheel;
    spin_unlock(&ps2_lock, f);
    if (!whole) return;
    struct mouse_event e = {0};
    e.dx = (int)p[1] - ((p[0] & 0x10) ? 256 : 0);
    e.dy = -((int)p[2] - ((p[0] & 0x20) ? 256 : 0));
    if (flip_y) e.dy = -e.dy;
    e.buttons = p[0] & 7;
    e.wheel = wheel ? (int8_t)(p[3] << 4) >> 4 : 0;
    input_mouse(&e);
}

/* The controller's input buffer empty, or a byte in its output buffer, within PS2_WAIT_NS: a
   device answers a command in a few of its own bytes, a millisecond each. */
#define PS2_WAIT_NS (100 * NS_PER_MS)
static bool wait_status(uint8_t bit, bool set) {
    uint64_t deadline = clock_after(PS2_WAIT_NS);
    for (;;) {
        if (((inb(0x64) & bit) != 0) == set) return true;
        if (clock_now_ns() >= deadline) return false;
        pause();
    }
}

static bool wait_write(void) { return wait_status(2, false); }
static bool wait_read(void) { return wait_status(1, true); }

static void ctl_cmd(uint8_t c) { wait_write(); outb(0x64, c); }

static int mouse_cmd(uint8_t c) {
    ctl_cmd(0xD4);
    wait_write();
    outb(0x60, c);
    if (!wait_read()) return -1;
    return inb(0x60);
}

/* A real mouse's Y axis is the right way up; an emulated one may not be (vm/vm.c).
   "mouse_y=normal" or "mouse_y=invert" on the command line overrides. */
static bool detect_flip_y(void) {
    if (cmdline_has("mouse_y=invert")) return true;
    if (cmdline_has("mouse_y=normal")) return false;
    return vm_ps2_flips_y();
}

static void flush(void) {
    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) inb(0x60);
}

/* The controller's configuration, keyboard scanning, and the mouse in wheel mode if it has one:
   at boot, and after a sleep, which reset the controller and both devices. Returns whether a
   mouse answered. The IRQ handlers must not take the replies: at boot they are not registered
   yet, after a sleep their lines are masked and ps2_quiet is set. */
static bool ps2_setup(void) {
    ctl_cmd(0xAD); /* disable keyboard port */
    ctl_cmd(0xA7); /* disable aux port */
    flush();
    ctl_cmd(0x20);
    wait_read();
    uint8_t cfg = inb(0x60);
    cfg |= 0x01 | 0x02 | 0x40; /* IRQ1, IRQ12, translation */
    cfg &= ~(0x10 | 0x20);    /* enable clocks */
    ctl_cmd(0x60);
    wait_write();
    outb(0x60, cfg);
    ctl_cmd(0xAE);
    ctl_cmd(0xA8);
    flush();

    /* keyboard: enable scanning */
    wait_write();
    outb(0x60, 0xF4);
    wait_read();
    inb(0x60);

    /* mouse (some emulated controllers need a moment after the port is enabled) */
    int ack = -1;
    bool found = false;
    for (int tries = 0; tries < 4 && ack != 0xFA; tries++) {
        ack = mouse_cmd(0xF6);
        if (ack != 0xFA) flush();
    }
    if (ack == 0xFA) {
        /* try IntelliMouse wheel mode: sample rates 200, 100, 80 */
        mouse_cmd(0xF3); mouse_cmd(200);
        mouse_cmd(0xF3); mouse_cmd(100);
        mouse_cmd(0xF3); mouse_cmd(80);
        if (mouse_cmd(0xF2) == 0xFA) {
            wait_read();
            uint8_t id = inb(0x60);
            mouse_wheel = (id == 3 || id == 4);
        }
        mouse_cmd(0xF3); mouse_cmd(60);
        found = mouse_cmd(0xF4) == 0xFA;
    }
    flush();
    pidx = 0;
    ps2_e0 = false;
    return found;
}

static bool ps2_resume(void *arg) {
    (void)arg;
    pic_mask(1);
    pic_mask(12);
    /* a handler already running on CPU 0 is done once this has the lock */
    uint64_t f = spin_lock(&ps2_lock);
    ps2_quiet = true;
    spin_unlock(&ps2_lock, f);
    f = spin_lock(&input_lock);
    mods = 0; /* keys held when it went to sleep were let go long ago */
    spin_unlock(&input_lock, f);
    bool had = input_mouse_present(), found = ps2_setup();
    if (found) input_mouse_attach();
    else if (had) kprintf("input: the PS/2 mouse did not answer after sleep\n");
    f = spin_lock(&ps2_lock);
    ps2_quiet = false;
    spin_unlock(&ps2_lock, f);
    pic_unmask(1);
    pic_unmask(12);
    return found || !had; /* a mouse that was never there is not missing */
}

static struct pm_device ps2_pm = {.name = "ps2", .stage = PM_STAGE_IO, .resume = ps2_resume};

void input_init(void) {
    if (inb(0x64) == 0xFF) { /* nothing decodes the port: no i8042 (Hyper-V Generation 2) */
        kprintf("input: no PS/2 controller\n");
        return;
    }
    bool found = ps2_setup();
    if (found) input_mouse_attach();
    pm_register(&ps2_pm);
    flip_y = found && detect_flip_y();
    irq_register(1, kbd_irq);
    irq_register(12, mouse_irq);
    pic_unmask(1);
    pic_unmask(2);
    pic_unmask(12);
    kprintf("input: PS/2 keyboard ready, mouse %s%s%s\n", found ? "ready" : "not found",
            mouse_wheel ? " (wheel)" : "", flip_y ? ", Y axis flipped" : "");
}

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
