# NexusOS architecture

A tour of how NexusOS is put together, from the first instruction the firmware
runs to the compositing window manager. Everything here is original — no GRUB,
Limine, or tutorial framework.

## Boot flow

```
BIOS ──> MBR (bootloader/boot.asm, 512 bytes, real mode)
         ├─ collect E820 memory map            -> 0x9000 (count) / 0x9008 (entries)
         ├─ set VBE 1024x768 LFB mode (int 10h) -> descriptor at 0x9700
         ├─ load kernel to 0x10000 (int 13h LBA, AH=42h)
         ├─ enable A20, load GDT, enter 32-bit protected mode
         └─ far-jump to 0x10000
       ──> kernel_entry_bios.asm (32-bit -> 64-bit)
           ├─ build temporary identity page tables at 0x70000 (0..4 GiB, 2 MiB pages)
           ├─ enable PAE + long mode (EFER.LME) + paging
           ├─ zero .bss
           └─ call kernel_main()

UEFI ──> BOOTX64.EFI (bootloader/boot.c, PE32+, long mode)
         ├─ open GOP, pick 1024x768x32 if offered    -> descriptor at 0x9700
         ├─ read \kernel.bin from the ESP, copy to 0x10000
         ├─ GetMemoryMap → translate to E820        -> 0x9000 / 0x9008
         ├─ ExitBootServices
         └─ jump to 0x10000
       ──> kernel_entry.asm (already 64-bit)
           ├─ load the kernel's GDT, reload segment registers and CS
           ├─ zero .bss
           └─ call kernel_main()

Both paths then run the same code:
       ──> kernel.c: kernel_main()
           console → idt → pic → pit → keyboard → sti
           → pmm → vmm → kmalloc → scheduler + demo tasks
           → ata/fat → mouse → wm (compositor) → shell()
```

On the BIOS path the MBR does the bare minimum in real mode (where BIOS services
are still available), because once we leave real mode the BIOS is gone, and the
entry stub handles the protected→long mode transition. On the UEFI path the
firmware has already done that climb, so the loader's job is to translate what
UEFI reports into the same hand-off the MBR writes. Either way, the real page tables are built
later by the VMM once the physical allocator exists.

## Memory model

**Physical layout**

| Range                     | Contents                                  |
| ------------------------- | ----------------------------------------- |
| `0x00000–0x9FFFF`         | Low memory (IVT, BDA) — reserved          |
| `0x07C00`                 | MBR entry                                  |
| `0x09000`+                | E820 count + entries                      |
| `0x09700` / `0x09800`     | Framebuffer descriptor / VBE scratch      |
| `0x0B8000`                | Legacy VGA text buffer                    |
| `0x10000`–`__kernel_end`  | Kernel image (+ BSS)                      |
| `__kernel_end`+           | PMM bitmap                                |
| above                     | Free frames                               |
| `~0xFD000000`             | VESA linear framebuffer (MMIO)            |

**Virtual layout (after `vmm_init`)**

| Range               | Contents                                   |
| ------------------- | ------------------------------------------ |
| `0x0`+              | Identity map of RAM (2 MiB pages)          |
| `0x200000000`+      | Kernel heap (4 KiB pages, demand-grown)    |
| `~0xFD000000`       | Framebuffer, identity-mapped (4 KiB pages) |

**The three allocators**

- **PMM** (`pmm.c`) — bitmap of 4 KiB physical frames. Parses E820 to find the
  top of usable RAM, marks everything used, then frees the usable regions and
  re-reserves low memory + the kernel + the bitmap. Falls back to a synthetic
  128 MiB region if firmware reports no usable RAM.
- **VMM** (`vmm.c`) — full 4-level paging. Identity-maps RAM with 2 MiB pages,
  installs a kernel-owned PML4, and exposes 4 KiB `map`/`unmap`/`translate`.
- **kmalloc** (`kmalloc.c`) — first-fit doubly-linked free list at virtual
  `0x200000000`, 16-byte aligned, demand-grown via `pmm_alloc_frame` + `vmm_map`,
  coalescing on free.

## Interrupts & timing

- **IDT** (`idt.c`, `idt_stubs.asm`) — 256 entries, every CPU exception vector
  handled, a panic dump (RIP/CR2/registers), and an `irq_register` table for
  device drivers.
- **PIC** (`pic.c`) — the 8259A pair remapped to vectors `0x20–0x2F`.
- **PIT** (`pit.c`) — channel-0 timer at 100 Hz; IRQ0 drives the tick counter
  and the scheduler's preemption.
- **Keyboard** (`keyboard.c`) — PS/2 scancode set 1, modifier state, a 256-byte
  ring buffer, blocking + non-blocking reads (with a serial fallback).

## Concurrency: scheduler + synchronization

- **Scheduler** (`sched.c`, `sched_switch.asm`) — preemptive round-robin. The
  PIT tick triggers a context switch (registers saved/restored in assembly).
  Tasks can sleep (`sched_sleep_ms`) or block; an idle task runs the shell.
