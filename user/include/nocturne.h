/* Nocturne OS system interface. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "abi.h"
#include "gfx.h"
#include <cdefs.h>
NOCTURNE_BEGIN_DECLS

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define ABS(a) ((a) < 0 ? -(a) : (a))

/* ---- native ABI (docs/nocturne-native-abi.md) ----
   These return a negative n_result on failure and never touch errno. */
typedef int32_t n_handle;
typedef int64_t n_result;

/* malloc's heap: bytes in its n_memory regions, and bytes in allocated blocks. */
void malloc_usage(size_t *heap, size_t *used);
uint64_t uptime_ms(void);
uint64_t uptime_us(void); /* the same clock in microseconds, for timing short work */
/* Native memory (docs/nocturne-native-abi.md §7): zero-filled pages with
 * N_MEMORY_READ, _READ_WRITE or _READ_EXECUTE access; reserve also takes
 * N_MEMORY_LOW added to the access, for pages below 2 GiB. reserve puts the
 * pages in *memory (NULL on failure). protect on a shared mapping keeps within
 * one mapping and its access (never execution), and holds for its pages not
 * mapped yet too. */
n_result n_memory_reserve(size_t bytes, int access, void **memory);
n_result n_memory_release(void *addr, size_t bytes);
n_result n_memory_protect(void *addr, size_t bytes, int access);
/* Shared pages: a handle is a private capability that travels to another app
 * only as a channel attachment (n_channel_send). A mapping keeps the pages
 * after the handle closes; n_memory_release of the whole mapping ends it. */
n_handle n_shared_create(size_t bytes);
/* Like n_shared_create, but pages are zero-filled at first touch by any mapper. */
n_handle n_shared_reserve(size_t bytes);
/* Map all of the pages (bytes is their page-rounded size) with N_MEMORY_READ
 * or N_MEMORY_READ_WRITE access into *mapping (NULL on failure). */
n_result n_shared_map(n_handle shared, size_t bytes, int access, void **mapping);

const char *n_result_text(n_result r); /* either sign */
n_result n_handle_close(n_handle h);   /* commits an N_DOCUMENT_REPLACE handle */
n_result n_handle_kind(n_handle h);    /* N_KIND_* */
/* Documents and folders: paths are complete, "C:\a\b" ("\a\b" is the same, and '/' works as '\').
   C: is the only drive. A name is 1 to 255 bytes of UTF-8 without control characters,
   \ / : * ? " < > |, or a space or period at its end; names compare without regard to case. */
n_handle n_document_open(const char *path, int access); /* N_DOCUMENT_* */
n_result n_document_read_at(n_handle h, uint64_t offset, void *buf, size_t n); /* bytes; 0 at the end */
n_result n_document_write_at(n_handle h, uint64_t offset, const void *buf, size_t n);
n_result n_document_size(n_handle h);
/* The open document's entry as n_entry_info gives it, with no name and the size this handle reads. */
n_result n_document_info(n_handle h, struct n_entry *out);
n_result n_document_set_size(n_handle h, uint64_t size); /* new bytes read as zero */
n_result n_document_claim(n_handle h, bool wait);        /* N_ERR_NOT_READY when not waiting */
n_result n_document_release(n_handle h);
n_result n_document_commit(n_handle h, int how);         /* N_COMMIT_*: save it, now or in the background */
n_result n_document_save(const char *path, const void *data, size_t size); /* whole and atomic */
n_result n_document_append(const char *path, const void *data, size_t size);
/* The whole document in fresh n_memory pages; free with n_memory_release(*data, *size). */
n_result n_document_load(const char *path, void **data, size_t *size);
n_result n_entry_info(const char *path, struct n_entry *out);
n_result n_entry_remove(const char *path);
n_result n_entry_move(const char *from, const char *to); /* never replaces: N_ERR_EXISTS */
/* n_entry_move that lets document from take the name of a document, which goes in the same commit
   (as n_entry_remove takes it: its open handles keep it). Any other entry there: N_ERR_EXISTS. */
n_result n_entry_replace(const char *from, const char *to);
/* A watch on C: (n_watch_take). N_WATCH_NAME watches the last name of path in its folder, whether
   an entry has it or not; N_WATCH_FOLDER every entry directly in the folder path names (not what
   is inside its folders). A change counts once a new opening, n_entry_info or n_folder_list sees
   it: an entry made, removed, moved in or out, renamed (case too) or replaced (n_entry_replace), or
   a document's written content taken in (a save, its last writer's close, the system's writer).
   The folder is held by what it is: moved, it is still watched; removed, the watch turns
   N_EVENT_CLOSED. Entries not there for the caller (PRIVATE) count nothing. N_ERR_NOT_FOUND
   without the folder, N_ERR_INVALID for another what. */
