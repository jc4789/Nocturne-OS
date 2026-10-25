/* Nocturne kernel <-> userland ABI: syscall numbers and shared structures. */
#pragma once
#include <stdint.h>
/* x86-64 native mapping granule: n_memory and shared pages come in these. */
#define N_PAGE_SIZE 4096ULL

/* Nocturne results. A system call that fails returns the negated code; the
   numbering is Nocturne's own (docs/nocturne-native-abi.md). Plain numbers,
   so preprocessor conditions can use them. */
#define N_OK 0
#define N_ERR_NOT_FOUND 1          /* no such entry, name or channel */
#define N_ERR_EXISTS 2             /* the name is already taken */
#define N_ERR_DENIED 3             /* not permitted for this caller */
#define N_ERR_NO_MEMORY 4
#define N_ERR_INVALID 5            /* malformed request */
#define N_ERR_WRONG_KIND 6         /* the handle cannot perform this operation */
#define N_ERR_NO_HANDLE 7          /* the handle is not open in this app */
#define N_ERR_BUSY 8
#define N_ERR_NOT_READY 9          /* nothing to take yet; wait for its event */
#define N_ERR_CLOSED 10            /* the other end has gone */
#define N_ERR_TIMEOUT 11
#define N_ERR_CANCELLED 12
#define N_ERR_TOO_LARGE 13         /* does not fit the given buffer or range */
#define N_ERR_NOT_EMPTY 14
#define N_ERR_IO 15                /* the device failed */
#define N_ERR_UNSUPPORTED 16
#define N_ERR_BAD_ADDRESS 17       /* memory the caller passed is not usable */
#define N_ERR_STOPPED 18           /* the calling app is being stopped */
#define N_ERR_NOT_FOLDER 19
#define N_ERR_IS_FOLDER 20
#define N_ERR_READ_ONLY 21
#define N_ERR_NO_SPACE 22
#define N_ERR_NOT_PROGRAM 23       /* the file is not a Nocturne program */
#define N_ERR_NAME_TOO_LONG 24
#define N_ERR_NO_APP 25            /* no such app (or no such child of the caller) */
#define N_ERR_TOO_MANY_ARGUMENTS 26
#define N_ERR_CONNECTION_RESET 27
#define N_ERR_CONNECTION_REFUSED 28
#define N_ERR_ADDRESS_IN_USE 29
#define N_ERR_NETWORK_DOWN 30
#define N_ERR_NOT_CONNECTED 31
#define N_ERR_UNREACHABLE 32
#define N_ERR_PROTOCOL 33
#define N_RESULT_CODES 34

/* System call numbers. A number keeps its meaning for as long as it exists, so
   programs built inside Nocturne keep working; the numbers of the first ABI's
   calls (removed at stage 5 of docs/nocturne-native-abi.md) stay unassigned. */
