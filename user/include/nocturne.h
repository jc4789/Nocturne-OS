/* Nocturne OS system interface. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "abi.h"
#include "audio_abi.h"
#include "settings_abi.h"
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
/* パスの持ち主の利用者を変える。N_CLASS_ADMIN 以上（§3）。 */
n_result n_entry_set_owner(const char *path, uint32_t user);
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
/* 身元と能力（docs/nocturne-native-abi.md §2）。 */
n_result n_identity_self(struct n_identity *out);
/* channel のもう一端の、カーネルが証明した身元。 */
n_result n_channel_peer(n_handle channel, struct n_identity *out);
/* cap から kind の能力を切り出す。根は全部を、ほかの能力は自分と同じ種類だけを。 */
n_handle n_cap_derive(n_handle cap, enum n_cap kind);
n_result n_cap_kind(n_handle cap);
/* 自分が持つ kind の能力の handle。無ければ N_ERR_NOT_FOUND。 */
n_handle n_cap_find(enum n_cap kind);
/* 自分が持つ能力の handle を out（N_CAP_COUNT 個の場所）へ並べる：その数。子へ渡す列を作るとき。 */
size_t n_caps_list(int32_t *out);
/* カーネルの窓の系が起こすアプリの身元を決める。 */

/* 受け口：各メッセージが新しい channel の端を一つ運ぶ channel。connect は組を作って一方の端を
   受け口へ送り、もう一方を返す。accept は次の端を受ける（flags は 0 か N_CHANNEL_WAIT、無ければ
   N_ERR_NOT_READY、送る側が全部閉じれば N_ERR_CLOSED）。受け口を受ける側が閉じると待つ端も閉じる。 */
n_handle n_inlet_connect(n_handle inlet);
n_handle n_inlet_accept(n_handle inlet, int flags);

/* サービス：名前はサービスの管理者が決める（カーネルは覚えない）。connect は名前のサービスへの
   channel を返す。listen は名前を管理者に申し込み、受け口を返す。受け口を全部閉じると名前は消える。
   register は管理者だけ：接続の要求（struct n_service_request）を受ける channel を返す。 */
n_handle n_service_connect(const char *name);
n_handle n_service_listen(const char *name);
n_handle n_service_register(n_handle service_root);
/* Apps: the handle reports the end (N_EVENT_APP_ENDED); there is no zombie to reap. */
n_handle n_app_launch(const char *path, const struct n_launch *launch);
n_result n_app_result(n_handle app, uint64_t deadline, int *status); /* a past deadline polls: N_ERR_NOT_READY */
n_result n_app_stop(n_handle app, int flags);                         /* N_APP_STOP_TREE */
n_result n_app_self(void);
n_result n_app_parent(void);
n_handle n_app_open(int64_t id); /* a running app by the identifier the app list shows */
n_result n_system_log(char *buf, size_t cap); /* the newest system log text; its length（ADMIN 以上） */
/* A handle whose messages are added to the system log. 開いた者が ADMIN 以上なら log's text も受ける
   (from the oldest record the kernel holds at its first receive on)。 */
n_handle n_system_log_open(void);
n_result n_app_id(n_handle app); /* its identifier, also after its end (identifiers are not reused) */
/* Drivers in apps (docs/nocturne-native-abi.md §9). A claim takes the N_CAP_PCI_CLAIM capability;
   its holder hands the N_KIND_PCI handle to the driver app it launches. A claim is exclusive until its
   last handle closes, which also turns the function's interrupts off. Isolation is required
   unless appdrivers-unisolated was explicitly selected at boot. A device left active needs
   recovery; n_pci_info reports N_PCI_NEEDS_RECOVERY if generic FLR was not possible. */
n_handle n_pci_claim(n_handle cap, uint8_t bus, uint8_t dev, uint8_t func);
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
   bit is set. take waits until a message among the first 64 * words has arrived, or deadline
   (N_CLOCK_NEVER: no limit; N_ERR_TIMEOUT), then writes their bits (message n: bit n % 64 of
   bits[n / 64]), clears them and returns how many words it wrote; later messages' bits stay. */
