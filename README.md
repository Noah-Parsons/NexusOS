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

Boots by **legacy BIOS** or **UEFI**, from the same kernel, into a graphical
desktop: a linear framebuffer driven by a compositing window manager, with
draggable windows and a mouse cursor. The interactive shell runs in a terminal
window (and is mirrored over serial for debugging). Both paths boot to the
desktop under QEMU (SeaBIOS and OVMF); see
[Running on real hardware](#running-on-real-hardware) for physical machines.

### What works

- **Two boot paths, one kernel.** No GRUB, gnu-efi, or EDK2 on either.
  - *Legacy BIOS* (the path the book describes): a 512-byte MBR
    (`bootloader/boot.asm`) collects the E820 map, sets a VBE linear-framebuffer
    mode, loads the kernel with int 13h LBA reads, and enters protected mode;
    `kernel/kernel_entry_bios.asm` builds boot page tables and switches to long
    mode.
  - *UEFI*: a custom PE32+ application (`bootloader/boot.c`) opens the Graphics
    Output Protocol, loads the kernel from the FAT EFI System Partition,
    snapshots the firmware memory map into the same E820 format, exits boot
    services, and jumps to the long-mode kernel (`kernel/kernel_entry.asm`). It
    uses a self-contained subset of the UEFI interfaces in `bootloader/efi.h`.
  - Both loaders leave the same hand-off in low memory (E820 table at `0x9000`,
    framebuffer descriptor at `0x9700`), so only the entry stub differs.
- **Long-mode kernel** linked at `0x10000`, BSS zeroed at entry.
- **Output** on the linear framebuffer (graphical text console), with the
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
- **Graphics.** The bootloader selects a linear-framebuffer mode — VBE mode
  0x118 (1024x768) under BIOS; under UEFI, GOP at 1024x768x32 if offered,
  otherwise whatever the firmware left active — and falls back to text/serial
  if neither is available. The kernel maps the framebuffer. `kernel/gfx.c` provides surface-based drawing
  (pixel/fill/blit/text) that targets either the screen or an off-screen buffer.
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
- `x86_64-w64-mingw32-gcc` (`gcc-mingw-w64-x86-64`) — UEFI loader only; emits
  the PE32+ binary UEFI needs, with no gnu-efi or EDK2 dependency
- `ld`, `objcopy` (binutils)
- `mtools` (`mformat`, `mmd`, `mcopy`) — builds the FAT images without root
- `ovmf` — UEFI firmware for QEMU, for the `*-uefi` targets (expects
  `/usr/share/ovmf/OVMF.fd`; override with `make run-uefi OVMF=/path/to/OVMF.fd`)
- `qemu-system-x86_64`
- `gdisk` (`sgdisk`) — only for `make usb`
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
sudo apt install nasm gcc gcc-mingw-w64-x86-64 binutils mtools ovmf \
                 qemu-system-x86 gdisk gdb
make rungui        # BIOS boot, GUI in a window; `make run` for headless serial only
make rungui-uefi   # the same under UEFI (OVMF); `make run-uefi` for headless
```

`make rungui` opens a QEMU window with the graphical desktop; `make run` is
serial-only (no window). Either way the kernel's boot log goes to serial:

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
[fb] 1024x768x32 @ 0x... pitch=4096
[test] kmalloc -> 0x... 0x... 0x...
NexusOS ready. Type 'help' for commands.
[sched] init, idle task id=0
...
[ata] primary slave ready
[fat] mounted (...)
nexus>
```

Under UEFI the loader first prints three lines of its own (`NexusOS UEFI
bootloader` and two `[boot]` lines) to the firmware console; under BIOS,
`boot.asm` prints `Booting NexusOS` to the screen. Everything from the banner
down is the kernel, identical on both paths.

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

| Target             | What it does                                                        |
| ------------------ | ------------------------------------------------------------------- |
| `make all`         | Build both boot images + the FAT12 test disk                        |
| `make bios`        | Build the BIOS image (`build/os.bin`) + the FAT12 test disk         |
| `make uefi`        | Build the UEFI ESP image (`build/esp.img`) + the FAT12 test disk    |
| `make run`         | BIOS: build + boot headless (serial, no window)                     |
| `make rungui`      | BIOS: build + boot in a GUI window (needs a display)                |
| `make debug`       | BIOS: boot paused and attach GDB                                    |
| `make text`        | BIOS: text-only image that skips VBE (for firmware that hangs on it) |
| `make run-uefi`    | UEFI: build + boot headless under OVMF                              |
| `make rungui-uefi` | UEFI: build + boot in a GUI window under OVMF                       |
| `make debug-uefi`  | UEFI: boot paused and attach GDB                                    |
| `make usb`         | UEFI: build `build/esp_gpt.img` — GPT-wrapped ESP for a USB stick   |
| `make clean`       | Remove `build/`                                                     |

## Running on real hardware

NexusOS boots from a USB stick on a real x86-64 machine — no emulator — by
either path:

- **UEFI:** `make usb`, write `build/esp_gpt.img` raw to a stick (DD mode),
  disable **Secure Boot**, and boot the stick as a UEFI device.
- **Legacy BIOS:** `make bios`, write `build/os.bin` raw to a stick, enable
  **CSM/Legacy** and **USB Legacy Support**, and boot the stick's non-UEFI
  entry. If the firmware hangs setting the graphics mode, use `make text`.

Two caveats are structural rather than bugs. The kernel speaks PS/2 (i8042)
only, so under UEFI a USB keyboard stops working the moment `ExitBootServices`
runs (under CSM the firmware's PS/2 emulation keeps it alive). And the storage
driver targets a legacy IDE primary slave, so `ls`/`cat` won't find a
SATA/AHCI/NVMe disk.

See **[docs/HARDWARE.md](docs/HARDWARE.md)** for the full walkthrough — flashing,
firmware settings, what works vs. the caveats, and a troubleshooting table.

### Debugging

```sh
make debug        # BIOS
make debug-uefi   # UEFI
```

Launches QEMU paused (`-s -S`) and attaches GDB with the matching kernel ELF
loaded.
A breakpoint is set on `kernel_main`. `continue` to start.

## Graphics

Under BIOS, `boot.asm` asks the video BIOS for VBE mode 0x118 (1024x768) with
the linear-framebuffer bit set. Under UEFI, `boot.c` opens the Graphics Output
Protocol and walks its mode list for 1024x768 with a 32-bit BGR/RGB layout
(keeping the firmware's current mode if there is no match). Either loader then
leaves a small descriptor at physical `0x9700` for the kernel: a 64-bit
framebuffer base followed by pitch, width, height, bpp, and a valid flag. The
base is 64-bit because real UEFI can park the framebuffer above 4 GiB (VBE
bases are 32-bit, so `boot.asm` writes a zero high dword). If no mode could be
set, the valid flag is 0 and the kernel stays on the text/serial console.

In the kernel, `kernel/fb.c` reads the descriptor and `kernel/vmm.c` maps the
framebuffer explicitly when it builds the kernel's page tables, since it can lie
outside identity-mapped RAM. `kernel/gfx.c` draws onto any
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
│   ├── SETUP.md          # toolchain + build/run notes
│   ├── HARDWARE.md       # flashing, firmware settings, real-hardware caveats
│   └── ARCHITECTURE.md   # boot flow, memory model, scheduler, compositor
├── bootloader/
│   ├── boot.asm          # BIOS MBR: E820, VBE, int 13h LBA load, protected mode
│   ├── boot.c            # UEFI app: GOP, load kernel.bin, E820 snapshot, handoff
│   └── efi.h             # self-contained subset of the UEFI interfaces used
└── kernel/
    ├── linker.ld         # loads at 0x10000, exports __bss_start/_end, __kernel_end
    ├── kernel_entry.asm  # UEFI entry: GDT reload, BSS zero, call kernel_main
    ├── kernel_entry_bios.asm # BIOS entry: boot page tables, long mode, BSS zero
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
| `0x00000000–0x0009FFFF`  | Low memory — reserved                       |
| `0x00009000`             | E820 entry count (u32)                      |
| `0x00009008`+            | E820 entries, 24 bytes each (max 64)        |
| `0x00009700`             | Framebuffer descriptor handed to the kernel |
| `0x000B8000`             | Legacy VGA text framebuffer                 |
| `0x00010000`             | Kernel `_start`                             |
| `0x00010000–__kernel_end`| Kernel image                                |
| `__kernel_end`+          | PMM bitmap                                  |
| `0x00070000–0x00075FFF`  | Boot page tables (BIOS path only)           |
| `0x00090000`             | Boot stack top (set in the entry stub)      |
| Above                    | Free frames                                 |
| firmware-chosen          | Linear framebuffer (MMIO; base reported by the loader — VBE below 4 GiB, GOP possibly above) |

Virtual (after `vmm_init`):

| Range                    | Contents                                    |
| ------------------------ | ------------------------------------------- |
| `0x0000000000000000`+    | Identity map of physical RAM (2 MiB pages)  |
| `0x0000000200000000`+    | Kernel heap (4 KiB pages, demand-grown)     |
| framebuffer base         | Framebuffer, identity-mapped (2 MiB pages)  |

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
