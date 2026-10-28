/* Keeping the system's state on C: (docs/nocturne-native-abi.md §12).

   A part of the system that applies something the user chose owns it: the desktop the pointer's
   speed and smooth gradients (gui/wm.c), sound the volume and muting (dev/audio.c), the screen's
   mode the mode chosen for each monitor (gui/screenmode.c). Each keeps its state in a document of
   its own, the user's AppData\Nocturne\<name>.conf: lines of key=value, a line that begins with '#'
   a note. The part says which keys are its and puts a value in place; keep knows no key. It reads
   every part's document back before the first program starts (keep_start), writes a part's
   document KEEP_AFTER_MS after its last change, and writes at once before the power goes
   (keep_flush). A part without a document of its own takes its lines from the document an older
   system kept them all in, once. The documents are no interface: a program asks the part (its
   system call) and watches it (sys/watch.h).

   BSP. keep_changed and keep_register take no lock and do not sleep (a part may hold its own
   lock); keep_start and keep_flush may sleep. line and text run on keep's thread, or on the thread
   that calls keep_flush, and may take the part's own lock. */
#pragma once
#include "kernel.h"

#define KEEP_AFTER_MS 500 /* a change goes to C: once its part has stopped changing this long */

struct keep {
    const char *name; /* its document in AppData\Nocturne: "Desktop" is Desktop.conf */
    /* A line of its document, or of an older system's (key and value without the spaces around
       them): whether key is the part's. With apply, a value the part takes is put in place (as a
       change of its own: what watches it hears of it); a value it does not take is left out. */
    bool (*line)(const char *key, const char *value, bool apply);
    /* The part's state now, as the lines of its document: the length of the whole text, which may
       be cap or more (then out holds what fits and a zero, and keep asks again with more room). */
    size_t (*text)(char *out, size_t cap);
    /* keep's own */
    struct keep *next;
    bool read;       /* its document was read (or there was none): a change from now on is kept */
    bool dirty;      /* changed since it was last written */
    bool retry;      /* the last write failed: the next change or keep_flush writes it */
    bool reading;    /* in the round of reading going on */
    bool missing;    /* it has no document of its own (that round) */
    bool took;       /* it took a line of an older system's document (that round) */
    uint64_t dirty_at; /* uptime_ms of the last change */
};

/* A part, before keep_start or after (then it is read at once, on keep's thread). */
void keep_register(struct keep *k);
/* C: and the user's folder are ready (store_start): every part registered so far is read back,
   then this returns. Changes are kept from then on. */
void keep_start(void);
/* The part changed its state: written KEEP_AFTER_MS after the last change. Nothing before the
   part's document was read. */
void keep_changed(struct keep *k);
/* Every part changed and not written yet is written now (a power change, before store_sync). */
void keep_flush(void);

/* For parts: a whole decimal number from low to high (a '-' first for a negative one). */
bool keep_number(const char *s, int64_t low, int64_t high, int64_t *out);
/* For text: what fmt gives, at out + *n while it fits; *n counts it all. */
void keep_put(char *out, size_t cap, size_t *n, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
