# Running NexusOS on real hardware

NexusOS boots two ways, from the same kernel:

- **UEFI** — a PE32+ loader (`bootloader/boot.c`) on a FAT32 EFI System
  Partition, booted via the removable-media path `\EFI\BOOT\BOOTX64.EFI`. This
  is the path for modern machines, including ones with no CSM at all.
- **Legacy BIOS** — a 512-byte MBR (`bootloader/boot.asm`) that loads the
  kernel with int 13h and climbs from real mode to long mode itself. This is the
  path *Build Your Own OS* describes, and the one to use on older machines or
  with CSM enabled.

Both loaders leave the kernel the same hand-off (E820 table at `0x9000`,
framebuffer descriptor at `0x9700`, kernel at `0x10000`), so everything after
the entry stub is identical. No GRUB, no shim, no emulator.

> **TL;DR (UEFI):** `make usb`, write `build/esp_gpt.img` raw to a USB stick
> (DD mode), disable Secure Boot, and boot the stick as a UEFI device.
>
> **TL;DR (BIOS):** `make bios`, write `build/os.bin` raw to a USB stick, enable
> **Legacy/CSM** + **USB Legacy Support**, and boot the stick's non-UEFI entry.
> If the screen hangs on the graphical mode, use `make text`.

## 1. Build an image

| Command     | Produces                                                              |
| ----------- | --------------------------------------------------------------------- |
| `make all`  | Both images below                                                     |
| `make uefi` | `build/esp.img` — a partitionless FAT32 "superfloppy" ESP             |
| `make usb`  | `build/esp_gpt.img` — the same volume wrapped in a GPT, for real UEFI |
| `make bios` | `build/os.bin` — raw disk image: 512-byte MBR followed by the kernel  |
| `make text` | `build/os.bin`, text-only — skips VBE for firmware that hangs on it   |

**For UEFI machines use `make usb`.** The plain `esp.img` has no partition
table; QEMU and some firmware will boot it anyway, but many real UEFI
implementations only enumerate a disk with a proper GPT and an ESP-typed
(`ef00`) partition. `make usb` builds exactly that, aligned at LBA 2048. Both
ESP images contain the loader at `\EFI\BOOT\BOOTX64.EFI` and the raw kernel at
`\kernel.bin`.

Building the UEFI loader needs the mingw-w64 cross-compiler
(`x86_64-w64-mingw32-gcc`), which emits the PE32+ binary UEFI requires without
gnu-efi or EDK2. `make usb` additionally needs `sgdisk` (the `gdisk` package).
The BIOS image needs only `nasm` and the host toolchain.

## 2. Write it to a USB stick

Images must be written **raw to the whole device**, not copied onto it as a file.

- **Rufus** (Windows): SELECT the image (rename `os.bin` to `os.img` if the
  picker filters by extension) → it switches to **DD Image mode** → START.
- **balenaEtcher**: Flash from file → the image → select USB → Flash.
- **dd** (Linux/macOS): `sudo dd if=build/esp_gpt.img of=/dev/sdX bs=1M conv=fsync`
  (or `build/os.bin`; double-check `/dev/sdX` — this erases the device).

> ⚠️ This erases the USB stick. Triple-check the target device.
>
> ⚠️ A flaky stick can write early sectors correctly and corrupt later ones,
> producing a hang partway through boot. If a boot misbehaves and the code is
> known-good, **try a different stick or port** before debugging the code.

## 3. Firmware settings

### UEFI image

1. **Secure Boot → Disabled.** The loader is unsigned; Secure Boot will refuse
   it. This is the make-or-break setting.
2. **Boot Mode → UEFI** (or "UEFI and Legacy").
3. Boot from the USB via the one-time boot menu (often F12/F11/F10/Esc). Pick
   the entry **with** the `UEFI:` prefix if the stick is listed twice.

"USB Legacy Support" does not help on this path — see the keyboard caveat below.

### BIOS image

1. **Boot Mode / CSM → enable Legacy / CSM** (turn off "UEFI only"). This is the
   make-or-break setting. Enabling CSM usually disables Secure Boot
   automatically.
2. **Boot Device Control → "Legacy OPROM"** (or "UEFI and Legacy") so the USB
   shows up as a legacy boot entry.
3. **USB Legacy Support → Enabled** (Advanced → USB Configuration). Required for
   a **USB keyboard** to work — the firmware emulates it as PS/2.
4. Boot from the USB (one-time boot menu). If the stick is listed twice, choose
   the entry **without** a `UEFI:` prefix.

## 4. What to expect

The UEFI loader prints to the firmware console before handing off:

```
NexusOS UEFI bootloader
[boot] GOP 0x...0400 x 0x...0300 @ 0x...
[boot] kernel loaded, 0x...  bytes
```

The BIOS loader prints `Booting NexusOS` (or `NexusOS text-mode boot` for the
`make text` image). From there both paths print the same kernel banner and
`[boot]`/`[pmm]`/`[vmm]`/`[heap]`/`[fb]`/`[sched]` log, then bring up the desktop
with a `nexus>` prompt in the terminal window. Type `help` for commands.

