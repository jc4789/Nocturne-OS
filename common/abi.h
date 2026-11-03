/* Nocturne kernel <-> userland ABI: syscall numbers and shared structures. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "input_abi.h"
#include "authority_abi.h"
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
#define N_ERR_WRONG_GENERATION 34  /* the program was built for another generation of the system */
#define N_RESULT_CODES 35

/* The calls of the kernel. A program never holds the SYSCALL instruction: it calls the slot of its
   entry image (below), and each name here is that slot's index. The ABI keeps no older form of a
   call: a program is built for one generation of the system (N_ABI_GENERATION). 1-18, 21-25,
   30-32, 36-37, 44, 51-55, 60-63, 73, 90 and 203-206 are unassigned. */
enum {
    SYS_EXIT = 0,
    SYS_SLEEP = 19, /* (deadline): until the monotonic clock reaches it (§8) */
    SYS_APP_CPU_TIME, /* (): ns the calling app's threads have run, the ended ones too (§8) */
    SYS_PROCLIST = 26, SYS_SYSINFO, SYS_YIELD, SYS_POWER,
    SYS_IMAGE_MAP,            /* (読むために開いた文書の handle, struct n_image *out): ドライバーの像を自分へ写す */
    SYS_SYSTEM_LOG = 33,
    SYS_HAL_LIST,             /* (struct n_hal_device *out, max): 台帳の写し。戻りは台帳の全部の数 */
    SYS_HAL_WATCH,            /* (): 台帳が変わると受け取れる見張り */
    SYS_CPU_INFO = 38, SYS_GPU_INFO, SYS_GPU_RENDER,
    SYS_AUDIO_DEVICE_RUN,     /* (device, 0 or 1): 輪の始めから流す、止める（§17） */
    SYS_GPU_RENDER_BATCH,
    SYS_GPU_BLIT,
    SYS_THREAD_CREATE = 45, SYS_THREAD_JOIN, SYS_THREAD_EXIT,
    SYS_WAIT_ADDRESS, SYS_WAKE_ADDRESS, SYS_THREAD_ID,
    SYS_SHARED_CREATE = 56,
    SYS_THREAD_SCHEDULE, /* (thread, N_SCHEDULE_*, weight or period ns): its class (§5) */
    SYS_THREAD_STACK_BOUNDS = 58, SYS_THREAD_DETACH,
    SYS_WIN_CREATE = 64, /* (width, height, title, flags, surfaces): the window handle */
    SYS_WIN_SURFACE,     /* (window, index): a handle to that surface of the window's size */
    SYS_WIN_PRESENT,     /* (window, x, y, width << 32 | height, index + 1 or 0): the present's number */
    SYS_WIN_SET_TITLE, SYS_SCREEN_INFO,
    SYS_WIN_MOVE, SYS_CLIPBOARD_SET, SYS_CLIPBOARD_GET,
    SYS_WIN_RESIZE,      /* (window, width, height): new surfaces of that size (SYS_WIN_SURFACE) */
    SYS_SCREEN_GRAB = 74, SYS_WIN_STATE, SYS_WIN_PRESENTATION,
    SYS_WIN_RECEIVE, /* (window, struct gui_event *out, max, deadline): count, 0 at the deadline */
    SYS_WIN_DIRECT,  /* (window, wanted): ask to be shown by the display driver directly (EV_DIRECT) */
    SYS_WIN_FRAME,   /* (window): EV_FRAME at the display's next frame, presented or not (WIN_FRAME_EVENTS) */
    SYS_WIN_POINTER, /* (window, const struct n_win_pointer *, size): the pointer over the client area */
    SYS_WIN_ACQUIRE, /* (window, deadline): a surface of a WIN_FRAME_QUEUE window free to draw into */
    /* シェルの口（display_abi.h）。第一引数は N_CAP_SHELL の能力。 */
    SYS_SHELL_PLACE,   /* (cap, window, const struct n_shell_place *, size): 窓の層と位置と端の予約 */
    SYS_SHELL_SCREENS, /* (cap, struct n_shell_screen *out, max): 画面の数 */
    SYS_SHELL_WINDOWS, /* (cap, struct n_shell_window *out, max): 窓の数 */
    SYS_SHELL_WINDOW,  /* (cap, 窓の番号, N_SHELL_RAISE など) */
    SYS_SHELL_WATCH,   /* (cap): 窓の一覧と焦点の watch */
    SYS_SHELL_HOTKEY,  /* (cap, window, usage, mods): そのキーの押下を EV_HOTKEY で受ける */
    SYS_SHARED_RESERVE = 88, SYS_WAIT_ADDRESS64,
    SYS_NET_FIRST = 96,
    /* The stack's adapters and routes. ADAPTERS (struct n_net_adapter *out, max) -> how many there
       are. ADDRESS_SET (N_CAP_NETWORK, adapter, ip, mask): ip 0 takes the address away.
       ROUTES_SET (N_CAP_NETWORK, const struct n_net_route *, count): the whole table.
       ROUTES (struct n_net_route *out, max) -> how many there are. WATCH (): a watch of all of it.
       UDP_SOCKET_ON (N_CAP_NETWORK, adapter, port): a socket on one adapter, which may have no
       address yet; its datagrams go first to it and leave by that adapter. */
    SYS_NET_ADAPTERS = SYS_NET_FIRST, SYS_NET_PING, SYS_NET_ADDRESS_SET, SYS_UDP_SOCKET, SYS_UDP_SEND, SYS_UDP_RECV,
    SYS_TCP_CONNECT, SYS_TCP_SEND, SYS_TCP_RECV, SYS_TCP_CLOSE, SYS_UDP_CLOSE, SYS_NET_TEST,
    SYS_NET_ROUTES_SET, SYS_TCP_SEND_UNTIL, SYS_TCP_CANCEL, SYS_NET_STATS,
    SYS_NET_SETUP_CANCEL_CHANNEL,
    /* Passive open (RFC 9293). A listener shares the TCP id space: TCP_CLOSE
       ends it and refuses its unaccepted connections, TCP_CANCEL wakes waiters.
       LISTEN (ip, port, backlog): ip 0 takes every local address, otherwise one of
       them (127/8 or the interface's); port 0 picks one, backlog <= 0 a default.
       ACCEPT (listener, deadline like TCP_RECV, struct n_sockaddr *remote or 0).
       ADDRESS (id, struct n_sockaddr *local, struct n_sockaddr *remote; either 0).
       FINISH (id): RFC 9293 CLOSE, the FIN after the queued data; receiving goes
       on until the peer finishes too, and the id stays the caller's until TCP_CLOSE. */
    SYS_TCP_LISTEN, SYS_TCP_ACCEPT, SYS_TCP_ADDRESS, SYS_TCP_FINISH,
    SYS_NET_ROUTES, SYS_NET_WATCH, SYS_UDP_SOCKET_ON,
    SYS_NET_ADAPTER_NAME, /* (adapter, char *out, cap) -> its driver's name's whole length; what fits is copied */
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
    SYS_AUDIO_DEVICE_OPEN,    /* (N_CAP_AUDIO_DEVICE の cap, id, struct n_audio_device_format *, struct n_audio_device_ring *in_out) -> device */
    SYS_AUDIO_DEVICE_LIST,    /* (cap, void *out, bytes): 全部のバイト数。足りなければ何も書かない。struct n_audio_device の並び */
    SYS_AUDIO_DEVICE_STATUS,  /* (device, struct n_audio_device_status *out) */
    SYS_RANDOM = SYS_AUDIO_DEVICE_STATUS + 2,
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
    /* A display driver shows screens (docs/nocturne-native-abi.md §9): its outputs' buffers */
    SYS_PCI_GIVE_SCREEN,   /* (pci, output, const struct n_screen_buffers *, or 0 to give it back) */
    SYS_PCI_SCREEN_VBLANK, /* (pci, const struct n_screen_vblank *): a vertical blank of an output */
    /* Paged memory and its pager (docs/nocturne-native-abi.md §7) */
    SYS_PAGER_CREATE,      /* (bytes, int32_t *memory_out): the pager handle */
    SYS_PAGER_TAKE,        /* (pager, struct n_pager_fault *out, max, deadline): count */
    SYS_PAGER_SUPPLY,      /* (pager, const struct n_pager_supply *runs, count) */
    SYS_PAGER_REVOKE,      /* (pager, offset, bytes, 0 or N_MEMORY_READ) */
    SYS_PAGER_FAIL,        /* (pager, offset, bytes) */
    /* A claimed function's part of the ACPI namespace (docs/nocturne-native-abi.md §9) */
    SYS_PCI_ACPI_EVAL,     /* (pci, const struct n_acpi_eval *, out, out_size): bytes written */
    SYS_PCI_ACPI_NOTIFY_TAKE, /* (pci, deadline): the next Notify value of the function's node */
    /* A display driver follows a screen: the mode it is to show, and with N_SCANOUT_DIRECT the
       window it shows directly */
    SYS_PCI_SCREEN_STATE,  /* (pci, output, seen serial, struct n_screen_state *out, deadline) */
    /* What such a driver may show in its buffers' place, which the panic screen is drawn into too */
    SYS_PCI_SCREEN_IMAGE,     /* (pci, output, surface, const struct n_scanout *): the image's number */
    SYS_PCI_SCREEN_IMAGE_END, /* (pci, number) */
    /* A display driver's monitor on an output: the modes it can show and the one it shows (the
       system says which it wants in struct n_screen_state) */
    SYS_PCI_SCREEN_MODES,     /* (pci, output, const struct n_screen_modes *) */
    SYS_PCI_SCREEN_FLIPS,     /* (pci, struct n_screen_flip *out, max, deadline): count, 0 at the deadline */
    /* Watches (docs/nocturne-native-abi.md §1): a handle receivable once what it watches has
       changed, until SYS_WATCH_TAKE takes the count. Each call after it gives one. */
    SYS_WATCH_TAKE = 207,     /* (watch): the changes since its last take (0: none) */
    SYS_ENTRY_WATCH,          /* (path, N_WATCH_NAME or N_WATCH_FOLDER): a watch on C: (§3) */
    SYS_AUDIO_DEVICE_WATCH,   /* (cap): a watch on the list of SYS_AUDIO_DEVICE_LIST (§17) */
    SYS_IDENTITY_SELF,        /* (struct n_identity *out) */
    SYS_CHANNEL_PEER,         /* (channel, struct n_identity *out) */
    SYS_CAP_DERIVE,           /* (cap, enum n_cap) -> handle */
    SYS_SERVICE_REGISTER,     /* (N_CAP_SERVICE_ROOT の cap) -> 接続の要求を受ける channel */
    SYS_SERVICE_CONNECT,      /* (name, name_len) -> channel */
    SYS_STORE_PRIVATE_OPEN,   /* (N_CAP_STORE_PRIVATE の cap, region, user, access) -> document（§3） */
    SYS_SETTINGS_APPLY,       /* (N_CAP_SETTINGS_APPLY の cap, domain, key, value, len) */
    SYS_SETTINGS_EFFECTIVE,   /* (domain, key, out, cap): 今の値のバイト数（§12） */
    SYS_SETTINGS_DESCRIBE,    /* (domain, key, struct n_setting_desc *out)（§12） */
    SYS_SETTINGS_WATCH,       /* (domain): その領域の今の値の watch（§12） */
    SYS_ENTRY_SET_OWNER,      /* (path, user): C: の名前の持ち主の利用者。admin 以上（§3） */
    SYS_CAP_KIND,             /* (cap) -> enum n_cap */
    SYS_CAP_FIND,             /* (enum n_cap) -> その能力を持つ自分の handle */
    SYS_POWER_STATE,          /* (N_CAP_SHELL の cap, struct n_power_state *out, size): 電源の段取りの今 */
    SYS_POWER_WATCH,          /* (N_CAP_SHELL の cap): SYS_POWER_STATE が報告するものの watch */
    SYS_SESSION_STOP,         /* (N_CAP_IDENTITY_GRANT の cap, session): その身元のアプリを止める。まだ終わっていない数 */
    SYS_CLOCK_WALL_STEP,      /* (N_CAP_CLOCK の cap, unix_ns): 壁時計（UTC）を飛ばす */
    SYS_CLOCK_WALL_SLEW,      /* (N_CAP_CLOCK の cap, offset_ns, ppm): 壁時計をゆっくり動かす */
    SYS_RTC_WRITE,            /* (N_CAP_CLOCK の cap, unix_seconds): RTC へ UTC を書く */
    SYS_SCREEN_MODE_APPLY,    /* (N_CAP_DISPLAY の cap, 画面の番号, const struct n_screen_mode *): 出すモードをドライバーへ頼む */
    SYS_SCREEN_ARRANGE,       /* (N_CAP_DISPLAY の cap, 画面の番号, const struct n_screen_arrange *): 画面の位置、主画面、倍率 */
    SYS_POWER_EVENTS,         /* (N_CAP_POWER の cap, struct n_power_events *out, size): 電源の事象の今 */
    SYS_POWER_EVENTS_WATCH,   /* (N_CAP_POWER の cap): ボタン、蓋、起床、ホストの依頼の watch */
    SYS_POWER_WINDOWS,        /* (N_CAP_POWER の cap, 閉じるよう頼むなら 1, char *top_title, size): アプリの窓の数 */
    SYS_POWER_SAY,            /* (N_CAP_POWER の cap, const struct n_power_state *, size): 窓を閉じる段取りを知らせる */
    SYS_POWER_WINDOWS_WATCH,  /* (N_CAP_POWER の cap): アプリの窓の数と題の watch */
    SYS_MAX                   /* above every number (what syscallfuzz tries) */
};

