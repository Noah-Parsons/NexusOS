# NexusOS

![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)
![Platform: x86-64](https://img.shields.io/badge/platform-x86--64-lightgrey.svg)
[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.22313291.svg)](https://doi.org/10.5281/zenodo.22313291)

An x86-64 operating system, written from scratch.

![NexusOS booted to its compositing desktop, with the shell running in a terminal window](docs/screenshot.png)

*NexusOS running its compositing window manager: draggable windows with
close/minimize buttons, the shell in a terminal window, and a taskbar with
per-window buttons and an uptime clock.*

> **Want to build one yourself?** This repo includes Build Your Own Operating System, a beginner-friendly
> book available on Leanpub — **[Build Your Own OS](docs/book/README.md)** — that teaches OS
> development from the first boot instruction to a graphical desktop on real
> hardware, using NexusOS as the worked example.

## Status

Boots on QEMU into a graphical desktop: a VESA linear framebuffer driven by a
compositing window manager, with draggable windows and a mouse cursor. The
interactive shell runs in a terminal window (and is mirrored over serial for
debugging).

### What works

- **Two-stage boot.** Custom 512-byte MBR bootloader transitions real → protected →
  long mode, collects the BIOS E820 memory map, and loads the kernel.
- **Long-mode kernel** linked at `0x10000`, BSS zeroed at entry.
- **Output** on a VESA linear framebuffer (graphical text console), with the
  legacy VGA text buffer (`0xB8000`) and serial (COM1, 115200 8N1) mirrored
  alongside for early boot and debugging.
- **Interrupts.** Full 256-entry IDT, all CPU exception vectors handled,
  panic dump prints RIP, CR2, and every general-purpose register on fault.
- **PIC.** 8259A pair remapped to `0x20–0x2F`, off the way of CPU exceptions.
- **PIT.** Channel-0 timer at 100 Hz, IRQ0 maintains a tick counter.
- **Keyboard.** PS/2 scancode set 1, shift/caps/extended-key state,
  256-byte ring buffer, blocking and nonblocking reads.
- **Physical memory.** E820 map parsed in the kernel; bitmap frame allocator
  reserves the low 1 MiB, kernel image, and bitmap itself.
- **Virtual memory.** Full 4-level paging. RAM identity-mapped with 2 MiB pages
  up to detected end (capped at 4 GiB), then a kernel-owned PML4 is installed.
  4 KiB `map`/`unmap`/`translate` API.
- **Kernel heap.** First-fit doubly-linked free list, 16-byte aligned, lives at
  virtual `0x200000000` (8 GiB, above the identity map). Demand-grows via
  `pmm_alloc_frame` + `vmm_map`, coalesces on free.
- **Preemptive scheduler.** Round-robin tasks with context switch on the PIT
  tick, `sched_sleep_ms`, and an idle task.
- **Synchronization.** Mutex, counting semaphore, and condition variable, all
  built on the scheduler's BLOCKED state with FIFO-on-release hand-off.
- **Storage.** Interrupt-driven PIO ATA driver (primary slave) and a
  read-only FAT12 filesystem (`ls`, `cat`).
- **Graphics.** Bootloader sets a VESA linear-framebuffer mode (1024x768,
  falling back to text mode if unavailable); the kernel maps the framebuffer.
  `kernel/gfx.c` provides surface-based drawing (pixel/fill/blit/text) that
  targets either the screen or an off-screen buffer.
- **Mouse.** PS/2 mouse on IRQ12 — 3-byte packet parsing, position clamped to
  the screen, button state.
- **Window manager.** A compositing WM (`kernel/wm.c`) with an off-screen back
  buffer: desktop → z-ordered windows are composed on change, presented every
  frame with the cursor as an overlay (so nothing under the cursor is ever
  clobbered). Click to focus/raise, drag windows by the title bar, close /
  minimize buttons, and a taskbar (per-window buttons + an uptime clock).
- **Terminal window.** The interactive shell runs in a draggable terminal
  window. `console_*` output feeds a character grid (`kernel/fbcon.c`) that the
  compositor renders inside that window.
- **Interactive shell.** `help`, `ticks`, `mem`, `tasks`, `prod`, `ls`,
  `cat <file>`, `halt`.

### What's next

Toward a usable GUI:

1. Per-window keyboard focus routing (multiple terminals / text fields).
2. More widgets — window resize, buttons, a few demonstration apps.
3. Syscall interface (`syscall`/`sysret`) + user mode (ring 3, ELF loader),
   so apps can run outside the kernel.
4. VFS layer over the existing FAT driver; write support.
5. SMP, ACPI parsing, APIC.

## Requirements

Toolchain (installed automatically if you use the devcontainer):

- `nasm`
- `gcc`
- `ld`, `objcopy` (binutils)
- `mtools` (`mformat`, `mcopy`) — builds the FAT12 disk image without root
- `qemu-system-x86_64`
- `gdb` (optional, for `make debug`)

## Quick start

### Codespaces / devcontainer

Open the repo in GitHub Codespaces or VS Code with the Dev Containers extension.
The container provisions the full toolchain. Then:

```sh
make run
```

### Local

```sh
sudo apt install nasm gcc binutils mtools qemu-system-x86 gdb
make rungui   # GUI in a window; use `make run` for headless serial only
```

`make rungui` opens a QEMU window with the graphical desktop; `make run` is
serial-only (no window). Either way the boot log goes to serial:

```
==========================================
           NexusOS  -  x86_64
==========================================
[boot] console ready
[boot] idt installed
[boot] pic remapped to 0x20
[boot] interrupts enabled
[boot] kernel ends at 0x...
[pmm] total = XXX MiB, free = XXX MiB
[vmm] mapped identity ...
[heap] base=0x200000000 ...
[fb] 1024x768x24 @ 0xFD000000 pitch=3072
NexusOS ready. Type 'help' for commands.
[sched] init, idle task id=0
...
[ata] primary slave ready
[fat] mounted (...)
nexus>
```

`Ctrl-A x` exits QEMU (serial/`-display none` runs); close the window
otherwise.

To open a real GUI window instead of the headless serial console (needs a
display — e.g. WSLg on Windows, or any X/Wayland session):

```sh
make rungui
```

Click into the window to grab the mouse, drag windows by their title bars, and
type into the terminal. `Ctrl-Alt-G` releases the mouse grab.

### Build targets

| Target        | What it does                                                       |
| ------------- | ------------------------------------------------------------------ |
| `make all`    | Build the GUI image (`build/os.bin`)                               |
| `make run`    | Build + boot headless (serial, no window)                          |
| `make rungui` | Build + boot in a GUI window (needs a display)                     |
| `make text`   | Build a **text-only** image — skips VBE, for real hardware whose firmware hangs on the VBE call |
| `make debug`  | Boot paused and attach GDB                                         |
| `make clean`  | Remove `build/`                                                    |

## Running on real hardware

NexusOS boots natively (BIOS/legacy) from a USB stick on a real x86-64 PC — no
emulator. The short version: write `build/os.bin` raw to a USB stick (DD mode),
enable **Legacy/CSM** and **USB Legacy Support** in firmware, and boot from it.
Use `make text` if the graphical mode hangs on your board.

See **[docs/HARDWARE.md](docs/HARDWARE.md)** for the full walkthrough — flashing,
firmware settings, what works vs. the caveats, and a troubleshooting table.

### Debugging

```sh
make debug
```

Launches QEMU paused (`-s -S`) and attaches GDB with the kernel ELF loaded.
A breakpoint is set on `kernel_main`. `continue` to start.

## Graphics

The bootloader asks the BIOS (VBE) for a 1024x768 linear-framebuffer mode
while still in real mode, and leaves a small descriptor — framebuffer address,
pitch, dimensions, and bpp — at physical `0x9700` for the kernel. If VBE fails,
it sets a valid flag to 0 and the kernel stays on the text/serial console.

In the kernel, `kernel/fb.c` maps the framebuffer (it lives above identity-
mapped RAM, so it is mapped page-by-page). `kernel/gfx.c` draws onto any
`surface_t` — the screen or an off-screen buffer — with pixel/fill/blit and
scaled 8x8 text (`kernel/font8x8.h`).

The window manager (`kernel/wm.c`) owns a compositor: it composes the scene
(desktop background → text console → z-ordered windows) into an off-screen
back buffer whenever something changes, then every frame blits that buffer to
the screen and paints the mouse cursor on top. Because the cursor is a
per-frame overlay (never stored in the scene), it never clobbers content.
`console_*` output feeds `kernel/fbcon.c`, now a character grid the compositor
renders inside a terminal window. Mouse clicks focus/raise windows; dragging a
title bar moves a window.

### Capturing a screenshot

`tools/screenshot.sh` boots NexusOS headless, drives the QEMU monitor to grab
the framebuffer, and writes `build/screen.png`:

```sh
bash tools/screenshot.sh           # just capture the boot screen
bash tools/screenshot.sh help      # type a command first, then capture
```

## Architecture

For a full tour — boot flow, the memory model, the scheduler and synchronization
primitives, the storage stack, and the gfx/compositor design — see
**[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**.

## File layout

```
.
├── .devcontainer/        # Codespaces / VS Code container definition
├── Makefile              # auto-discovers kernel/*.c and kernel/*.asm
├── tools/
│   └── screenshot.sh     # boot headless, capture the framebuffer to a PNG
├── docs/
│   └── SETUP.md          # toolchain + build/run notes
├── bootloader/
│   └── boot.asm          # MBR: E820, VBE mode set, GDT, PM transition
└── kernel/
    ├── linker.ld         # loads at 0x10000, exports __bss_start/_end, __kernel_end
    ├── kernel_entry.asm  # long-mode trampoline, BSS zero, call kernel_main
    ├── kernel.c          # boot sequence, shell
    │
    ├── types.h           # u8/u16/u32/u64, bool, attribute macros
    ├── io.h              # port I/O, control registers, invlpg
    ├── string.{h,c}      # memset/memcpy/memcmp/strlen + numeric formatters
    ├── console.{h,c}     # unified framebuffer + VGA + serial output
    │
    ├── idt.{h,c}         # IDT table, dispatch, panic, irq_register
    ├── idt_stubs.asm     # 256 ISR stubs, common save/restore
    ├── pic.{h,c}         # 8259A remap, mask/unmask/EOI
    ├── pit.{h,c}         # channel-0 timer
    ├── keyboard.{h,c}    # PS/2 scancode set 1, ring buffer
    │
    ├── pmm.{h,c}         # E820 + bitmap frame allocator
    ├── vmm.{h,c}         # 4-level paging, map/unmap/translate
    ├── kmalloc.{h,c}     # first-fit heap
    │
    ├── sched.{h,c}       # preemptive round-robin scheduler
    ├── sched_switch.asm  # context-switch register save/restore
    ├── sync.{h,c}        # mutex, semaphore, condvar
    │
    ├── ata.{h,c}         # PIO ATA driver (primary slave)
    ├── fat.{h,c}         # read-only FAT12
    │
    ├── fb.{h,c}          # linear-framebuffer mapping + raw pixel access
    ├── gfx.{h,c}         # surface drawing: pixel/fill/blit/glyph/text
    ├── font8x8.h         # 8x8 bitmap font (ASCII 0x00..0x7F)
    ├── fbcon.{h,c}       # text console as a character grid
    ├── mouse.{h,c}       # PS/2 mouse (IRQ12)
    └── wm.{h,c}          # compositing window manager + cursor
```

Add a new `.c` or `.asm` under `kernel/` and the Makefile picks it up
automatically — no Makefile edits needed for routine additions.

## Memory layout

Physical:

| Range                    | Contents                                    |
| ------------------------ | ------------------------------------------- |
| `0x00000000–0x0009FFFF`  | Low memory (IVT, BDA, etc.) — reserved      |
| `0x00007C00`             | Bootloader entry (MBR)                      |
| `0x00009000`+            | E820 entry count + 24-byte entries          |
| `0x00009700`             | Framebuffer descriptor handed to the kernel |
| `0x00009800`             | VBE mode-info scratch block                 |
| `0x000B8000`             | Legacy VGA text framebuffer                 |
| `0x00010000`             | Kernel `_start`                             |
| `0x00010000–__kernel_end`| Kernel image                                |
| `__kernel_end`+          | PMM bitmap                                  |
| Above                    | Free frames                                 |
| `~0xFD000000`            | VESA linear framebuffer (MMIO, see boot.asm)|

Virtual (after `vmm_init`):

| Range                    | Contents                                    |
| ------------------------ | ------------------------------------------- |
| `0x0000000000000000`+    | Identity map of physical RAM (2 MiB pages)  |
| `0x0000000200000000`+    | Kernel heap (4 KiB pages, demand-grown)     |
| `~0xFD000000`            | Framebuffer, identity-mapped (4 KiB pages)  |

## Shell commands

| Command      | What it does                                           |
| ------------ | ------------------------------------------------------ |
| `help`       | List commands                                          |
| `ticks`      | Print PIT tick count since boot                        |
| `mem`        | Print frame counts + heap used/free                    |
| `tasks`      | Scheduler stats + mutex invariant check                |
| `prod`       | Producer/consumer (semaphore) queue stats              |
| `ls`         | List the FAT12 root directory                          |
| `cat <file>` | Print a file from the FAT12 disk                       |
| `halt`       | `cli; hlt` loop — clean stop                           |

## License

See `LICENSE`.