n_handle n_entry_watch(const char *path, uint32_t what);
n_result n_folder_create(const char *path);
n_result n_folder_list(const char *path, struct n_entry *out, size_t max, uint64_t start); /* count */
/* Every entry of a folder in a new array (free it); returns the count. */
n_result n_folder_entries(const char *path, struct n_entry **out);
/* Message channels: one send is one message. attach is a document or shared
   handle (or -1) that travels with it and arrives as a new handle in *attached. */
n_result n_channel_pair(n_handle out[2]);
n_result n_channel_send(n_handle h, const void *data, size_t n, n_handle attach);
/* N_CHANNEL_WAIT waits; N_CHANNEL_PEEK only reports the next message's size;
   N_CHANNEL_DISCARD copies at most cap bytes of it, then drops it.
   A message larger than cap stays queued and gives N_ERR_TOO_LARGE. */
n_result n_channel_receive(n_handle h, void *buf, size_t cap, int flags, n_handle *attached);
/* Message channels only. Kernel-authenticated send-time app ID; all threads
   of one app agree, including across connections or transferred endpoints.
   message is required. Peek reports credentials without taking an attachment. */
n_result n_channel_receive_from(n_handle h, void *buf, size_t cap, int flags, struct n_channel_message *message);
/* Named rendezvous, not a folder or network name: the key bytes are matched in
   full, so callers put their own identity into it. listen owns the key until
   its handle closes (N_ERR_EXISTS meanwhile); connect gives a message channel
   whose other end waits in the listener (N_EVENT_RECEIVABLE) for accept, which
   without one gives N_ERR_NOT_READY. A listener that closes closes them too. */
n_handle n_channel_listen(const char *key);
n_handle n_channel_connect(const char *key);
n_handle n_channel_accept(n_handle listener);
/* Apps: the handle reports the end (N_EVENT_APP_ENDED); there is no zombie to reap. */
n_handle n_app_launch(const char *path, const struct n_launch *launch);
n_result n_app_result(n_handle app, int64_t timeout_ms, int *status); /* -1 waits; 0 polls */
n_result n_app_stop(n_handle app, int flags);                         /* N_APP_STOP_TREE */
n_result n_app_self(void);
n_result n_app_parent(void);
n_handle n_app_open(int64_t id); /* a running app by the identifier the app list shows */
n_result n_system_log(char *buf, size_t cap); /* the newest system log text; its length */
/* A handle whose messages are added to the system log, and which receives the log's text from the
   oldest record the kernel holds at its first receive on. */
n_handle n_system_log_open(void);
n_result n_app_id(n_handle app); /* its identifier, also after its end (identifiers are not reused) */
/* Drivers in apps (docs/nocturne-native-abi.md §9). Only init may claim a PCI function; it
   hands the N_KIND_PCI handle to the driver app it launches. A claim is exclusive until its
   last handle closes, which also turns the function's interrupts off. Isolation is required
   unless appdrivers-unisolated was explicitly selected at boot. A device left active needs
   recovery; n_pci_info reports N_PCI_NEEDS_RECOVERY if generic FLR was not possible. */
n_handle n_pci_claim(uint8_t bus, uint8_t dev, uint8_t func);
n_result n_pci_info(n_handle pci, struct n_pci_function *out);
/* Config space: width 1, 2 or 4 at an offset below 4096 aligned to it. Reads may name another
   function (N_PCI_TARGET, e.g. the upstream bridge) or N_PCI_SELF; writes only reach the
   claimed function. A read returns the value (non-negative). */
n_result n_pci_config_read(n_handle pci, uint32_t target, uint32_t offset, uint32_t width);
n_result n_pci_config_write(n_handle pci, uint32_t offset, uint32_t width, uint32_t value);
/* A memory BAR as a shared object over its device memory (uncached; n_shared_view makes
   write-combining parts or smaller windows to map or to send to another app). */
n_handle n_pci_bar(n_handle pci, uint32_t bar);
/* Turn on `count` message-signalled interrupts (MSI-X if offered, else MSI; count is rounded
   down to what the function and the system give) and return how many. Each one that arrives
   sets its bit until n_pci_interrupt_take returns it; the handle is N_EVENT_RECEIVABLE while a
   bit is set. take waits up to timeout_ms (-1 always, 0 not at all; N_ERR_TIMEOUT). */
