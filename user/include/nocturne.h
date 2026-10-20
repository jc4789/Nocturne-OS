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
int msleep(unsigned ms);
uint64_t uptime_ms(void);
/* Native memory (docs/nocturne-native-abi.md §7): zero-filled pages with
 * N_MEMORY_READ, _READ_WRITE or _READ_EXECUTE access; reserve also takes
 * N_MEMORY_LOW added to the access, for pages below 2 GiB. reserve returns NULL
 * and sets errno on failure; release and protect return -1. */
void *n_memory_reserve(size_t bytes, int access);
int n_memory_release(void *addr, size_t bytes);
int n_memory_protect(void *addr, size_t bytes, int access);
/* Shared pages: a handle is a private capability that travels to another app
 * only as a channel attachment (n_channel_send). A mapping keeps the pages
 * after the handle closes; n_memory_release of the whole mapping ends it. */
n_handle n_shared_create(size_t bytes);
/* Like n_shared_create, but pages are zero-filled at first touch by any mapper. */
n_handle n_shared_reserve(size_t bytes);
/* Map all of the pages (bytes is their page-rounded size) with N_MEMORY_READ
 * or N_MEMORY_READ_WRITE access; NULL and errno on failure. */
void *n_shared_map(n_handle shared, size_t bytes, int access);

const char *n_result_text(n_result r); /* either sign */
n_result n_handle_close(n_handle h);   /* commits an N_DOCUMENT_REPLACE handle */
n_result n_handle_kind(n_handle h);    /* N_KIND_* */
/* Documents and folders: paths are complete, starting at "/". */
n_handle n_document_open(const char *path, int access); /* N_DOCUMENT_* */
n_result n_document_read_at(n_handle h, uint64_t offset, void *buf, size_t n); /* bytes; 0 at the end */
n_result n_document_write_at(n_handle h, uint64_t offset, const void *buf, size_t n);
n_result n_document_size(n_handle h);
n_result n_document_set_size(n_handle h, uint64_t size); /* new bytes read as zero */
n_result n_document_claim(n_handle h, bool wait);        /* N_ERR_NOT_READY when not waiting */
n_result n_document_release(n_handle h);
n_result n_document_save(const char *path, const void *data, size_t size); /* whole and atomic */
n_result n_document_append(const char *path, const void *data, size_t size);
/* The whole document in fresh n_memory pages; free with n_memory_release(*data, *size). */
n_result n_document_load(const char *path, void **data, size_t *size);
n_result n_entry_info(const char *path, struct n_entry *out);
n_result n_entry_remove(const char *path);
n_result n_entry_move(const char *from, const char *to); /* never replaces: N_ERR_EXISTS */
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
n_handle n_system_log_open(void); /* a handle whose messages are added to the system log */
n_result n_app_id(n_handle app); /* its identifier; 0 once it has ended */
/* Drivers in apps (docs/nocturne-native-abi.md §9). Only init may claim a PCI function; it
   hands the N_KIND_PCI handle to the driver app it launches. A claim is exclusive until its
   last handle closes, which also turns the function's interrupts off. */
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
   the claim) draws the whole screen again. N_ERR_NOT_FOUND: the boot screen is elsewhere. */
n_result n_pci_hold_screen(n_handle pci, bool hold);
/* Zeroed RAM that a claimed function may read and write by DMA: a shared object (N_DMA_*
   flags), held while any handle or mapping holds it. n_dma_addresses gives the bus address of
   each page from first_page (count of them; returns how many it wrote). */
n_handle n_dma_create(n_handle pci, size_t bytes, uint32_t flags);
n_result n_dma_addresses(n_handle shared, size_t first_page, uint64_t *out, size_t count);
/* A new shared object over part of another (page-aligned offset and bytes) with the given
   caching (N_CACHE_*): mapping it maps the same pages. */
