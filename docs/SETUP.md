# NexusOS Development Setup

## Prerequisites

You need `nasm`, a host `gcc`/`ld`/`objcopy` (binutils), `qemu-system-x86_64`,
and `mtools` (the FAT12 disk image is built with `mformat`/`mcopy`, no root
required). `gdb` is optional, for `make debug`.

### Ubuntu/Debian
```bash
sudo apt-get install nasm gcc binutils make mtools qemu-system-x86 gdb
```

### macOS (with Homebrew)
```bash
brew install nasm qemu mtools x86_64-elf-binutils
```

### Windows
The build is Linux-oriented; the simplest path is **WSL** (Ubuntu), where the
commands above apply unchanged. A native MSYS2 setup also works:
```bash
pacman -S nasm gcc binutils mtools qemu
```

## Building NexusOS

```bash
make all
```

This will:
1. Assemble the BIOS bootloader and link the kernel with the BIOS entry stub
2. Combine them into `build/os.bin`
3. Build the UEFI loader (`build/BOOTX64.EFI`) and the UEFI kernel, and put
   both on a FAT32 EFI System Partition image, `build/esp.img`
4. Build the FAT12 disk image `build/fat.img` (with a couple of sample files)

`make bios` or `make uefi` builds just one path. The UEFI loader needs the
mingw-w64 cross-compiler (`gcc-mingw-w64-x86-64`).

## Running NexusOS

```bash
make rungui   # GUI in a QEMU window (needs a display, e.g. WSLg)
make run      # headless: serial console only, no window
```

QEMU boots with the OS image as the primary disk and the FAT12 image as a second
disk. `make run` is serial-only (`Ctrl-A x` to exit); `make rungui` opens a
window with the graphical desktop.

Other targets: `make text` (text-only image for real hardware whose firmware
hangs on VBE), `make debug` (boot paused under GDB), `make clean`. The same
run targets exist for UEFI under OVMF: `make run-uefi`, `make rungui-uefi`,
`make debug-uefi`, plus `make usb` for a flashable GPT image.

To capture the framebuffer to a PNG (handy on headless/WSL setups):

```bash
bash tools/screenshot.sh        # -> build/screen.png
```

## Debugging

```bash
make debug
```

This starts QEMU in debug mode and connects GDB.

## Troubleshooting

If `make all` fails:
- Ensure `nasm`, `gcc`, `ld`, and `mtools` are in your PATH
- Try `make clean` then `make all`

If QEMU doesn't start:
- Ensure `qemu-system-x86_64` is installed
- On WSL, a window needs WSLg; otherwise run headless and use
  `tools/screenshot.sh` to capture the framebuffer

## See also

- [HARDWARE.md](HARDWARE.md) — booting NexusOS on a real PC (USB, BIOS/CSM)
- [ARCHITECTURE.md](ARCHITECTURE.md) — how the OS is put together
- [Project README](https://github.com/GuiloScion/NexusOS#readme) — feature overview and build targets
