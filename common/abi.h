/* Nocturne kernel <-> userland ABI: syscall numbers and shared structures. */
#pragma once
#include <stdint.h>

enum {
    SYS_EXIT = 0, SYS_READ, SYS_WRITE, SYS_OPEN, SYS_CLOSE, SYS_LSEEK, SYS_STAT, SYS_FSTAT,
    SYS_READDIR, SYS_MKDIR, SYS_UNLINK, SYS_RENAME, SYS_CHDIR, SYS_GETCWD, SYS_SPAWN, SYS_WAITPID,
    SYS_GETPID, SYS_KILL, SYS_SBRK, SYS_SLEEP, SYS_UPTIME, SYS_TIME, SYS_PIPE, SYS_DUP2,
    SYS_DUP, SYS_POLL, SYS_PROCLIST, SYS_SYSINFO, SYS_YIELD, SYS_POWER, SYS_FCNTL, SYS_GETPPID,
    SYS_KILLTREE, SYS_DMESG, SYS_PCILIST, SYS_MMAP, SYS_MUNMAP, SYS_MPROTECT,
    SYS_CPU_INFO, SYS_GPU_INFO, SYS_GPU_RENDER, SYS_AUDIO_FLUSH,
    SYS_GPU_RENDER_BATCH,
    SYS_GPU_BLIT,
    SYS_CLOCK_INFO, SYS_THREAD_CREATE, SYS_THREAD_JOIN, SYS_THREAD_EXIT,
    SYS_WAIT_ADDRESS, SYS_WAKE_ADDRESS, SYS_THREAD_ID, SYS_CPU_INDEX,
    SYS_THREAD_TLS,
    SYS_WIN_CREATE = 64, SYS_WIN_MAP, SYS_WIN_PRESENT, SYS_WIN_SET_TITLE, SYS_SCREEN_INFO,
    SYS_WIN_MOVE, SYS_CLIPBOARD_SET, SYS_CLIPBOARD_GET, SYS_WIN_RESIZE, SYS_GUI_LAUNCH,
    SYS_SCREEN_GRAB, SYS_WIN_STATE,
    SYS_NET_INFO = 96, SYS_NET_PING, SYS_NET_DNS, SYS_UDP_SOCKET, SYS_UDP_SEND, SYS_UDP_RECV,
    SYS_TCP_CONNECT, SYS_TCP_SEND, SYS_TCP_RECV, SYS_TCP_CLOSE, SYS_UDP_CLOSE, SYS_NET_TEST,
    SYS_MAX = 128
};

/* file types */
/* SYS_MMAP / SYS_MPROTECT protection bits (reading is always allowed) */
#define N_PROT_WRITE 2
#define N_PROT_EXEC 4

#define N_FT_FILE 1
#define N_FT_DIR 2
#define N_FT_CHAR 3
#define N_FT_PIPE 4
#define N_FT_WINDOW 5

struct n_dirent {
    char name[256];
    uint32_t type;
    uint32_t reserved;
    uint64_t size;
};

struct n_stat {
    uint32_t type;
    uint32_t mode;
    uint64_t size;
    int64_t mtime;
};

struct n_procinfo {
    int pid, ppid;
    int state; /* 0 ready 1 running 2 blocked 3 zombie */
    int is_user;
    char name[32];
    uint64_t cpu_ms;
    uint64_t mem_kb;
    uint64_t start_ms;
};

struct n_sysinfo {
    uint64_t total_mem, free_mem, heap_used, uptime_ms;
    int ntasks;
    uint32_t fb_w, fb_h;
    char cpu[64];
    char os[64];
};

/* Versioned extensions keep the original n_sysinfo layout unchanged. */
struct n_clockinfo {
    uint32_t version, flags;
    uint64_t tsc_hz, tsc_sample, uptime_ms;
    uint64_t reference_page, reference_sample_100ns;
};
#define N_CLOCK_USER_TSC 1u
#define N_CLOCK_HV_REFERENCE 2u
/* Read-only Hyper-V reference page. Hypervisor updates use sequence protocol. */
struct n_reference_tsc {
    volatile uint32_t sequence;
    uint32_t reserved;
    volatile uint64_t scale;
    volatile int64_t offset;
};