enum {
    SYS_EXIT = 0,
    SYS_SLEEP = 19, SYS_UPTIME, SYS_TIME,
    SYS_PROCLIST = 26, SYS_SYSINFO, SYS_YIELD, SYS_POWER,
    SYS_SYSTEM_LOG = 33, SYS_PCILIST,
    SYS_CPU_INFO = 38, SYS_GPU_INFO, SYS_GPU_RENDER, SYS_AUDIO_FLUSH,
    SYS_GPU_RENDER_BATCH,
    SYS_GPU_BLIT,
    SYS_CLOCK_INFO, SYS_THREAD_CREATE, SYS_THREAD_JOIN, SYS_THREAD_EXIT,
    SYS_WAIT_ADDRESS, SYS_WAKE_ADDRESS, SYS_THREAD_ID, SYS_CPU_INDEX,
    SYS_THREAD_TLS,
    SYS_CHANNEL_LISTEN, SYS_CHANNEL_CONNECT, SYS_CHANNEL_ACCEPT,
    SYS_SHARED_CREATE,
    SYS_THREAD_STACK_BOUNDS = 58, SYS_THREAD_DETACH,
    SYS_WIN_CREATE = 64, SYS_WIN_MAP, SYS_WIN_PRESENT, SYS_WIN_SET_TITLE, SYS_SCREEN_INFO,
    SYS_WIN_MOVE, SYS_CLIPBOARD_SET, SYS_CLIPBOARD_GET, SYS_WIN_RESIZE, SYS_GUI_LAUNCH,
    SYS_SCREEN_GRAB, SYS_WIN_STATE, SYS_WIN_PRESENTATION,
    SYS_WIN_RECEIVE, /* (window, struct gui_event *out, max, timeout_ms): count, 0 on timeout */
    SYS_WIN_DIRECT,  /* (window, wanted): ask to be shown by the display driver directly (EV_DIRECT) */
    SYS_SHARED_RESERVE = 88, SYS_WAIT_ADDRESS64,
    SYS_NET_INFO = 96, SYS_NET_PING, SYS_NET_DNS, SYS_UDP_SOCKET, SYS_UDP_SEND, SYS_UDP_RECV,
    SYS_TCP_CONNECT, SYS_TCP_SEND, SYS_TCP_RECV, SYS_TCP_CLOSE, SYS_UDP_CLOSE, SYS_NET_TEST,
    SYS_NET_DNS_ALL, SYS_TCP_SEND_UNTIL, SYS_TCP_CANCEL, SYS_NET_STATS,
    SYS_NET_SETUP_CANCEL_CHANNEL,
    /* Passive open (RFC 9293). A listener shares the TCP id space: TCP_CLOSE
       ends it and refuses its unaccepted connections, TCP_CANCEL wakes waiters.
       LISTEN (ip, port, backlog): ip 0 takes every local address, otherwise one of
       them (127/8 or the interface's); port 0 picks one, backlog <= 0 a default.
       ACCEPT (listener, timeout_ms like TCP_RECV, struct n_sockaddr *remote or 0).
       ADDRESS (id, struct n_sockaddr *local, struct n_sockaddr *remote; either 0).
       FINISH (id): RFC 9293 CLOSE, the FIN after the queued data; receiving goes
       on until the peer finishes too, and the id stays the caller's until TCP_CLOSE. */
    SYS_TCP_LISTEN, SYS_TCP_ACCEPT, SYS_TCP_ADDRESS, SYS_TCP_FINISH,
    SYS_NET_LIMIT = 128,
    /* Native ABI (docs/nocturne-native-abi.md): typed handles, documents and
       folders by full path, message channels, apps and event queues. */
    SYS_HANDLE_CLOSE = 128, SYS_HANDLE_KIND,
    SYS_DOCUMENT_OPEN, SYS_DOCUMENT_READ_AT, SYS_DOCUMENT_WRITE_AT, SYS_DOCUMENT_SIZE,
    SYS_DOCUMENT_SET_SIZE, SYS_DOCUMENT_CLAIM, SYS_DOCUMENT_RELEASE, SYS_DOCUMENT_SAVE,
    SYS_DOCUMENT_APPEND,
    SYS_ENTRY_INFO, SYS_ENTRY_REMOVE, SYS_ENTRY_MOVE, SYS_FOLDER_CREATE, SYS_FOLDER_LIST,
    SYS_CHANNEL_PAIR, SYS_CHANNEL_SEND, SYS_CHANNEL_RECEIVE,
    SYS_APP_LAUNCH, SYS_APP_RESULT, SYS_APP_STOP, SYS_APP_SELF, SYS_APP_PARENT,
    SYS_EVENTS_CREATE, SYS_EVENTS_WATCH, SYS_EVENTS_UNWATCH, SYS_EVENTS_WATCH_ADDRESS,
    SYS_EVENTS_TIMER, SYS_EVENTS_WAIT,
    SYS_APP_OPEN, SYS_APP_ID, SYS_SYSTEM_LOG_OPEN,
    SYS_AUDIO_OPEN, SYS_AUDIO_SUBMIT, SYS_AUDIO_STATUS, SYS_AUDIO_VOLUME,
    SYS_RANDOM,
    /* (bytes, access): N_MEMORY_* access, N_MEMORY_LOW added for pages below 2 GiB */
    SYS_MEMORY_RESERVE, SYS_MEMORY_RELEASE, SYS_MEMORY_PROTECT,
    SYS_SHARED_MAP, /* (shared handle, bytes, N_MEMORY_READ or N_MEMORY_READ_WRITE) */
    /* Drivers in apps (docs/nocturne-native-abi.md §9): the right to a PCI function, its
       memory as shared objects and its interrupts. */
    SYS_PCI_CLAIM, SYS_PCI_INFO, SYS_PCI_CONFIG_READ, SYS_PCI_CONFIG_WRITE, SYS_PCI_BAR,
    SYS_PCI_INTERRUPTS, SYS_PCI_INTERRUPT_TAKE,
    SYS_DMA_CREATE, SYS_DMA_ADDRESSES, SYS_SHARED_VIEW,
    SYS_SHARED_REVOKE, SYS_GPU_RENDER_LIST,
    SYS_CHANNEL_RECEIVE_FROM, SYS_SHARED_COMPOSE,
    SYS_PCI_HOLD_SCREEN, /* (pci, hold): no drawing to the boot screen in the function's BAR */
    SYS_DOCUMENT_COMMIT, /* (document, N_COMMIT_*) */
    SYS_DOCUMENT_INFO,   /* (document, struct n_entry *out, sizeof *out) */
    SYS_ENTRY_REPLACE,   /* (from, to): SYS_ENTRY_MOVE that may replace a document */
    /* SYS_ENTRY_INFO and SYS_FOLDER_LIST with the size of the caller's struct n_entry */
    SYS_ENTRY_INFO_SIZED,  /* (path, struct n_entry *out, sizeof *out) */
    SYS_FOLDER_LIST_SIZED, /* (path, struct n_entry *out, max, start, sizeof *out): count */
    /* A display driver draws the screen's scanout (docs/nocturne-native-abi.md §9) */
    SYS_PCI_GIVE_SCREEN,   /* (pci, surface or N_SCANOUT_BOOT, const struct n_scanout *) */
    SYS_PCI_SCREEN_VBLANK, /* (pci, count, timestamp_ns): one vertical blank of that scanout */
    /* Paged memory and its pager (docs/nocturne-native-abi.md §7) */
    SYS_PAGER_CREATE,      /* (bytes, int32_t *memory_out): the pager handle */
    SYS_PAGER_TAKE,        /* (pager, struct n_pager_fault *out, max, timeout_ms): count */
    SYS_PAGER_SUPPLY,      /* (pager, const struct n_pager_supply *runs, count) */
    SYS_PAGER_REVOKE,      /* (pager, offset, bytes, 0 or N_MEMORY_READ) */
    SYS_PAGER_FAIL,        /* (pager, offset, bytes) */
    /* A claimed function's part of the ACPI namespace (docs/nocturne-native-abi.md §9) */
    SYS_PCI_ACPI_EVAL,     /* (pci, const struct n_acpi_eval *, out, out_size): bytes written */
    SYS_PCI_ACPI_NOTIFY_TAKE, /* (pci, timeout_ms): the next Notify value of the function's node */
    /* A display driver follows the screen: the mode it is to show, and with N_SCANOUT_DIRECT the
       window it shows directly */
    SYS_PCI_SCREEN_STATE,  /* (pci, seen serial, struct n_screen_state *out, timeout_ms) */
    /* What such a driver may show in its scanout's place, which the panic screen is drawn into too */
    SYS_PCI_SCREEN_IMAGE,     /* (pci, surface, const struct n_scanout *): the image's number */
    SYS_PCI_SCREEN_IMAGE_END, /* (pci, number) */
    /* A display driver's monitor: the modes it can show and the one it shows (the system says
       which it wants in struct n_screen_state) */
    SYS_PCI_SCREEN_MODES,     /* (pci, const struct n_screen_modes *) */
    SYS_MAX = 208          /* above every number (what syscallfuzz tries) */
};