/* ---- 入口の像（kernel/src/sys/entry.c、docs/nocturne-native-abi.md §0） ----
   カーネルは全プロセスに読取り専用の像を一つ写す。番地はプロセスごとに違い、struct n_start の entry が
   教える。溝の番号 i の関数は entry + i * N_ENTRY_SLOT_BYTES にあり、C の呼び方で呼ぶ
   （引数は rdi、rsi、rdx、rcx、r8、結果は rax）。溝の順は呼出しの名前の値のとおり。
   カーネルは、syscall がその番号の溝から来たことを確かめ、違えばプロセスを止める。 */
#define N_ENTRY_SLOT_BYTES 16u
enum {
    /* 溝のうち SYS_MAX から上は、カーネルへ入らない像の中のコード */
    N_ENTRY_CLOCK_MONOTONIC = SYS_MAX, /* uint64_t (void)：単調時計の ns */
    N_ENTRY_CLOCK_BOOT,                /* uint64_t (void)：眠りも数える時計の ns */
    N_ENTRY_CLOCK_WALL,                /* int64_t (void)：壁時計（UTC）の ns */
    N_ENTRY_CLOCK_TSC_HZ,              /* uint64_t (void)：TSC の周波数 */
    N_ENTRY_SLOTS
};

/* 公開の呼出し。C:\Nocturne の外のプログラムの像には、これだけがカーネルへ入る溝になる。ほかの溝は
   -N_ERR_DENIED を返す。C:\Nocturne の中のプログラムの像には全部の呼出しがある。表は面の広さを
   決めるだけで、呼べるかどうかの本当の検査は能力と身元で行う。 */
