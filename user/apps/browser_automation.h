#ifndef NOCTURNE_BROWSER_AUTOMATION_H
#define NOCTURNE_BROWSER_AUTOMATION_H

/* A test tool, not a product feature. With --wpt-automation (which only tests/wptrunner.c
 * gives) WPT's testdriver hands the browser steps (web_host.automation); these turn one step
 * into the gui_events the window's own input would give for it, so that they then meet the
 * hit testing, focus, deferral and default actions real input meets. The device is the
 * automation's own pointer and keyboard: where its pointer is, which buttons and which
 * modifier keys it holds. No browser state here, so the guest test can check it as it is. */
struct automation_device {
    int x, y;           /* the pointer, on the page surface */
    uint32_t buttons;   /* the native mask: 1 left, 2 right, 4 middle, 8 back, 16 forward */
    unsigned modifiers; /* AUTOMATION_* bits of the modifier keys held */
};
enum { AUTOMATION_SHIFT = 1, AUTOMATION_SHIFT_RIGHT = 2, AUTOMATION_CONTROL = 4,
       AUTOMATION_CONTROL_RIGHT = 8, AUTOMATION_ALT = 16, AUTOMATION_ALT_RIGHT = 32 };

/* A WebDriver button (0 left, 1 middle, 2 right, 3 back, 4 forward) as a native mask bit. */
static uint32_t automation_button(uint32_t button) {
    static const uint32_t bits[] = {1, 4, 2, 8, 16};
    return button < 5 ? bits[button] : 0;
}
static uint32_t automation_mods(const struct automation_device *d) {
    return (d->modifiers & (AUTOMATION_SHIFT | AUTOMATION_SHIFT_RIGHT) ? NMOD_SHIFT : 0) |
           (d->modifiers & (AUTOMATION_CONTROL | AUTOMATION_CONTROL_RIGHT) ? NMOD_CTRL : 0) |
           (d->modifiers & (AUTOMATION_ALT | AUTOMATION_ALT_RIGHT) ? NMOD_ALT : 0);
}

/* A key's WebDriver code point (the WebDriver spec's table of special keys, U+E000..U+E05D,
 * or a character) as the native key: *key the NKEY_* code or the character, or *modifier an
 * AUTOMATION_* bit for Shift, Control and Alt. Returns why it cannot be typed, NULL when it
 * can. The native keyboard has no Meta and no key for Cancel, Help, Clear, Pause, Unidentified
 * or ZenkakuHankaku, and a character in U+0100..U+01FF would read as a named key there. */
static const char *automation_key(uint32_t raw, uint32_t *key, unsigned *modifier) {
    *key = 0; *modifier = 0;
    if (raw >= 0xE000 && raw <= 0xE05D) {
        static const uint16_t named[0x5E] = {
            [0x03] = NKEY_BACKSPACE, [0x04] = NKEY_TAB, [0x06] = NKEY_ENTER, [0x07] = NKEY_ENTER,
            [0x0C] = NKEY_ESC, [0x0D] = ' ', [0x0E] = NKEY_PGUP, [0x0F] = NKEY_PGDN,
            [0x10] = NKEY_END, [0x11] = NKEY_HOME, [0x12] = NKEY_LEFT, [0x13] = NKEY_UP,
            [0x14] = NKEY_RIGHT, [0x15] = NKEY_DOWN, [0x16] = NKEY_INSERT, [0x17] = NKEY_DELETE,
            [0x18] = ';', [0x19] = '=',
            [0x1A] = '0', [0x1B] = '1', [0x1C] = '2', [0x1D] = '3', [0x1E] = '4',
            [0x1F] = '5', [0x20] = '6', [0x21] = '7', [0x22] = '8', [0x23] = '9',
            [0x24] = '*', [0x25] = '+', [0x26] = ',', [0x27] = '-', [0x28] = '.', [0x29] = '/',
            [0x31] = NKEY_F1, [0x32] = NKEY_F2, [0x33] = NKEY_F3, [0x34] = NKEY_F4,
            [0x35] = NKEY_F5, [0x36] = NKEY_F6, [0x37] = NKEY_F7, [0x38] = NKEY_F8,
            [0x39] = NKEY_F9, [0x3A] = NKEY_F10, [0x3B] = NKEY_F11, [0x3C] = NKEY_F12,
            [0x54] = NKEY_PGUP, [0x55] = NKEY_PGDN, [0x56] = NKEY_END, [0x57] = NKEY_HOME,
            [0x58] = NKEY_LEFT, [0x59] = NKEY_UP, [0x5A] = NKEY_RIGHT, [0x5B] = NKEY_DOWN,
            [0x5C] = NKEY_INSERT, [0x5D] = NKEY_DELETE,
        };
        switch (raw) {
        case 0xE008: *modifier = AUTOMATION_SHIFT; return NULL;
        case 0xE009: *modifier = AUTOMATION_CONTROL; return NULL;
        case 0xE00A: *modifier = AUTOMATION_ALT; return NULL;
        case 0xE050: *modifier = AUTOMATION_SHIFT_RIGHT; return NULL;
        case 0xE051: *modifier = AUTOMATION_CONTROL_RIGHT; return NULL;
        case 0xE052: *modifier = AUTOMATION_ALT_RIGHT; return NULL;
        case 0xE03D: case 0xE053: return "The native keyboard has no Meta key";
        }
        *key = named[raw - 0xE000];
        return *key ? NULL : "The native keyboard has no key for this WebDriver key";
    }
    if (raw >= 0xD800 && raw <= 0xDFFF) return "A key must be a Unicode scalar value";
    if (raw > 0x10FFFF) return "A key must be a Unicode scalar value";
    if (raw >= 0x100 && raw < 0x200) return "The native keyboard cannot type U+0100..U+01FF";
    *key = raw == '\r' ? NKEY_ENTER : raw;
    return NULL;
}