n_handle n_shared_view(n_handle shared, size_t offset, size_t bytes, uint32_t cache);
/* Sound: each n_audio_open is a stream of N_AUDIO_RATE stereo frames (two int16_t, left first)
   of its own, mixed with the others and scaled by the master volume. A stream holds about
   170 ms; n_audio_submit takes what fits and returns the frames taken (with N_AUDIO_WAIT it
   waits for room; without, a full stream gives N_ERR_NOT_READY and N_EVENT_SENDABLE reports
   room). Closing lets the queued sound play out; n_audio_flush drops it at once. */
n_handle n_audio_open(void); /* N_ERR_BUSY when every stream is taken */
n_result n_audio_submit(n_handle a, const int16_t *frames, size_t count, uint32_t flags);
n_result n_audio_status(n_handle a, struct n_audio_status *out);
n_result n_audio_flush(n_handle a);
n_result n_audio_volume(void);            /* the master volume, 0 to 100 */
n_result n_audio_set_volume(int percent); /* 0 to 100; also unmutes */
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
/* A complete path for a name the user typed: a name not starting at "/" is
   taken inside the launch setting FOLDER (the launcher's current folder, "/"
   if it gave none). Returns the length, or N_ERR_TOO_LARGE. The system
   resolves "." and "..". */
n_result n_path_complete(const char *name, char *out, size_t cap);
n_result n_folder_set(const char *name); /* this app's FOLDER; N_ERR_NOT_FOLDER if it is not one */
/* name taken inside folder (a complete path), with "." and ".." resolved. */
n_result n_path_join(const char *folder, const char *name, char *out, size_t cap);
/* Whether handle h reaches a terminal: the launcher names such handles in the
   setting TERMINAL, e.g. "0 1 2". */
bool n_terminal(n_handle h);

int proclist(struct n_procinfo *out, int max);
int sysinfo(struct n_sysinfo *si);
int cpu_info(struct n_cpuinfo *info);
int clock_info(struct n_clockinfo *info);
/* Native tasks sharing this process's address space and descriptors. */
int thread_create(void (*fn)(void *), void *arg);
int thread_join(int tid);
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
/* Actual reserved stack interval of the calling native task. */
int thread_stack_bounds(uintptr_t *low, uintptr_t *high);
/* timeout_ms == UINT32_MAX waits indefinitely; count == 0 wakes all.
 * The wait returns 0 when woken, -N_ERR_NOT_READY when *address no longer
 * holds expected, -N_ERR_TIMEOUT or -N_ERR_STOPPED; wake returns how many
 * waiters it woke.  errno is left alone. */
n_result wait_on_address(volatile uint32_t *address, uint32_t expected, unsigned timeout_ms);
/* The 8-byte form; wake_address on the same address wakes it. */
n_result wait_on_address64(volatile uint64_t *address, uint64_t expected, unsigned timeout_ms);
n_result wake_address(volatile uint32_t *address, unsigned count);
void yield(void);
int poweroff(void);
int reboot(void);
int pcilist(struct n_pciinfo *out, int max);
long __syscall(long n, long a, long b, long c, long d, long e);

/* run a program (searching /bin) and wait for it; returns exit status or negative error */
int run_wait(const char *path, char *const argv[]);
/* find a program: "ls" -> "/bin/ls"; returns false if not found */
bool find_program(const char *name, char *out, size_t n);

/* line editor for terminal programs (echo, editing keys, history). Returns the line length, or -1
   at end of input (Ctrl+D). RL_SECRET echoes '*' and keeps the line out of the history. */
#define RL_SECRET 1
int readline(const char *prompt, char *buf, size_t size, int flags);

/* ---- networking (net.c) ----
   IPv4 addresses are uint32_t in host order (10.0.2.15 is 0x0A00020F). Functions return -1 and
   set errno on failure. Timeouts are in milliseconds; -1 waits forever. */
int net_info(struct n_netinfo *ni);
int net_ping(uint32_t ip, int seq, int timeout_ms); /* round-trip time in ms */
int net_resolve(const char *name, uint32_t *ip);    /* DNS lookup or dotted quad */
int net_resolve_all(const char *name, uint32_t *ips, size_t capacity, struct n_dns_result *result);
int net_resolve_at(const char *name, uint32_t *ips, size_t capacity, struct n_dns_result *result,
                   const struct n_sockaddr *resolver);
