.PHONY: all bios uefi run rungui debug text run-uefi rungui-uefi debug-uefi check-ovmf usb clean FORCE

# NexusOS boots two ways from one kernel:
#
#   Legacy BIOS  bootloader/boot.asm (512-byte MBR) + kernel/kernel_entry_bios.asm
#                -> build/os.bin, a raw disk image. This is the path the book
#                describes, so the plain targets (run, rungui, debug, text) use it.
#
#   UEFI         bootloader/boot.c (BOOTX64.EFI)  + kernel/kernel_entry.asm
#                -> build/esp.img (FAT32 EFI System Partition) and, via
#                `make usb`, build/esp_gpt.img for real hardware.
#                Targets: run-uefi, rungui-uefi, debug-uefi, usb.
#
# Both loaders leave the same hand-off in low memory (E820 table at 0x9000,
# framebuffer descriptor at 0x9700) and load the kernel at 0x10000, so every
# C file and every other assembly file is shared; only the entry stub differs.

NASM      := nasm
CC        := gcc
LD        := ld
OBJCOPY   := objcopy
QEMU      := qemu-system-x86_64
# mingw-w64 cross-compiler: emits the PE32+ binary UEFI needs directly, with no
# gnu-efi/EDK2 dependency. Install with: apt install gcc-mingw-w64-x86-64
EFICC     := x86_64-w64-mingw32-gcc

NASM_BOOT_FLAGS   := -f bin
NASM_KERNEL_FLAGS := -f elf64

# Extra nasm defines for the BIOS bootloader. `make text` sets -dTEXT_ONLY to
# skip VBE for firmware that hangs in the BIOS VBE calls under CSM.
BOOT_DEFINES ?=

CC_FLAGS := -ffreestanding -fno-pic -fno-stack-protector -fno-builtin \
            -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
            -m64 -Wall -Wextra -std=c11 -c

LD_FLAGS := -m elf_x86_64 -nostdlib -T kernel/linker.ld

# UEFI application: build as a relocatable PE (subsystem 10 = EFI application),
# entry efi_main, no host CRT.
EFI_CFLAGS  := -ffreestanding -fno-stack-protector -mno-red-zone -Wall -Wextra -std=c11
EFI_LDFLAGS := -nostdlib -Wl,-dll -shared -Wl,--subsystem,10 -e efi_main

# OVMF firmware image. The `ovmf` apt package installs this combined file;
# override on the command line if yours lives elsewhere:
#   make run-uefi OVMF=/usr/share/OVMF/OVMF_CODE.fd
OVMF ?= /usr/share/ovmf/OVMF.fd

BUILD_DIR := build

