# Nocturne OS

*A small operating system for quiet nights.*

Nocturne is a hobby x86-64 operating system written from scratch: the kernel, a compositing window
manager, a TCP/IP stack, a C library, a shell, a terminal emulator and about 50 programs. It was
written with an AI coding assistant (Claude Code) over several long sessions, and is meant for
virtual machines (QEMU and Hyper-V); it has not been tried on real hardware. It is a toy, not a
general-purpose OS: see [Limits](#limits). Three borrowed pieces of code are included:
- Limine loads the kernel and the initial ramdisk.
- BearSSL provides TLS.
- TinyCC is the C compiler that runs inside the OS.

Nocturne also has an **AI agent**. `agent` connects to an LLM over HTTPS, and the model can then run
commands, write C, compile it with `tcc`, start it and look at the screen to check the result.
Everything it writes is kept on a persistent disk.

![Desktop and start menu](docs/desktop-menu.png)

| | |
|---|---|
| ![Terminal](docs/terminal.png) | ![Text editor](docs/editor.png) |
| ![Paint](docs/paint.png) | ![System monitor](docs/sysmon.png) |
| ![Mandelbrot](docs/mandelbrot.png) | ![3D shapes](docs/shapes.png) |

## What's inside

**Kernel** (`kernel/`, about 9,000 lines of C)
- Boots through Limine (BIOS or UEFI) into a higher-half x86-64 kernel.
- Memory: physical page allocator, 4-level paging, kernel heap, and a separate address space for every process.
  - W^X: no page is both writable and executable. ELF segments are mapped with their own permissions, the stack and heap are no-execute (NX), and the kernel's own code, read-only data and data are mapped separately. Write protection also applies in ring 0.
  - `mmap`, `munmap` and `mprotect` are real, which is how `tcc -run` gets executable memory for the code it just compiled.
- Processes: ring-3 user processes with ELF loading, preemptive scheduling (1 kHz PIT), `int 0x80` system calls, `spawn` and `waitpid`, process trees, and `kill`.
- Files: a VFS with a RAM filesystem (populated from a tar initrd), `/dev` (console, null, zero, random), pipes, and `poll`.
- Persistent storage: an ATA PIO disk driver and a FAT32 filesystem (read and write), which is mounted at `/data`.
- Drivers: PS/2 keyboard and wheel mouse, framebuffer, serial, CMOS clock, PCI enumeration, and ACPI power-off and reboot.
- Networking: drivers for the Intel e1000 (QEMU, VirtualBox, VMware) and the DEC 21140 "tulip" (Hyper-V's legacy network adapter), and a small TCP/IP stack: Ethernet, ARP, IPv4, ICMP, UDP, a DHCP client, a DNS resolver and a TCP client.
  - TCP keeps out-of-order segments and reassembles them, and does NewReno congestion control (slow start, fast retransmit and fast recovery) with a retransmission timeout taken from the measured round-trip time.
- Graphics: a compositing window manager. It runs in the kernel, which keeps it simple and fast, but a bug in it can bring the whole system down.
  - Window buffers are shared memory and redraws are damage-tracked.
  - Windows have drop shadows and can be dragged, resized, maximized and minimized.
  - The desktop has a taskbar, start menu, desktop icons, toast notifications and a clipboard.
  - The night-sky wallpaper is generated procedurally.

**Userland** (`user/`)
- libc: stdio, `printf` with correctly rounded floating point, `malloc`, string, math and time functions, `qsort`, `strtod`, `setjmp`, and more. It is a subset of C99/POSIX, not a complete one.
  - It also includes an HTTP/1.1 client with HTTPS, a JSON parser and builder, and a PNG encoder.
- Shell (`sh`):
  - Pipes, redirection (`< > >> 2>`), `&`, `;`, `&&` and `||`, quoting, globbing, `$?` and script arguments (`$1`, `$#`, `$@`). There are no variables, `if` or loops.
  - History, tab completion and scripts.
- Terminal: ANSI colours, UTF-8, scrollback, copy and paste, and resizing.
- Command-line tools: `ls cat cp mv rm mkdir touch tree grep wc head hexdump echo ps kill free uptime date uname dmesg lspci sleep clear reboot poweroff neofetch fortune moonsay`
- Network tools: `ifconfig`, `ping`, `host` (DNS lookup) and `fetch` (an HTTP and HTTPS client).
- Development: the `tcc` C compiler, with headers and a static libc in `/usr`. The source of every program in `/bin` is in `/usr/src/apps`.
- `agent`: the AI agent (see below). `screenshot` saves the screen as a PNG.
- Desktop apps: Files, Text Editor, Paint, Calculator, Clock and System Monitor.
- Games and toys: Snake, Tetris, Minesweeper, Mandelbrot, 3D Shapes and Game of Life.

## Running it in Hyper-V

You need Windows with Hyper-V enabled. Run this once from an **elevated** (administrator) PowerShell in this folder:

```powershell
powershell -ExecutionPolicy Bypass -File hyperv.ps1
```

This creates a **Generation 1** VM called `Nocturne` with the following settings:
- 512 MB of static memory and 1 CPU.
- The disk on IDE.
- A legacy network adapter on the **Default Switch**, so the VM gets an address by DHCP and can reach the internet.
- No checkpoints.
- COM1 connected to `\\.\pipe\nocturne-com1`.

The script then starts the VM and opens a VMConnect window.

Other options:

| Command | What it does |
|---|---|
| `hyperv.ps1 -Update` | Copy a freshly built disk into the existing VM and restart it. |
| `hyperv.ps1 -Iso` | Boot from `build\nocturne.iso` on the DVD drive instead of the VHD. |
| `hyperv.ps1 -Remove` | Delete the VM. |
| `hyperv.ps1 -NoNetwork` | Create the VM without a network adapter. |
| `hyperv.ps1 -Switch "Name"` | Connect the network adapter to a different virtual switch. |
| `hyperv.ps1 -Name Foo -MemoryMB 1024` | Use a different VM name and memory size. |

To create the VM by hand instead, make a Generation 1 VM and attach `build\nocturne.vhd` (or `nocturne.vhdx`) as an IDE disk. Generation 2 will **not** work: it has no PS/2 or IDE hardware, only Hyper-V's VMBus devices, and Nocturne has no drivers for those. For the same reason the network adapter must be a *legacy* one.

**Tips for VMConnect**
- Click inside the window to capture the mouse. **Ctrl+Alt+Left Arrow** releases it.
- The Windows key only reaches the VM in full-screen mode. Use **Ctrl+Esc** to open the start menu instead.
- The boot menu has a **1024x768** entry and a **text console only** entry, in case the default 1280x800 mode isn't available.
- Kernel log: connect any named-pipe client, such as PuTTY (Serial, `\\.\pipe\nocturne-com1`), to COM1.
- **Mouse moving up when you move down?** Hyper-V on Windows 11 hosts reports the emulated PS/2 mouse's vertical axis backwards. Nocturne detects Hyper-V (and the host build) and flips the axis itself. If it guesses wrong on your host, press **E** on the boot menu entry and add `cmdline: mouse_y=normal` (or `mouse_y=invert`).

## Building

Everything needed is in `tools/`:
- A portable MSYS2 with clang, lld, nasm, mtools, xorriso, qemu and python.
- Limine.

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1          # build everything into .\build
powershell -ExecutionPolicy Bypass -File build.ps1 run      # build and boot in QEMU
powershell -ExecutionPolicy Bypass -File build.ps1 clean
```

Outputs in `build\`:
- `nocturne.img`: raw MBR disk, used by QEMU.
- `data.img`: the persistent `/data` disk for QEMU.
- `nocturne.vhd`: fixed VHD for Hyper-V.
- `nocturne.vhdx`
- `nocturne.iso`: hybrid BIOS/UEFI ISO.

## Tests

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 test        # about 4 minutes
powershell -ExecutionPolicy Bypass -File build.ps1 test-quick  # skips compiling every app
powershell -ExecutionPolicy Bypass -File build.ps1 test-full   # adds tcc rebuilding itself inside the OS
```

`scripts/test.py` boots the image headless in QEMU with a scratch data disk holding `tests/`. Your own
`data.img` is not touched. At boot, `init` runs the suite. The suite compiles its own runner with the
in-OS `tcc` and runs 42 tests, checking each one's exit status and output. Results come back over the
serial port, and afterwards the host checks the FAT32 volume the OS wrote to with `fatcheck.py`. The
tests cover:
- Shell and tools, and `malloc` stress.
- W^X: writing code or read-only data and running the stack or heap must crash, and `mprotect` must work.
- FAT32 and the RAM filesystem: many sizes, long names, 150-file directories, rename and nested directories.
- `tcc`: printf and math, compiling every app, and self-hosting.
- The GUI: a window appears in a screen grab and is gone after it closes.
- Network: DHCP, ping, DNS, HTTP, HTTPS, and a rejected bad certificate.
- TCP: 14 MiB to and from a host-side server, clean and with simulated loss and reordering, checking every byte.
- The agent, offline: it never needs an API key.

The network tests need internet access from the host. Use `python scripts/test.py --no-net` without it.

`scripts/qtest.py` drives a running image by hand: keystrokes, mouse moves and screenshots.

```
python scripts/qtest.py sleep:6 "type:neofetch\n" shot:neofetch goto:51,125 dclick shot:files
```

## Using it

| Keys | Action |
|---|---|
| Super or Ctrl+Esc | Open the start menu (arrow keys and Enter work in it) |
| Ctrl+Alt+T | New terminal |
| Alt+Tab | Switch windows |
| Alt+F4 | Close window |
| Double-click a title bar | Maximize or restore |
| Drag the bottom-right corner | Resize (Terminal, Files, Text Editor, Clock, Mandelbrot, 3D Shapes) |
| Ctrl+Shift+C / Ctrl+Shift+V | Copy screen / paste in the terminal |
| Shift+PgUp / PgDn, mouse wheel | Terminal scrollback |
| Ctrl+C | Interrupt the running command |

The root filesystem lives in RAM, so files you create there are lost when you reboot. Save anything you want to keep under `/data`. `/home` starts with a few sample files; try `sh hello.sh`.

## Networking

![Network tools](docs/network.png)

The network comes up by itself: a DHCP client runs in the kernel at boot, so `ifconfig` normally
shows an address a second or two after the desktop appears.

- **QEMU**: the default e1000 card with user-mode networking works as-is (`build.ps1 run`). `-nic user,model=tulip` exercises the Hyper-V driver.
- **Hyper-V**: `hyperv.ps1` adds a legacy network adapter on the Default Switch. A *synthetic* (VMBus) adapter will not be seen.

```
ping -c 3 example.com
host example.com
fetch -i http://example.com
fetch -o /home/1mb.zip http://speedtest.tele2.net/1MB.zip
```

`fetch https://...` uses TLS 1.2 through BearSSL. Certificates are checked against the Mozilla root store, which
is compiled into libc. The CMOS clock supplies the time for the validity check.

Network limits: TCP is client-side only (no listening sockets), with no window scaling or SACK, so a connection has at most 64 KiB in flight. There is no IP fragmentation and no IPv6.

## Persistent storage: /data

Everything outside `/data` lives in RAM and is reset on every boot. `/data` is a FAT32 partition
labelled `NOCTDATA` on a second disk, so files saved there survive reboots and rebuilds.
- **QEMU**: `build\data.img` (512 MB) is created on the first build and is never overwritten.
  `build.ps1 clean` leaves it alone. Delete it by hand to start fresh.
- **Hyper-V**: `hyperv.ps1` attaches `hyperv\<Name>-data.vhdx` (2 GB, dynamic) as the second IDE disk.
  `-Update` replaces only the boot disk and keeps the data disk.

`/data/bin` is on `PATH`, so programs installed there run by name.

## Writing C inside Nocturne

`tcc` is TinyCC 0.9.28, built for Nocturne:

```
tcc -run /usr/src/apps/mandel.c           # compile in memory and run
tcc -o /data/bin/hello hello.c            # build a program; it now runs as `hello`
```

Programs are linked statically against Nocturne's libc. They can use:
- `nocturne.h`: system calls, windows and widgets.
- `gfx.h`: drawing.
- `http.h`: HTTP and HTTPS.
- `json.h` and `png.h`.
- BearSSL.

tcc can also compile itself: its own source rebuilds inside the OS into a working compiler.

## The AI agent

![agent](docs/agent.png)

`agent` is a coding agent that runs inside the OS. It streams replies from any OpenAI-compatible
chat completions endpoint (over HTTPS, from Nocturne's own TLS stack) and gives the model these tools:
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

The model's instructions are in `/etc/agent/system.md`. Sessions and the agent's notes are kept in
`/data/agent`. Settings are in `/data/etc/agent.conf`:
- `endpoint` and `model`.
- `max_tokens` and `context_limit`.
- `reasoning`, `show_thinking` and `vision`. Set `vision=0` for models that cannot take images.

**API key.** The key is stored only on the data disk, never in the repository or the boot image:
- QEMU: `python scripts\setkey.py KEYFILE [--endpoint URL] [--model NAME]`
- Hyper-V: `hyperv.ps1 -Update -ApiKeyFile KEYFILE`
- Or start `agent` and paste the key when it asks.

`KEYFILE` holds just the key, or contains a line like `api key: ...`. The defaults are
`https://hyper.charm.land/v1` and `glm-5.3-flash`. Keep key files outside this folder.

## Limits

What Nocturne does not have, so nobody is surprised:
- **One CPU.** There is no SMP; extra virtual CPUs are ignored.
- **No users or permissions.** Every process is isolated in its own address space, but all of them can read and write every file.
- **No `fork`/`exec`.** Processes are started with `spawn`. There are no signals beyond kill, no threads, no dynamic linking and no swap.
- **The window manager runs in the kernel.**
- **Slow, simple disks.** The ATA driver uses PIO with polling (no DMA). FAT32 has no journal, so power loss during a write can leave the volume inconsistent. Names may be up to 255 characters.
- **The networking limits are listed above.**
- **The AI agent runs the model's commands without asking.** Anything it does stays inside the VM, but it can delete files on `/data`.
- **Only checked in virtual machines.** It has been tested in QEMU and Hyper-V, never on real hardware.

## Layout

```
boot/        limine.conf
common/      ABI shared by kernel and userland, 2D graphics library, fonts
kernel/src/  arch/ (GDT, IDT, APIC), mm/, sys/ (processes, scheduler, syscalls), fs/, dev/, gui/, net/
user/        libc/, include/, apps/ (one .c file per program)
rootfs/      files copied into the initrd (/etc, /home)
ports/tcc/   TinyCC configuration and runtime glue for Nocturne
third_party/ BearSSL, TinyCC
scripts/     image builder, initrd packer, sysroot builder, test.py, qtest.py, setkey.py, fatcheck.py
tests/       the in-OS test suite (runtests.c and the C programs it compiles)
hyperv.ps1   Hyper-V VM setup
```

## Credits

- [Limine](https://github.com/limine-bootloader/limine) bootloader (BSD-2-Clause)
- [Spleen](https://github.com/fcambus/spleen) bitmap fonts (BSD-2-Clause, see `common/FONT-LICENSE-spleen.txt`)
- [BearSSL](https://bearssl.org/) TLS library (MIT)
- [TinyCC](https://bellard.org/tcc/) C compiler (LGPL-2.1, source in `third_party/tinycc`)
- Mozilla's CA certificate bundle, via [curl](https://curl.se/docs/caextract.html) (MPL-2.0)
- Everything else was written for Nocturne.