int net_stats(struct n_netstats *stats);
/* Calling thread only: abort DNS/ARP/connect if this request channel's peer
   closes. -1 clears the context. Established/shared TCP I/O is unaffected. */
int net_set_setup_cancel_channel(n_handle channel);
bool ip_parse(const char *s, uint32_t *ip);
char *ip_format(uint32_t ip, char *buf); /* buf: at least 16 bytes */
int udp_socket(uint16_t port);           /* 0 picks an ephemeral port */
int udp_sendto(int s, uint32_t ip, uint16_t port, const void *buf, size_t len);
int udp_recvfrom(int s, void *buf, size_t len, int timeout_ms, struct n_sockaddr *from);
int udp_close(int s);
int tcp_connect(uint32_t ip, uint16_t port, int timeout_ms);
long tcp_send(int s, const void *buf, size_t len);
/* Absolute uptime_ms deadline; zero waits without a deadline. A positive
   result is the accepted prefix, never a claim that the peer acknowledged it. */
long tcp_send_until(int s, const void *buf, size_t len, uint64_t deadline_ms);
long tcp_send_some(int s, const void *buf, size_t len); /* prefix or EAGAIN, never waits for space */
/* Abort the connection, waking send/receive waiters with ECANCELED. */
int tcp_cancel(int s);
long tcp_recv(int s, void *buf, size_t len, int timeout_ms); /* 0 = connection closed */
int tcp_close(int s);
/* Passive open: tcp_listen returns a listener on a local address (0: all of them, or e.g.
   127.0.0.1 for this system only; port 0 picks one; backlog <= 0 uses a default), tcp_accept
   the next established connection, ordinary for tcp_send/tcp_recv/tcp_close.
   tcp_close on the listener refuses connections not yet accepted. */
int tcp_listen(uint32_t ip, uint16_t port, int backlog);
int tcp_accept(int listener, int timeout_ms, struct n_sockaddr *remote);
int tcp_address(int s, struct n_sockaddr *local, struct n_sockaddr *remote); /* either may be NULL */
int tcp_finish(int s); /* stop sending (FIN after the queued data) but keep receiving; tcp_close later */
/* simulated packet loss/reordering on this process's TCP connections, and TCP counters (either may be NULL) */
int net_test(const struct n_netfault *set, struct n_tcpstats *stats);
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

bool gui_available(void);
int screen_size(int *w, int *h);
window_t *win_open(int w, int h, const char *title, int flags);
void win_close(window_t *win);
void win_update(window_t *win);
void win_update_rect(window_t *win, int x, int y, int w, int h);
/* WIN_DOUBLE_BUFFER only: acquire a writable canvas before drawing a complete
   frame. No retained/partial drawing across submissions; update invalidates c.
   Returns zero or a negative error. Completion is also reported as EV_FRAME. */
int win_begin_frame(window_t *win);
void win_set_title(window_t *win, const char *title);
void win_move(window_t *win, int x, int y);
int win_state(window_t *win); /* WIN_STATE_* bitmask, negative on failure */
int win_fullscreen(window_t *win,bool enabled); /* resize delivered by EV_RESIZE */
/* wait up to timeout_ms (-1 = forever) for an event; returns 1 if *e was filled, 0 on timeout,
   -1 if the window is gone. EV_RESIZE is handled (buffer reallocated) before being returned. */
int win_event(window_t *win, struct gui_event *e, int timeout_ms);
int clipboard_set(const char *s, size_t n);
int clipboard_get(char *buf, size_t n);
int gui_launch(const char *path, const char *arg);
/* copy the whole screen (taskbar included) into buf as 0xAARRGGBB pixels. bytes = 0 only reports
   the size. 0 or -1 (E2BIG: buffer too small) */
int screen_grab(uint32_t *buf, size_t bytes, int *w, int *h);

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