#define N_PUBLIC_CALLS(X) \
    X(SYS_EXIT) X(SYS_SLEEP) X(SYS_APP_CPU_TIME) X(SYS_SYSINFO) X(SYS_YIELD) X(SYS_CPU_INFO) X(SYS_HAL_LIST) \
    X(SYS_THREAD_CREATE) X(SYS_THREAD_JOIN) X(SYS_THREAD_EXIT) X(SYS_THREAD_ID) X(SYS_THREAD_SCHEDULE) \
    X(SYS_THREAD_STACK_BOUNDS) X(SYS_THREAD_DETACH) X(SYS_WAIT_ADDRESS) X(SYS_WAIT_ADDRESS64) X(SYS_WAKE_ADDRESS) \
    X(SYS_MEMORY_RESERVE) X(SYS_MEMORY_RELEASE) X(SYS_MEMORY_PROTECT) \
    X(SYS_SHARED_CREATE) X(SYS_SHARED_RESERVE) X(SYS_SHARED_MAP) \
    X(SYS_WIN_CREATE) X(SYS_WIN_SURFACE) X(SYS_WIN_PRESENT) X(SYS_WIN_SET_TITLE) X(SYS_WIN_MOVE) X(SYS_WIN_RESIZE) \
    X(SYS_WIN_STATE) X(SYS_WIN_PRESENTATION) X(SYS_WIN_RECEIVE) X(SYS_WIN_DIRECT) X(SYS_WIN_FRAME) \
    X(SYS_WIN_POINTER) X(SYS_WIN_ACQUIRE) X(SYS_SCREEN_INFO) X(SYS_CLIPBOARD_SET) X(SYS_CLIPBOARD_GET) \
    X(SYS_NET_ADAPTERS) X(SYS_NET_ADAPTER_NAME) X(SYS_NET_WATCH) X(SYS_NET_SETUP_CANCEL_CHANNEL) \
    X(SYS_UDP_SOCKET) X(SYS_UDP_SEND) X(SYS_UDP_RECV) X(SYS_UDP_CLOSE) \
    X(SYS_TCP_CONNECT) X(SYS_TCP_SEND) X(SYS_TCP_SEND_UNTIL) X(SYS_TCP_RECV) X(SYS_TCP_CLOSE) X(SYS_TCP_CANCEL) \
    X(SYS_TCP_LISTEN) X(SYS_TCP_ACCEPT) X(SYS_TCP_ADDRESS) X(SYS_TCP_FINISH) \
    X(SYS_HANDLE_CLOSE) X(SYS_HANDLE_KIND) \
    X(SYS_DOCUMENT_OPEN) X(SYS_DOCUMENT_READ_AT) X(SYS_DOCUMENT_WRITE_AT) X(SYS_DOCUMENT_SIZE) X(SYS_DOCUMENT_SET_SIZE) \
    X(SYS_DOCUMENT_CLAIM) X(SYS_DOCUMENT_RELEASE) X(SYS_DOCUMENT_SAVE) X(SYS_DOCUMENT_APPEND) X(SYS_DOCUMENT_COMMIT) \
    X(SYS_DOCUMENT_INFO) X(SYS_ENTRY_INFO) X(SYS_ENTRY_INFO_SIZED) X(SYS_ENTRY_REMOVE) X(SYS_ENTRY_MOVE) \
    X(SYS_ENTRY_REPLACE) X(SYS_ENTRY_WATCH) X(SYS_FOLDER_CREATE) X(SYS_FOLDER_LIST) X(SYS_FOLDER_LIST_SIZED) \
    X(SYS_CHANNEL_PAIR) X(SYS_CHANNEL_SEND) X(SYS_CHANNEL_RECEIVE) X(SYS_CHANNEL_RECEIVE_FROM) X(SYS_CHANNEL_PEER) \
    X(SYS_APP_LAUNCH) X(SYS_APP_RESULT) X(SYS_APP_STOP) X(SYS_APP_SELF) X(SYS_APP_PARENT) X(SYS_APP_OPEN) X(SYS_APP_ID) \
    X(SYS_EVENTS_CREATE) X(SYS_EVENTS_WATCH) X(SYS_EVENTS_UNWATCH) X(SYS_EVENTS_WATCH_ADDRESS) X(SYS_EVENTS_TIMER) \
    X(SYS_EVENTS_WAIT) X(SYS_WATCH_TAKE) \
    X(SYS_SYSTEM_LOG_OPEN) X(SYS_RANDOM) X(SYS_IDENTITY_SELF) X(SYS_SERVICE_CONNECT) X(SYS_IMAGE_MAP) \
    X(SYS_SETTINGS_EFFECTIVE) X(SYS_SETTINGS_DESCRIBE) X(SYS_SETTINGS_WATCH)

