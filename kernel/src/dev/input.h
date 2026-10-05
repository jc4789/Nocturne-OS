#pragma once
#include <stdint.h>
#include <stdbool.h>

/* special key codes (printable keys use their character value) */
enum {
    KEY_ESC = 0x1B, KEY_BACKSPACE = 0x08, KEY_TAB = 0x09, KEY_ENTER = 0x0A,
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_INSERT, KEY_DELETE, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8,
    KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_LSHIFT, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL,
    KEY_LALT, KEY_RALT, KEY_CAPS, KEY_SUPER, KEY_MENU, KEY_UNKNOWN,
};

#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   4
#define MOD_SUPER 8

struct key_event {
    uint16_t key;  /* KEY_* or character (lowercase/shifted as typed) */
    uint8_t mods;
    bool pressed;
};

struct mouse_event {
    int dx, dy, wheel;
    uint8_t buttons; /* bit0 left, bit1 right, bit2 middle */
};

typedef void (*key_sink_t)(const struct key_event *e);
typedef void (*mouse_sink_t)(const struct mouse_event *e);

void input_init(void);
void input_set_sinks(key_sink_t k, mouse_sink_t m);
bool input_mouse_present(void);