n_result n_pci_interrupts(n_handle pci, uint32_t count);
n_result n_pci_interrupt_take(n_handle pci, int64_t timeout_ms);
/* While the driver changes how the CPU reaches the boot screen in the function's BAR (as
   n_pci_info reports it), the system must not write there. hold true returns once nothing is
   being written and keeps the screen untouched; holds nest, and the last false (or the end of
   the claim) resumes framebuffer drawing, not necessarily GPU scanout. It does not guarantee
   recovery after a driver crash or reset. No timeout ever draws over a hold: that is the
   very race it prevents. The end of the claim stops the function (its BAR mappings, interrupts
   and bus mastering) before CPU drawing resumes. A hold that lasts as long as the boot option
   fbhold=<s> says (no time by default) stops the driver's app, and the end of its claim releases
   the hold.
   The driver whose surface the screen is drawn into (n_pci_give_screen) holds it the same way.
   N_ERR_NOT_FOUND: the boot screen is elsewhere and the screen is not drawn into its surface. */
n_result n_pci_hold_screen(n_handle pci, bool hold);
/* A display function's driver (PCI class 03) that set up its own scanout gives the system that
   surface: from the return on the desktop is drawn there, and never into the boot screen, until
   the driver gives N_SCANOUT_BOOT, or its claim ends (crash included), which stops the function
   first. Drawing then resumes in the boot screen's memory, but the display engine may not show it
   any more. surface: a writable shared object made through this claim, its BAR (prefetchable;
   drawn write-combined, so a view of it the driver maps must be write-combining too) or a DMA
   object whose type is not write-back (a write-combining or uncached view sets it), or a view of
   either; desc says where in it (offset, pitch) and how (N_SCANOUT_XRGB8888, XRGB2101010 or
   XBGR8888).
   Its width and height are the mode's the display engine shows (the one the system chose, struct
   n_screen_state's mode, once the driver offered modes; up to 65535 each): the desktop takes that
   size, and windows fullscreen or maximized follow it. Giving the same surface again at another
   size changes the desktop's size the same way; the boot screen given back is drawn at its own.
   Returns once nothing is drawn into what was shown before; the new surface is drawn in full.
   N_ERR_DENIED: not a display function, another claim's object, or a type the surface cannot have;
   N_ERR_INVALID: a size of 0 or above 65535, an unknown format, a range beyond the object;
   N_ERR_NO_MEMORY: the desktop does not fit in memory at that size (the screen stays as it was);
   N_ERR_BUSY: another claim's surface is shown. */
n_result n_pci_give_screen(n_handle pci, n_handle surface, const struct n_scanout *desc);
/* A display function's driver tells the system the monitor it drives and the modes it can show
   there (struct n_screen_modes): the system chooses the one shown (a mode on trial in Settings,
   else the user's choice for that monitor, else the monitor's preferred mode, else the largest
   near 60 Hz) and says so in struct n_screen_state's mode, which n_pci_screen_state reads. The
   driver shows that mode (n_pci_give_screen at its size) and gives the modes again with done set
   to the state's mode_serial and the mode it shows: when that is not the mode wanted, the system
   never asks for that one again on this monitor and chooses another. Given with done 0 (when the
   driver starts, or the monitor changed or was plugged in), the system chooses anew, even the
   same mode again. count 0: the driver drives no monitor any more (and its claim's end does the
   same). N_ERR_DENIED: not a display function; N_ERR_INVALID: more than N_SCREEN_MODES_MAX modes,
   or a mode with a size or refresh of 0, or a size above 65535; N_ERR_BUSY: another claim's
   surface is shown or its modes offered. */
n_result n_pci_screen_modes(n_handle pci, const struct n_screen_modes *modes);
/* The surface given had vertical blank number count at timestamp_ns (the driver's clock). While
   blanks come, the desktop starts its frames at them instead of its own period; 100 ms without
   one and the period paces again. N_ERR_DENIED: the screen is not drawn into this claim's surface. */
n_result n_pci_screen_vblank(n_handle pci, uint64_t count, uint64_t timestamp_ns);
/* For the driver whose surface the screen is drawn into or whose modes the system has: waits up
   to timeout_ms (-1: no limit) while the screen's state is still the one numbered seen (0 never
   is), then writes it to out (struct n_screen_state says the mode to show, and with
   N_SCANOUT_DIRECT what the driver shows and draws while an app's window is shown directly).
   N_ERR_TIMEOUT: out is the state numbered seen. N_ERR_DENIED: the screen is not drawn into this
   claim's surface and it offered no modes. */
