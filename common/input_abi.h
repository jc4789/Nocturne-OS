/* 入力の ABI：キーの物理の番号（HID の usage）、窓の事象のキーの値、配列、入力の装置（§9、§12）。

   カーネルは HID の usage と修飾、相対量・絶対位置を渡す。文字を知らない。usage から文字への変換は libc の
   n_key_translate（user/libc/keymap.c）だけが行う。配列の番号はカーネルにとってただの数で、装置ごとに覚えて
   事象に写すだけ。 */
#ifndef COMMON_INPUT_ABI_H
#define COMMON_INPUT_ABI_H
#include <stdint.h>

/* HID Usage Tables の Keyboard/Keypad page (0x07) の番号。カーネルと呼ぶ側が名指しで使うもの。 */
enum n_key_usage {
    N_USAGE_NONE = 0,
    N_USAGE_FIRST = 0x04, /* これより下は鍵でない（ErrorRollOver など） */
    N_USAGE_T = 0x17,
    N_USAGE_ESC = 0x29,
    N_USAGE_TAB = 0x2B,
    N_USAGE_CAPS_LOCK = 0x39,
    N_USAGE_F4 = 0x3D,
    N_USAGE_LEFT_CTRL = 0xE0, N_USAGE_LEFT_SHIFT, N_USAGE_LEFT_ALT, N_USAGE_LEFT_GUI,
    N_USAGE_RIGHT_CTRL, N_USAGE_RIGHT_SHIFT, N_USAGE_RIGHT_ALT, N_USAGE_RIGHT_GUI,
};

/* 修飾。押されている間と Caps Lock の状態はカーネルが持つ。アプリに届くのは SHIFT・CTRL・ALT・SUPER で、
   CAPS と RIGHT_ALT は n_key_translate が読んで落とす。 */
/* コンソールから読むキーの押下の一通（N_KIND_CONSOLE）。カーネルは usage のまま渡し、文字は読む側の libc が
   作る（n_console_receive）。text は host が打った文字で、usage は 0。 */
struct n_console_key {
    uint32_t usage, text, layout, mods;
};

#define NMOD_SHIFT 1u
#define NMOD_CTRL 2u
#define NMOD_ALT 4u
#define NMOD_SUPER 8u
#define NMOD_CAPS 16u
#define NMOD_RIGHT_ALT 32u

/* struct gui_event の key の値：文字なら Unicode のスカラー値。名前のキーは Unicode の外。 */
enum {
    NKEY_ESC = 0x1B, NKEY_BACKSPACE = 0x08, NKEY_TAB = 0x09, NKEY_ENTER = 0x0A,
    NKEY_UP = 0x110000, NKEY_DOWN, NKEY_LEFT, NKEY_RIGHT, NKEY_HOME, NKEY_END, NKEY_PGUP, NKEY_PGDN,
    NKEY_INSERT, NKEY_DELETE, NKEY_F1, NKEY_F2, NKEY_F3, NKEY_F4, NKEY_F5, NKEY_F6, NKEY_F7, NKEY_F8,
    NKEY_F9, NKEY_F10, NKEY_F11, NKEY_F12, NKEY_LSHIFT, NKEY_RSHIFT, NKEY_LCTRL, NKEY_RCTRL,
    NKEY_LALT, NKEY_RALT, NKEY_CAPS, NKEY_SUPER, NKEY_MENU,
    NKEY_UNKNOWN, /* 今の配列に値の無いキー。物理のキーは usage が言う。 */
};
/* key が挿し入れる文字か（制御の文字と名前のキーでない）。 */
#define N_KEY_IS_TEXT(k) ((uint32_t)(k) >= 32u && (uint32_t)(k) != 127u && (uint32_t)(k) <= 0x10FFFFu && \
                          !((uint32_t)(k) >= 0xD800u && (uint32_t)(k) <= 0xDFFFu))

/* 配列。表は user/libc/keymap.c にある。 */
enum n_key_layout {
    N_LAYOUT_US,
    N_LAYOUT_JIS,
    N_LAYOUT_UK,
    N_LAYOUT_COUNT
};

/* 入力の装置の値（settings_abi.h の N_INPUT_DEVICE）。今の値の読出しは、つながっている装置と選択のある装置の
   記録の並びを返す。適用は一つの記録で、数の欄が全部 N_INPUT_SAME なら選択を消す。 */
#define N_INPUT_KEYBOARD 1u
#define N_INPUT_POINTER 2u
#define N_INPUT_ATTACHED 4u /* 今つながっている。読むときだけ。 */
#define N_INPUT_SAME INT32_MIN /* この欄は利用者の既定に従う */
/* 記録の頭。後ろに識別子と名の UTF-8 が続く（終端なし）。識別子は USB なら "usb:vvvv:pppp:xxxxxxxx:i"（x は通し番号か
   場所の hash、i はインターフェース）。名は装置が名乗るもので、読むときだけ入り、無ければ長さ 0。 */
struct n_setting_input_device {
    uint32_t size;               /* 記録の全バイト数。次の記録はここから 8 の倍数に揃えた先 */
    uint32_t id_length, name_length;
    uint32_t state;              /* 読むときだけ：N_INPUT_* */
    int32_t pointer_speed;       /* N_INPUT_POINTER_SPEED と同じ */
    int32_t pointer_accel;       /* N_INPUT_POINTER_ACCEL と同じ */
    int32_t scroll_natural;      /* N_INPUT_SCROLL_NATURAL と同じ */
    int32_t repeat_delay_ms;     /* N_INPUT_REPEAT_DELAY_MS と同じ */
    int32_t repeat_interval_ms;  /* N_INPUT_REPEAT_INTERVAL_MS と同じ */
    int32_t layout;              /* N_INPUT_LAYOUT と同じ */
};
#define N_INPUT_RECORD_NEXT(size) (((size) + 7u) & ~7u)
#endif