/* Wheel CSS px as native wheel lines (line px each), never losing a nonzero delta. */
static int32_t automation_lines(double px, int line) {
    if (!(px != 0) || line <= 0) return 0;
    double lines = px / line;
    if (lines > 1e6) lines = 1e6;
    if (lines < -1e6) lines = -1e6;
    int32_t n = (int32_t)(lines < 0 ? lines - 0.5 : lines + 0.5);
    return n ? n : px > 0 ? 1 : -1;
}

/* One step as at most three events into out, the device changed as the step does. Returns
 * how many, or -1 with *error when the step cannot be done. A press or release away from the
 * pointer moves it there first; a modifier key changes only what later events carry. */
static int automation_events(struct automation_device *d, const struct web_automation_step *step,
                             int line, struct gui_event out[3], const char **error) {
    int n = 0;
    *error = NULL;
    if (step->kind == WEB_AUTOMATION_KEY_DOWN || step->kind == WEB_AUTOMATION_KEY_UP) {
        uint32_t key; unsigned modifier;
        if ((*error = automation_key(step->value, &key, &modifier))) return -1;
        bool down = step->kind == WEB_AUTOMATION_KEY_DOWN;
        if (modifier) {
            if (down) d->modifiers |= modifier; else d->modifiers &= ~modifier;
            return 0;
        }
        out[n++] = (struct gui_event){.type = EV_KEY, .key = key, .mods = automation_mods(d),
                                      .pressed = down, .x = d->x, .y = d->y};
        return n;
    }
    if (!isfinite(step->x) || !isfinite(step->y) || fabs(step->x) > 1e9 || fabs(step->y) > 1e9) {
        *error = "A pointer position must be finite"; return -1;
    }
    int x = (int)floor(step->x), y = (int)floor(step->y);
    if (step->kind == WEB_AUTOMATION_WHEEL) {
        int32_t down = automation_lines(step->dy, line), right = automation_lines(step->dx, line);
        /* The native wheel scrolls across with Shift held. */
        if (down) out[n++] = (struct gui_event){.type = EV_WHEEL, .x = x, .y = y, .wheel = down, .mods = automation_mods(d)};
        if (right) out[n++] = (struct gui_event){.type = EV_WHEEL, .x = x, .y = y, .wheel = right,
                                                 .mods = automation_mods(d) | NMOD_SHIFT};
        return n;
    }
    if (step->kind != WEB_AUTOMATION_MOVE && step->kind != WEB_AUTOMATION_DOWN && step->kind != WEB_AUTOMATION_UP) {
        *error = "Unknown automation step"; return -1;
    }
    uint32_t bit = step->kind == WEB_AUTOMATION_MOVE ? 0 : automation_button(step->value);
    if (step->kind != WEB_AUTOMATION_MOVE && !bit) { *error = "Unknown pointer button"; return -1; }
    /* WebDriver: a move to where the pointer already is dispatches nothing. */
    if (x != d->x || y != d->y) {
        out[n++] = (struct gui_event){.type = EV_MOUSE_MOVE, .x = x, .y = y, .buttons = d->buttons, .mods = automation_mods(d)};
        d->x = x; d->y = y;
    }
    /* WebDriver: pressing a held button or releasing one not held does nothing. */
    if (step->kind == WEB_AUTOMATION_MOVE || (step->kind == WEB_AUTOMATION_DOWN) == ((d->buttons & bit) != 0)) return n;
    /* The buttons after the change, as the window's own pointer events carry them. */
    if (step->kind == WEB_AUTOMATION_DOWN) d->buttons |= bit; else d->buttons &= ~bit;
    out[n++] = (struct gui_event){.type = step->kind == WEB_AUTOMATION_DOWN ? EV_MOUSE_DOWN : EV_MOUSE_UP,
                                  .x = x, .y = y, .buttons = d->buttons, .mods = automation_mods(d)};
    return n;
}

#endif
