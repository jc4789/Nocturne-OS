/* The interface toolkit of Nocturne (libc/ui.c): one engine that lays out,
   paints and runs the widgets of every app, whatever language it is written
   in, and the smooth shapes and text it draws with.

   An app describes a window as a tree of widgets in the view format below and
   gets back events ("button 3 was clicked", "the slider moved to 40"). The
   engine keeps everything between: hover, presses, focus, the caret, scrolling,
   animations, the theme. Apps talk to it in messages (a ui_session):
     - C and the js program call ui_session_send / ui_session_peek in the same process;
     - programs that cannot call C (Go) send the same bytes over a channel to
       uihost, which runs a session for them.
   docs/nocturne-native-abi.md §11 describes the format for binding authors.

   Colours are 0xAARRGGBB; the alpha byte is the opacity. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gfx.h"
#include "nocturne.h"
#include "display_abi.h"
#include <cdefs.h>
NOCTURNE_BEGIN_DECLS

/* ---- the theme ---- */
#define UI_THEME_COLOURS(X) \
    X(background, 0xFF16151Du) X(sidebar, 0xFF1D1C27u) X(surface, 0xFF211F2Cu) X(surfaceHover, 0xFF2A2837u) \
    X(surfaceActive, 0xFF332F44u) X(control, 0xFF2A2838u) X(controlHover, 0xFF34314Au) X(border, 0xFF2F2C3Eu) \
    X(divider, 0xFF2A2837u) X(text, 0xFFEEECF6u) X(textDim, 0xFFA6A2BAu) X(textFaint, 0xFF6F6B85u) \
    X(accent, 0xFF8B7CF6u) X(accentHover, 0xFF9C8FF8u) X(accentPressed, 0xFF7A6AE8u) X(accentText, 0xFFFFFFFFu) \
    X(warning, 0xFFF2B866u) X(danger, 0xFFF07A7Au) X(success, 0xFF6FD6A0u) X(shadow, 0x66000000u)
#define UI_THEME_SIZES(X) \
    X(radius, 10) X(controlRadius, 8) X(fontSize, 14) X(smallSize, 12.5f) X(titleSize, 22) X(headingSize, 15)

struct ui_theme {
#define UI_COLOUR_FIELD(name, value) uint32_t name;
#define UI_SIZE_FIELD(name, value) float name;
    UI_THEME_COLOURS(UI_COLOUR_FIELD)
    UI_THEME_SIZES(UI_SIZE_FIELD)
#undef UI_COLOUR_FIELD
#undef UI_SIZE_FIELD
};
extern const struct ui_theme ui_theme;

/* ---- sessions ----
   Messages from the app (all numbers little-endian; f64 is an IEEE double,
   str is u32 length + UTF-8 bytes):
     'O' u32 window, u32 width, u32 height, u32 flags (UI_WINDOW_FLAGS),
         u32 background (0: the theme's), str title       open a window
     'V' u32 window, node                                  show this view
     'T' u32 window, str title
     'C' u32 window                                        close it
     'M' u32 request, f64 size, u32 weight (UI_TEXT_*), str text   measure text
     'Q'                                                   the theme
     'S' u32 surface, u32 width, u32 height [u32 flags]    make or resize a surface (UI_SURFACE_DEEP)
     'P' u32 surface [u32 x, y, width, height]             its pixels (in that part) changed
     'F' u32 window                                        UI_EVENT_FRAME at the next frame, changed or not
     'D' u32 surface                                       drop it
   A node is: u8 kind length, kind, u16 property count, u16 child count, the
   properties, the children. A property is: u8 name length, name, u8 type,
   value; types 1 f64, 2 str, 3 u32 n + n f64, 4 u32 n + n str, 5 u32 n + n
   bytes. Unknown kinds draw nothing and unknown properties are ignored, so a
   newer engine runs older apps. Kinds and properties are listed in
   docs/nocturne-native-abi.md §11.

   Messages to the app (replies, 'o' 'm' 'q' 's', come ahead of waiting events,
   in the order of their requests):
     'o' u32 window, i32 result (0 or -N_ERR_*), u32 width, u32 height
     'e' u32 window, u32 view (which 'V' it refers to, counting from 1), u32 event
         (UI_EVENT_*), u32 node (its place in that view, counted in pre-order from
         0; 0xFFFFFFFF for the window), u32 mods (NMOD_*), u32 flags, f64 a, f64 b, str text
     'm' u32 request, f64 width, f64 line height, f64 baseline
     'q' u32 count, count x (u8 name length, name, f64 value)
     's' u32 surface, i32 result, u32 width, u32 height; through uihost the
         message carries the surface's shared pages (n_shared_map them) */