n_result n_pci_interrupts(n_handle pci, uint32_t count);
n_result n_pci_interrupt_take(n_handle pci, uint64_t *bits, uint32_t words, uint64_t deadline);
/* While the driver changes how the CPU reaches the boot screen in the function's BAR (as
   n_pci_info reports it), the system must not write there. hold true returns once nothing is
   being written and keeps the screen untouched; holds nest, and the last false (or the end of
   the claim) resumes framebuffer drawing, not necessarily GPU scanout. It does not guarantee
   recovery after a driver crash or reset. No timeout ever draws over a hold: that is the
   very race it prevents. The end of the claim stops the function (its BAR mappings, interrupts
   and bus mastering) before CPU drawing resumes. A hold that lasts as long as the boot option
   fbhold=<s> says (no time by default) stops the driver's app, and the end of its claim releases
   the hold.
   The driver whose buffers screen 0 is drawn into (n_pci_give_screen) holds it the same way.
   N_ERR_NOT_FOUND: the boot screen is elsewhere and screen 0 is not drawn into its buffers. */
n_result n_pci_hold_screen(n_handle pci, bool hold);
/* A display function's driver (PCI class 03) shows screens on its outputs (output: the driver's
   own number for a connector, a head or a display engine's pipe). Output 0 of the function the
   boot screen is drawn through is screen 0, the one with the taskbar; every other output has a
   screen of its own, the same one each time a driver of that function drives it.
   n_pci_give_screen: the buffers the output shows its screen from (struct n_screen_buffers): from
   the return on, the screen is drawn there and takes their size, and never into the boot screen
   any more for screen 0, until the driver gives them back (buffers NULL) or its claim ends (crash
   included), which stops the function first; the screen is then off, and screen 0 drawn into the
   boot screen's memory again, which the display engine may no longer show. Each buffer is a
   writable shared object made through this claim, its BAR (prefetchable; drawn write-combined, so
   a view of it the driver maps must be write-combining too) or a DMA object whose type is not
   write-back (a write-combining or uncached view sets it), or a view of either. The size is the
   mode's the display engine shows (the one the system chose, struct n_screen_state's mode, once
   the driver offered modes; up to 65535 each); windows fullscreen or maximized on the screen
   follow it. Returns once nothing is drawn into the output's buffers before; the new ones are
   drawn in full; the first of them is the one shown. A frame is drawn only into a buffer the
   output does not show (with one buffer, into the one shown) and asked for as a flip. Returns the
   serial of the output's last flip before (0: none): a flip up to it was of the buffers before,
   and one not taken yet is gone.
   N_ERR_DENIED: not a display function, another claim's object, or a type the buffer cannot have;
   N_ERR_INVALID: a count of 0 or above N_SCREEN_BUFFERS_MAX, a size of 0 or above 65535, an
   unknown format, a range beyond the object, or nothing to give back. */
n_result n_pci_give_screen(n_handle pci, uint32_t output, const struct n_screen_buffers *buffers);
/* The flips the screens ask of the claim's outputs (struct n_screen_flip), at most max into out,
   waiting until deadline (N_CLOCK_NEVER: no limit) for one: their count, 0 at the deadline. A flip
   not taken yet is replaced by the output's next one. The driver shows the flip's buffer from the
   next vertical blank on and says so with n_pci_screen_vblank. */
n_result n_pci_screen_flips(n_handle pci, struct n_screen_flip *out, size_t max, uint64_t deadline);
/* A vertical blank of an output whose buffers its screen is drawn into (struct n_screen_vblank):
   the moment the display engine measured, on the system's monotonic clock, and the flip it shows.
   The screen's next frame waits for the flip's blank; a screen without blanks reported shows
   flips as they are asked. N_ERR_DENIED: the output shows no buffers of this claim. */
n_result n_pci_screen_vblank(n_handle pci, const struct n_screen_vblank *vblank);
/* A display function's driver tells the system the monitor it drives on an output and the modes
   it can show there (struct n_screen_modes): the display service chooses the one shown (a mode on trial in
   Settings, else the user's choice for that monitor, else the monitor's preferred mode, else the
   largest at the highest refresh) and says so in struct n_screen_state's mode, which
   n_pci_screen_state reads. The driver shows that mode (n_pci_give_screen at its size) and gives
   the modes again with done set to the state's mode_serial and the mode it shows: when that is not
   the mode wanted, the system never asks for that one again on this monitor and chooses another.
   Given with done 0 (when the driver starts, or the monitor changed or was plugged in), the system
   chooses anew, even the same mode again. count 0: the driver drives no monitor on that output
   any more (and its claim's end does the same). N_ERR_DENIED: not a display function;
   N_ERR_INVALID: more than N_SCREEN_MODES_MAX modes, or a mode with a size or refresh of 0, or a
   size above 65535. */