n_result n_pci_screen_state(n_handle pci, uint64_t seen, struct n_screen_state *out, int64_t timeout_ms);
/* For that driver: a surface its display engine may show in its scanout's place (an app's image
   shown directly), given as n_pci_give_screen's surface is (format without N_SCANOUT_DIRECT;
   N_SCANOUT_XBGR8888 too), of its own size (the engine may scale it to the mode's). The system
   never draws there but once: its panic screen, which goes to the scanout and to every such
   surface at its size, as the engine keeps showing what it was flipped to last and no driver runs
   any more. Returns the surface's number (positive); n_pci_screen_image_end with it lets go before
   the driver frees the memory. A scanout without N_SCANOUT_DIRECT, the boot screen given back, or
   the claim's end lets go of them all. N_ERR_DENIED: the screen is not drawn into this claim's
   N_SCANOUT_DIRECT scanout, or the object is not one n_pci_give_screen takes; N_ERR_INVALID: a size
   of 0 or above 65535, an unknown format, a range beyond the object, or (end) no such number. */
n_result n_pci_screen_image(n_handle pci, n_handle surface, const struct n_scanout *desc);
n_result n_pci_screen_image_end(n_handle pci, n_result image);
/* The function's part of the ACPI namespace, as the firmware describes it: its device node
   (N_ACPI_SELF), that node's device children (N_ACPI_CHILD(i), in namespace order), and the node
   of the bridge it is below (N_ACPI_UPSTREAM; the host bridge's on a root bus). n_pci_acpi_eval
   evaluates `name`, one name segment directly under that node ("_DSM", "_ADR"), with nargs
   (at most 7) integer, string or buffer arguments whose bytes are in data (at most 64 KiB), and
   writes the result to out: struct n_acpi_value records, the first one the result, then the
   bytes of its strings and buffers; it returns how many bytes it wrote (at most 1 MiB).
   N_ERR_DENIED, on every node and whether it exists or not: the methods through which the system
   itself runs devices and the platform: power and reset (_PS0 to _PS3, _ON_, _OFF, _RST), removal
   and docking (_EJ0 to _EJ4, _LCK, _DCK), resources (_DIS, _SRS), the handshake that decides what
   the firmware controls (_OSC, _OST), wake (_PSW, _DSW), initialisation and handlers (_INI, _REG,
   _EVT), system sleep (_PTS, _WAK, _TTS, _GTS, _BFS); and on N_ACPI_UPSTREAM every name not
   starting with _ (the bridge's own methods belong to the platform and every function below it).
   N_ERR_NOT_FOUND: no such node or object; N_ERR_INVALID: a child number past the last (a
   function without a node has no children), a malformed name or argument; N_ERR_TOO_LARGE: out
   is too small; N_ERR_IO: the AML failed; N_ERR_UNSUPPORTED: the system runs without AML.
   n_pci_acpi_notify_take returns the next Notify value of the function's node from its first
   call on (it waits like n_pci_interrupt_take; 32 not taken are kept, the oldest give way;
   N_ERR_NOT_FOUND: the function has no node). */
n_result n_pci_acpi_eval(n_handle pci, uint32_t node, const char *name, const struct n_acpi_value *args,
                         uint32_t nargs, const void *data, size_t data_size, void *out, size_t out_size);
n_result n_pci_acpi_notify_take(n_handle pci, int64_t timeout_ms);
/* Zeroed RAM that a claimed function may read and write by DMA: a shared object (N_DMA_*
   flags), held while any handle or mapping holds it. n_dma_addresses gives the bus address of
   each page from first_page (count of them; returns how many it wrote). */
n_handle n_dma_create(n_handle pci, size_t bytes, uint32_t flags);
n_result n_dma_addresses(n_handle shared, size_t first_page, uint64_t *out, size_t count);
/* A new shared object over part of another (page-aligned offset and bytes) with the given
   caching (N_CACHE_*): mapping it maps the same pages. */
n_handle n_shared_view(n_handle shared, size_t offset, size_t bytes, uint32_t cache);
/* A view with an immutable maximum access (N_MEMORY_READ or READ_WRITE).
   Derived views and transferred handles cannot enlarge this access. */
n_handle n_shared_view_access(n_handle shared, size_t offset, size_t bytes, uint32_t cache, uint32_t access);
/* One shared object over ordered slices, retaining their backing, cache types
   and cancellation dependencies. READ/READ_WRITE cannot exceed any source's
   maximum rights. Source handles may close after success. No pages are copied. */
n_handle n_shared_compose(const struct n_shared_range *ranges, size_t count, uint32_t access);
/* Only the app which issued a view may revoke it. Existing mappings and all
   derived views lose access; the underlying object and independent views remain alive. */