enum {
    UI_EVENT_CLICK = 1,  /* button, list item, box with "clickable" */
    UI_EVENT_TOGGLE,     /* toggle, checkbox: a = the new state (0 or 1) */
    UI_EVENT_SLIDE,      /* slider: a = value; flags UI_DONE when the drag ended */
    UI_EVENT_CHOOSE,     /* segmented: a = the option's index */
    UI_EVENT_TEXT,       /* field: text = what it should hold now */
    UI_EVENT_SUBMIT,     /* field: Enter, text = its value */
    UI_EVENT_KEY,        /* a key no widget took: text = "Escape", "Enter", "ArrowUp"..., "F12" or the character,
                            a = its NKEY_ code or character; flags UI_UP when it was let go */
    UI_EVENT_CLOSE,      /* the close button (flags UI_GONE: the window is already gone) */
    UI_EVENT_CANVAS,     /* a canvas or surface got a size it was not made for: a, b = width, height */
    UI_EVENT_RESIZE,     /* a, b = the window's new size */
    UI_EVENT_POINTER,    /* over a canvas or surface with "pointer": a, b = the position in it; flags UI_DOWN
                            or UI_UP (neither: a move) and the buttons held, UI_BUTTONS(flags) (1 left, 2 right,
                            4 middle). Once a button is down there, the pointer is its until all are up; when
                            it leaves, one more move comes with a position outside. */
    UI_EVENT_WHEEL,      /* over a canvas or surface with "pointer": a = notches (positive: towards the user) */
    UI_EVENT_FOCUS,      /* a field got (a = 1) or lost (a = 0, flags UI_CANCEL when Escape gave it up) the
                            keyboard; for the window (node 0xFFFFFFFF): the window did */
    UI_EVENT_DIRECT,     /* the window (node 0xFFFFFFFF) asked for win_direct: the display driver started (a = 1)
                            or stopped (a = 0) showing the app's own images instead of the window (EV_DIRECT) */
    UI_EVENT_FRAME,      /* the window (node 0xFFFFFFFF): a frame of the screen took what ui_surface_changed showed,
                            or came after ui_window_frame; one event for any number of changes and requests;
                            a = the display's frame period in ms (the next frame comes about that much later). It
                            comes at the display's pace (WIN_FRAME_EVENTS), so an animation that draws its next
                            picture on it draws once a frame at any refresh rate; while the window is not shown
                            it waits */
};
/* A field holds its caret and selection itself (arrows, Home/End, Shift, Ctrl for words, Ctrl+A/C/X/V,
   clicks and drags); the keys it has no use for come as UI_EVENT_KEY. Some properties act when their
   value changes from one view to the next, so an app counts up to ask again:
     "focus" (field)   a positive number: take the keyboard (caret at the end); zero or less: give it up
     "reveal" (any)    scroll the scroll box it is in until it can be seen
   and "selectOnFocus" (field) selects all its text when it gets the keyboard; "ellipsis" (text)
   keeps it on one line and cuts what does not fit with "…"; "mono" (text, field) sets it in the
   monospaced face. */
