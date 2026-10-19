# Nocturne OS

*A small operating system for quiet nights.*

[日本語](README.md)

Nocturne is a hobby x86-64 operating system written from scratch: a kernel, a compositing window manager,
a TCP/IP stack, a C library, a web browser engine, a shell, a terminal and about 55 programs.
It was written with AI coding assistants (Claude Code and others) over many long sessions.

**Nocturne is not UNIX and not POSIX.** It is a GUI-first operating system of its own: programs use the system
through Nocturne's native API (handles to documents, folders, channels, apps and events). There is no `fork`,
no file descriptors, no signals and no `unistd.h`. Programs from other systems are ported by moving the program
to the native API, not by bending Nocturne to fit them.

It mostly runs in virtual machines (QEMU and Hyper-V), but it also boots on real hardware from a USB stick,
with the desktop, the browser, a USB keyboard and mouse and wired networking working. It is a toy, not a
general-purpose OS for daily use (see [Limits](#limits)).

![Desktop and start menu](docs/desktop-menu.png)

| | |
|---|---|
| ![Terminal](docs/terminal.png) | ![Text editor](docs/editor.png) |
| ![Paint](docs/paint.png) | ![System monitor](docs/sysmon.png) |
| ![Mandelbrot](docs/mandelbrot.png) | ![3D shapes](docs/shapes.png) |

## Highlights

- **Kernel**: boots from BIOS or UEFI through Limine. User threads on several CPUs, the "Kei" (継) fair scheduler,
  W^X memory management and a native system-call ABI.
- **Desktop**: a compositing window manager in the kernel, with shadows, double buffering, a taskbar, a start menu,
  notifications, a clipboard and a night-sky wallpaper. The default resolution is 1920×1080.
- **Web browser**: Lexbor builds the HTML tree and QuickJS runs JavaScript; the DOM, CSS, layout, painting and
  Web APIs are Nocturne's own. It has Workers, IndexedDB, WebSocket, WebGL (Mesa software rendering),
  WebAssembly, video (FFmpeg, MSE) and developer tools.
- **Networking**: its own TCP/IP stack (TCP connect and listen, window scaling, loopback), DHCP, DNS,
  HTTP/1.1 and HTTP/2, and TLS 1.2 through BearSSL.
- **Development**: TinyCC compiles C inside the OS; `js` runs JavaScript and TypeScript; `tsgo` is the
  TypeScript 7 compiler; Go is ported (`GOOS=nocturne`); C++20 is cross-compiled from the host.
- **AI agent**: `agent` connects to an LLM over HTTPS and, inside the OS, runs commands, writes and compiles C,
  and looks at the screen to check the result.
- **Hyper-V**: the synthetic devices of a Generation 2 VM, the integration services and the Enhanced Session
  (an RDP server).

## What's inside

### Kernel (`kernel/`, about 29,000 lines of C and assembly)

- **Boot**: Limine (BIOS or UEFI) into a 64-bit higher-half kernel. The boot menu has three entries: the default
  1920×1080, 1024×768, and text console only (`nogui`).
- **CPUs**: up to 16. CPU 0 (the BSP) owns the kernel, interrupts, drivers, the GUI and the network stack; every
  other CPU (AP) runs app threads from its own run queue.
  - The "Kei" scheduler (`sys/kei.c`, `sys/sched.c`): fair selection by weighted virtual time, one queue per AP,
    idle APs taking waiting threads, and keeping a thread when a switch costs more than it gains.
  - TLB shootdown by IPI, PCID, AVX state saved with XSAVE, top performance set through CPPC or HWP on real
    hardware, and a sampling profiler (`prof`).
  - The clock is a 1 kHz local APIC timer (or the PIT); ticks are derived from the TSC or Hyper-V's reference clock.
- **Memory**: 4-level paging, an address space for every app, and a kernel heap.
  - W^X: no page is ever both writable and executable. Generated code is written first and then made executable.
  - The native memory API (`n_memory_reserve`, `protect`, `release`) and shared pages. Shared pages reach another
    app only as an attachment to a channel message.
  - The initrd is unpacked into the RAM root, and the archive's pages are then given back.
- **Files**: the root is in RAM (built from the initrd and lost on reboot). `/data` is a FAT32 volume named
  `NOCTDATA` that survives reboots; it is found on MBR, GPT or unpartitioned disks.
- **Drivers**
  - Storage: ATA (PIO), NVMe, USB mass storage (BOT + SCSI) and Hyper-V SCSI.
  - USB: xHCI, USB 2.0 and SuperSpeed hubs, HID keyboards and mice.
  - Input: PS/2 keyboard and wheel mouse, Hyper-V synthetic keyboard and absolute mouse, USB HID.
  - Network: Intel e1000, DEC 21140/21143 (tulip), Hyper-V's synthetic adapter, Realtek RTL8125B (2.5GbE) and
    USB Ethernet (CDC ECM/NCM).
  - Display: the framebuffer Limine hands over. virtio-gpu (virgl) is experimental and off by default (`gpu-enable`).
  - Sound: Intel AC'97, and sound sent to the Enhanced Session.
  - Also: ACPI tables (power-off and reboot), PCI and PCIe ECAM, MSI/MSI-X, PAT, the CMOS clock, serial and entropy.
- **GUI**: the window manager runs as a kernel thread. That keeps it simple and fast, but a bug in it brings the
  whole system down.
  - Window pixels are memory shared with the app, and only changed areas are redrawn. Compositing and presenting
    to the screen are also split across the APs.
  - Double-buffered windows with a frame-completion event (`EV_FRAME`), and a configurable compositing period
    (`wmrefresh`, 120 Hz by default).
  - 8 bpc and 10 bpc colour: when the firmware's display is 10 bpc (2:10:10:10), the compositor keeps a 10-bit deep
    plane and windows can draw 10-bit pixels with `WIN_DEEP_COLOR`. On an 8 bpc display the wallpaper and 10-bit
    video are ordered-dithered to 8 bits, so gradients do not band. Adding `fbsim10` to `cmdline` exercises the
    10 bpc path on an 8 bpc display.
  - Shadows, rounded title bars, move, resize, maximize, minimize and full screen.
  - A taskbar (start button, window buttons, volume, date and time), a start menu, notifications and a clipboard
    of up to 1 MiB.
  - Kernel text uses GNU Unifont Japanese 18.0.01 bitmaps (57,086 glyphs, 8×16 half-width and 16×16 full-width).
- **Hyper-V**: VMBus and the synthetic devices, heartbeat, shutdown and restart from Hyper-V Manager, time sync,
  KVP (Hyper-V Manager shows the VM's IP address and OS), Hyper-V sockets, and the RDP server on them for the
  Enhanced Session.

### The native API

This is Nocturne as a program sees it (`user/include/nocturne.h`, `common/abi.h`; the design is in
[docs/nocturne-native-abi.md](docs/nocturne-native-abi.md)).

- **Handles**: every resource is a typed handle (document, folder, channel, window, shared pages, app, events,
  device, connection). Failures return a negative `N_ERR_*`, and `n_result_text` describes it.
- **Documents and folders** are always named by a complete path starting with `/`. The kernel has no working folder
  and no file position. The shell's current folder reaches apps as the setting `FOLDER=`. There are whole-document
  load, atomic save and append, reads and writes at a given offset, and exclusive claims.
- **Channels**: two-way message passing, in place of pipes. A message can carry a document or shared-page handle.
  Channels can also be listened for and connected to by name.
- **Apps**: `n_app_launch` starts an app with its arguments, settings and handed-over handles, `n_app_result` gets
  its result and `n_app_stop` stops it. There are no signals. The terminal is the setting `TERMINAL=0 1 2`, and
  Ctrl+C is a message on the `INTERRUPTS=3` channel.
- **Events**: level-triggered event queues that wait for receivable, sendable, closed, app ended, timer and address.
- **Threads**: native threads (`thread_create` and friends) and `wait_on_address`, which works across apps.
  C11 `<threads.h>` is there too.
- **Also**: audio streams (`n_audio_*`), windows (`win_open` and friends), clocks, random numbers, system
  information, and TCP, UDP and DNS (see [Networking](#networking)).

An ISO C library (`stdio`, `stdlib`, `string`, `math`, `time`, `threads` and so on) sits on top. POSIX headers
(`unistd.h`, `fcntl.h`, `sys/*.h`) are deliberately not provided.

### Userland (`user/`)

- **libc**: the native API and part of ISO C11 (`printf` with correctly rounded floating point, `strtod`, `qsort`,
  `setjmp` and more).
  - An HTTP/1.1 and HTTP/2 client with HTTPS, a JSON parser and builder, and a PNG encoder.
  - Drawing (`gfx.h`), TrueType (`font.h`), image loading (`image.h`), audio and video decoding through FFmpeg
    (`media.h`) and parallel work (`parallel.h`).
  - The web engine (`user/libc/web/`, about 54,000 lines of C and 42,000 of JavaScript). See [The web browser](#the-web-browser).
- **Shell** (`sh`)
  - Pipes (channels), redirection (`< > >> 2>`), `&`, `;`, `&&`, `||`, quoting, `*` and `?` globbing, `~` and `#` comments.
  - `$?` and script arguments (`$0` to `$9`, `$#`, `$@`). There are no variables, `if` or loops.
  - History, tab completion, and `cd`, `pwd`, `history`, `help`.
- **Terminal**: 16 ANSI colours, UTF-8, 1000 lines of scrollback, copying the screen and pasting, and resizing.
  Full-width characters take one cell; two-cell wide characters are not handled yet.
- **Commands**: `ls cat cp mv rm mkdir rmdir touch tree grep wc head hexdump echo sleep date uptime uname
  ps kill free dmesg lspci clear true false reboot poweroff neofetch fortune moonsay screenshot`
- **Network**: `ifconfig`, `ping`, `host` (DNS) and `fetch` (HTTP and HTTPS).
- **Sound**: `beep` (tones and melodies), `play` (WAV) and `volume`, for scripts; everything they do can also be
  done from the desktop.
- **Development**: `tcc`, `js`, `tsgo`, `wasm2c`, `cppdemo`. The source of every app in `/bin` is in `/usr/src/apps`.
- **AI**: `agent` (see [The AI agent](#the-ai-agent)).
- **Desktop apps**: Terminal, Files, Web Browser, Text Editor, Paint, Calculator, Clock, System Monitor, Piano,
  Sound Player, Media Player and About.
- **Games and toys**: Snake, Tetris, Minesweeper (all three with sound effects), Mandelbrot, 3D Shapes and Game of Life.

## Running it

### QEMU

After [building](#building), start QEMU with:

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 run
```

`build.ps1 run` (`make run`) uses 2048 MiB, 4 CPUs and TCG (emulation) by default. Change them with variables,
for example `make run QEMU_ACCEL=whpx QEMU_MEMORY=4096M`. Sound is an AC'97 card and the network is QEMU's
default e1000 with user-mode NAT.

Under WHPX, a boot disk on IDE makes Limine read the roughly 450 MB initrd through IDE PIO, and the kernel takes
about 3 minutes to start. With the boot disk on AHCI it takes about 8 seconds. This is the setup used for development:

```powershell
& "C:\Program Files\qemu\qemu-system-x86_64.exe" -M pc -accel whpx -smp 4 -m 4096M -vga std `
  -device ahci,id=ahci `
  -drive id=boot,file=build/nocturne.img,format=raw,if=none,snapshot=on `
  -device ide-hd,drive=boot,bus=ahci.0,bootindex=0 `
  -drive file=build/data.img,format=raw,if=ide,index=1 `
  -audiodev dsound,id=snd -device AC97,audiodev=snd -serial stdio
```

The kernel has no AHCI driver, so keep the `/data` disk (`data.img`) on IDE.

### Hyper-V

You need Windows with Hyper-V enabled. Run this once from an **elevated** (administrator) PowerShell in this folder:

```powershell
powershell -ExecutionPolicy Bypass -File hyperv.ps1 -MemoryMB 4096
```

It creates a **Generation 2** VM called `Nocturne`, starts it and opens VMConnect. The settings:
- Static memory (512 MB by default) and 1 CPU. The boot image unpacks an initrd of about 450 MB into RAM, so raise
  it with `-MemoryMB` (development testing uses 4096 MiB). The CPU count can be raised in Hyper-V Manager, and the
  kernel uses the added CPUs.
- Secure Boot off (the boot loader is not signed). No checkpoints.
- The boot disk (`hyperv\<Name>-boot.vhdx`) on SCSI 0:0 and the [data disk](#persistent-storage-data)
  (`hyperv\<Name>-data.vhdx`, 2 GB dynamic) on SCSI 0:1.
- A network adapter on the **Default Switch**, so the VM gets an address by DHCP and can reach the internet.
- The Enhanced Session over Hyper-V sockets. If the host has Enhanced Session Mode turned off, the script turns it on.
- COM1 connected to `\\.\pipe\<lowercase name>-com1`.

**Enhanced Session.** VMConnect asks for a display size once Nocturne is up. VMConnect connects to the RDP server
inside Nocturne, and the desktop takes the size you pick; when you resize the window, the desktop follows. Keyboard
and mouse (with no mouse capture), clipboard text (text copied in Windows pastes with Ctrl+V in Nocturne's editor
and address bar, or Ctrl+Shift+V in the terminal; the other way with Ctrl+C, or Ctrl+Shift+C for the terminal's
screen) and sound (when VMConnect's **Local Resources > Remote audio** is "Play on this computer", its default) all
go through the same connection. Choose **View > Enhanced Session** to go back to the basic console, which shows the
desktop at the framebuffer's size.

Other options:

| Command | What it does |
|---|---|
| `hyperv.ps1 -Update` | Copy a freshly built boot disk into the existing VM and restart it. The data disk is kept. Refuses media older than kernel.elf or initrd.tar. |
| `hyperv.ps1 -Generation 1` | Create a Generation 1 VM instead (see below). |
| `hyperv.ps1 -Iso` | Boot from `build\nocturne.iso` on the DVD drive instead of the disk. |
| `hyperv.ps1 -Remove` | Delete the VM. The data disk is deleted only with `-DeleteData`. |
| `hyperv.ps1 -NoStart` | Create the VM without starting it. |
| `hyperv.ps1 -NoNetwork` | Create the VM without a network adapter. |
| `hyperv.ps1 -Switch "Name"` | Connect to a different virtual switch. |
| `hyperv.ps1 -Name Foo -MemoryMB 4096` | Use a different VM name and memory size. |
| `hyperv.ps1 -Update -ApiKeyFile KEYFILE` | Write the AI agent's API key to the data disk (`-Endpoint` and `-Model` too). |

To create a Gen 2 VM by hand, turn Secure Boot off, attach `build\nocturne.vhdx` as a SCSI disk, and set
`Set-VM -EnhancedSessionTransportType HvSocket` for the Enhanced Session.

**Generation 1** emulates a legacy PC: `build\nocturne.vhd` on IDE, the data disk on IDE 0:1 and a *legacy*
network adapter (an emulated DEC 21140). VMConnect shows the basic console only.

**Tips for VMConnect**
- In the basic console, click inside the window to capture the mouse. **Ctrl+Alt+Left Arrow** releases it.
- The Windows key only reaches the VM in full-screen mode. Use **Ctrl+Esc** to open the start menu instead.
- The boot menu has a 1024×768 entry and a text-console-only entry in case 1920×1080 isn't available. Without
  1920×1080, Limine picks a mode the firmware offers.
- The kernel log goes to COM1. Any named-pipe client, such as PuTTY (Serial, `\\.\pipe\nocturne-com1`), shows it.
  `scripts\hv-serial.ps1` copies it to a file.
- **Mouse moving up when you move down?** On Gen 1, Hyper-V on Windows 11 hosts reports the emulated PS/2 mouse's
  vertical axis backwards. Nocturne detects Hyper-V (and the host build) and flips the axis itself. If it guesses
  wrong, press **E** on the boot menu entry and add `mouse_y=normal` (or `mouse_y=invert`) to `cmdline`.

**Development scripts** (`scripts\`, elevated):

| Script | What it does |
|---|---|
| `hv-boot.ps1 -Name VM -Build` | Build, refresh the VM's boot media, boot, record COM1 and take a console screenshot. |
| `hv-esm.ps1 -Name VM` | Open an Enhanced Session in VMConnect and save a picture of the window. |
| `python hv-rdp.py --vm VM --size 1280x720 "type:neofetch\n" dclick:53,130` | Test the Enhanced Session without VMConnect: a small RDP client types and clicks, and saves the desktop it receives as a PNG. `clip:TEXT` and `chord:1d,2a,2f` test the clipboard, `resize:1600x900` a window resize. The sound it receives is saved as a WAV next to the PNG, with the tones in it listed. |
| `hv-input.ps1 -Name VM -Steps "text:...","click:1"` | Send keyboard and mouse input to the basic console. |
| `hv-shot.ps1 -Name VM` | Screenshot the VM's console without VMConnect. |
| `hv-sock.ps1 -Name VM -Port N` | Connect from the host to a Hyper-V socket service in the guest. |
| `hv-serial.ps1 -Pipe P -Log F` | Copy a COM port pipe to a log file. |

### Real hardware

Write `build\nocturne.img` (an MBR disk with FAT32 that boots from both BIOS and UEFI) to a USB stick, turn
Secure Boot off and boot it. On a 12-thread AMD machine (booted from USB, RTL8125B wired LAN, USB keyboard and
mouse) it reached the desktop and showed a video page in the browser. This is still early:
- There is no step-by-step guide and no list of supported machines.
- There is no sound (Intel HD Audio is not supported), and no GPU driver; the firmware's display mode is used.
- To keep `/data`, some disk (a USB stick, NVMe and so on) needs a FAT32 volume named `NOCTDATA`. Without one,
  `/data` lives in RAM and is lost on reboot.

## Building

The host is Windows. The toolchain lives in `tools/`, which is not part of the repository (it is in `.gitignore`):
- `tools/msys64`: MSYS2 (UCRT64) with clang 22, lld, nasm, mtools, xorriso, Python, make and QEMU.
- `tools/limine`: the Limine binaries.
- `tools/mesa-tools`: bison, flex and m4, for regenerating Mesa's parsers.
- Host LLVM 18.1.8 (`C:\Program Files\LLVM\bin`, or set `NOCTURNE_LLVM_BIN`) for C++ apps and the WebAssembly
  parts. libc++ is extracted from llvm-project at `llvmorg-18.1.8` (`NOCTURNE_LLVM_SOURCE`) and cached in `build/cxx`.

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1          # build everything into .\build
powershell -ExecutionPolicy Bypass -File build.ps1 run      # build and boot in QEMU
powershell -ExecutionPolicy Bypass -File build.ps1 clean    # delete build (keeps data.img)
python -X utf8 scripts/build.py -j 8 cxx                    # any Makefile target (cxx, bitmap-fonts, ...)
```

Outputs in `build\`:
- `kernel.elf`, `initrd.tar`: the kernel and the contents of the RAM root.
- `nocturne.img`: raw MBR boot disk (QEMU, USB sticks).
- `nocturne.vhd`: fixed VHD for Hyper-V Generation 1.
- `nocturne.vhdx`: dynamic VHDX for Hyper-V Generation 2.
- `nocturne.iso`: hybrid BIOS/UEFI ISO.
- `data.img`: the `/data` disk for QEMU (512 MB). Created on the first build and never overwritten.
- `data-blank.vhdx`: an empty data disk for Hyper-V (2 GB).

**Optional components** (the OS builds without them):
- **Go** ([ports/go](ports/go/README.md)): build Go 1.27.2 with `ports/go/nocturne.patch` applied, then
  `make GO=<Go source>/bin/go.exe` puts `ports/go/apps/*` into `/bin`.
- **TypeScript-Go** ([ports/tsgo](ports/tsgo/README.md)): add `TSGO=<TypeScript-Go source>` to get `/bin/tsgo`.
  `js file.ts` uses it.
- **virgl** ([ports/virgl](ports/virgl/README.md)): only for trying virtio-gpu in the host's QEMU (`QEMU_GPU=virgl`).

## Tests

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 test        # the full suite (takes a while)
powershell -ExecutionPolicy Bypass -File build.ps1 test-quick  # skips compiling every app and tcc rebuilding itself
powershell -ExecutionPolicy Bypass -File build.ps1 test-full   # adds tcc rebuilding itself inside the OS
```

`scripts/test.py` boots the image headless in QEMU with a throwaway data disk holding `tests/`; your own
`data.img` is not touched. The in-OS `tcc` compiles the test runner (`tests/runtests.c`), which runs about 220
tests in about 100 groups and checks each one's exit status and output. Results come back over the serial port,
and afterwards the host checks the FAT32 volume the OS wrote with `fatcheck.py`. The tests cover:
- The shell and commands, `malloc`, the native ABI, threads and parallel work.
- W^X: writing code or read-only data, and running the stack or heap, must stop the app.
- FAT32 and the RAM filesystem: many sizes, long names, 150 entries, rename and nested folders.
- `tcc`: printf and math, compiling every app, and self-hosting. C++.
- The GUI: a window appears on screen and is gone after it closes.
- Sound: streams, waiting and non-waiting writes, eight streams at once, the volume, `beep`, and `play` with many
  WAV formats. QEMU records the AC'97 output to `build/test-audio.wav`, and the host checks each tone's pitch,
  channel and length. Media decoding is tested too.
- The web engine: element positions, painted pixels, CSS, forms, charsets, URLs, Workers, IndexedDB, WebAssembly,
  and parts of WPT and html5lib. Broken and deeply nested pages must not crash it.
- Network: DHCP, ping, DNS, HTTP, HTTPS, a rejected bad certificate, HTTP/2 and TLS resumption.
- TCP: 14 MiB to and from a host-side server, with simulated loss and reordering, checking every byte.
- The agent, offline: it never needs an API key.

The network tests need internet access from the host. Without it, use `python scripts/test.py --no-net`.
`--accel whpx`, `--cpus`, `--memory` and group names (`python scripts/test.py web tcp`) can also be given.

`scripts/qtest.py` drives a running image with keystrokes, mouse moves and screenshots:

```
python scripts/qtest.py sleep:6 "type:neofetch\n" shot:neofetch goto:51,125 dclick shot:files
```

## Using it

| Keys | Action |
|---|---|
| Super or Ctrl+Esc | Start menu (arrow keys and Enter work in it) |
| Ctrl+Alt+T | New terminal |
| Alt+Tab | Send the front window to the back |
| Alt+F4 | Close the window |
| Double-click a title bar | Maximize or restore |
| Drag the bottom-right corner | Resize (Terminal, Files, Text Editor, Clock, Mandelbrot, 3D Shapes, Web Browser) |
| Ctrl+Shift+C / Ctrl+Shift+V | Copy the screen / paste in the terminal |
| Shift+PgUp / PgDn, mouse wheel | Terminal scrollback |
| Ctrl+C | Stop the running command |

The root filesystem lives in RAM, so files you create there are lost when you reboot. Save anything you want to
keep under `/data`. `/home` has a few sample files; try `sh hello.sh`. `/home/JavaScript` has samples for `js`.

### Persistent storage: /data

Everything outside `/data` lives in RAM and is reset on every boot. `/data` is a FAT32 volume named `NOCTDATA`,
so files saved there survive reboots and rebuilds.
- **QEMU**: `build\data.img` (512 MB), created on the first build and never overwritten. `build.ps1 clean` leaves
  it alone. Delete it by hand to start fresh.
- **Hyper-V**: `hyperv.ps1` attaches `hyperv\<Name>-data.vhdx` (2 GB, dynamic) as the second disk. `-Update`
  replaces only the boot disk.
- **Real hardware**: a FAT32 volume named `NOCTDATA` on any disk.

Apps are looked up in `/bin` and then `/data/bin`, so programs installed there run by name.

## Sound

- **Volume:** click the speaker on the taskbar, next to the clock. Drag the slider to change it (a short tick plays
  at the new level), or click the speaker inside the panel to mute. Scrolling over the taskbar's speaker changes the
  volume too. The panel also says where the sound goes (the sound card, or Remote desktop in an Enhanced Session).
- **Sound Player:** double-click a `.wav` file in Files, or open Sound Player from the desktop or the start menu.
  It lists the other sounds in the same folder and plays on through them. It has play/pause, stop, previous, next,
  and a bar you can click or drag to jump. Space plays or pauses, Left and Right jump 5 seconds, Up and Down change
  sound. It reads PCM WAV (8/16/24/32-bit or float, mono to multichannel, any rate).
- **Media Player:** plays MP3, FLAC, AAC and Ogg (Vorbis, Opus) audio and MP4, WebM, AVI and MOV video (H.264,
  HEVC, VP8, VP9 and more) through FFmpeg. Double-clicking audio or video in Files opens it. Space plays or pauses,
  Left and Right jump 5 seconds, and Ctrl+O opens a file or URL.
- **Music:** `/home/Music` has three short pieces made for Nocturne (synthesized at build time by `scripts/mksounds.py`).
- **Piano** plays two octaves from the mouse or the keyboard (Z S X D C V G B H N J M for the lower octave,
  Q 2 W 3 E R 5 T 6 Y 7 U I for the upper), in three sounds (Tab), with Left and Right to change octave.
- Snake, Tetris and Minesweeper have sound effects.

Sound comes out of an Intel AC'97 card: `build.ps1 run` gives QEMU one, played through Windows' speakers.
Hyper-V has no sound card, so the Enhanced Session carries sound to VMConnect; outside an Enhanced Session, sound
is discarded at the speed it would play (so programs still take as long as their sound does).

**For programs:** open a stream with `n_audio_open` and send 48 kHz stereo 16-bit frames with `n_audio_submit`.
It waits (`N_AUDIO_WAIT`) while about 170 ms are queued, and `n_audio_status` tells how much has played, for keeping
a picture in step with the sound. Up to eight streams play at once. For games, `sound_effect(hz, hz2, ms, wave)`
plays a short tone or noise without ever waiting, and `wav_open`/`wav_read`/`wav_seek` read any WAV file as 48 kHz
stereo (`nocturne.h`). Scripts can use `beep`, `play` and `volume`.

## Networking

![Network tools](docs/network.png)

The network comes up by itself: a DHCP client runs in the kernel at boot, so `ifconfig` normally shows an address
a second or two after the desktop appears.

- **QEMU**: the default e1000 card with user-mode NAT works as-is. `-nic user,model=tulip` exercises the tulip
  driver (Hyper-V Generation 1's legacy adapter).
- **Hyper-V**: Generation 2 uses the synthetic (VMBus) adapter, Generation 1 a legacy one.

```
ping -c 3 example.com
host example.com
fetch -i http://example.com
fetch -o /home/1mb.zip http://speedtest.tele2.net/1MB.zip
```

- **TCP/IP** (`kernel/src/net/`): Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS and TCP.
  - TCP connects and listens (RFC 9293), with window scaling (RFC 7323, 1 MiB send and receive buffers by default),
    NewReno congestion control, a retransmission timeout from the measured round-trip time, and reassembly of
    out-of-order segments.
  - Loopback for `127.0.0.1` and the machine's own address.
  - DNS has a cache, retries over TCP when a reply is truncated, and returns every address.
- **For programs:** Nocturne's own API, not sockets: `tcp_connect`, `tcp_send`, `tcp_recv`; `tcp_listen` and
  `tcp_accept` for servers; `udp_socket` and friends for UDP; `net_resolve_all` for names (`nocturne.h`).
  TCP and TLS streams and an HTTP client sit on top (`http.h`).
- **HTTP:** HTTP/1.1 (persistent connections, chunked, gzip) and HTTP/2 (ALPN, HPACK, shared connections).
  Connections are pooled.
- **TLS:** TLS 1.2 through BearSSL. Certificates are checked against the Mozilla root store compiled into libc,
  and the CMOS clock supplies the time for the validity check. Sessions can be resumed. Go programs use Go's own
  TLS (up to 1.3).

Limits: no IPv6, no IP fragmentation, no TCP SACK or timestamps. One network adapter is used.

## The web browser

![The web browser showing Wikipedia](docs/browser.png)

Open **Web Browser** from the desktop or the start menu, or run `browser URL` (or `browser /path/file.html`).
Files opens `.html` files in it. It starts on a page with a search box and links. Each window is one process;
there are no tabs, you open more windows instead (`window.open` also makes a new window).

**How it works**
- Lexbor 3.1.0 tokenizes HTML and builds the tree, and QuickJS (2026-06-04) runs JavaScript. There is a single DOM,
  Nocturne's native one; the CSS cascade, box generation and layout, painting, hit testing and the Web APIs are
  Nocturne's own.
- A separate process, `webfetch`, does the networking (DNS, TCP, TLS, HTTP/1.1 and HTTP/2, cookies, CORS,
  WebSocket, cache). Workers, video decoding, MSE and IndexedDB I/O also run in their own processes.
- Style, layout subtrees and raster tiles are processed on several CPUs.

**HTML and CSS**
- block, inline, inline-block, float, table, flexbox, grid, `display: contents`, relative, absolute and fixed
  positioning, `::before` and `::after`.
- Selectors (`:has()`, `:is()`, `:not()`, `:nth-child()` and more), `@media`, `@supports`, `@layer`, custom
  properties, `calc()`, CSSOM and constructable stylesheets.
- Backgrounds, linear and radial gradients, rounded borders, shadows, `outline`, CSS masks, 2D transforms
  (`transform`, `rotate`, `scale`, `translate`), and `@keyframes` and transitions (for transforms and opacity).
- PNG, JPEG, GIF, WebP, SVG, ICO and BMP images.
- Forms (text fields, checkboxes, radio buttons, selects, date/time and number inputs, validation, file picking),
  find in page, Shadow DOM and custom elements.
- Text: the default serif is Noto Serif, sans-serif is Inter, and monospace and the UI use Maple Mono NF. Japanese
  falls back to Noto Sans/Serif CJK JP and Plangothic, and emoji are drawn with Noto Color Emoji (COLRv1, with ZWJ
  sequences and flags). Web fonts (`@font-face`) are read by FreeType: TrueType, OpenType CFF, WOFF, WOFF2 and
  uncompressed EOT.

**JavaScript and Web APIs**
- DOM, events, the observers, Range and Selection, history, URL, Blob, FormData, Fetch and XHR, Streams and WebSocket.
- Dedicated Workers and SharedWorker (shared across windows), MessageChannel and BroadcastChannel.
- Cookies (SameSite, the Public Suffix List), localStorage and sessionStorage, IndexedDB and OPFS.
- Canvas 2D, OffscreenCanvas, WebGL and WebGL2 (Mesa's GLSL compiler and softpipe, rendering in software).
- WebAssembly: validated by WABT, with wasm2c's C output compiled to native code by TCC inside the OS. There is an
  x86-64 JIT, an interpreter, JSPI, shared memory and Atomics, and threads.
- `<video>` and `<audio>` (a subset of FFmpeg 9.0.2's decoders), MSE, HLS and DASH, WebVTT captions, full screen
  and picture-in-picture.
- Web Crypto (hashes and AES), Intl (English and Japanese), Trusted Types and CSP.

**Developer tools**: F12 (or Ctrl+Shift+I) opens the Elements (DOM tree, styles, computed values, box model, picking
on the page), Console and Network panels. `browser --log-console`, `--net-log` and `--debug-js` send the console
and network log to the system log (serial).

| Keys | Action |
|---|---|
| Ctrl+L | Address bar (words without a dot search DuckDuckGo Lite) |
| Ctrl+F | Find in page (Enter for the next match) |
| Alt+Left / Backspace, Alt+Right | Back, forward |
| F5 or Ctrl+R | Reload |
| Esc | Stop loading (and leave full screen) |
| F12, Ctrl+Shift+I / J / C | Developer tools (Elements / Console / pick an element) |
| Space, PgUp/PgDn, arrows, wheel | Scroll |

**Real sites**: examples checked in QEMU (WHPX, 4 CPUs, 4096 MiB, 1920×1080). Each is a record of that day,
not a guarantee.
- 2026-10-13: DuckDuckGo search and result links, Reddit search, GitHub's menus, Baidu Baike pages.
- 2026-10-14: Baidu Baike text, images, search and back/forward. YouTube played, paused, seeked and scrolled, but
  stalls and overlapping captions remained. CNN's front page showed, and its video page stayed black
  ([record](docs/browser-wasm-delivery-20261014.md), in Japanese).
- 2026-10-16: Puter's Firefox (WebAssembly) started with its default GPU rendering.

**Not there yet**: TLS 1.3, Service Workers, WebRTC, Web Audio, WebGPU, module Workers, `position: sticky`,
3D transforms, many properties such as `clip-path`, full OpenType shaping and variable fonts, AV1 and HDR, RSA and
elliptic-curve signatures, and download and bookmark UI. Layout can take seconds on heavy pages. The gaps are listed
in [docs/browser-missing-20261012.md](docs/browser-missing-20261012.md) (in Japanese; some have been filled since).

## Development

### Writing C inside Nocturne

`tcc` is TinyCC 0.9.28rc, built for Nocturne:

```
tcc -run /usr/src/apps/mandel.c           # compile in memory and run
tcc -o /data/bin/hello hello.c            # build a program; it now runs as `hello`
```

Programs are linked statically against Nocturne's libc. They can use:
- `nocturne.h`: the native API, windows and a tiny UI kit.
- `gfx.h` (drawing), `font.h` (TrueType), `image.h` (images), `media.h` (audio and video), `gpu.h`.
- `threads.h`, `parallel.h`.
- `http.h` (HTTP and HTTPS), `json.h`, `png.h` and BearSSL.

tcc can also compile itself: its own source rebuilds inside the OS into a working compiler (`build.ps1 test-full`).

### C++

The host's clang cross-compiles C++20 apps for Nocturne. Put them in `user/apps/*.cpp` and build with
`python -X utf8 scripts/build.py cxx`; they can call the C APIs directly. Run `cppdemo` inside the OS for an example
using libc++ strings and containers together with the C GUI API. Exceptions, RTTI, iostream, threads and some other
parts are not available. See [docs/cpp-support.md](docs/cpp-support.md) (in Japanese).

### Go

[ports/go](ports/go/README.md) ports Go to `GOOS=nocturne GOARCH=amd64`. It runs on Nocturne's API, not as a UNIX
flavour. Goroutines run on several CPUs, and `os`, `os/exec`, `net` (TCP, UDP, DNS, `net.Listen`) and `crypto/tls`
work. Programs are cross-compiled on the host; `ports/go/apps/<name>` becomes `/bin/<name>`.

### JavaScript and TypeScript

`js` is a JavaScript and TypeScript runtime for the terminal (QuickJS), and `tsgo` is the official TypeScript 7
compiler ([ports/tsgo](ports/tsgo/README.md)).

```
js hello.js                 # ES modules, CommonJS, JSON
js app.ts one two           # type-check and compile with tsgo, then run (arguments in nocturne.args)
js --init myapp             # create package.json, tsconfig.json and src/main.ts
cd myapp && tsgo && js dist/main.js
js -e 'console.log(1 + 2)'  # -p prints the result, js - reads input, js alone starts a prompt
```

- **Modules**: `import` and `export`, top-level `await`, `require` (which can load ES modules too), and
  `import ... with { type: "json" }`. `.mjs` and `.cjs` are decided by the extension, `.js` by the `"type"` of the
  nearest `package.json`, or by its syntax if there is none. Packages are looked up in `node_modules` folders above,
  using `package.json`'s `exports` (conditions `nocturne`, `import`, `require`, `default`) and `main`.
- **System**: `import { readText, writeText, list, run, args } from "nocturne"` gives documents, folders, settings,
  terminal input and output, and launching apps. Failures are Errors with a `code` (`"NOT_FOUND"` and so on).
  There is no Node.js `fs` or `process` (programs are ported; Nocturne is not made to look like Node).
- **Web APIs**: `console`, `setTimeout` and friends, `queueMicrotask`, `performance`, `fetch` (HTTP, HTTPS and
  data:, following redirects, `AbortController`), `Headers`, `Request`, `Response`, `URL`, `TextEncoder`,
  `TextDecoder`, `crypto` (`getRandomValues`, `subtle`), `structuredClone`, `atob`, `btoa`, `EventTarget` and
  `DOMException`. DOMException, Encoding, URL and Web Crypto share the browser's implementation. `fetch` talks on a
  thread inside the app, so other work goes on while it waits.
- **Types**: the `nocturne` type definitions are in `/node_modules/@types/nocturne`, so projects in any folder find
  them. `js file.ts` checks with `--strict` and does not run code with type errors (output goes to `/tmp/js-ts`;
  packages are looked up from the source's location). TypeScript needs a build that includes `tsgo`.
- **Exit**: an uncaught exception or unhandled Promise rejection prints its stack and exits with status 1; a
  top-level `await` that never finishes exits with 13. The prompt only prints them and carries on.

The runtime is `user/apps/js.c` (system bindings, module loading, the event loop) plus
`user/apps/js_runtime.js`, `js_console.js` and `js_web.js`.

### WebAssembly

`wasm2c` is WABT's converter. Its C output can be compiled with the in-OS `tcc` together with the runtime in
`/usr/include` and `/usr/src/wasm2c`.

## The AI agent

![agent](docs/agent.png)

`agent` is a coding agent that runs inside the OS. It streams replies from any OpenAI-compatible chat completions
endpoint (over HTTPS, through Nocturne's own TLS) and gives the model these tools:
- `run_command`: runs a command in the shell.
- `read_file`, `write_file`, `edit_file` and `list_dir`.
- `fetch_url`.
- `screenshot`: the model sees the desktop as an image, so it can check the GUI programs it writes.

It writes C, compiles it with `tcc`, runs it and fixes what breaks.

```
agent                                   # interactive session (type /help)
agent write a tetris clone in /data/projects/tetris and install it as /data/bin/tet
agent -r                                # resume the previous session
```

The model's instructions are in `/etc/agent/system.md`, sessions and the agent's notes in `/data/agent`, and
settings in `/data/etc/agent.conf`:
- `endpoint` and `model`.
- `max_tokens` and `context_limit`.
- `reasoning`, `show_thinking` and `vision`. Set `vision=0` for models that cannot take images.

**API key.** The key is stored only on the data disk, never in the repository or the boot image:
- QEMU: `python scripts\setkey.py KEYFILE [--endpoint URL] [--model NAME]`
- Hyper-V: `hyperv.ps1 -Update -ApiKeyFile KEYFILE`
- Or start `agent` and paste the key when it asks (`/key` enters it again).

`KEYFILE` holds just the key, or contains a line like `api key: ...`. The defaults are `https://hyper.charm.land/v1`
and `glm-5.3-flash`. Keep key files outside this folder.

## Limits

What Nocturne does not have, so nobody is surprised:
- **No users or permissions.** Every app is isolated in its own address space, but all of them can read and write every file.
- **CPU 0 does the kernel's work.** App threads run on every CPU, but most system calls, interrupts, drivers, the
  GUI and the network stack are handled on CPU 0.
- **The window manager runs in the kernel.**
- **No dynamic linking and no swap.** Every program is linked statically.
- **Simple disks.** ATA uses PIO, and NVMe has one command in flight. The kernel has no AHCI driver. FAT32 has no
  journal, so power loss during a write can leave the volume inconsistent. Names may be up to 255 characters.
- **The networking limits** are listed [above](#networking) (IPv4 only, TLS up to 1.2).
- **A plain Enhanced Session.** It carries the picture, keyboard, mouse, sound and text on the clipboard: no images
  or files on the clipboard, no microphone, drive or printer sharing. The RDP server uses no encryption; it is
  reachable only through a Hyper-V socket on the host, never over the network.
- **Sound out only, AC'97 or the Enhanced Session only.** No Intel HD Audio, no USB audio, no recording.
- **No GPU driver.** Drawing is done on the CPU (WebGL too).
- **The AI agent runs the model's commands without asking.** Anything it does stays inside the VM, but it can delete files on `/data`.
- **The browser is a work in progress.** Many sites display, but some features don't work and some pages are slow (see [above](#the-web-browser)).
- **Real hardware has only just been tried.** Few machines have been checked, and there is no guide yet.

## Layout

```
boot/         limine.conf
common/       ABI shared by kernel and userland (abi.h, gpu_abi.h), 2D drawing, Unifont bitmaps
kernel/src/   arch/ (GDT, IDT, APIC, SMP, FPU, PAT, P-states, Hyper-V detection), mm/,
              sys/ (apps, the Kei scheduler, system calls, events, profiler),
              fs/ (VFS, RAM, FAT32, initrd, channels), dev/ (ATA, NVMe, xHCI and USB, PCI, ACPI, sound, display, input),
              gui/ (window manager, desktop), net/ (TCP/IP, e1000, tulip, RTL8125B),
              hv/ (VMBus and Hyper-V devices), rdp/ (the Enhanced Session's RDP server)
user/         libc/ (web/ is the browser engine), include/, apps/ (apps, browser helper processes, js's .js files)
rootfs/       files copied into the initrd (/etc, /home, /node_modules, /usr/share/fonts and more)
ports/        build settings and patches for ported code (tcc, go, tsgo, lexbor, freetype, ffmpeg, mesa, wabt, virgl)
third_party/  vendored sources (see Credits)
scripts/      image, initrd and sysroot builders, test.py, qtest.py, setkey.py, fatcheck.py, hv-*.ps1
tests/        the in-OS test suite (runtests.c and the programs it compiles), host-side checks, test material
docs/         README pictures, design documents and work logs
build.ps1     build entry point
hyperv.ps1    Hyper-V VM setup
```

`docs/` holds many dated work logs (mostly in Japanese). Each records its own day, and later changes have made
parts of them out of date.

## Credits

**Boot and base**
- [Limine](https://github.com/limine-bootloader/limine) bootloader (BSD-2-Clause).
- [BearSSL](https://bearssl.org/) TLS library (MIT).
- [TinyCC](https://bellard.org/tcc/) 0.9.28rc C compiler (LGPL-2.1, source in `third_party/tinycc`).
- Mozilla's CA certificate bundle, via [curl](https://curl.se/docs/caextract.html) (MPL-2.0).
- kei_sched, the selection core of the Kei scheduler (MIT, `kernel/src/sys/kei.c`).
- The RTL8125B driver (`kernel/src/net/rtl8125.c`) follows the register layout and initialization sequence of
  Realtek's FreeBSD driver rtl_bsd_drv_v1102.01. This product includes software developed by Bill Paul.
  (4-clause BSD; the full notice is at the top of the file.)

**Browser and JavaScript**
- [QuickJS](https://bellard.org/quickjs/) 2026-06-04 (MIT, `third_party/quickjs`).
- [Lexbor](https://lexbor.com/) 3.1.0 (Apache-2.0).
- [FreeType](https://freetype.org/) 2.14.3, under the FreeType Project License (FTL), not the alternative GPLv2.
  Portions of Nocturne are based on the work of the FreeType Team.
- [Brotli](https://github.com/google/brotli) 1.2.0 (MIT, for WOFF2).
- A subset of [FFmpeg](https://ffmpeg.org/) 9.0.2's decoders (LGPL-2.1 or later; the GPL parts are disabled).
- [Mesa](https://mesa3d.org/) 26.3.0-devel's GLSL, NIR and softpipe (MIT and others, `third_party/mesa/licenses`).
- [WABT](https://github.com/WebAssembly/wabt) 1.0.42 (Apache-2.0).
- [FormatJS](https://formatjs.github.io/) (MIT), Unicode CLDR and Unicode data (Unicode License V3), collation
  tables from ICU, and the IANA time zone database.
- [web-streams-polyfill](https://github.com/MattiasBuelens/web-streams-polyfill) 4.2.0 (MIT).
- The [Public Suffix List](https://publicsuffix.org/) (MPL-2.0) and the WHATWG Encoding JIS0208 table (CC BY 4.0).
- [stb_truetype and stb_image](https://github.com/nothings/stb) by Sean Barrett (public domain),
  the [JebP](https://github.com/matanui159/jebp) WebP decoder (MIT No Attribution), and
  [NanoSVG](https://github.com/memononen/nanosvg) by Mikko Mononen (zlib license; one local change: the default
  `preserveAspectRatio`).

**Development**
- libc++ 18.1.8 (Apache-2.0 with LLVM Exceptions, extracted from llvm-project at build time).
- [Go](https://go.dev/) 1.27.2 (BSD-3-Clause, only when built with `GO=`).
- [TypeScript-Go](https://github.com/microsoft/typescript-go) 7.0.2 (Apache-2.0, only when built with `TSGO=`).
- Build-time Python modules Mako, MarkupSafe and PyYAML (`third_party/python-build`, not in the image).

**Fonts** (all under the SIL Open Font License 1.1; licenses in `rootfs/usr/share/fonts`)
- Kernel bitmaps from [GNU Unifont Japanese](https://unifoundry.com/unifont/index.html) 18.0.01. The original BDF,
  the license and the conversion are in `third_party/unifont/`, and the license inside the boot image is in
  `/usr/share/licenses/unifont/`. Regenerate with `python -X utf8 scripts/build.py bitmap-fonts`.
- Maple Mono NF (unmodified TTFs; each file's SHA-256 is in `MapleMono-manifest.json`).
- [Inter](https://rsms.me/inter/) by Rasmus Andersson.
- [Noto](https://fonts.google.com/noto) Sans/Serif, Sans/Serif CJK JP and Color Emoji.
- Plangothic (Fitzgerald P. Köeingsegg).

Everything else was written for Nocturne.