n_result n_shared_revoke(n_handle view);
/* Paged memory (docs/nocturne-native-abi.md §7): `bytes` of shared memory (in *memory, an
   ordinary N_KIND_SHARED handle to map or to send) whose pages the creating app, its pager,
   supplies through the returned N_KIND_PAGER handle, which no other app can use or receive.
   A touch of a page without the access it needs waits until the pager answered it:
   n_pager_take gives such faults (up to max; timeout_ms -1 waits, 0 polls: N_ERR_TIMEOUT;
   N_ERR_CLOSED once the memory is gone), and the handle is N_EVENT_RECEIVABLE while some wait.
   n_pager_supply binds runs of pages to RAM the pager holds (write-back; a DMA object's too),
   given as a root or a view of one (not a composite), which every mapper then reaches without
   asking again; revoking the view a page came through (or a view it descends from) unbinds
   that page everywhere. n_pager_revoke takes the frames (access 0) or the write access
   (N_MEMORY_READ) of a run away from every mapper at once; n_pager_fail makes the waiting
   touches of a run access violations. When the pager handle closes, bound pages stay and the
   others fail. Paged memory has no views; n_memory_protect holds for its unbound pages too. */
n_handle n_pager_create(size_t bytes, n_handle *memory);
n_result n_pager_take(n_handle pager, struct n_pager_fault *out, size_t max, int64_t timeout_ms);
n_result n_pager_supply(n_handle pager, const struct n_pager_supply *runs, size_t count);
n_result n_pager_revoke(n_handle pager, uint64_t offset, uint64_t bytes, uint32_t access);
n_result n_pager_fail(n_handle pager, uint64_t offset, uint64_t bytes);
/* Sound: each n_audio_open is a stream of N_AUDIO_RATE stereo frames (two int16_t, left first)
   of its own, mixed with the others and scaled by the master volume. A stream holds about
   170 ms; n_audio_submit takes what fits and returns the frames taken (with N_AUDIO_WAIT it
   waits for room; without, a full stream gives N_ERR_NOT_READY and N_EVENT_SENDABLE reports
   room). Closing lets the queued sound play out; n_audio_flush drops it at once, with what the
   sound card was given but has not begun to play (only what the device already holds is still
   heard: on AC'97 at most 20 ms). */
n_handle n_audio_open(void); /* N_ERR_BUSY when every stream is taken */
n_result n_audio_submit(n_handle a, const int16_t *frames, size_t count, uint32_t flags);
n_result n_audio_status(n_handle a, struct n_audio_status *out);
n_result n_audio_flush(n_handle a);
/* The master volume and muting as set says (N_AUDIO_SET_VOLUME, N_AUDIO_SET_MUTED; 0 only reads),
   then *io filled with them and what plays the sound (struct n_audio_output, abi.h). */
n_result n_audio_output(struct n_audio_output *io, uint32_t set);
/* A watch on what n_audio_output reports: the volume, muting and what plays the sound. */
n_handle n_audio_output_watch(void);
/* Watches (N_KIND_WATCH, docs/nocturne-native-abi.md §1): N_EVENT_RECEIVABLE once what the watch
   watches has changed, until n_watch_take takes the changes; it returns how many came since the
   last take (0: none, at most N_WATCH_CHANGES_MAX). N_EVENT_CLOSED once what it watches is no
   more; a take then gives what was left, then N_ERR_CLOSED. Wait for one in an event queue. */
n_result n_watch_take(n_handle watch);
/* Event queues: level-triggered; a ready handle is reported by every wait. */
n_handle n_events_create(void);
n_result n_events_watch(n_handle q, n_handle h, uint32_t interest, uint64_t token);
n_result n_events_watch_address(n_handle q, volatile uint32_t *address, uint32_t expected, uint64_t token);
n_result n_events_timer(n_handle q, uint64_t ms, bool repeat, uint64_t token);
n_result n_events_unwatch(n_handle q, uint64_t token); /* 0 unknown, 1 removed, 2 already notified */
n_result n_events_wait(n_handle q, struct n_event *out, size_t max, int64_t timeout_ms); /* count */
uint64_t n_clock_uptime_ms(void);
n_result n_random(void *buf, size_t n); /* n bytes from the system entropy pool */
int64_t n_clock_wall(void); /* seconds since 1970-01-01 UTC */
void n_sleep_ms(uint64_t ms);
/* The launch settings ("name=value"), NULL-terminated. */
extern char *const *n_settings;
/* Whether path is complete: "C:\...", "\..." or "/...". "C:" and "C:a" are not: they are in the
   current folder, like "a". */
bool n_path_is_complete(const char *path);
/* name taken inside folder (a complete path; the root when it is not one), written as "C:\a\b"
   with "." and ".." resolved. A complete name stands alone. Returns the length, or N_ERR_TOO_LARGE. */
n_result n_path_join(const char *folder, const char *name, char *out, size_t cap);
/* A complete path for a name the user typed: n_path_join inside the launch setting FOLDER (the
   launcher's current folder, "C:\" if it gave none). */