# --- Sources ---------------------------------------------------------
ENTRY_SRCS      := kernel/kernel_entry.asm kernel/kernel_entry_bios.asm
KERNEL_C_SRCS   := $(wildcard kernel/*.c)
KERNEL_ASM_SRCS := $(filter-out $(ENTRY_SRCS),$(wildcard kernel/*.asm))

KERNEL_C_OBJS   := $(patsubst kernel/%.c,$(BUILD_DIR)/%.o,$(KERNEL_C_SRCS))
KERNEL_ASM_OBJS := $(patsubst kernel/%.asm,$(BUILD_DIR)/%.o,$(KERNEL_ASM_SRCS))
SHARED_OBJS     := $(KERNEL_C_OBJS) $(KERNEL_ASM_OBJS)

# The entry object must be first so _start lands at 0x10000.
UEFI_ENTRY_OBJ := $(BUILD_DIR)/kernel_entry.o
BIOS_ENTRY_OBJ := $(BUILD_DIR)/kernel_entry_bios.o

# UEFI outputs
EFI_APP       := $(BUILD_DIR)/BOOTX64.EFI
KERNEL_ELF    := $(BUILD_DIR)/kernel.elf
KERNEL_BIN    := $(BUILD_DIR)/kernel.bin
ESP_IMAGE     := $(BUILD_DIR)/esp.img
ESP_GPT_IMAGE := $(BUILD_DIR)/esp_gpt.img

# BIOS outputs
BOOTLOADER      := $(BUILD_DIR)/bootloader.bin
BIOS_KERNEL_ELF := $(BUILD_DIR)/kernel_bios.elf
BIOS_KERNEL_BIN := $(BUILD_DIR)/kernel_bios.bin
OS_IMAGE        := $(BUILD_DIR)/os.bin

# Shared
FAT_IMAGE := $(BUILD_DIR)/fat.img

all: bios uefi

bios: $(OS_IMAGE) $(FAT_IMAGE)

uefi: $(ESP_IMAGE) $(FAT_IMAGE)

$(BUILD_DIR):
	mkdir -p $@

# --- Shared kernel objects --------------------------------------------
$(BUILD_DIR)/%.o: kernel/%.c | $(BUILD_DIR)
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/%.o: kernel/%.asm | $(BUILD_DIR)
	$(NASM) $(NASM_KERNEL_FLAGS) $< -o $@

# --- BIOS: MBR bootloader + kernel ------------------------------------
# The bootloader depends on FORCE so it is always reassembled. make can't see
# changes to BOOT_DEFINES (e.g. `make bios` vs `make text`), so without this a
# switch between GUI and text-only builds would silently reuse the stale image.
FORCE:

$(BOOTLOADER): bootloader/boot.asm FORCE | $(BUILD_DIR)
	$(NASM) $(NASM_BOOT_FLAGS) $(BOOT_DEFINES) $< -o $@

$(BIOS_KERNEL_ELF): $(BIOS_ENTRY_OBJ) $(SHARED_OBJS) kernel/linker.ld
	$(LD) $(LD_FLAGS) $(BIOS_ENTRY_OBJ) $(SHARED_OBJS) -o $@

$(BIOS_KERNEL_BIN): $(BIOS_KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

# boot.asm reads 127 sectors (63.5 KiB) of kernel; fail loudly if it outgrows that.
$(OS_IMAGE): $(BOOTLOADER) $(BIOS_KERNEL_BIN)
	@test $$(stat -c %s $(BIOS_KERNEL_BIN)) -le 65024 || \
	    { echo "kernel_bios.bin exceeds the 127 sectors boot.asm loads"; exit 1; }
	cat $(BOOTLOADER) $(BIOS_KERNEL_BIN) > $@
	@truncate -s '>1M' $@

# --- UEFI: loader + kernel --------------------------------------------
$(EFI_APP): bootloader/boot.c bootloader/efi.h | $(BUILD_DIR)
	$(EFICC) $(EFI_CFLAGS) $(EFI_LDFLAGS) -o $@ bootloader/boot.c

$(KERNEL_ELF): $(UEFI_ENTRY_OBJ) $(SHARED_OBJS) kernel/linker.ld
	$(LD) $(LD_FLAGS) $(UEFI_ENTRY_OBJ) $(SHARED_OBJS) -o $@

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

# A FAT32 volume holding the loader at the removable-media default path
# (\EFI\BOOT\BOOTX64.EFI) plus the raw kernel at the root. Built with mtools
# so we need neither root nor a loopback mount.
$(ESP_IMAGE): $(EFI_APP) $(KERNEL_BIN) | $(BUILD_DIR)
	dd if=/dev/zero of=$@ bs=1M count=64 status=none
	mformat -i $@ -F ::
	mmd    -i $@ ::/EFI
	mmd    -i $@ ::/EFI/BOOT
	mcopy  -i $@ $(EFI_APP)    ::/EFI/BOOT/BOOTX64.EFI
	mcopy  -i $@ $(KERNEL_BIN) ::/kernel.bin

# The plain esp.img above is a partitionless "superfloppy" -- fine for QEMU and
# most firmware. Some real UEFI implementations only boot from a GPT disk with a
# proper EFI System Partition, so `make usb` wraps the same FAT32 volume in a GPT
# as a single ESP-typed partition (aligned at LBA 2048). No root/loopback needed;
# sgdisk edits the image file directly and we dd the filesystem into the slot.
$(ESP_GPT_IMAGE): $(ESP_IMAGE) | $(BUILD_DIR)
	dd if=/dev/zero of=$@ bs=1M count=67 status=none
	sgdisk -o -n 1:2048:+64M -t 1:ef00 -c 1:"NexusOS ESP" $@ >/dev/null
	dd if=$(ESP_IMAGE) of=$@ bs=512 seek=2048 conv=notrunc status=none

usb: $(ESP_GPT_IMAGE)
	@echo "GPT USB image ready: $(ESP_GPT_IMAGE)"
	@echo "Flash it raw to your USB stick (Rufus 'DD Image' mode, balenaEtcher, or dd)."

# --- Secondary FAT12 disk (ATA + FAT driver test) --------------------
# 1.44 MB floppy image on the primary-slave IDE channel, as the driver expects.
$(FAT_IMAGE): | $(BUILD_DIR)
	dd if=/dev/zero of=$@ bs=1024 count=1440 status=none
	mformat -i $@ -f 1440 ::
	@printf 'hello, world from a FAT12 filesystem\n' > $(BUILD_DIR)/_hello.txt
	@printf 'NexusOS storage stack test file.\nIf you can read this, ATA + FAT12 work.\n' > $(BUILD_DIR)/_readme.txt
	mcopy -i $@ $(BUILD_DIR)/_hello.txt  ::HELLO.TXT
	mcopy -i $@ $(BUILD_DIR)/_readme.txt ::README.TXT

# --- Run: legacy BIOS (as in the book) ------------------------------
BIOS_QEMU_FLAGS := -drive format=raw,file=$(OS_IMAGE),if=ide,index=0 \
                   -drive format=raw,file=$(FAT_IMAGE),if=ide,index=1 \
                   -m 256M -no-reboot

run: bios
	$(QEMU) $(BIOS_QEMU_FLAGS) -serial stdio -display none

# Interactive GUI window (needs a display, e.g. WSLg on Windows). Click into
# the window to grab the mouse; Ctrl-Alt-G releases it.
rungui: bios
	$(QEMU) $(BIOS_QEMU_FLAGS) -serial stdio -display gtk

debug: bios
	$(QEMU) $(BIOS_QEMU_FLAGS) -serial stdio -display none -s -S &
	gdb -ex "target remote localhost:1234" \
	    -ex "symbol-file $(BIOS_KERNEL_ELF)" \
	    -ex "break kernel_main"

# Text-only BIOS image: skips VBE for firmware that hangs in the BIOS VBE calls.
# Cleans first so the bootloader is reassembled with the new define.
text:
	$(MAKE) clean
	$(MAKE) BOOT_DEFINES=-dTEXT_ONLY bios

# --- Run: UEFI under OVMF -------------------------------------------
# ESP on the primary master, the FAT12 test disk on the primary slave (where
# the ATA driver looks for it).
UEFI_QEMU_FLAGS := -bios $(OVMF) \
                   -drive format=raw,file=$(ESP_IMAGE),if=ide,index=0 \
                   -drive format=raw,file=$(FAT_IMAGE),if=ide,index=1 \
                   -m 256M -no-reboot

check-ovmf:
	@test -f $(OVMF) || { echo "OVMF firmware not found at $(OVMF); install the 'ovmf' package or set OVMF=..."; exit 1; }

run-uefi: uefi check-ovmf
	$(QEMU) $(UEFI_QEMU_FLAGS) -serial stdio -display none

rungui-uefi: uefi check-ovmf
	$(QEMU) $(UEFI_QEMU_FLAGS) -serial stdio -display gtk

debug-uefi: uefi check-ovmf
	$(QEMU) $(UEFI_QEMU_FLAGS) -serial stdio -display none -s -S &
	gdb -ex "target remote localhost:1234" \
	    -ex "symbol-file $(KERNEL_ELF)" \
	    -ex "break kernel_main"

clean:
	rm -rf $(BUILD_DIR)