#define UI_DONE 1u  /* slide: the drag ended */
#define UI_GONE 2u  /* close: the window is already gone */
#define UI_DOWN 4u  /* pointer: a button went down */
#define UI_UP 8u    /* pointer: a button came up; key: the key was let go */
#define UI_CANCEL 16u /* focus: Escape gave the keyboard up */
#define UI_BUTTONS(flags) ((flags) >> 8 & 0xFFu)

/* The window flags an app may ask for: as for win_open. With WIN_DOUBLE_BUFFER each
   picture waits for a free buffer (paced by the display) and is painted whole; with
   WIN_DEEP_COLOR deep surfaces keep their extra bits on a 10 bpc desktop. */
#define UI_WINDOW_FLAGS \
    (WIN_RESIZABLE | WIN_CENTER | WIN_START_MAXIMIZED | WIN_ALWAYS_ON_TOP | WIN_DOUBLE_BUFFER | WIN_DEEP_COLOR)
/* A surface with a deep plane after its pixels: one byte a pixel, laid out as for
   win_deep (display.h). */
#define UI_SURFACE_DEEP 1u

struct ui_session;
struct ui_session *ui_session_new(void);
void ui_session_free(struct ui_session *s); /* closes its windows */
/* One message from the app: 0, or -N_ERR_INVALID when it is malformed. */
n_result ui_session_send(struct ui_session *s, const void *msg, size_t n);
/* The oldest message to the app (valid until ui_session_pop), or NULL. */
const void *ui_session_peek(struct ui_session *s, size_t *n);
void ui_session_pop(struct ui_session *s);
/* Its windows are watched in this event queue with tokens below 2^32; a caller
   may add its own handles with larger tokens and wait on it itself. */
n_handle ui_session_queue(struct ui_session *s);
/* Milliseconds until an animation or the caret needs the engine, -1 for none. */
int ui_session_timeout(struct ui_session *s);
/* Takes what the windows sent and runs due animations, without waiting. */
void ui_session_pump(struct ui_session *s);
/* Waits up to timeout ms (-1: no limit) for the windows or a timer, then pumps;
   returns whether messages to the app are waiting. */
bool ui_session_wait(struct ui_session *s, int timeout);
bool ui_session_open(struct ui_session *s); /* whether it has a window */
/* A surface's pixels (0xFFRRGGBB, opaque) for an app in the same process to draw
   into, and the shared pages behind them for uihost to hand on; both stay
   valid until the surface is resized or dropped. */
canvas_t *ui_session_surface(struct ui_session *s, uint32_t surface);
n_handle ui_session_surface_handle(struct ui_session *s, uint32_t surface);
/* The window_t behind a window, for an app in the same process that needs what only the
   window has (win_state, win_fullscreen, its handle to wait on with other handles); the
   engine draws into it and takes its events. NULL when there is no such window. */
window_t *ui_session_window(struct ui_session *s, uint32_t window);
/* The deep plane of a UI_SURFACE_DEEP surface, or NULL when it has none or the
   desktop has 8 bits per channel (ask again each picture, like win_deep). */
uint8_t *ui_session_surface_deep(struct ui_session *s, uint32_t surface);

/* ---- the session from C ----
   A view is written node by node: ui_node opens one (and returns its place,
   which events name), its properties follow, then its children, then ui_end:

       struct ui_msg m;
       ui_view(&m, 1);
       ui_node(&m, "column"); ui_num(&m, "padding", 16);
           ok = ui_node(&m, "button"); ui_str(&m, "text", "OK"); ui_end(&m);
       ui_end(&m);
       ui_send(s, &m);                                                       */