n_result n_path_complete(const char *name, char *out, size_t cap);
n_result n_folder_set(const char *name); /* this app's FOLDER; N_ERR_NOT_FOLDER if it is not one */
/* The last name of path, inside it ("" for the root). */
const char *n_path_name(const char *path);
/* The complete path a file: URL names: "file:///C:/Users/User/a%20b.html" is C:\Users\User\a b.html,
   a path without a drive ("file:///Users/User") is on C: too, and "file://localhost/" is "file:///";
   the query and fragment are not part of it. Returns the length, N_ERR_INVALID when url is not a
   file: URL of a path, or N_ERR_TOO_LARGE. */
n_result n_path_from_url(const char *url, char *out, size_t cap);
/* The file: URL of a path, made complete first (n_path_join): C:\Users\User\a b.html is
   file:///C:/Users/User/a%20b.html. Returns the length, or N_ERR_TOO_LARGE. */
n_result n_path_to_url(const char *path, char *out, size_t cap);
/* The user's folder (the launch setting USERFOLDER) and temporary folder (TEMP); "C:\" without them. */
const char *n_user_folder(void);
const char *n_temp_folder(void);
/* The folder an app keeps its data in, <user's folder>\AppData\<name>, made when missing.
   Returns the length of the path written to out. */
n_result n_app_data_folder(const char *name, char *out, size_t cap);
/* Whether handle h reaches a terminal: the launcher names such handles in the
   setting TERMINAL, e.g. "0 1 2". */
bool n_terminal(n_handle h);

/* The count of processes and threads (at most max written), or a negative n_result. */
n_result proclist(struct n_procinfo *out, int max);
/* 0, or a negative n_result; errno is left alone. */
n_result sysinfo(struct n_sysinfo *si);
n_result cpu_info(struct n_cpuinfo *info);
n_result clock_info(struct n_clockinfo *info);
/* Native tasks sharing this process's address space and descriptors. create
 * returns the thread's id (never 0); a process has at most 128 running threads,
 * past that N_ERR_NOT_READY. The stack is 256 KiB, or with create_sized at least
 * stack_bytes (0: 256 KiB); its pages are given on first touch. A thread ends
 * when fn returns; join waits for that, and detach says no one will join it.
 * All return a negative n_result on failure and leave errno alone. */
n_result thread_create(void (*fn)(void *), void *arg);
n_result thread_create_sized(void (*fn)(void *), void *arg, size_t stack_bytes);
n_result thread_join(int tid);
n_result thread_detach(int tid);
NOCTURNE_NORETURN void thread_exit(void);
int thread_id(void);
unsigned cpu_index(void);
/* Process-library thread-local slots, independent of errno and thread identity.
 * Libraries use these instead of ELF thread-local variables, which tcc -run
 * programs cannot link. */
#define N_TLS_SLOTS 16
#define N_TLS_WASM_RUNTIME 3 /* Slots 0/1/2 are parallel, layout and CSS. */
#define N_TLS_GL_CONTEXT 4   /* the thread's current WebGL/OpenGL context */
#define N_TLS_GL_DISPATCH 5  /* Mesa's current dispatch table */
#define N_TLS_GL_CURRENT 6   /* Mesa's current gl_context; slot 7 is C11 threads */
#define N_TLS_CALL_ONCE 8    /* Mesa's call-once-with-data argument */
void *thread_local_get(unsigned slot);
void thread_local_set(unsigned slot, void *value);
/* Actual reserved stack interval of the calling native task: 0, or a negative n_result. */
n_result thread_stack_bounds(uintptr_t *low, uintptr_t *high);
/* timeout_ms == UINT32_MAX waits indefinitely; count == 0 wakes all.
 * The wait returns 0 when woken, -N_ERR_NOT_READY when *address no longer
 * holds expected, -N_ERR_TIMEOUT or -N_ERR_STOPPED; wake returns how many
 * waiters it woke.  errno is left alone. */
n_result wait_on_address(volatile uint32_t *address, uint32_t expected, unsigned timeout_ms);
/* The 8-byte form; wake_address on the same address wakes it. */
n_result wait_on_address64(volatile uint64_t *address, uint64_t expected, unsigned timeout_ms);
n_result wake_address(volatile uint32_t *address, unsigned count);
void yield(void);
/* These return only on failure, with a negative n_result. */
n_result poweroff(void);
n_result reboot(void);
/* The count of PCI functions (at most max written), or a negative n_result. */
n_result pcilist(struct n_pciinfo *out, int max);
long __syscall(long n, long a, long b, long c, long d, long e);

/* run a program (find_program) and wait for it; returns exit status or negative error */
int run_wait(const char *path, char *const argv[]);
/* A program's complete path: a bare name ("ls") is looked for in N_FOLDER_SYSTEM, then in each
   folder of N_FOLDER_PROGRAMS; any other is completed (n_path_complete). False if there is none. */