/* ---- native handles ---- */
#define N_KIND_DOCUMENT 1
#define N_KIND_FOLDER 2
#define N_KIND_CHANNEL 3
#define N_KIND_WINDOW 4
#define N_KIND_SHARED 5
#define N_KIND_APP 6
#define N_KIND_EVENTS 7
#define N_KIND_DEVICE 8
#define N_KIND_CONNECTION 9
#define N_KIND_PCI 10       /* the right to drive one PCI function */
#define N_KIND_PAGER 11     /* the right to supply one paged memory object; never sent */

/* The folders of C: every app and the system know. Apps start in the user's folder, C:\Users\<name>,
   which the setting USERFOLDER names; its AppData\Temp, emptied at every start, is the setting TEMP. */
#define N_FOLDER_SYSTEM   "C:\\Nocturne\\System"  /* the system's programs */
#define N_FOLDER_PROGRAMS "C:\\Programs"           /* the apps, each in a folder of its own */

/* n_document_open access: one of these, not a bit set. */
#define N_DOCUMENT_READ 1     /* read an existing document */
#define N_DOCUMENT_EDIT 2     /* read and write an existing document */
#define N_DOCUMENT_CREATE 3   /* a new, empty document; N_ERR_EXISTS if the name is taken */
#define N_DOCUMENT_REPLACE 4  /* new content, swapped in whole when saved (or by n_handle_close) */

