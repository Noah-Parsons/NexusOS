#!/usr/bin/env bash
# NexusOS devcontainer setup
# Installs the toolchain needed to build and run NexusOS in a Codespace.

set -euo pipefail

echo "==> Updating apt indexes..."
sudo apt-get update -y

echo "==> Installing build toolchain..."
sudo apt-get install -y --no-install-recommends \
    build-essential \
    nasm \
    binutils \
    gdb \
    qemu-system-x86 \
    ovmf \
    gcc-mingw-w64-x86-64 \
    mtools

echo "==> Verifying tools..."
for tool in nasm gcc ld objcopy make qemu-system-x86_64 gdb x86_64-w64-mingw32-gcc; do
    if command -v "$tool" >/dev/null 2>&1; then
        printf "  %-22s %s\n" "$tool" "$($tool --version 2>&1 | head -n1)"
    else
        echo "  MISSING: $tool" >&2
        exit 1
    fi
done

echo ""
echo "==> Checking for OVMF firmware..."
if [ -f /usr/share/ovmf/OVMF.fd ]; then
    echo "    OVMF.fd            /usr/share/ovmf/OVMF.fd"
else
    echo "    NOTE: /usr/share/ovmf/OVMF.fd not found; pass OVMF=<path> to make."
fi

echo ""
echo "==> NexusOS dev environment ready."
echo "    Build:  make"
echo "    Run:    make run       (BIOS; Ctrl-A then X to quit QEMU)"
echo "            make run-uefi  (UEFI under OVMF)"
echo "    Debug:  make debug"