bool find_program(const char *name, char *out, size_t n);
/* Whether the complete path folder is one find_program looks in: N_FOLDER_SYSTEM or a folder of
   N_FOLDER_PROGRAMS. */
bool program_folder(const char *folder);

/* line editor for terminal programs (echo, editing keys, history). Returns the line length, or -1
   at end of input (Ctrl+D). RL_SECRET echoes '*' and keeps the line out of the history. */
#define RL_SECRET 1
int readline(const char *prompt, char *buf, size_t size, int flags);

/* ---- networking (net.c) ----
   IPv4 addresses are uint32_t in host order (10.0.2.15 is 0x0A00020F). Functions return their
   result or a negative n_result, and leave errno alone. Timeouts are in milliseconds; -1 waits
   forever, and an expired wait is -N_ERR_NOT_READY or -N_ERR_TIMEOUT. */
n_result net_info(struct n_netinfo *ni);
n_result net_ping(uint32_t ip, int seq, int timeout_ms); /* round-trip time in ms */
n_result net_resolve(const char *name, uint32_t *ip);    /* DNS lookup or dotted quad */
n_result net_resolve_all(const char *name, uint32_t *ips, size_t capacity, struct n_dns_result *result);
n_result net_resolve_at(const char *name, uint32_t *ips, size_t capacity, struct n_dns_result *result,
                        const struct n_sockaddr *resolver);
n_result net_stats(struct n_netstats *stats);
/* Calling thread only: abort DNS/ARP/connect if this request channel's peer
   closes. -1 clears the context. Established/shared TCP I/O is unaffected. */
n_result net_set_setup_cancel_channel(n_handle channel);
bool ip_parse(const char *s, uint32_t *ip);
char *ip_format(uint32_t ip, char *buf); /* buf: at least 16 bytes */
n_result udp_socket(uint16_t port);      /* a connection handle; 0 picks an ephemeral port */
n_result udp_sendto(n_handle s, uint32_t ip, uint16_t port, const void *buf, size_t len);
n_result udp_recvfrom(n_handle s, void *buf, size_t len, int timeout_ms, struct n_sockaddr *from);
n_result udp_close(n_handle s);
n_result tcp_connect(uint32_t ip, uint16_t port, int timeout_ms); /* a connection handle */
n_result tcp_send(n_handle s, const void *buf, size_t len);
/* Absolute uptime_ms deadline; zero waits without a deadline. A positive
   result is the accepted prefix, never a claim that the peer acknowledged it. */
n_result tcp_send_until(n_handle s, const void *buf, size_t len, uint64_t deadline_ms);
/* The accepted prefix, or -N_ERR_NOT_READY; never waits for space. */
n_result tcp_send_some(n_handle s, const void *buf, size_t len);
/* Abort the connection, waking send/receive waiters with -N_ERR_CANCELLED. */
n_result tcp_cancel(n_handle s);
n_result tcp_recv(n_handle s, void *buf, size_t len, int timeout_ms); /* 0 = connection closed */
n_result tcp_close(n_handle s);
/* Passive open: tcp_listen returns a listener on a local address (0: all of them, or e.g.
   127.0.0.1 for this system only; port 0 picks one; backlog <= 0 uses a default), tcp_accept
   the next established connection, ordinary for tcp_send/tcp_recv/tcp_close.
   tcp_close on the listener refuses connections not yet accepted. */
n_result tcp_listen(uint32_t ip, uint16_t port, int backlog);
n_result tcp_accept(n_handle listener, int timeout_ms, struct n_sockaddr *remote);
n_result tcp_address(n_handle s, struct n_sockaddr *local, struct n_sockaddr *remote); /* either may be NULL */
n_result tcp_finish(n_handle s); /* stop sending (FIN after the queued data) but keep receiving; tcp_close later */
/* simulated packet loss/reordering on this process's TCP connections, and TCP counters (either may be NULL) */
n_result net_test(const struct n_netfault *set, struct n_tcpstats *stats);
bool net_wait_up(int timeout_ms); /* wait for DHCP; false if no network */

/* ---- GUI ---- */
typedef struct window {
    n_handle handle; /* N_KIND_WINDOW: its input is reported by n_events_watch as N_EVENT_RECEIVABLE */
    int w, h; /* client size */
    canvas_t c;
    int flags;
    struct gui_event q[32];
    int qn, qi;
    bool closed;
    uint32_t *buffers[WIN_BUFFER_COUNT];
    unsigned buffer_index;
    bool frame_acquired;
} window_t;

/* The calls below return their result (0 unless noted), or a negative n_result,
   and leave errno alone. */