/* n_document_commit: save the document (a REPLACE handle's version is published first and the
   handle reads it from then on). */
#define N_COMMIT_WAIT 1       /* until it is on the medium */
#define N_COMMIT_START 2      /* return at once; N_EVENT_SAVED reports the outcome */
#define N_COMMIT_RESULT 3     /* the last started save's outcome (N_ERR_NOT_READY: not yet) */

/* An entry of a folder, or the entry a path names. id is the entry itself on C:: it
   stays with the entry when it moves and is never given to another one. New fields come at
   the end: a call that returns entries takes the size of the caller's struct n_entry and
   writes that much of each, entries that far apart (N_ERR_INVALID below N_ENTRY_FIRST_BYTES,
   N_ERR_UNSUPPORTED past what the kernel knows). SYS_ENTRY_INFO and SYS_FOLDER_LIST, the
   calls of programs built before id, write N_ENTRY_FIRST_BYTES. */
struct n_entry {
    char name[256];
    uint32_t kind;        /* N_KIND_DOCUMENT or N_KIND_FOLDER */
    uint32_t modified_ns; /* nanoseconds within the second `modified` names */
    uint64_t size;
    int64_t modified;     /* wall seconds */
    uint64_t id;
};
#define N_ENTRY_FIRST_BYTES 280 /* the first struct n_entry: name to modified */

/* n_channel_receive flags */
#define N_CHANNEL_WAIT 1
#define N_CHANNEL_PEEK 2  /* report the next message's size, leave it queued */
#define N_CHANNEL_DISCARD 4 /* copy up to cap bytes of the next message, then drop it; its size */

/* Credentials belong to the message's actual sending app, not the endpoint's
   creator or its current holder. sender_app is the non-reused n_app_self ID. */
struct n_channel_message {
    int32_t attached;       /* -1 on peek/discard, or when no handle was sent */
    uint32_t reserved;
    uint64_t sender_app;
};

/* n_app_launch. Handle i of the new app is handles[i] of the launcher (-1:
   none); by convention 0 is its input, 1 its output and 2 its log. Settings
   are "NAME=value" strings that ISO C getenv reads. */
struct n_launch {
    const char *const *arguments;
    const char *const *settings;
    const int32_t *handles;
    uint32_t argument_count, setting_count, handle_count, flags;
};
/* The most bytes a launch's argument and setting strings (each with its nul) and their two
   null-terminated pointer arrays (8 bytes a pointer) may take: N_ERR_TOO_MANY_ARGUMENTS past it. */
#define N_LAUNCH_MAX_BYTES (4ULL << 20)
#define N_APP_STOP_TREE 1  /* n_app_stop: also stop every app it launched */

/* Event interests and reports (level-triggered). */
#define N_EVENT_RECEIVABLE 1u  /* a message, input, connection or data waits */
#define N_EVENT_SENDABLE 2u
#define N_EVENT_CLOSED 4u      /* the other end has gone */
#define N_EVENT_APP_ENDED 8u
#define N_EVENT_TIMER 16u
#define N_EVENT_ADDRESS 32u    /* a watched memory word was notified */
#define N_EVENT_SAVED 64u      /* a document save started with N_COMMIT_START has its outcome */
struct n_event {
    uint64_t token;
    uint32_t events;
    uint32_t reserved;
};

/* n_memory_reserve / n_memory_protect access. Writable and executable are
   never granted together: generated code is written, then made executable. */
#define N_MEMORY_READ 1
#define N_MEMORY_READ_WRITE 2
#define N_MEMORY_READ_EXECUTE 3
/* Added to a reserve's access: place the pages below 2 GiB, for generated code
   whose instructions hold sign-extended 32-bit absolute addresses. */
#define N_MEMORY_LOW 0x10

struct n_procinfo {
    int pid, ppid;
    int state; /* 0 ready 1 running 2 blocked 3 zombie */
    int is_user;
    char name[32];
    uint64_t cpu_ms;
    uint64_t mem_kb;
    uint64_t start_ms;
    uint64_t wait_phys; /* blocked in an address wait: the physical word, else 0 */
};