n_result n_pci_screen_modes(n_handle pci, uint32_t output, const struct n_screen_modes *modes);
/* For the driver whose buffers an output's screen is drawn into or whose modes on it the system
   has: waits until deadline (N_CLOCK_NEVER: no limit) while the screen's state is still the one
   numbered seen (0 never is), then writes it to out (struct n_screen_state says the mode to show, and with
   N_SCANOUT_DIRECT what the driver shows and draws while an app's window is shown directly).
   N_ERR_TIMEOUT: out is the state numbered seen. N_ERR_DENIED: the output shows no buffers of
   this claim and it offered no modes there. */
n_result n_pci_screen_state(n_handle pci, uint32_t output, uint64_t seen, struct n_screen_state *out,
                            uint64_t deadline);
/* For that driver: a surface the output's display engine may show in its buffers' place (an app's
   image shown directly), given as a buffer is (format without N_SCANOUT_DIRECT; N_SCANOUT_XBGR8888
   too), of its own size (the engine may scale it to the mode's). The system never draws there but
   once: its panic screen, which goes to the buffers and to every such surface at its size, as the
   engine keeps showing what it was flipped to last and no driver runs any more. Returns the
   surface's number (positive); n_pci_screen_image_end with it lets go before the driver frees the
   memory. Buffers without N_SCANOUT_DIRECT, the buffers given back, or the claim's end let go of
   them all. N_ERR_DENIED: the output shows no N_SCANOUT_DIRECT buffers of this claim, or the
   object is not one n_pci_give_screen takes; N_ERR_INVALID: a size of 0 or above 65535, an unknown
   format, a range beyond the object, or (end) no such number. */