/* 世代。呼出し、構造体、システムのサービスの文の形を変えたら一つ上げる。溝を末尾へ足すだけなら上げない。 */
#define N_ABI_GENERATION 1u

/* プログラムの刻印：ELF の PT_NOTE の一つ（user/libc/sys.c が全プログラムに入れる）。カーネルは起こす前に
   読む。世代が違うか、溝がカーネルより多ければ、N_ERR_WRONG_GENERATION で起こさない。 */
#define N_PROGRAM_NOTE_NAME "Nocturne"
#define N_PROGRAM_NOTE_TYPE 1u
struct n_program_note {
    uint32_t name_bytes, desc_bytes, type; /* 9、8、N_PROGRAM_NOTE_TYPE */
    char name[12];                         /* N_PROGRAM_NOTE_NAME と nul、4 バイトへ詰める */
    uint32_t generation;                   /* 組んだときの N_ABI_GENERATION */
    uint32_t slots;                        /* 組んだときの N_ENTRY_SLOTS */
};

/* スレッドの区画。FS の基底はここを指し、頭の語は自分の番地（%fs:0）。C ライブラリーが作る。 */
struct n_thread_block {
    struct n_thread_block *self;
    uintptr_t entry; /* struct n_start の entry */
    /* ドライバーの像は自分の C ライブラリーを持つ。一つのスレッドは、触れた写しごとに区画を一つ持つ。
       FS が指すのはスレッドを作った写しの区画で、他の写しの区画はその others に繋がる。 */
    const void *owner;              /* この区画を持つ写しの目印 */
    struct n_thread_block *others;
    void (*leave)(struct n_thread_block *self); /* スレッドの終わり：写しが片付け、others から外れる */
};

/* ドライバーの像（SYS_IMAGE_MAP）：番地 0 から組んだ ET_DYN の ELF で、プログラムと同じ刻印を持つ。カーネルは
   読み込み可能な節を写すだけで、再配置もコードの実行もしない。読み取り専用の節は同じ文書を使う全プロセスで
   frame を共有し、書ける節はプロセスごとの写しになる。像はプロセスの終わりまで残る。呼ぶ者が entry を呼ぶ。
   権限はプログラムと同じで、像が使える溝もプログラムと同じ。 */
struct n_image {
    uint64_t base;  /* 像の番地 0 が写った番地 */
    uint64_t entry; /* 像の入口 */
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
#define N_KIND_WATCH 13     /* a watch (SYS_WATCH_TAKE): receivable once what it watches changed */
#define N_KIND_CONSOLE 15   /* the console: text goes out, struct n_console_key comes in (n_console_receive) */
#define N_KIND_CAPABILITY 14 /* 特権を一つ持つ（enum n_cap、SYS_CAP_KIND）。持つことが権限の証明。 */

/* SYS_ENTRY_WATCH: what is watched on C: */
#define N_WATCH_NAME 1u   /* the last name of the path in its folder, whether an entry has it or not */
#define N_WATCH_FOLDER 2u /* every entry directly in the folder the path names */
/* SYS_WATCH_TAKE counts up to this and stays there until the count is taken */
#define N_WATCH_CHANGES_MAX 0xffffffffu

/* The folders of C: every app and the system know. Apps start in the user's folder, C:\Users\<name>,
   which n_user_folder names from the identity; its AppData\Temp, emptied at every login, is n_temp_folder's. */
#define N_FOLDER_SYSTEM   "C:\\Nocturne\\System"  /* the system's programs */
#define N_FOLDER_PROGRAMS "C:\\Programs"           /* the apps, each in a folder of its own */
/* certificate authorities the system trusts besides the roots built into TLS (user/libc/tls.c) */
#define N_FOLDER_CERTIFICATES "C:\\Nocturne\\Certificates"

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
    uint32_t owner;       /* the user the name belongs to (0: the system) */
    uint32_t reserved;
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

/* n_app_launch. 新しいアプリの handle i は起動元の handles[i]（-1 は無し）。慣例で 0 が入力、
   1 が出力、2 がログ。起動の属性は型のある欄で渡す：
     folder       今のフォルダー（NULL は起動元の今のフォルダー。起動元も持たなければ子も持たず、
                  子は身元の利用者のフォルダーで始まる）。
     terminals    端末へつながる handle の番号。
     interrupts   端末の Ctrl+C の知らせを受ける channel の handle の番号（0 は無し）。
   利用者のフォルダーは身元から accounts が引く（n_user_folder）ので渡さない。
   environment は移植のプログラムのための "NAME=value" の列で、そのプログラムの libc の getenv だけ
   が読む。システムの部品はこれを読まず、これで組まない。 */
struct n_launch {
    const char *const *arguments;
    const char *const *environment;
    const int32_t *handles;
    uint32_t argument_count, environment_count, handle_count, flags;
    const char *folder;
    const int32_t *terminals;
    uint32_t terminal_count;
    int32_t interrupts;
    /* 子の身元。NULL：親と同じ user・session・cls。cls は親を超えられない。user・session を親と変えるには、
       identity_grant に N_CAP_IDENTITY_GRANT の handle を渡す。identity が NULL なら読まない。
       program は書けない（0）：カーネルが path から決める。能力は handles の列で渡す（子は SYS_CAP_FIND で引く）。 */
    const struct n_identity *identity;
    int32_t identity_grant;
    uint32_t reserved;
};
/* n_launch の flags：子を呼ぶ側と同じプログラムにする。プログラムの部品が親の N_SCOPE_PROGRAM の設定を読むため。 */
#define N_LAUNCH_SAME_PROGRAM 1
/* アプリの入口の第三引数。カーネルが n_launch の欄を新しいアプリの stack に写したもの。文字列と
   配列は同じ stack にある。folder は完全なパスか NULL。 */
struct n_start {
    uintptr_t entry;          /* 入口の像の番地（上の「入口の像」） */
    char *const *environment; /* NULL で終わる */
    const char *folder;
    const int32_t *terminals;
    uint32_t terminal_count;
    int32_t interrupts;
};
/* The most bytes a launch's argument and environment strings (each with its nul) and their two
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
    /* when it happened, on the monotonic clock (ns): a timer's moment on its grid; for the others
       when the kernel was told of the change the wait reports */
    uint64_t time_ns;
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
    int state; /* 0 ready 1 running 2 blocked 3 ended */
    int is_user;
    char name[32];
    uint64_t cpu_ns;
    uint64_t mem_kb;
    uint64_t start_ns;  /* on the monotonic clock */
    int app;            /* the app this thread belongs to: its pid, 0 for the kernel's threads */
    uint32_t user, session, cls; /* the app's identity (authority_abi.h) */
};