/* free_mem: what an app can have now, the free frames and those of programs no task runs, which
   the system gives back to an app that asks for them. */
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

/* worker_cpus: the online APs, all of which run kernel jobs. scheduler_cpus:
   CPU 0 plus the APs that also run user tasks, so the two overlap. */
struct n_cpuinfo {
    uint32_t version, detected_cpus, online_cpus, worker_cpus, scheduler_cpus, flags;
    uint64_t parallel_jobs, worker_chunks;
};
#define N_CPU_AP_WORKERS 1u
#define N_CPU_SSE2 2u
#define N_CPU_AVX 4u
#define N_CPU_USER_THREADS 8u

struct n_pciinfo {
    uint8_t bus, dev, func, cls, subcls, progif;
    uint16_t vendor, device;
};

/* A claimed PCI function (n_pci_info). Addresses are bus addresses, which are physical
   addresses: where an IOMMU isolates the function (N_PCI_ISOLATED) its domain maps each DMA
   object at the same address, and nothing else but the firmware's reserved ranges. */
#define N_PCI_ISOLATED 1u /* n_pci_function.flags: its DMA reaches only the claim's DMA objects */
#define N_PCI_NEEDS_RECOVERY 2u /* previous driver ended active; native device recovery is required */
#define N_PCI_BAR_MEMORY 1u
#define N_PCI_BAR_64BIT 2u
#define N_PCI_BAR_PREFETCH 4u
#define N_PCI_BAR_IO 8u
#define N_PCI_NONE 0xFFFFFFFFu  /* no such function, BAR or framebuffer */
#define N_PCI_SELF 0xFFFFFFFFu  /* n_pci_config_read: the claimed function itself */
#define N_PCI_TARGET(bus, dev, func) ((uint32_t)(bus) << 8 | (uint32_t)(dev) << 3 | (uint32_t)(func))
struct n_pci_bar {
    uint64_t address, size;
    uint32_t flags, reserved;
};
struct n_pci_function {
    uint8_t bus, dev, func, revision;
    uint16_t vendor, device, subsystem_vendor, subsystem_id;
    uint8_t cls, subcls, progif, interrupt_pin;
    uint32_t msi_vectors, msix_vectors; /* what the function offers, 0 without */
    uint32_t upstream;                  /* N_PCI_TARGET of the bridge above it, or N_PCI_NONE */
    uint32_t flags;                     /* N_PCI_ISOLATED, N_PCI_NEEDS_RECOVERY */
    struct n_pci_bar bar[6];
    /* The screen the firmware set up, when its pixels lie in one of this function's BARs
       (framebuffer_bar, else N_PCI_NONE): drivers must keep that memory as it is. */
    uint64_t framebuffer_address, framebuffer_size;
    uint32_t framebuffer_bar, framebuffer_width, framebuffer_height, framebuffer_pitch;
    uint32_t framebuffer_bpp, reserved2;
};
/* n_dma_create flags */
#define N_DMA_CONTIGUOUS 1u  /* one physically contiguous run */
#define N_DMA_32BIT 2u       /* every page below 4 GiB */
/* n_shared_view caching. RAM may be viewed in any of them, device memory only uncached or
   write-combining, other shared pages only as they are. */
#define N_CACHE_DEFAULT 0u   /* the object's own: write-back for RAM, uncached for device memory */
#define N_CACHE_WRITE_BACK 1u
#define N_CACHE_WRITE_COMBINE 2u
#define N_CACHE_UNCACHED 3u

/* n_pci_give_screen: where in a surface the display engine reads the screen from. width and
   height are the size the desktop is drawn at (the mode's the driver shows; the desktop takes
   it), pitch is the bytes from one row to the next, offset the first row's place in the surface. */
#define N_SCANOUT_XRGB8888 1u    /* 32 bits: x8 r8 g8 b8, blue in the lowest byte */
#define N_SCANOUT_XRGB2101010 2u /* 32 bits: x2 r10 g10 b10, blue in the lowest bits */
#define N_SCANOUT_XBGR8888 3u    /* 32 bits: x8 b8 g8 r8, red in the lowest byte */
#define N_SCANOUT_BOOT (-1)      /* the surface argument: the firmware's framebuffer again */
/* A flag in format: the driver can show a window's own images instead of the desktop and draw
   the pointer itself while it does (n_pci_screen_state says when and where). */