- **Synchronization** (`sync.c`) — mutex, counting semaphore, and condition
  variable, all built on the scheduler's BLOCKED state with FIFO-on-release
  hand-off. Critical sections are made atomic with `cli`/`sti`; `INT 0x80`
  yields without requiring interrupts.

The `tasks` and `prod` shell commands run live demos that assert the mutex and
semaphore invariants across hundreds of thousands of context switches.

## Storage stack

- **ATA** (`ata.c`) — PIO-mode IDE driver for the primary-channel slave.
  Interrupt-driven reads (blocks on a semaphore posted by IRQ14). Treats a
  floating bus (`0xFF`) as "no device" so a diskless machine boots cleanly.
- **FAT12** (`fat.c`) — read-only. Parses the BPB, caches the FAT, walks the
  cluster chain. Backs the `ls` and `cat` shell commands.

## Graphics & the window manager

The display stack is layered so drawing code is independent of the screen:

- **fb** (`fb.c`) — maps the VESA linear framebuffer (above identity-mapped RAM)
  and exposes raw pixel access.
- **gfx** (`gfx.c`) — a `surface_t` (any pixel buffer) plus `pixel`/`fill`/
  `blit` and scaled 8×8 text (`font8x8.h`). The same code draws to the screen or
  to an off-screen buffer.
- **fbcon** (`fbcon.c`) — the text console as a **character grid** (not direct
  drawing). `console_*` output updates the grid; a version counter lets the
  compositor skip re-rendering when nothing changed.
- **wm** (`wm.c`) — the compositor + window manager. It owns an off-screen back
  buffer and a list of z-ordered windows. A compositor task:
  1. **composes** the scene (desktop → windows, with the terminal window
     rendering the fbcon grid) into the back buffer whenever something changes;
  2. **presents** every frame: blit the back buffer to the screen, then paint
     the mouse cursor as an overlay on top.

  Because the cursor is a per-frame overlay (never stored in the scene), it
  never clobbers content underneath. Mouse clicks focus/raise windows; title
  bars drag; close/minimize buttons and a taskbar (with an uptime clock) round
  it out.

- **mouse** (`mouse.c`) — PS/2 mouse on IRQ12; 3-byte packet parsing, position
  clamped to the screen, button state. The compositor reads this for the cursor
  and window interaction.

If the framebuffer isn't available (VBE failed / text-only build), `console_*`
falls back to the VGA text buffer and the WM is skipped.

## Code map

```
bootloader/boot.asm   BIOS MBR: E820, VBE, LBA kernel load, protected mode
bootloader/boot.c     UEFI loader: GOP, kernel load, E820 translation, hand-off
bootloader/efi.h      the subset of UEFI interfaces boot.c uses
kernel/
  kernel_entry_bios.asm  BIOS: 32→64-bit trampoline, paging, BSS, call kernel_main
  kernel_entry.asm    UEFI: GDT reload, BSS, call kernel_main
  kernel.c            boot sequence + shell
  types.h io.h        primitive types; port I/O, control registers
  string.{c,h}        mem*/strlen + numeric formatters
  console.{c,h}       unified output: serial + VGA text + framebuffer console
  idt.{c,h} idt_stubs.asm   IDT, dispatch, panic, irq_register
  pic.{c,h} pit.{c,h} keyboard.{c,h}   8259A, 100 Hz timer, PS/2 keyboard
  pmm.{c,h}           bitmap physical frame allocator (E820)
  vmm.{c,h}           4-level paging, map/unmap/translate
  kmalloc.{c,h}       first-fit kernel heap
  sched.{c,h} sched_switch.asm   preemptive scheduler + context switch
  sync.{c,h}          mutex, semaphore, condvar
  ata.{c,h} fat.{c,h} PIO ATA driver; read-only FAT12
  fb.{c,h}            framebuffer mapping + raw pixels
  gfx.{c,h} font8x8.h surface drawing (pixel/fill/blit/text) + 8×8 font
  fbcon.{c,h}         text console as a character grid
  mouse.{c,h}         PS/2 mouse (IRQ12)
  wm.{c,h}            compositing window manager + cursor + taskbar
  linker.ld           loads at 0x10000; exports __bss_start/_end, __kernel_end
tools/screenshot.sh   boot headless and capture the framebuffer to a PNG
```

## Design notes

- **Single address space, ring 0.** Everything (kernel + "tasks") runs in the
  kernel's address space at ring 0 today. User mode (ring 3 + per-process
  address spaces + an ELF loader) is the next major step.
- **The Makefile auto-discovers sources** — drop a new `.c`/`.asm` under
  `kernel/` and it's picked up; no Makefile edits for routine additions.
- **Real-hardware resilience** lives mostly in the boot path and PMM — see
  [HARDWARE.md](HARDWARE.md).