| Feature       | UEFI image                                         | BIOS image                                              |
| ------------- | -------------------------------------------------- | ------------------------------------------------------- |
| Boot → 64-bit | ✅ firmware hands over long mode already            | ✅ with CSM enabled; boot.asm + entry stub do the climb  |
| Graphics      | ✅ via GOP (1024×768×32 preferred)                  | ⚠️ only if the video BIOS supports VBE mode 0x118 (1024×768); otherwise text console, or use `make text` |
| Keyboard      | ⚠️ PS/2 (i8042) only — see below                    | ✅ usually, via USB Legacy Support                       |
| Mouse         | ⚠️ PS/2 on IRQ12 only                               | ⚠️ PS/2 only; USB-mouse legacy emulation is unreliable   |
| `ls` / `cat`  | ❌ needs a legacy IDE disk on the primary slave     | ❌ same                                                  |

**The UEFI keyboard caveat is the big one.** `kernel/keyboard.c` speaks PS/2
scancode set 1 through the i8042 controller, and there is no USB stack in the
kernel. The firmware's USB-keyboard emulation dies with `ExitBootServices`, so a
USB keyboard that worked in the boot menu will stop working the moment the
kernel starts. A machine with a real i8042 (or an embedded controller that
emulates one, as many laptops have) will work; a machine without one gives you a
running system you can't type into. Under CSM the firmware keeps emulating PS/2,
which is why the BIOS image usually has a working keyboard. Serial is mirrored
throughout, so a serial console is the reliable way in on any hardware.

The OS runs fully (shell, scheduler, memory, demos) in text mode regardless of
graphics or disk.

## Why these fixes exist (robustness notes)

Real firmware is messier than QEMU. NexusOS handles several real-world quirks:

- **LBA disk load (BIOS).** `boot.asm` loads the kernel with the int 13h LBA
  extensions (AH=42h), not CHS — CHS multi-track reads hang on many real
  USB-boot BIOSes.
- **Text-only build (BIOS).** Some CSM firmware *hangs inside* the VBE
  `int 0x10` call, and a hung BIOS call can't be recovered from. `make text`
  builds an image that skips VBE entirely and uses the text console.
- **E820 fallback.** Some firmware returns a memory map with **no usable
  region** (an ASUS board under CSM returned only reserved MMIO entries). The
  UEFI loader classifies `EfiConventionalMemory`, `EfiBootServicesCode/Data`
  and `EfiLoaderCode/Data` as usable and coalesces adjacent runs to stay under
  64 entries; if the kernel still sees no usable region, `pmm_init` assumes a
  conservative 128 MiB (`PMM_SYNTH_BASE`/`PMM_SYNTH_LEN`) so it can boot anyway.
- **64-bit framebuffer base.** Real UEFI can park the linear framebuffer above
  4 GiB, so the hand-off at `0x9700` stores the base as a `u64` (the BIOS loader
  writes VBE's 32-bit base with a zero high dword). `vmm.c` maps the
  framebuffer explicitly rather than assuming it falls inside the identity map.
- **Missing-disk handling.** ATA init treats a floating bus (status `0xFF`) as
  "no device" and bounds its wait loop, so a machine with no legacy IDE disk
  doesn't hang the boot.
- **Kernel size checks.** The UEFI loader refuses a `kernel.bin` larger than
  ~896 KiB so the copy to `0x10000` can't run past the 1 MiB line; the Makefile
  refuses a BIOS kernel larger than the 127 sectors `boot.asm` reads.

## Troubleshooting

| Symptom                                     | Likely cause / fix                                                              |
| ------------------------------------------- | -------------------------------------------------------------------------------- |
| Firmware skips the USB entirely (UEFI)      | Secure Boot still on, or the stick has no GPT — use `make usb`, not `esp.img`     |
| Firmware skips the USB entirely (BIOS)      | CSM/Legacy not enabled, or you picked the `UEFI:` boot entry                      |
| `[boot] FATAL: open \kernel.bin`            | `kernel.bin` missing from the ESP root — rebuild the image                        |
| `[boot] FATAL: ExitBootServices`            | Memory map kept changing across 8 retries; usually a firmware quirk               |
| Loader output, then nothing                 | Attach a serial console; the firmware console is gone after hand-off              |
| `[boot] no GOP; kernel will use text/serial`| Firmware exposes no Graphics Output Protocol; serial still works                  |
| Hangs right after `Booting NexusOS` (BIOS)  | The video BIOS hangs in VBE — use `make text`. (Or a flaky stick — re-flash.)     |
| Boots but keyboard does nothing             | UEFI: no i8042 on this machine — use serial. BIOS: enable USB Legacy Support      |
| `total = 0 MiB` / out of memory             | E820 came through empty; the 128 MiB fallback should catch this — rebuild         |
| `[ata] no device on primary channel`        | Expected on SATA/AHCI/NVMe machines — not an error; `ls`/`cat` just won't work    |

See [ARCHITECTURE.md](ARCHITECTURE.md) for how the boot path and these
subsystems fit together.