/* free_mem: what an app can have now, the free frames and those of programs no task runs, which
   the system gives back to an app that asks for them. */
struct n_sysinfo {
    uint64_t total_mem, free_mem, heap_used, uptime_ns; /* uptime: the monotonic clock */
    int ntasks;
    uint32_t fb_w, fb_h;
    char cpu[64];
    char os[64];
};

/* ---- time (docs/nocturne-native-abi.md §8) ----
   Every time the ABI passes is in ns. A deadline is a moment on the monotonic clock: N_CLOCK_NEVER
   is none, and one at or before the call's moment looks without waiting. */
#define N_NS_PER_US 1000ULL
#define N_NS_PER_MS 1000000ULL
#define N_NS_PER_S 1000000000ULL
#define N_CLOCK_NEVER UINT64_MAX

/* n_thread_schedule: the class a thread of the calling app runs in.
   N_SCHEDULE_FAIR: a share of the CPUs in proportion to weight, among the threads that want them;
   an app's threads have [1, N_SCHEDULE_WEIGHT_DEFAULT] (lower is a smaller share).
   N_SCHEDULE_DEADLINE: a job each period of period ns (its device's: a screen's frame, a sound
   block), [N_SCHEDULE_PERIOD_MIN_NS, N_SCHEDULE_PERIOD_MAX_NS]: woken, it runs before every fair
   thread, the one due first first, while its job stays within what the system reserves it each
   period; past that it runs as a fair thread until its next period. An app's deadline threads
   together are reserved at most half of one CPU. */
#define N_SCHEDULE_FAIR 0u
#define N_SCHEDULE_DEADLINE 1u
#define N_SCHEDULE_WEIGHT_DEFAULT 1024u
#define N_SCHEDULE_PERIOD_MIN_NS 125000ULL
#define N_SCHEDULE_PERIOD_MAX_NS 1000000000ULL

/* SYS_CPU_INFO. detected_cpus: the CPUs the firmware lists; online_cpus: those running;
   scheduler_cpus: those that run apps' threads, every online one. The kernel spreads no work of its
   own over CPUs: worker_cpus, parallel_jobs and worker_chunks are 0, and N_CPU_AP_WORKERS is never
   set. */
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

/* 機械で見つかった物の一つ（n_hal_list、docs/hal.md の台帳）。 */
enum n_hal_bus { N_HAL_BUS_PCI, N_HAL_BUS_USB, N_HAL_BUS_VMBUS, N_HAL_BUS_PORT };
enum n_hal_state {
    N_HAL_UNBOUND, /* 持っている実装が無い */
    N_HAL_RUNNING, /* 実装が持っている */
    N_HAL_FAILED,  /* 実装が持てなかった。理由は reason */
};
struct n_hal_device {
    uint32_t id;     /* 起動の間、二度と使わない番号 */
    uint32_t parent; /* 親の番号。0：親なし */
    uint32_t bus;    /* enum n_hal_bus */
    uint32_t state;  /* enum n_hal_state */
    int32_t reason;  /* N_HAL_FAILED の負の N_ERR_* */
    int32_t node;    /* NUMA の node。-1：不明 */
    union {
        struct n_pciinfo pci;
        struct { uint16_t vendor, product; uint8_t cls, port; uint32_t route; } usb;
        struct { uint8_t type[16]; } vmbus;
        struct { uint32_t base; } port;
    } at;
    char name[64];   /* 人に見せる名前。"": 無い */
    char driver[48]; /* 持っている実装の名。プロセスの実装はプログラムの名 */
};

/* PCI の機能の引き方の一行。fields が見る欄を言う。カーネルの実装の表とサービスの宣言が使う。 */
enum {
    N_PCI_MATCH_VENDOR = 1, N_PCI_MATCH_DEVICE = 2, N_PCI_MATCH_CLASS = 4, N_PCI_MATCH_SUBCLASS = 8, N_PCI_MATCH_PROGIF = 16,
};
struct n_pci_match {
    uint16_t vendor, device;
    uint8_t cls, subcls, progif, fields;
};
#define N_PCI_ID(v, d) {.vendor = (v), .device = (d), .fields = N_PCI_MATCH_VENDOR | N_PCI_MATCH_DEVICE}
#define N_PCI_ID_CLASS(v, d, c, s)                                                                                 \
    {.vendor = (v), .device = (d), .cls = (c), .subcls = (s),                                                      \
     .fields = N_PCI_MATCH_VENDOR | N_PCI_MATCH_DEVICE | N_PCI_MATCH_CLASS | N_PCI_MATCH_SUBCLASS}
#define N_PCI_CLASS(c, s) {.cls = (c), .subcls = (s), .fields = N_PCI_MATCH_CLASS | N_PCI_MATCH_SUBCLASS}
#define N_PCI_CLASS_PROGIF(c, s, p)                                                                                \
    {.cls = (c), .subcls = (s), .progif = (p), .fields = N_PCI_MATCH_CLASS | N_PCI_MATCH_SUBCLASS | N_PCI_MATCH_PROGIF}
#define N_PCI_VENDOR_CLASS_PROGIF(v, c, s, p)                                                                      \
    {.vendor = (v), .cls = (c), .subcls = (s), .progif = (p),                                                      \
     .fields = N_PCI_MATCH_VENDOR | N_PCI_MATCH_CLASS | N_PCI_MATCH_SUBCLASS | N_PCI_MATCH_PROGIF}
static inline bool n_pci_matches(const struct n_pci_match *m, const struct n_pciinfo *f) {
    return (!(m->fields & N_PCI_MATCH_VENDOR) || m->vendor == f->vendor) &&
           (!(m->fields & N_PCI_MATCH_DEVICE) || m->device == f->device) &&
           (!(m->fields & N_PCI_MATCH_CLASS) || m->cls == f->cls) &&
           (!(m->fields & N_PCI_MATCH_SUBCLASS) || m->subcls == f->subcls) &&
           (!(m->fields & N_PCI_MATCH_PROGIF) || m->progif == f->progif);
}

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

