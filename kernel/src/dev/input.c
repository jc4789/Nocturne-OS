/* i8042 PS/2 keyboard (scancode set 1 via translation) and mouse. */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/input.h"

static key_sink_t key_sink;
static mouse_sink_t mouse_sink;
static bool mouse_ok, mouse_wheel;
static uint8_t mods;
static bool caps;
static bool e0;

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

static void emit_key(uint16_t key, bool pressed) {
    struct key_event e = {key, mods, pressed};
    if (key_sink) key_sink(&e);
}

static void kbd_irq(struct regs *r) {
    (void)r;
    uint8_t st = inb(0x64);
    if (!(st & 1)) return;
    if (st & 0x20) { /* mouse data arrived on the keyboard IRQ: let IRQ12 path handle it */
        return;
    }
    uint8_t sc = inb(0x60);
    if (sc == 0xE0) { e0 = true; return; }
    if (sc == 0xE1) return;
    bool pressed = !(sc & 0x80);
    uint8_t code = sc & 0x7F;
    uint16_t key = 0;
    if (e0) {
        e0 = false;
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

/* ---- mouse ---- */
static uint8_t packet[4];
static int pidx;
static bool flip_y;

static void mouse_irq(struct regs *r) {
    (void)r;
    uint8_t st = inb(0x64);
    if (!(st & 1) || !(st & 0x20)) return; /* nothing, or a keyboard byte: leave it for IRQ1 */
    uint8_t b = inb(0x60);
    if (pidx == 0 && !(b & 0x08)) return; /* resync */
    packet[pidx++] = b;
    int need = mouse_wheel ? 4 : 3;
    if (pidx < need) return;
    pidx = 0;
    if (packet[0] & 0xC0) return; /* overflow */
    struct mouse_event e;
    e.dx = (int)packet[1] - ((packet[0] & 0x10) ? 256 : 0);
    e.dy = -((int)packet[2] - ((packet[0] & 0x20) ? 256 : 0));
    if (flip_y) e.dy = -e.dy;
    e.buttons = packet[0] & 7;
    e.wheel = 0;
    if (mouse_wheel) {
        int8_t z = (int8_t)(packet[3] << 4) >> 4;
        e.wheel = z;
    }
    if (mouse_sink) mouse_sink(&e);
}

static bool wait_write(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(0x64) & 2)) return true;
        pause();
    }
    return false;
}

static bool wait_read(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(0x64) & 1) return true;
        pause();
    }
    return false;
}

static void ctl_cmd(uint8_t c) { wait_write(); outb(0x64, c); }

static int mouse_cmd(uint8_t c) {
    ctl_cmd(0xD4);
    wait_write();
    outb(0x60, c);
    if (!wait_read()) return -1;
    return inb(0x60);
}

/* Hyper-V on Windows 11 hosts reports the emulated PS/2 mouse's Y axis upside down (a known
   regression that also hits Windows XP and Haiku guests). Detect it from the hypervisor's CPUID
   signature and host build; "mouse_y=normal" or "mouse_y=invert" on the command line overrides. */
static bool detect_flip_y(void) {
    if (cmdline_has("mouse_y=invert")) return true;
    if (cmdline_has("mouse_y=normal")) return false;
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(c & (1u << 31))) return false; /* no hypervisor */
    cpuid(0x40000000, 0, &a, &b, &c, &d);
    if (b != 0x7263694D || c != 0x666F736F || d != 0x76482074 || a < 0x40000002) return false; /* "Microsoft Hv" */
    cpuid(0x40000002, 0, &a, &b, &c, &d);
    kprintf("input: Hyper-V host build %u (%u.%u)\n", a, b >> 16, b & 0xFFFF);
    return a >= 22000; /* Windows 11 and later */
}

static void flush(void) {
    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) inb(0x60);
}

void input_init(void) {
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
        if (mouse_cmd(0xF4) == 0xFA) mouse_ok = true;
    }
    flush();
    flip_y = mouse_ok && detect_flip_y();
    irq_register(1, kbd_irq);
    irq_register(12, mouse_irq);
    pic_unmask(1);
    pic_unmask(2);
    pic_unmask(12);
    kprintf("input: PS/2 keyboard ready, mouse %s%s%s\n", mouse_ok ? "ready" : "not found",
            mouse_wheel ? " (wheel)" : "", flip_y ? ", Y axis flipped" : "");
}

void input_set_sinks(key_sink_t k, mouse_sink_t m) {
    uint64_t f = irq_save();
    key_sink = k;
    mouse_sink = m;
    irq_restore(f);
}

bool input_mouse_present(void) { return mouse_ok; }