#define N_SCANOUT_DIRECT 0x100u
#define N_SCANOUT_FORMAT_MASK 0xffu
struct n_scanout {
    uint32_t width, height, pitch, format;
    uint64_t offset;
};

/* A display mode: the visible pixels and the vertical refresh in millihertz (59940 is 59.94 Hz).
   N_SCREEN_MODE_PREFERRED marks the monitor's own preferred timing (its EDID's). */
#define N_SCREEN_MODE_PREFERRED 1u
struct n_screen_mode {
    uint32_t width, height;
    uint32_t refresh_mhz;
    uint32_t flags;
};
/* n_pci_screen_modes: the monitor a display driver drives and what it can show there. The system
   chooses the mode (struct n_screen_state's mode); the driver says what it showed, each time it
   acted on one. monitor tells monitors apart (the same text for the same monitor every time:
   the EDID's maker, product and serial number), name is the monitor's for people (may be empty);
   both end with a zero. modes are the progressive modes the driver checked it can show, count of
   them, no two alike (width, height, refresh). shown is the mode the driver shows now, all zero
   when it shows none; done is the n_screen_state mode_serial it acted on last (0: none yet). */
#define N_SCREEN_MODES_MAX 128u
struct n_screen_modes {
    char monitor[32];
    char name[32];
    uint64_t done;
    struct n_screen_mode shown;
    uint32_t count, reserved;
    struct n_screen_mode modes[N_SCREEN_MODES_MAX];
};

/* n_pci_screen_state: what the screen should show, for the driver whose scanout the screen is
   drawn into or whose monitor's modes the system has. serial changes with every change of what
   follows and is never 0.
   mode is the mode the system wants the driver to show (one of the modes it gave, all zero while
   it wants none); mode_serial changes with every wish, even for the same mode again (a monitor
   offered anew). The driver shows it at that size (n_pci_give_screen) and gives n_pci_screen_modes
   with done = mode_serial and what it shows then.
   For a driver whose scanout has N_SCANOUT_DIRECT: app is the app (n_app_self's number) whose
   fullscreen window the system lets show its own images directly, 0 when the desktop is shown.
   While app is not 0 the driver draws the pointer: at pointer_x, pointer_y on the screen (where
   its hot spot goes) when pointer_shown, the image in pointer (rows of N_SCREEN_POINTER_SIZE
   pixels of 0xAARRGGBB, not premultiplied, transparent outside the shape) with its hot spot at
   hot_x, hot_y; pointer_serial changes when the image does. */
#define N_SCREEN_POINTER_SIZE 32u
struct n_screen_state {
    uint64_t serial;
    uint32_t app;
    uint32_t pointer_shown;
    int32_t pointer_x, pointer_y;
    uint32_t hot_x, hot_y;
    uint32_t pointer_serial, reserved;
    uint64_t mode_serial;
    struct n_screen_mode mode;
    uint32_t pointer[N_SCREEN_POINTER_SIZE * N_SCREEN_POINTER_SIZE];
};

/* n_pci_acpi_eval: an ACPI object, an argument or a result. An integer's value is the number; a
   string's or buffer's is where its bytes start (in the arguments' data, or in the result) and
   length how many (a string's without a terminating zero); a package's value is the index of its
   first element among the result's values and length how many. A reference (to a namespace
   object) carries nothing a driver can use. */
#define N_ACPI_NONE 0u      /* no value (a method that returned nothing, an uninitialised element) */
#define N_ACPI_INTEGER 1u
#define N_ACPI_STRING 2u
#define N_ACPI_BUFFER 3u
#define N_ACPI_PACKAGE 4u
#define N_ACPI_REFERENCE 5u
struct n_acpi_value {
    uint32_t type, length;
    uint64_t value;
};
/* Which node an evaluation is relative to: the function's own device node, the node of the bridge
   it is below (the host bridge's on a root bus), or the function node's device child number i. */
#define N_ACPI_SELF 0xFFFFFFFFu
#define N_ACPI_UPSTREAM 0xFFFFFFFEu
#define N_ACPI_CHILD(i) ((uint32_t)(i))
/* What SYS_PCI_ACPI_EVAL evaluates: name (one name segment, 4 characters, as "_DSM") on node,
   with nargs arguments (integers, strings or buffers) whose bytes are in data. */