struct n_cpuinfo {
    uint32_t version, detected_cpus, online_cpus, worker_cpus, scheduler_cpus, flags;
    uint64_t parallel_jobs, worker_chunks;
};
#define N_CPU_AP_WORKERS 1u
#define N_CPU_SSE2 2u
#define N_CPU_AVX 4u
#define N_CPU_USER_THREADS 8u

struct n_pollfd {
    int fd;
    short events, revents;
};
#define N_POLLIN 1
#define N_POLLOUT 4
#define N_POLLHUP 16

struct n_pciinfo {
    uint8_t bus, dev, func, cls, subcls, progif;
    uint16_t vendor, device;
};

/* spawn flags */
#define SPAWN_DETACH 1

/* power */
#define POWER_OFF 0
#define POWER_REBOOT 1

/* fcntl */
#define F_GETFL 3
#define F_SETFL 4
#define F_SETTTY 100 /* mark a pipe as a terminal (the terminal emulator does this), so isatty() is true */
#define F_NLOCK 1024   /* Nocturne: wait for a vnode lease owned by this open file */
#define F_NUNLOCK 1025 /* release that lease; the last close also releases it */

/* ---- GUI ---- */
enum {
    EV_NONE = 0, EV_KEY, EV_MOUSE_MOVE, EV_MOUSE_DOWN, EV_MOUSE_UP, EV_CLOSE, EV_FOCUS, EV_UNFOCUS,
    EV_WHEEL, EV_RESIZE,
};

struct gui_event {
    uint32_t type;
    int32_t x, y;       /* mouse position in client coordinates / new size for EV_RESIZE */
    uint32_t key;       /* KEY_* or character */
    uint32_t mods;
    uint32_t buttons;
    int32_t wheel;
    uint32_t pressed;
};

/* window flags */
#define WIN_RESIZABLE 1
#define WIN_NO_DECOR 2
#define WIN_CENTER 4
#define WIN_START_MAXIMIZED 8
#define WIN_STATE_VISIBLE 1
#define WIN_STATE_FOCUSED 2

struct screen_info {
    uint32_t width, height;
};

/* key codes (shared with kernel/dev/input.h) */
#ifndef KEY_UP
enum {
    NKEY_ESC = 0x1B, NKEY_BACKSPACE = 0x08, NKEY_TAB = 0x09, NKEY_ENTER = 0x0A,
    NKEY_UP = 0x100, NKEY_DOWN, NKEY_LEFT, NKEY_RIGHT, NKEY_HOME, NKEY_END, NKEY_PGUP, NKEY_PGDN,
    NKEY_INSERT, NKEY_DELETE, NKEY_F1, NKEY_F2, NKEY_F3, NKEY_F4, NKEY_F5, NKEY_F6, NKEY_F7, NKEY_F8,
    NKEY_F9, NKEY_F10, NKEY_F11, NKEY_F12,
};
#endif
#define NMOD_SHIFT 1
#define NMOD_CTRL 2
#define NMOD_ALT 4

/* networking */
struct n_netinfo {
    uint8_t present, up;
    uint8_t mac[6];
    uint8_t ip[4], mask[4], gateway[4], dns[4];
    uint64_t rx_packets, tx_packets;
    char driver[32];
};
/* SYS_NET_TEST: simulated loss and reordering on the calling process's TCP connections (for tests),
   in per mille of the segments that carry data; all zero turns it off. */
struct n_netfault {
    uint16_t rx_drop, rx_reorder, tx_drop;
};
struct n_tcpstats { /* since boot, all connections */
    uint64_t segs_out, segs_in, retransmits, fast_retransmits, timeouts, dupacks_in, ooo_in;
    uint64_t faults_dropped, faults_reordered;
};
struct n_sockaddr {
    uint32_t ip; /* host byte order: a.b.c.d is a<<24 | b<<16 | c<<8 | d */
    uint16_t port;
};
