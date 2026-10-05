# Nocturne OS

*A small operating system for quiet nights.*

Nocturne is a hobby x86-64 operating system written from scratch in one night: the kernel, a
compositing window manager, a C library, a shell, a terminal emulator and 46 programs.
Limine is the only borrowed piece of code: it loads the kernel and the initial ramdisk.

![Desktop and start menu](docs/desktop-menu.png)

| | |
|---|---|
| ![Terminal](docs/terminal.png) | ![Text editor](docs/editor.png) |
| ![Paint](docs/paint.png) | ![System monitor](docs/sysmon.png) |
| ![Mandelbrot](docs/mandelbrot.png) | ![3D shapes](docs/shapes.png) |

## What's inside

**Kernel** (`kernel/`, about 7,000 lines of C)
- Boots through Limine (BIOS or UEFI) into a higher-half x86-64 kernel.
- Memory: physical page allocator, 4-level paging, kernel heap, and a separate address space for every process.
- Processes: ring-3 user processes with ELF loading, preemptive scheduling (1 kHz PIT), `int 0x80` system calls, `spawn` and `waitpid`, process trees, and `kill`.
- Files: a VFS with a RAM filesystem (populated from a tar initrd), `/dev` (console, null, zero, random), pipes, and `poll`.
- Drivers: PS/2 keyboard and wheel mouse, framebuffer, serial, CMOS clock, PCI enumeration, and ACPI power-off and reboot.
- Graphics: a compositing window manager that runs in the kernel.
  - Window buffers are shared memory and redraws are damage-tracked.
  - Windows have drop shadows and can be dragged, resized, maximized and minimized.
  - The desktop has a taskbar, start menu, desktop icons, toast notifications and a clipboard.
  - The night-sky wallpaper is generated procedurally.

**Userland** (`user/`)
- libc: stdio with real `printf` (floats included), `malloc`, string, math and time functions, `qsort`, `strtod`, and more.
- Shell (`sh`):
  - Pipes, redirection (`< > >> 2>`), `&`, `;`, `&&` and `||`, quoting, globbing and `$?`.
  - History, tab completion and scripts.
- Terminal: ANSI colours, UTF-8, scrollback, copy and paste, and resizing.
- Command-line tools: `ls cat cp mv rm mkdir touch tree grep wc head hexdump echo ps kill free uptime date uname dmesg lspci sleep clear reboot poweroff neofetch fortune moonsay`
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
- No network adapter and no checkpoints.
- COM1 connected to `\\.\pipe\nocturne-com1`.

The script then starts the VM and opens a VMConnect window.

Other options:

| Command | What it does |
|---|---|
| `hyperv.ps1 -Update` | Copy a freshly built disk into the existing VM and restart it. |
| `hyperv.ps1 -Iso` | Boot from `build\nocturne.iso` on the DVD drive instead of the VHD. |
| `hyperv.ps1 -Remove` | Delete the VM. |
| `hyperv.ps1 -Name Foo -MemoryMB 1024` | Use a different VM name and memory size. |

To create the VM by hand instead, make a Generation 1 VM and attach `build\nocturne.vhd` (or `nocturne.vhdx`) as an IDE disk. Generation 2 will **not** work: it has no PS/2 or IDE hardware, only Hyper-V's VMBus devices, and Nocturne has no drivers for those.

**Tips for VMConnect**
- Click inside the window to capture the mouse. **Ctrl+Alt+Left Arrow** releases it.
- The Windows key only reaches the VM in full-screen mode. Use **Ctrl+Esc** to open the start menu instead.
- The boot menu has a **1024x768** entry and a **text console only** entry, in case the default 1280x800 mode isn't available.
- Kernel log: connect any named-pipe client, such as PuTTY (Serial, `\\.\pipe\nocturne-com1`), to COM1.

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
- `nocturne.vhd`: fixed VHD for Hyper-V.
- `nocturne.vhdx`
- `nocturne.iso`: hybrid BIOS/UEFI ISO.

`scripts/qtest.py` boots the image headless in QEMU and drives it from a small script of keystrokes, mouse moves and screenshots, which is how the system was tested:

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

The filesystem lives in RAM, so files you create or edit are lost when you reboot. `/home` starts with a few sample files; try `sh hello.sh`.

## Layout

```
boot/        limine.conf
common/      ABI shared by kernel and userland, 2D graphics library, fonts
kernel/src/  arch/ (GDT, IDT, APIC), mm/, sys/ (processes, scheduler, syscalls), fs/, dev/, gui/
user/        libc/, include/, apps/ (one .c file per program)
rootfs/      files copied into the initrd (/etc, /home)
scripts/     image builder, initrd packer, QEMU test driver
hyperv.ps1   Hyper-V VM setup
```

## Credits

- [Limine](https://github.com/limine-bootloader/limine) bootloader (BSD-2-Clause)
- [Spleen](https://github.com/fcambus/spleen) bitmap fonts (BSD-2-Clause, see `common/FONT-LICENSE-spleen.txt`)
- Everything else was written for Nocturne.