n_result n_pci_screen_image(n_handle pci, uint32_t output, n_handle surface, const struct n_scanout *desc);
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
n_result n_pci_acpi_notify_take(n_handle pci, uint64_t deadline);
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
   n_pager_take gives such faults (up to max, waiting until deadline: N_ERR_TIMEOUT;
   N_ERR_CLOSED once the memory is gone), and the handle is N_EVENT_RECEIVABLE while some wait.
   n_pager_supply binds runs of pages to RAM the pager holds (write-back; a DMA object's too),
   given as a root or a view of one (not a composite), which every mapper then reaches without
   asking again; revoking the view a page came through (or a view it descends from) unbinds
   that page everywhere. n_pager_revoke takes the frames (access 0) or the write access
   (N_MEMORY_READ) of a run away from every mapper at once; n_pager_fail makes the waiting
   touches of a run access violations. When the pager handle closes, bound pages stay and the
   others fail. Paged memory has no views; n_memory_protect holds for its unbound pages too. */
n_handle n_pager_create(size_t bytes, n_handle *memory);
n_result n_pager_take(n_handle pager, struct n_pager_fault *out, size_t max, uint64_t deadline);
n_result n_pager_supply(n_handle pager, const struct n_pager_supply *runs, size_t count);
n_result n_pager_revoke(n_handle pager, uint64_t offset, uint64_t bytes, uint32_t access);
n_result n_pager_fail(n_handle pager, uint64_t offset, uint64_t bytes);
/* Sound: the service audio mixes the streams (docs/nocturne-native-abi.md §17). A stream has the format
   it was opened with, at any rate and with any number of channels; the service converts it to the output
   it has chosen. The sound goes through a ring shared with the service: a stream that keeps playing makes
   no call per buffer. A stream holds about 170 ms; n_audio_submit takes what fits and returns the frames
   taken (with N_AUDIO_WAIT it waits for room; without, a full stream gives N_ERR_NOT_READY). n_audio_flush
   drops what is queued at once, with what the output was given and has not begun to play. n_audio_close
   lets what is queued play out (it waits for that) and frees the stream. A stream outlives the service: when
   the service starts again, the next call opens the stream there and goes on. One thread at a time. */
typedef struct n_audio_stream n_audio_stream;
n_result n_audio_open(const struct n_audio_format *format, n_audio_stream **out);
n_result n_audio_submit(n_audio_stream *stream, const void *frames, size_t count, uint32_t flags);
n_result n_audio_status(n_audio_stream *stream, struct n_audio_status *out);
n_result n_audio_flush(n_audio_stream *stream);
void n_audio_close(n_audio_stream *stream);
/* The outputs and what the service has decided: a fresh array of bytes (free it), a run of struct n_audio_output
   whose length is the result; 0 with none. */
n_result n_audio_outputs(struct n_audio_output **out);
/* The name of what plays the sound, fresh (free it); NULL when nothing does. */
char *n_audio_output_name(void);
/* What n_audio_outputs reports has changed: a byte comes on this channel for each change. Volume and muting
   are settings (n_setting_watch, N_DOMAIN_AUDIO). Close it to stop. */
n_handle n_audio_output_watch(void);
n_result n_audio_output_watch_take(n_handle watch);
/* The sound devices' way, for the one who holds N_CAP_AUDIO_DEVICE (the service; §17). */
n_result n_audio_device_list(n_handle cap, void *out, size_t bytes);
n_handle n_audio_device_open(n_handle cap, uint32_t id, const struct n_audio_device_format *format, struct n_audio_device_ring *ring);
n_result n_audio_device_run(n_handle device, int run);
n_result n_audio_device_status(n_handle device, struct n_audio_device_status *out);
n_handle n_audio_device_watch(n_handle cap);
/* What the kernel goes by (docs/nocturne-native-abi.md §12, settings_abi.h). Applying takes the right
   to (N_CAP_SETTINGS_APPLY), and is the settings service's: a program asks the service. effective:
   the bytes of the value now, however many, and what fits cap in out. */
n_result n_settings_apply(n_handle cap, uint32_t domain, uint32_t key, const void *value, size_t size);
n_result n_settings_effective(uint32_t domain, uint32_t key, void *out, size_t cap);
n_result n_settings_describe(uint32_t domain, uint32_t key, struct n_setting_desc *out);
n_handle n_settings_watch(uint32_t domain);
/* Watches (N_KIND_WATCH, docs/nocturne-native-abi.md §1): N_EVENT_RECEIVABLE once what the watch
   watches has changed, until n_watch_take takes the changes; it returns how many came since the
   last take (0: none, at most N_WATCH_CHANGES_MAX). N_EVENT_CLOSED once what it watches is no
   more; a take then gives what was left, then N_ERR_CLOSED. Wait for one in an event queue. */
n_result n_watch_take(n_handle watch);
/* Event queues: level-triggered; a ready handle is reported by every wait. */
n_handle n_events_create(void);
n_result n_events_watch(n_handle q, n_handle h, uint32_t interest, uint64_t token);
n_result n_events_watch_address(n_handle q, volatile uint32_t *address, uint32_t expected, uint64_t token);
/* A timer due at a moment of the monotonic clock, then every period ns on its grid (0: once). */
n_result n_events_timer(n_handle q, uint64_t due, uint64_t period, uint64_t token);
n_result n_events_unwatch(n_handle q, uint64_t token); /* 0 unknown, 1 removed, 2 already notified */
n_result n_events_wait(n_handle q, struct n_event *out, size_t max, uint64_t deadline); /* count, 0 at the deadline */
/* The clocks in ns (abi.h), read without a call: monotonic (stands still while the machine sleeps;
   every deadline is on it), boot (goes on through sleep), wall (Unix time). */
uint64_t n_clock_monotonic(void);
uint64_t n_clock_boot(void);
int64_t n_clock_wall(void);
uint64_t n_clock_after(uint64_t ns); /* the deadline ns from now (N_CLOCK_NEVER past the clock's end) */
uint64_t n_app_cpu_ns(void); /* the CPU time this app's threads have used, ended ones too (SYS_APP_CPU_TIME) */
void n_sleep_until(uint64_t deadline);
n_result n_random(void *buf, size_t n); /* n bytes from the system entropy pool */
/* Whether path is complete: "C:\...", "\..." or "/...". "C:" and "C:a" are not: they are in the
   current folder, like "a". */
bool n_path_is_complete(const char *path);
/* name taken inside folder (a complete path; the root when it is not one), written as "C:\a\b"
   with "." and ".." resolved. A complete name stands alone. Returns the length, or N_ERR_TOO_LARGE. */
n_result n_path_join(const char *folder, const char *name, char *out, size_t cap);
/* A complete path for a name the user typed: n_path_join inside the current folder (n_folder). */
n_result n_path_complete(const char *name, char *out, size_t cap);
const char *n_folder(void); /* このアプリの今のフォルダー。完全なパス。 */
n_result n_folder_set(const char *name); /* 今のフォルダーを変える。N_ERR_NOT_FOLDER if it is not one */
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
/* この process の利用者のフォルダーと一時フォルダー。身元から accounts が引く（libc/services.c）。
   システムの身元には無く、"C:\" を返す。 */
const char *n_user_folder(void);
const char *n_temp_folder(void);
/* The folder an app keeps its data in, <user's folder>\AppData\<name>, made when missing.
   Returns the length of the path written to out. */
n_result n_app_data_folder(const char *name, char *out, size_t cap);
/* Whether handle h reaches a terminal (struct n_start の terminals). */
bool n_terminal(n_handle h);
/* 端末の Ctrl+C の知らせを受ける channel（struct n_start の interrupts）。無ければ -N_ERR_NOT_FOUND。 */
n_handle n_interrupts(void);
/* The environment of this launch (struct n_start), ended by NULL: for ported programs. */
extern char *const *__libc_environment;

/* The count of processes and threads (at most max written), or a negative n_result. */
n_result proclist(struct n_procinfo *out, int max);
/* 全部の行の新しい配列（free する）。数を返す。 */
n_result n_processes(struct n_procinfo **out);
/* 0, or a negative n_result; errno is left alone. */
n_result sysinfo(struct n_sysinfo *si);
n_result cpu_info(struct n_cpuinfo *info);
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
/* The class (schedule, abi.h N_SCHEDULE_*) thread tid of this app (0: the calling thread) runs in: value is
   its weight for N_SCHEDULE_FAIR, its period in ns for N_SCHEDULE_DEADLINE. */
n_result n_thread_schedule(int tid, uint32_t schedule, uint64_t value);
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
#define N_TLS_NET_CANCEL 9   /* net_set_setup_cancel_channel's channel, plus 1 */
void *thread_local_get(unsigned slot);
void thread_local_set(unsigned slot, void *value);
/* Actual reserved stack interval of the calling native task: 0, or a negative n_result. */
n_result thread_stack_bounds(uintptr_t *low, uintptr_t *high);
/* The wait lasts until deadline (N_CLOCK_NEVER: no limit); count == 0 wakes all.
 * The wait returns 0 when woken, -N_ERR_NOT_READY when *address no longer
 * holds expected, -N_ERR_TIMEOUT or -N_ERR_STOPPED; wake returns how many
 * waiters it woke.  errno is left alone. */
n_result wait_on_address(volatile uint32_t *address, uint32_t expected, uint64_t deadline);
/* The 8-byte form; wake_address on the same address wakes it. */
n_result wait_on_address64(volatile uint64_t *address, uint64_t expected, uint64_t deadline);
n_result wake_address(volatile uint32_t *address, unsigned count);
void yield(void);
/* These return only on failure, with a negative n_result. */
n_result poweroff(n_handle cap); /* N_CAP_POWER */
n_result reboot(n_handle cap);   /* N_CAP_POWER */
/* N_CAP_POWER（§16）：POWER_* を実行する。n_power_events は事象の今、n_power_events_watch はその watch。
   n_power_windows はアプリの窓の数を返し、一番上の題を top_title へ写す。ask_to_close なら全部に閉じるよう頼む。
   n_power_windows_watch はその数と題の watch。n_power_say は窓を閉じる段取りを知らせる。 */
n_result n_power_request(n_handle cap, uint32_t action);
n_result n_power_events(n_handle cap, struct n_power_events *out);
n_handle n_power_events_watch(n_handle cap);
n_result n_power_windows(n_handle cap, bool ask_to_close, char *top_title, size_t size);
n_result n_power_say(n_handle cap, const struct n_power_state *state);
n_handle n_power_windows_watch(n_handle cap);
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
   result or a negative n_result, and leave errno alone. Waits end at a deadline of the monotonic
   clock (N_CLOCK_NEVER: none), and an expired wait is -N_ERR_NOT_READY or -N_ERR_TIMEOUT. */
/* The adapters: how many there are, the first max of them in out. */
n_result n_net_adapters(struct n_net_adapter *out, size_t max);
/* An adapter's driver's name: its whole length, what fits in out (not terminated). */
n_result n_net_adapter_name(uint32_t adapter, char *out, size_t cap);
/* The routes: how many there are, the first max of them in out. */
n_result n_net_routes(struct n_net_route *out, size_t max);
/* A watch (n_watch_take) of the adapters, their addresses and the routes. */
n_handle n_net_watch(void);
/* The service network's own: an adapter's address (ip 0: none), the whole route table, and a UDP
   socket on one adapter. cap: N_CAP_NETWORK. */
n_result n_net_address_set(n_handle cap, uint32_t adapter, uint32_t ip, uint32_t mask);
n_result n_net_routes_set(n_handle cap, const struct n_net_route *routes, size_t count);
n_result udp_socket_on(n_handle cap, uint32_t adapter, uint16_t port);
n_result net_ping(uint32_t ip, int seq, uint64_t deadline); /* round-trip time in ns */
n_result net_stats(struct n_netstats *stats);
/* Calling thread only: abort name lookups, ARP, connects and UDP waits if this request channel's
   peer closes. -1 clears the context. Established/shared TCP I/O is unaffected. */
n_result net_set_setup_cancel_channel(n_handle channel);
bool ip_parse(const char *s, uint32_t *ip);
char *ip_format(uint32_t ip, char *buf); /* buf: at least 16 bytes */
n_result udp_socket(uint16_t port);      /* a connection handle; 0 picks an ephemeral port */
n_result udp_sendto(n_handle s, uint32_t ip, uint16_t port, const void *buf, size_t len);
n_result udp_recvfrom(n_handle s, void *buf, size_t len, uint64_t deadline, struct n_sockaddr *from);
n_result udp_close(n_handle s);
n_result tcp_connect(uint32_t ip, uint16_t port, uint64_t deadline); /* a connection handle */
n_result tcp_send(n_handle s, const void *buf, size_t len);
/* Waits for room until deadline. A positive result is the accepted prefix, never a claim that
   the peer acknowledged it. */
n_result tcp_send_until(n_handle s, const void *buf, size_t len, uint64_t deadline);
/* The accepted prefix, or -N_ERR_NOT_READY; never waits for space. */
n_result tcp_send_some(n_handle s, const void *buf, size_t len);
/* Abort the connection, waking send/receive waiters with -N_ERR_CANCELLED. */
n_result tcp_cancel(n_handle s);
n_result tcp_recv(n_handle s, void *buf, size_t len, uint64_t deadline); /* 0 = connection closed */
n_result tcp_close(n_handle s);
/* Passive open: tcp_listen returns a listener on a local address (0: all of them, or e.g.
   127.0.0.1 for this system only; port 0 picks one; backlog <= 0 uses a default), tcp_accept
   the next established connection, ordinary for tcp_send/tcp_recv/tcp_close.
   tcp_close on the listener refuses connections not yet accepted. */
n_result tcp_listen(uint32_t ip, uint16_t port, int backlog);
n_result tcp_accept(n_handle listener, uint64_t deadline, struct n_sockaddr *remote);
n_result tcp_address(n_handle s, struct n_sockaddr *local, struct n_sockaddr *remote); /* either may be NULL */
n_result tcp_finish(n_handle s); /* stop sending (FIN after the queued data) but keep receiving; tcp_close later */
/* simulated packet loss/reordering on this process's TCP connections, and TCP counters (either may be NULL) */
n_result net_test(const struct n_netfault *set, struct n_tcpstats *stats);
bool net_wait_up(uint64_t deadline); /* wait until an adapter has an address; false if there is no adapter, or at the deadline */

/* ---- GUI ---- */
typedef struct window {
    n_handle handle; /* N_KIND_WINDOW: its input is reported by n_events_watch as N_EVENT_RECEIVABLE */
    int w, h; /* client size */
    /* What the app draws into: the window's surface (WIN_FRAME_QUEUE: the one acquired, px NULL
       while none is) */
    canvas_t c;
    int flags;
    struct gui_event q[32];
    int qn, qi;
    bool closed;
    unsigned nsurf;                        /* its surfaces, mapped here */
    uint32_t *surface[N_WIN_SURFACES_MAX];
    size_t surface_bytes;
    int current;                           /* WIN_FRAME_QUEUE: the surface acquired, -1 for none */
    uint64_t presented;                    /* the last present's number (EV_FRAME's serial) */
} window_t;

/* The calls below return their result (0 unless noted), or a negative n_result,
   and leave errno alone. */
bool gui_available(void);
n_result screen_size(int *w, int *h); /* w and h are written only on success */
/* Opens a window into *out; *out is NULL on failure. */
n_result win_open(int w, int h, const char *title, int flags, window_t **out);
void win_close(window_t *win);
/* Presents what was drawn; its number goes to presented (EV_FRAME's serial says which present a
   frame showed). A window without WIN_FRAME_QUEUE presents a rectangle of its surface (win_update: all
   of it), which the system copies. A WIN_FRAME_QUEUE window presents its acquired surface whole
   and has none after it; -N_ERR_BUSY when no frame was begun. */
n_result win_update(window_t *win);
n_result win_update_rect(window_t *win, int x, int y, int w, int h);
/* WIN_FRAME_QUEUE only: waits for a surface free to draw into (SYS_WIN_ACQUIRE) and makes it c. A
   frame is drawn whole: the surface holds an older picture, not the one presented last. */
n_result win_begin_frame(window_t *win);
/* New surfaces of w x h (1 to N_WIN_SIZE_MAX each) with as much of the picture shown as fits, in
   place of the window's; win_event does this for EV_RESIZE. Without memory the window keeps what it
   had (N_ERR_NO_MEMORY). */
n_result win_resize(window_t *win, int w, int h);
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
/* wait until deadline (N_CLOCK_NEVER: no limit) for an event; returns 1 if *e was filled, 0 at the
   deadline, negative once the window is gone. EV_RESIZE is handled before it is returned: the window has new
   surfaces of the size in the event, with as much of the picture shown as fits. An EV_KEY's key is its
   character or NKEY_ in the layout of the keyboard that typed it (n_key_translate). */
n_result win_event(window_t *win, struct gui_event *e, uint64_t deadline);
/* The one place a key's usage, modifiers and layout become a character (user/libc/keymap.c): sets e->key, and leaves in
   e->mods what programs read. */
void n_key_translate(struct gui_event *e);
/* Reads the console (N_KIND_CONSOLE) as text: each press becomes its character, in the layout of the
   keyboard that typed it, or the terminal's sequence for a moving key. flags as n_channel_receive's. */
n_result n_console_receive(n_handle h, void *buf, size_t cap, uint32_t flags);
n_result clipboard_set(const char *s, size_t n);
n_result clipboard_get(char *buf, size_t n); /* the whole length; at most n bytes are copied */
/* Starts a program (find_program: "notepad" is enough), with arg as its one argument and without
   input, output or log: an ordinary launch with the caller's identity. Its identifier, or a negative
   n_result. */
n_result gui_launch(const char *path, const char *arg);
/* copy the whole screen (taskbar included) into buf as 0xAARRGGBB pixels. bytes = 0 only reports
   the size. -N_ERR_TOO_LARGE: the buffer is too small */
n_result screen_grab(n_handle cap, uint32_t *buf, size_t bytes, int *w, int *h); /* N_CAP_SCREEN_READ */

/* ---- sound (sound.c) ----
   Streams come from n_audio_open (above) and hold about 170 ms (SOUND_QUEUE frames at SOUND_RATE). */
#define SOUND_RATE  48000
#define SOUND_QUEUE 8192
#define SOUND_FORMAT ((struct n_audio_format){.rate = SOUND_RATE, .channels = 2, .sample = N_AUDIO_S16})
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