/* The pixel formats of a display's buffers and images. */
#define N_SCANOUT_XRGB8888 1u    /* 32 bits: x8 r8 g8 b8, blue in the lowest byte */
#define N_SCANOUT_XRGB2101010 2u /* 32 bits: x2 r10 g10 b10, blue in the lowest bits */
#define N_SCANOUT_XBGR8888 3u    /* 32 bits: x8 b8 g8 r8, red in the lowest byte */
/* A flag in format: the driver can show a window's own images instead of the desktop and draw
   the pointer itself while it does (n_pci_screen_state says when and where). */
#define N_SCANOUT_DIRECT 0x100u
#define N_SCANOUT_FORMAT_MASK 0xffu
/* n_pci_screen_image: where in a surface the display engine reads an image from: width x height
   pixels, pitch bytes from one row to the next, the first row offset bytes into the surface. */
struct n_scanout {
    uint32_t width, height, pitch, format;
    uint64_t offset;
};
/* n_pci_give_screen: the buffers an output shows a screen from, all of one mode and format. width
   and height are the mode's (the screen takes that size), pitch the bytes from one row to the
   next, format N_SCANOUT_* (with N_SCANOUT_DIRECT when the driver can show windows directly).
   Each buffer is a surface (a writable shared object made through the claim) and the offset of
   its first row in it; count of them, 1 to N_SCREEN_BUFFERS_MAX. The system draws a frame only
   into a buffer the output does not show and then asks for it (n_pci_screen_flips); with one
   buffer it has no other and draws into the one shown. */
#define N_SCREEN_BUFFERS_MAX 4u
struct n_screen_buffer {
    int32_t surface;
    uint32_t reserved;
    uint64_t offset;
};
struct n_screen_buffers {
    uint32_t width, height, pitch, format;
    uint32_t count, reserved;
    struct n_screen_buffer buffer[N_SCREEN_BUFFERS_MAX];
};
/* n_pci_screen_flips: show buffer of output from its next vertical blank on. serial counts the
   output's flips from 1; the driver says when the flip was shown (n_pci_screen_vblank). A newer
   flip of the same output replaces one not taken yet. */
struct n_screen_flip {
    uint32_t output, buffer;
    uint64_t serial;
};
/* n_pci_screen_vblank: a vertical blank of output. count is the driver's count of its blanks,
   time_ns the moment it began on the system's monotonic clock in nanoseconds (struct n_clock,
   n_clock_monotonic: the driver converts the display engine's time to it), buffer the buffer shown from
   it on and flip_serial the last flip that blank shows (0: none yet). A driver reports each blank
   that shows a flip as the display engine signals it, and the others while a flip waits. */
struct n_screen_vblank {
    uint32_t output, buffer;
    uint64_t count, time_ns, flip_serial;
};

/* A display mode: the visible pixels, the vertical refresh in millihertz (59940 is 59.94 Hz), the
   bits per channel and the pixel format (N_SCANOUT_*) the driver scans out in that mode (0: it does
   not say). N_SCREEN_MODE_PREFERRED marks the monitor's own preferred timing (its EDID's).
   N_SCREEN_MODE_VRR: the monitor shows the mode at a variable refresh, a frame as soon as it is
   ready, from refresh_mhz down to its n_screen_modes vrr_min_mhz. */
#define N_SCREEN_MODE_PREFERRED 1u
#define N_SCREEN_MODE_VRR 2u
struct n_screen_mode {
    uint32_t width, height;
    uint32_t refresh_mhz;
    uint32_t flags;
    uint32_t bits_per_channel;
    uint32_t format;
};
/* n_pci_screen_modes: the monitor a display driver drives and what it can show there. The display
   service chooses the mode (struct n_screen_state's mode); the driver says what it showed, each time
   it acted on one. monitor tells monitors apart (the same text for the same monitor every time:
   the EDID's maker, product and serial number), name is the monitor's for people (may be empty);
   both end with a zero. modes are the progressive modes the driver checked it can show, count of
   them, no two alike (width, height, refresh, format). shown is the mode the driver shows now, all zero
   when it shows none; done is the n_screen_state mode_serial it acted on last (0: none yet).
   vrr_min_mhz is the lowest refresh of the monitor's variable refresh range (its EDID's), 0
   without one. */
#define N_SCREEN_MODES_MAX 128u
struct n_screen_modes {
    char monitor[32];
    char name[32];
    uint64_t done;
    struct n_screen_mode shown;
    uint32_t count, vrr_min_mhz;
    struct n_screen_mode modes[N_SCREEN_MODES_MAX];
};

/* n_pci_screen_state: what an output's screen should show, for the driver whose buffers the
   screen is drawn into or whose monitor's modes the system has. serial changes with every change
   of what follows and is never 0.
   mode is the mode the display service wants the driver to show (one of the modes it gave, all zero
   while it wants none: the driver keeps what is shown); mode_serial changes with every wish, even for the same mode again (a monitor
   offered anew). The driver shows it at that size (n_pci_give_screen) and gives n_pci_screen_modes
   with done = mode_serial and what it shows then.
   For a driver whose buffers have N_SCANOUT_DIRECT: app is the app (n_app_self's number) whose
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

/* power。SYS_POWER は (N_CAP_POWER の cap, POWER_*)。カーネルは言われたとおりに実行する：POWER_OFF と
   POWER_REBOOT はアプリを止めて電源を変え、呼出しは戻らない。POWER_SLEEP と POWER_HIBERNATE は結果を
   SYS_POWER_STATE で言い、呼出しはすぐ戻る。窓に閉じるよう頼むか、いつするかは機械のサービスが決める。 */
#define POWER_OFF 0
#define POWER_REBOOT 1
#define POWER_SLEEP 2
#define POWER_HIBERNATE 3

/* 電源の段取り（SYS_POWER_STATE）。カーネルは型のある値を出し、人へ言うのは読む側。serial は変わるたびに
   増える。読むのは最新の一つで、途中の段が飛ぶことがある。N_POWER_CLOSING と N_POWER_CANCELLED は機械の
   サービスが SYS_POWER_SAY で言う。 */