struct ui_bytes { uint8_t *p; size_t n, cap; bool bad; };
struct ui_msg {
    struct ui_bytes b;
    uint32_t nodes;  /* nodes opened so far */
    uint32_t depth, capdepth;
    struct ui_open { size_t at; uint16_t props, kids; } *open;
};
void ui_view(struct ui_msg *m, uint32_t window);       /* starts a 'V' message */
uint32_t ui_node(struct ui_msg *m, const char *kind);  /* a child of the open node; its place */
void ui_end(struct ui_msg *m);                         /* closes the last node opened */
void ui_num(struct ui_msg *m, const char *name, double value);
void ui_str(struct ui_msg *m, const char *name, const char *s);
void ui_strn(struct ui_msg *m, const char *name, const char *s, size_t n);
void ui_nums(struct ui_msg *m, const char *name, const double *v, uint32_t n);
void ui_strs(struct ui_msg *m, const char *name, const char *const *v, uint32_t n);
void ui_blob(struct ui_msg *m, const char *name, const void *p, uint32_t n);
/* Sends the view and frees the message; N_ERR_INVALID if it was not complete. */
n_result ui_send(struct ui_session *s, struct ui_msg *m);

/* A window: its size comes back in *width and *height (either may be NULL). */
n_result ui_window(struct ui_session *s, uint32_t window, int width, int height, uint32_t flags, const char *title,
                   int *out_width, int *out_height);
void ui_window_close(struct ui_session *s, uint32_t window);
void ui_window_title(struct ui_session *s, uint32_t window, const char *title);
/* UI_EVENT_FRAME at the display's next frame even if nothing changes ('F'): an animation that waits for
   the display before it decides what to draw (a browser's animation frames). 0 when it was asked. */
n_result ui_window_frame(struct ui_session *s, uint32_t window);
/* A surface of width x height pixels to draw into (NULL if it cannot be made);
   ui_surface_changed shows what was drawn, ui_surface_changed_rect only the part
   that changed (only that part is copied to the screen). */
canvas_t *ui_surface(struct ui_session *s, uint32_t surface, int width, int height, uint32_t flags);
void ui_surface_changed(struct ui_session *s, uint32_t surface);
void ui_surface_changed_rect(struct ui_session *s, uint32_t surface, int x, int y, int width, int height);

/* An 'e' message taken apart (text points into the message). */
struct ui_event {
    uint32_t window, view, type, node, mods, flags;
    double a, b;
    const char *text;
    size_t text_n;
};
bool ui_event_read(const void *msg, size_t n, struct ui_event *e);
/* The next message, if it is an event (it is then popped); other messages are dropped. */
bool ui_next_event(struct ui_session *s, struct ui_event *e, char *text, size_t cap);

/* ---- drawing (what the engine paints with; for apps that draw their own content) ---- */
/* A box with corners of radius r (clamped to half the smaller side). */
void ui_round_fill(canvas_t *c, float x, float y, float w, float h, float r, uint32_t argb);
/* A border of the given width inside the box. */
void ui_round_stroke(canvas_t *c, float x, float y, float w, float h, float r, float width, uint32_t argb);
/* The soft shadow of a box: opacity fades out over blur pixels around its edge. */
void ui_round_shadow(canvas_t *c, float x, float y, float w, float h, float r, float blur, uint32_t argb);
/* A box whose colour goes from top to bottom. */
void ui_round_gradient(canvas_t *c, float x, float y, float w, float h, float r, uint32_t top, uint32_t bottom);
/* A line with round ends. */
void ui_line(canvas_t *c, float x0, float y0, float x1, float y1, float width, uint32_t argb);

/* Interface text in Noto Sans, or Maple Mono with UI_TEXT_MONO (weight: UI_TEXT_* bits; bold is
   synthesized where a face has none), with CJK from the bundled fallbacks; size in pixels per em.
   ui_text draws with the top of the line box at y and returns the width. */
enum { UI_TEXT_BOLD = 1, UI_TEXT_MONO = 2 };
float ui_text(canvas_t *c, float x, float y, const char *utf8, size_t n, float size, int weight, uint32_t argb);
float ui_text_width(const char *utf8, size_t n, float size, int weight);
/* Height of a line box (ascent + descent) and the baseline's distance from its top. */
float ui_line_height(float size, int weight, float *baseline);

NOCTURNE_END_DECLS