struct n_acpi_eval {
    uint32_t node, nargs;
    const char *name;
    const struct n_acpi_value *args;
    const void *data;
    uint64_t data_size;
};

/* n_pager_take: a page of paged memory was touched without the access it needs. offset is
   the page's (page aligned) in the memory, access N_MEMORY_READ or N_MEMORY_READ_WRITE (a
   write), app the identifier of the app that touched it first. */
struct n_pager_fault {
    uint64_t offset;
    uint32_t access;
    uint32_t app;
};
/* n_pager_supply: [offset, offset+bytes) of the paged memory is bound to the pages of the
   shared object `source` (a handle of the pager's) from source_offset on, with `access`
   (N_MEMORY_READ or N_MEMORY_READ_WRITE). All three are page aligned. */
struct n_pager_supply {
    uint64_t offset, bytes, source_offset;
    int32_t source;
    uint32_t access;
};

/* Page-aligned slices concatenated by n_shared_compose, in array order. */
struct n_shared_range {
    int32_t shared;
    uint32_t reserved;
    uint64_t offset, bytes;
};

/* power */
#define POWER_OFF 0
#define POWER_REBOOT 1

/* n_audio_status: where a stream stands. Device time counts this stream's
 * consumed PCM only. */
struct n_audio_status {
    uint32_t queued_frames;
    uint32_t device_pending_frames;
    uint64_t played_frames;
    uint64_t committed_frames;
    uint32_t generation;
    uint32_t flags;
};
#define N_AUDIO_CLOCK_DEVICE 1u
#define N_AUDIO_CLOCK_ESTIMATED 2u
#define N_AUDIO_CLOCK_REMOTE 4u
#define N_AUDIO_RATE 48000     /* n_audio_submit: frames of two int16_t, left first */
#define N_AUDIO_WAIT 1u        /* n_audio_submit: wait for room instead of N_ERR_NOT_READY */

/* ---- GUI ---- */
enum {
    EV_NONE = 0, EV_KEY, EV_MOUSE_MOVE, EV_MOUSE_DOWN, EV_MOUSE_UP, EV_CLOSE, EV_FOCUS, EV_UNFOCUS,
    EV_WHEEL, EV_RESIZE, EV_FRAME,
    EV_DIRECT, /* key 1: the window's own images are shown directly (win_direct); 0: no longer */
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
#define WIN_ALWAYS_ON_TOP 16
/* Opt-in full-frame protocol. MAP(fd, index, WIN_MAP_ACQUIRE) waits until
   the compositor releases that buffer. PRESENT's final argument is index+1.
   No writes are allowed between submission and acquisition. Legacy calls
   (final argument zero) retain their rectangle snapshot semantics. */
#define WIN_DOUBLE_BUFFER 32
#define WIN_BUFFER_COUNT 2
#define WIN_MAP_ACQUIRE 1
/* EV_FRAME: key = completed index, x = reusable index; completion means all
   compositor reads have joined, not hardware vblank or physical scanout. */
#define WIN_STATE_VISIBLE 1
#define WIN_STATE_FOCUSED 2
#define WIN_STATE_FULLSCREEN 4
#define WIN_STATE_DIRECT 8 /* the display driver shows the window's own images (EV_DIRECT) */

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
/* DNS count is the complete candidate count, even when caller capacity is
   smaller. A second call may retrieve the cached complete result. */
enum n_dns_status {
    N_DNS_OK, N_DNS_NXDOMAIN, N_DNS_NODATA, N_DNS_SERVFAIL, N_DNS_REFUSED,
    N_DNS_FORMAT, N_DNS_TIMEOUT, N_DNS_UNREACHABLE, N_DNS_CANCELLED, N_DNS_RESOURCE,
};
struct n_dns_result { uint32_t version, status, count, ttl; };
#define N_TCP_SEND_NONBLOCK 1u /* SYS_TCP_SEND_UNTIL final flags argument */
struct n_netstats {
    uint64_t rx_dropped, rx_invalid, rx_budget_yields;
    uint64_t rx_completion_pending, rx_completion_retries, rx_completion_cancelled, rx_ring_high_water;
    uint64_t ap_send_bytes[16], ap_recv_bytes[16], ap_calls[16], ap_fallbacks[16];
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