enum n_power_stage {
    N_POWER_IDLE,
    N_POWER_CLOSING,   /* 窓が閉じるのを待つ。detail は残っている窓の題 */
    N_POWER_CANCELLED, /* detail の題の窓が残ったので取りやめた */
    N_POWER_STOPPING,  /* 引き返さない：止まる */
    N_POWER_SAVING,    /* 休止の像を C: へ書く */
    N_POWER_REFUSED,   /* 眠りと休止に入らなかった。reason と detail */
    N_POWER_FAILED,    /* 眠りと休止が途中で失敗した。reason と detail */
    N_POWER_DEVICE_LOST, /* 戻って来ない装置がある。reason と detail */
    N_POWER_AWAKE,     /* 眠りから戻った：時計が進んだ */
};
enum n_power_reason {
    N_POWER_REASON_NONE,
    N_POWER_REASON_IOMMU,         /* IOMMU の状態が確かめられないか、戻らなかった */
    N_POWER_REASON_NO_S3,         /* ファームウェアに S3 が無い */
    N_POWER_REASON_NO_WAKE_PAGE,  /* 1 MiB より下に起床の頁が無い */
    N_POWER_REASON_DEVICE,        /* detail の装置は眠りから戻れない */
    N_POWER_REASON_DEVICE_BUSY,   /* detail の装置が今は眠れない */
    N_POWER_REASON_DEVICE_STUCK,  /* detail の装置が止まらなかった */
    N_POWER_REASON_APP_DRIVER,    /* アプリが PCI の装置を動かしている */
    N_POWER_REASON_DISK_FLUSH,    /* ディスクが cache を書き戻さなかった */
    N_POWER_REASON_CPU_STOP,      /* CPU が止まらなかった */
    N_POWER_REASON_FIRMWARE,      /* ファームウェアが眠らなかった */
    N_POWER_REASON_LOW_ROOM,      /* 1 MiB より下に場所が無い */
    N_POWER_REASON_NOT_ON_DISK,   /* C: がディスクに無い */
    N_POWER_REASON_NO_MEMORY,     /* メモリーが足りない */
    N_POWER_REASON_NO_SPACE,      /* C: の空きが足りない */
    N_POWER_REASON_IMAGE_TOO_LARGE, /* 像がメモリーか C: に入らなかった */
    N_POWER_REASON_STORE_WRITE,   /* C: へ書けなかった */
    N_POWER_REASON_COUNT
};
struct n_power_state {
    uint64_t serial;
    uint32_t stage;         /* enum n_power_stage */
    uint32_t action;        /* POWER_* */
    uint32_t reason;        /* enum n_power_reason */
    uint32_t forceable;     /* N_POWER_CLOSING：もう一度選ぶと待たない */
    uint32_t can_sleep;     /* 眠りを勧めてよい */
    uint32_t can_hibernate; /* 休止を勧めてよい */
    char detail[64];
};

/* 電源の事象（SYS_POWER_EVENTS）。カーネルは数えて知らせるだけで、どうするかは機械のサービスが決める。
   数は起動からの累積で、減らない。機械を眠りから起こした一押しは数えない。watch（SYS_POWER_EVENTS_WATCH）は
   ボタン、蓋、起床、ホストの依頼で鳴る。last_input_ns は鳴らさない。時刻は monotonic の ns で、0 はまだ無い。 */
struct n_power_events {
    uint64_t button_presses;
    uint64_t lid_changes;
    uint64_t wakes;         /* 眠りと休止から戻った回数 */
    uint64_t host_asks;     /* VM のホストの依頼の回数。最後の依頼は host_action */
    uint64_t last_wake_ns;
    uint64_t last_input_ns; /* キーとポインターの最後の入力 */
    uint32_t host_action;   /* POWER_* */
    uint32_t lid_present;   /* 蓋の装置がある */
    uint32_t lid_closed;
    uint32_t can_sleep;     /* 機械が眠れる：ファームウェアと装置の事実 */
    uint32_t can_hibernate; /* 機械が休止できる：同じ */
    uint32_t reserved;
};

/* ---- GUI ---- */
enum {
    EV_NONE = 0, EV_KEY, EV_MOUSE_MOVE, EV_MOUSE_DOWN, EV_MOUSE_UP, EV_CLOSE, EV_FOCUS, EV_UNFOCUS,
    EV_WHEEL, EV_RESIZE, EV_FRAME,
    EV_DIRECT,     /* key 1: the window's own images are shown directly (win_direct); 0: no longer */
    EV_SURFACE,    /* buttons: the surfaces (bit i: surface i) given back to the app to draw into */
    /* Input for the window was lost: the mailbox held as many events as the system keeps for a
       window that does not take them. Keys and buttons held before it are to be taken as released;
       the next events are the ones that came after. */
    EV_INPUT_LOST,
    EV_MOUSE_LEAVE, /* 指が窓から離れた。シェルの層の窓だけが受ける */
    EV_HOTKEY,      /* usage と mods の押下（SYS_SHELL_HOTKEY） */
};

/* A window's event. time_ns is a moment on the system's monotonic clock in nanoseconds (struct
   n_clock, n_clock_monotonic): for input, when the device's report reached the system; for EV_FRAME, when the
   frame was shown (the vertical blank the display device measured, or the copy's end on a screen
   that has none). */
struct gui_event {
    uint32_t type;
    int32_t x, y;       /* mouse position in client coordinates / new size for EV_RESIZE */
    uint32_t key;       /* EV_KEY: 文字か NKEY_（input_abi.h）。カーネルは usage を渡し、n_key_translate が決める */
    uint32_t mods;      /* NMOD_* */
    uint32_t buttons;
    int32_t wheel;
    uint32_t pressed;
    uint64_t time_ns;
    uint64_t serial;    /* EV_FRAME: the number of the last present the frame showed */
    uint32_t usage;     /* EV_KEY, EV_HOTKEY: HID の usage（Keyboard page）。0 は文字だけの入力で、key が文字 */
    uint32_t layout;    /* EV_KEY, EV_HOTKEY: 打った装置の配列（enum n_key_layout） */
    uint32_t clicks;    /* EV_MOUSE_DOWN: 同じ場所で続けて押した回数（1 から）。間隔は N_INPUT_DOUBLE_CLICK_MS */
    uint32_t reserved;
};

/* window flags */
#define WIN_RESIZABLE 1
#define WIN_NO_DECOR 2
#define WIN_CENTER 4
#define WIN_START_MAXIMIZED 8
#define WIN_ALWAYS_ON_TOP 16
/* A window's pixels are surfaces: shared objects (N_KIND_SHARED) of width * height pixels of
   0xAARRGGBB (and the deep plane after them with WIN_DEEP_COLOR), which SYS_WIN_SURFACE hands out
   and the app maps (n_shared_map) or passes on to another app that draws for it. The compositor
   reads the same pages.
   A window without WIN_FRAME_QUEUE has one surface, which it keeps and draws into in part: each
   SYS_WIN_PRESENT names the rectangle it changed, and the system takes a copy of that rectangle
   (what is shown never has half of a change).
   A window with WIN_FRAME_QUEUE has SYS_WIN_CREATE's surface count of surfaces (2 to
   N_WIN_SURFACES_MAX; 0 is 2), each a whole frame: SYS_WIN_ACQUIRE gives a surface the app may
   draw into, SYS_WIN_PRESENT queues it whole, and the compositor reads it without a copy. The
   surface on the screen stays the compositor's until another is shown; then EV_SURFACE gives it
   back. Presents wait in turn, one shown a frame; with WIN_MAILBOX a present replaces the one
   still waiting, which EV_SURFACE gives back at once (a game that draws faster than the screen
   shows the newest frame). No writes are allowed to a surface between its present and its return. */
