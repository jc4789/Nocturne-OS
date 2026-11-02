/* キーの usage から文字へ。文字を決めるのはここだけ。カーネルは HID の usage と修飾と配列の番号を渡し、
   窓の事象を取り出す win_event がここを呼ぶ。行は {押しただけ, Shift, AltGr}。0 は値が無い。 */
#include "nocturne.h"

typedef const uint16_t layout_table[256][3];

#define LETTERS \
    [0x04] = {'a', 'A'}, {'b', 'B'}, {'c', 'C'}, {'d', 'D'}, {'e', 'E'}, {'f', 'F'}, {'g', 'G'}, {'h', 'H'}, \
    {'i', 'I'}, {'j', 'J'}, {'k', 'K'}, {'l', 'L'}, {'m', 'M'}, {'n', 'N'}, {'o', 'O'}, {'p', 'P'}, \
    {'q', 'Q'}, {'r', 'R'}, {'s', 'S'}, {'t', 'T'}, {'u', 'U'}, {'v', 'V'}, {'w', 'W'}, {'x', 'X'}, \
    {'y', 'Y'}, {'z', 'Z'}
#define SPACE [0x2C] = {' ', ' '}

static layout_table us = {
    LETTERS, SPACE,
    [0x1E] = {'1', '!'}, {'2', '@'}, {'3', '#'}, {'4', '$'}, {'5', '%'}, {'6', '^'}, {'7', '&'}, {'8', '*'}, {'9', '('}, {'0', ')'},
    [0x2D] = {'-', '_'}, {'=', '+'}, {'[', '{'}, {']', '}'}, {'\\', '|'}, {'\\', '|'}, {';', ':'}, {'\'', '"'}, {'`', '~'},
    {',', '<'}, {'.', '>'}, {'/', '?'},
    [0x64] = {'\\', '|'},
};

/* JIS X 6002 の配列。ろの鍵（International1）と ¥ の鍵（International3）。¥ の鍵は C: のパスに使う '\' を打つ。 */
static layout_table jis = {
    LETTERS, SPACE,
    [0x1E] = {'1', '!'}, {'2', '"'}, {'3', '#'}, {'4', '$'}, {'5', '%'}, {'6', '&'}, {'7', '\''}, {'8', '('}, {'9', ')'}, {'0', 0},
    [0x2D] = {'-', '='}, {'^', '~'}, {'@', '`'}, {'[', '{'}, {']', '}'}, {']', '}'}, {';', '+'}, {':', '*'}, {0, 0},
    {',', '<'}, {'.', '>'}, {'/', '?'},
    [0x87] = {'\\', '_'},
    [0x89] = {'\\', '|'},
};

static layout_table uk = {
    LETTERS, SPACE,
    [0x1E] = {'1', '!'}, {'2', '"'}, {'3', 0xA3}, {'4', '$', 0x20AC}, {'5', '%'}, {'6', '^'}, {'7', '&'}, {'8', '*'}, {'9', '('}, {'0', ')'},
    [0x2D] = {'-', '_'}, {'=', '+'}, {'[', '{'}, {']', '}'}, {'#', '~'}, {'#', '~'}, {';', ':'}, {'\'', '@'}, {'`', 0xAC, 0xA6},
    {',', '<'}, {'.', '>'}, {'/', '?'},
    [0x64] = {'\\', '|'},
};

static const layout_table *const layouts[N_LAYOUT_COUNT] = {
    [N_LAYOUT_US] = &us, [N_LAYOUT_JIS] = &jis, [N_LAYOUT_UK] = &uk,
};

/* 配列によらないキー。 */
static const uint32_t named[256] = {
    [0x28] = NKEY_ENTER, NKEY_ESC, NKEY_BACKSPACE, NKEY_TAB,
    [0x39] = NKEY_CAPS, NKEY_F1, NKEY_F2, NKEY_F3, NKEY_F4, NKEY_F5, NKEY_F6, NKEY_F7, NKEY_F8, NKEY_F9, NKEY_F10, NKEY_F11, NKEY_F12,
    [0x49] = NKEY_INSERT, NKEY_HOME, NKEY_PGUP, NKEY_DELETE, NKEY_END, NKEY_PGDN, NKEY_RIGHT, NKEY_LEFT, NKEY_DOWN, NKEY_UP,
    [0x54] = '/', '*', '-', '+', NKEY_ENTER, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '.',
    [0x65] = NKEY_MENU,
    [0xE0] = NKEY_LCTRL, NKEY_LSHIFT, NKEY_LALT, NKEY_SUPER, NKEY_RCTRL, NKEY_RSHIFT, NKEY_RALT, NKEY_SUPER,
};

void n_key_translate(struct gui_event *e) {
    uint32_t usage = e->usage, mods = e->mods;
    e->mods = mods & (NMOD_SHIFT | NMOD_CTRL | NMOD_ALT | NMOD_SUPER);
    if (!usage) return; /* 文字だけの入力：key が文字 */
    if (usage < 256 && named[usage]) {
        e->key = named[usage];
        return;
    }
    const layout_table *layout = e->layout < N_LAYOUT_COUNT ? layouts[e->layout] : &us;
    const uint16_t *row = usage < 256 ? (*layout)[usage] : NULL;
    if (!row || !row[0]) {
        e->key = NKEY_UNKNOWN;
        return;
    }
    if ((mods & NMOD_RIGHT_ALT) && row[2]) {
        e->key = row[2];
        e->mods &= ~NMOD_ALT;
        return;
    }
    bool letter = row[0] >= 'a' && row[0] <= 'z';
    bool upper = (mods & NMOD_SHIFT) != 0;
    if (letter && (mods & NMOD_CAPS)) upper = !upper;
    e->key = upper && row[1] ? row[1] : row[0];
}