bool gui_available(void);
n_result screen_size(int *w, int *h); /* w and h are written only on success */
/* Opens a window into *out; *out is NULL on failure. */
n_result win_open(int w, int h, const char *title, int flags, window_t **out);
void win_close(window_t *win);
/* -N_ERR_BUSY for a double-buffered window whose frame was not begun. */
n_result win_update(window_t *win);
n_result win_update_rect(window_t *win, int x, int y, int w, int h);
/* WIN_DOUBLE_BUFFER only: acquire a writable canvas before drawing a complete
   frame. No retained/partial drawing across submissions; update invalidates c.
   Completion is also reported as EV_FRAME. */
n_result win_begin_frame(window_t *win);
n_result win_set_title(window_t *win, const char *title);
n_result win_move(window_t *win, int x, int y);
n_result win_state(window_t *win); /* WIN_STATE_* bitmask */
n_result win_fullscreen(window_t *win,bool enabled); /* resize delivered by EV_RESIZE */
/* Asks that, while the window is fullscreen, focused and nothing of the system is drawn over it,
   the display driver show the app's own images (nvrm's direct scanout) instead of the desktop.
   EV_DIRECT (key 1) says when that starts and (key 0) when it ends; in between the window's own
   buffer is not shown. Without a display driver that can, it never starts. */
n_result win_direct(window_t *win, bool wanted);
/* A window opened with WIN_FRAME_EVENTS gets EV_FRAME at the display's next frame even if it presents
   nothing: an animation waits for the display before it decides what to draw. One EV_FRAME answers the
   request and the presents before it; a window that is not shown gets it when it is shown again. */
n_result win_frame(window_t *win);
/* The pointer the window wants over its client area (SYS_WIN_POINTER): one of the desktop's shapes
   (N_POINTER_*), or an image of at most N_POINTER_IMAGE_MAX square with its hot spot (copied at the
   call) and the shape for where an image is not shown. The desktop draws it, and its own shapes
   over its menus, frames and resize band. It stays until the next call; a new window has
   N_POINTER_DEFAULT. */
n_result win_pointer(window_t *win, const struct n_win_pointer *pointer);
n_result win_pointer_shape(window_t *win, uint32_t shape);
/* wait up to timeout_ms (-1 = forever) for an event; returns 1 if *e was filled, 0 on timeout,
   negative once the window is gone. EV_RESIZE is handled (buffer reallocated) before being returned. */
n_result win_event(window_t *win, struct gui_event *e, int timeout_ms);
n_result clipboard_set(const char *s, size_t n);
n_result clipboard_get(char *buf, size_t n); /* the whole length; at most n bytes are copied */
/* start a program (find_program: "notepad" is enough) in its own window, with arg as its one argument */
n_result gui_launch(const char *path, const char *arg);
/* copy the whole screen (taskbar included) into buf as 0xAARRGGBB pixels. bytes = 0 only reports
   the size. -N_ERR_TOO_LARGE: the buffer is too small */
n_result screen_grab(uint32_t *buf, size_t bytes, int *w, int *h);

/* ---- sound (sound.c) ----
   Streams come from n_audio_open (above; up to 8 play at once) and hold about 170 ms
   (SOUND_QUEUE frames). */
#define SOUND_RATE  48000
#define SOUND_QUEUE 8192
enum { SND_SINE, SND_SQUARE, SND_TRIANGLE, SND_SAW, SND_NOISE };
/* A sound effect that never makes the program wait: a tone gliding from hz to hz2 over ms
   milliseconds. Effects play one after another; one that does not fit in the queue is dropped.
   Returns false if it was not played (no sound, or no room). */
bool sound_effect(int hz, int hz2, int ms, int wave);

/* A WAV file read as SOUND_RATE stereo frames: integer PCM of 8, 16, 24 or 32 bits or 32-bit
   float, any number of channels (mono goes to both sides; past two, the first two are used) and
   any rate (resampled). */
struct wav {
    n_handle document;
    int tag, channels, bits, block;
    uint64_t offset;                  /* the next byte of the document to read */
    uint32_t rate, frames;           /* the file's own rate and length */
    uint32_t data_off, data_len, pos; /* pos: the next frame to read from the file */
    uint32_t src;                     /* the source frame playing now */
    uint64_t step, frac;
    int32_t al, ar, bl, br;
    bool eof;
    size_t len, at;
    uint8_t buf[8192];
};
const char *wav_open(struct wav *w, const char *path); /* NULL, or what is wrong */
void wav_close(struct wav *w);
void wav_seek(struct wav *w, uint32_t frame); /* in the file's frames */
int wav_read(struct wav *w, int16_t *out, int frames); /* fewer than asked at the end */
uint32_t wav_ms(const struct wav *w, uint32_t frame);
NOCTURNE_END_DECLS