#define WIN_FRAME_QUEUE 32
#define WIN_MAILBOX 256
#define N_WIN_SURFACES_MAX 8
/* A window's client area is 1 to N_WIN_SIZE_MAX pixels each way (SYS_WIN_CREATE, SYS_WIN_RESIZE);
   it may be larger than the screens, which show what fits. */
#define N_WIN_SIZE_MAX 16384
/* EV_FRAME: a frame of the screen took what the window presented and was shown. y is the
   screen's frame period in nanoseconds (the next frame comes about that long after this one; on
   a variable-refresh screen, the shortest), time_ns when the frame was shown, serial the number
   of the last present it showed (SYS_WIN_PRESENT's result), key the surface it shows (a
   WIN_FRAME_QUEUE window's). One EV_FRAME answers any number of presents. Frames come at the
   screen's vertical blanks (else its mode's refresh), so an app that draws on EV_FRAME draws once
   a frame at any refresh rate. With WIN_FRAME_EVENTS, SYS_WIN_FRAME asks for the next one without
   presenting (an animation that waits for the display before it decides to draw). A window that
   is not shown gets it when it is shown again. A window on two screens is paced by the one under
   its centre.
   (WIN_DEEP_COLOR, 64, is in display_abi.h.) */
#define WIN_FRAME_EVENTS 128
#define WIN_STATE_VISIBLE 1
#define WIN_STATE_FOCUSED 2
#define WIN_STATE_FULLSCREEN 4
#define WIN_STATE_DIRECT 8 /* the display driver shows the window's own images (EV_DIRECT) */

/* The pointer over a window. The window manager owns the pointer: it draws it, keeps the
   art and hot spot of every shape, and decides what is shown. A window only declares what
   it wants over its own client area with SYS_WIN_POINTER; the declaration stays until the
   next one (a new window wants N_POINTER_DEFAULT). It is shown while the pointer is over
   the client area and nothing of the window manager's is there (a menu, a frame, the
   resize band, a drag of a window), and during a drag that started in the client area. The
   shapes are the CSS UI 4 cursor keywords, one for one, so a browser passes its keyword on:
   the art is the window manager's own. */
enum {
    N_POINTER_DEFAULT = 0,   /* the arrow */
    N_POINTER_NONE,          /* no pointer is drawn */
    N_POINTER_CONTEXT_MENU,
    N_POINTER_HELP,
    N_POINTER_POINTER,       /* a hand: a link */
    N_POINTER_PROGRESS,      /* the arrow, busy in the background */
    N_POINTER_WAIT,
    N_POINTER_CELL,
    N_POINTER_CROSSHAIR,
    N_POINTER_TEXT,
    N_POINTER_VERTICAL_TEXT,
    N_POINTER_ALIAS,
    N_POINTER_COPY,
    N_POINTER_MOVE,
    N_POINTER_NO_DROP,
    N_POINTER_NOT_ALLOWED,
    N_POINTER_GRAB,
    N_POINTER_GRABBING,
    N_POINTER_E_RESIZE, N_POINTER_N_RESIZE, N_POINTER_NE_RESIZE, N_POINTER_NW_RESIZE,
    N_POINTER_S_RESIZE, N_POINTER_SE_RESIZE, N_POINTER_SW_RESIZE, N_POINTER_W_RESIZE,
    N_POINTER_EW_RESIZE, N_POINTER_NS_RESIZE, N_POINTER_NESW_RESIZE, N_POINTER_NWSE_RESIZE,
    N_POINTER_COL_RESIZE,
    N_POINTER_ROW_RESIZE,
    N_POINTER_ALL_SCROLL,
    N_POINTER_ZOOM_IN,
    N_POINTER_ZOOM_OUT,
    N_POINTER_SHAPES          /* how many there are */
};
/* An image instead of a shape (a page's cursor: url(...)): width and height 1 to
   N_POINTER_IMAGE_MAX, pixels width*height of 0xAARRGGBB, not premultiplied, rows top first,
   the hot spot (hot_x < width, hot_y < height) is the pixel at the pointer's position. The
   window manager copies the image at the call. width, height and the hot spot all 0 mean no
   image: shape is shown. With an image, shape must be a shape all the same.
   The limit is the size every way of showing the pointer can show (N_SCREEN_POINTER_SIZE: the
   display driver's own pointer, the remote desktop's): a larger image is the caller's to refuse. */
#define N_POINTER_IMAGE_MAX N_SCREEN_POINTER_SIZE
struct n_win_pointer {
    uint32_t shape;              /* N_POINTER_* */
    uint32_t width, height;      /* 0: no image */
    uint32_t hot_x, hot_y;
    uint32_t reserved;           /* 0 */
    const uint32_t *pixels;
};

struct screen_info {
    uint32_t width, height;
};

/* networking */
/* An adapter. It keeps its id for as long as the system runs, also unplugged (present 0). ip and
   mask are in host byte order (a.b.c.d is a<<24 | b<<16 | c<<8 | d); ip 0: no address yet. */
struct n_net_adapter {
    uint32_t id;
    uint8_t present;
    uint8_t reserved;
    uint8_t mac[6];
    uint32_t ip, mask;
    uint64_t rx_packets, tx_packets;
};
/* The route to destination & mask: by adapter, to gateway (0: on the link). The longest mask goes
   first, then the lowest metric. An adapter's own network needs no route. */
struct n_net_route {
    uint32_t destination, mask, gateway;
    uint32_t adapter;
    uint32_t metric;
};
#define N_TCP_SEND_NONBLOCK 1u /* SYS_TCP_SEND_UNTIL final flags argument */
struct n_netstats {
    uint64_t rx_dropped, rx_invalid, rx_budget_yields;
    uint64_t rx_completion_pending, rx_completion_retries, rx_completion_cancelled, rx_ring_high_water;
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
/* DNS representation bounds, not hostname quotas. RFC 1035 2.3.4: labels <=63 octets, name including
   label lengths/root <=255 wire octets. The longest unescaped textual form has 254 bytes including
   its final dot. */
#define DNS_NAME_WIRE_BYTES 255u
#define DNS_NAME_TEXT_BYTES 254u

struct n_sockaddr {
    uint32_t ip; /* host byte order: a.b.c.d is a<<24 | b<<16 | c<<8 | d */
    uint16_t port;
};
