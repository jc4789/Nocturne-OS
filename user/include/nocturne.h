/* Nocturne OS system interface. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "abi.h"
#include "fcntl.h"
#include "gfx.h"
#include <cdefs.h>
NOCTURNE_BEGIN_DECLS

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define ABS(a) ((a) < 0 ? -(a) : (a))

typedef long ssize_t;
typedef int pid_t;

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int open(const char *path, int flags, ...);
int close(int fd);
long lseek(int fd, long off, int whence);
int ftruncate(int fd, long size); /* new bytes read as zero */
int stat(const char *path, struct n_stat *st);
int fstat(int fd, struct n_stat *st);
int readdir(int fd, int idx, struct n_dirent *d);
int mkdir(const char *path, ...);
int unlink(const char *path);
int rmdir(const char *path);
int chdir(const char *path);
char *getcwd(char *buf, size_t n);
int spawn(const char *path, char *const argv[], const int fdmap[3], int flags);
int waitpid(int pid, int *status, int flags);
#define WNOHANG 1
int getpid(void);
int getppid(void);
int kill(int pid);
int killtree(int pid, int include_self);
void *sbrk(long inc);
/* malloc's heap: bytes in its n_memory regions, and bytes in allocated blocks. */
void malloc_usage(size_t *heap, size_t *used);
int msleep(unsigned ms);
unsigned sleep(unsigned s);
uint64_t uptime_ms(void);
int pipe(int fds[2]);
/* Native memory (docs/nocturne-native-abi.md §7): zero-filled pages with
 * N_MEMORY_READ, _READ_WRITE or _READ_EXECUTE access; reserve also takes
 * N_MEMORY_LOW added to the access, for pages below 2 GiB. reserve returns NULL
 * and sets errno on failure; release and protect return -1. */
void *n_memory_reserve(size_t bytes, int access);
int n_memory_release(void *addr, size_t bytes);
int n_memory_protect(void *addr, size_t bytes, int access);
/* Native shared-page object. Descriptors are private capabilities; importing
 * another process's descriptor is permitted only between direct parent/child.
 * A mapping retains the object after close and is released by whole munmap. */
int n_shared_create(size_t bytes);
/* Like n_shared_create, but pages are zero-filled at first touch by any mapper. */
int n_shared_reserve(size_t bytes);
void *n_shared_map(int owner_pid,int fd,size_t bytes,int prot);
/* Offer a shared-page descriptor to the direct parent/child that will read a
 * message naming the returned ticket; receive claims it as a new descriptor.
 * An unclaimed ticket keeps the pages alive until the sender exits. */
long n_shared_send(int fd);
int n_shared_receive(int owner_pid,long ticket);
/* Asynchronous waits on a 32-bit word. Watches join the same FIFO as
 * wait_on_address (also across processes sharing the page); a notified watch
 * makes the queue readable as 64-bit tokens. watch fails with EAGAIN when the
 * word differs. unwatch: 1 removed while waiting, 2 already notified, 0 unknown. */
int n_address_watch_queue(void);
int n_address_watch(int queue,volatile uint32_t *address,uint32_t expected,uint64_t token);
int n_address_unwatch(int queue,uint64_t token);
/* Native named rendezvous, not a filesystem or Unix socket namespace.
 * Listen atomically owns key until its last duplicated fd closes (EEXIST).
 * Connect/accept return nonblocking duplex read/write/poll fds; accept with
 * no pending peer fails with EAGAIN. Closing the peer yields EOF/EPIPE/HUP.
 * Key bytes are matched in full; callers must encode their own identity. */
int n_channel_listen(const char *key);
int n_channel_connect(const char *key);
int n_channel_accept(int listener_fd);

/* ---- native ABI (docs/nocturne-native-abi.md) ----
   These return a negative n_result on failure and never touch errno. */
typedef int32_t n_handle;
typedef int64_t n_result;
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
/* N_CHANNEL_WAIT waits; N_CHANNEL_PEEK only reports the next message's size.
   A message larger than cap stays queued and gives N_ERR_TOO_LARGE. */
n_result n_channel_receive(n_handle h, void *buf, size_t cap, int flags, n_handle *attached);
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

int dup(int fd);
int dup2(int fd, int newfd);
int poll(struct n_pollfd *fds, int n, int timeout_ms);
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
/* timeout_ms == UINT32_MAX waits indefinitely; count == 0 wakes all. */
int wait_on_address(volatile uint32_t *address, uint32_t expected, unsigned timeout_ms);
/* The 8-byte form; wake_address on the same address wakes it. */
int wait_on_address64(volatile uint64_t *address, uint64_t expected, unsigned timeout_ms);
int wake_address(volatile uint32_t *address, unsigned count);
void yield(void);
int poweroff(void);
int reboot(void);
int fcntl(int fd, int cmd, ...);
int dmesg(char *buf, size_t n);
int pcilist(struct n_pciinfo *out, int max);
int isatty(int fd);
extern char **environ;
int execvp(const char *file, char *const argv[]); /* spawn, wait and exit */
int execv(const char *path, char *const argv[]);
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
   closes. fd=-1 clears the context. Established/shared TCP I/O is unaffected. */
int net_set_setup_cancel_channel(int fd);
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
/* simulated packet loss/reordering on this process's TCP connections, and TCP counters (either may be NULL) */
int net_test(const struct n_netfault *set, struct n_tcpstats *stats);
bool net_wait_up(int timeout_ms); /* wait for DHCP; false if no network */

/* ---- GUI ---- */
typedef struct window {
    int fd;
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

/* ---- tiny UI kit (ui.c) ---- */
#define UI_BG      RGB(30, 28, 52)
#define UI_PANEL   RGB(40, 38, 70)
#define UI_FG      RGB(232, 232, 245)
#define UI_DIM     RGB(150, 150, 185)
#define UI_ACCENT  RGB(130, 110, 240)
#define UI_ACCENT2 RGB(246, 200, 110)
#define UI_BTN     RGB(56, 52, 98)
#define UI_BTN_HI  RGB(84, 74, 150)
void ui_button(canvas_t *c, int x, int y, int w, int h, const char *label, bool hot, bool pressed);
void ui_text_center(canvas_t *c, int x, int y, int w, const char *s, uint32_t col, int font);
bool ui_hit(int px, int py, int x, int y, int w, int h);
/* draws a single-line text field; returns nothing. */
void ui_textfield(canvas_t *c, int x, int y, int w, const char *text, bool focus);
/* edit a line buffer with a key event; returns 1 on Enter, -1 on Escape, 0 otherwise */
int ui_edit_key(char *buf, size_t cap, const struct gui_event *e);

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
    int fd, tag, channels, bits, block;
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
