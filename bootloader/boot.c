/* boot.c -- NexusOS UEFI bootloader (PE32+ EFI application).
 *
 * Replaces the old 512-byte BIOS/MBR stub (boot.asm). The firmware hands us
 * control already in 64-bit long mode with identity-mapped low memory, so
 * there is no real->protected->long climb to perform. This loader:
 *
 *   1. Opens the Graphics Output Protocol, selects a linear-framebuffer mode
 *      (prefers 1024x768x32), and records the framebuffer descriptor.
 *   2. Opens the FAT volume it was loaded from and reads \kernel.bin into a
 *      buffer, plus reserves the fixed load region at physical 0x10000.
 *   3. Snapshots the UEFI memory map, exits boot services, then -- with the
 *      firmware out of the way -- writes the handoff structures the kernel
 *      already expects:
 *          0x9000  : E820 entry count           (uint32)
 *          0x9008  : E820 entries, 24 bytes each (base,len : u64; type,attr : u32)
 *          0x9700  : framebuffer descriptor      (lfb,pitch,w,h,bpp,valid : u32[6])
 *      copies the kernel to 0x10000, and jumps to it.
 *
 * The kernel side (pmm.c, fb.c, kernel_entry.asm) is therefore unchanged apart
 * from the entry stub no longer needing the 32->64 bit transition.
 */

#include "efi.h"

/* ---- handoff layout shared with the kernel -------------------------------
 * Mirror of E820_COUNT_ADDR / E820_ENTRIES_ADDR (pmm.h) and FB_INFO_ADDR
 * (fb.c). Kept as literals here so the bootloader has no kernel includes. */
#define E820_COUNT_ADDR    0x9000UL
#define E820_ENTRIES_ADDR  0x9008UL
#define FB_INFO_ADDR       0x9700UL
#define KERNEL_LOAD_ADDR   0x10000UL
#define MAX_E820_ENTRIES   64        /* 64*24 = 1536 bytes, ends well below 0x9700 */

typedef struct __attribute__((packed)) {
    UINT64 base;
    UINT64 length;
    UINT32 type;          /* 1 = usable, 2 = reserved (matches the kernel) */
    UINT32 acpi_attrs;
} e820_entry_t;

/* ---- freestanding helpers (mingw may emit calls to these) ---------------- */
void *memset(void *dst, int c, unsigned long n) {
    UINT8 *p = dst;
    while (n--) *p++ = (UINT8)c;
    return dst;
}
void *memcpy(void *dst, const void *src, unsigned long n) {
    UINT8 *d = dst; const UINT8 *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

static EFI_SYSTEM_TABLE  *ST;
static EFI_BOOT_SERVICES *BS;

/* ---- tiny text output (ASCII -> CHAR16, no wide literals needed) ---------- */
static void print(const char *s) {
    CHAR16 buf[128];
    UINTN i = 0;
    while (*s && i < 126) {
        if (*s == '\n') buf[i++] = (CHAR16)'\r';
        buf[i++] = (CHAR16)(UINT8)*s++;
    }
    buf[i] = 0;
    ST->ConOut->OutputString(ST->ConOut, buf);
}

static void print_hex(UINT64 v) {
    static const char digits[] = "0123456789ABCDEF";
    char out[19];
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; i++)
        out[2 + i] = digits[(v >> ((15 - i) * 4)) & 0xF];
    out[18] = 0;
    print(out);
}

static void die(const char *msg) {
    print("[boot] FATAL: ");
    print(msg);
    print("\n[boot] halted.\n");
    for (;;) __asm__ volatile ("cli; hlt");
}

/* ASCII -> CHAR16 into a caller buffer, for file names. */
static void to_wide(const char *s, CHAR16 *out) {
    while (*s) *out++ = (CHAR16)(UINT8)*s++;
    *out = 0;
}

/* ---- framebuffer snapshot (filled before ExitBootServices) --------------- */
static UINT64 fb_base;
static UINT32 fb_pitch, fb_width, fb_height, fb_valid;

static void setup_gop(void) {
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;

    if (EFI_IS_ERROR(BS->LocateProtocol(&gop_guid, 0, (void **)&gop)) || !gop) {
        print("[boot] no GOP; kernel will use text/serial\n");
        fb_valid = 0;
        return;
    }

    /* Prefer 1024x768 with a 32-bit BGR/RGB layout; otherwise keep whatever
     * mode the firmware left active. */
    UINT32 best = gop->Mode->Mode;
    int    found = 0;
    for (UINT32 m = 0; m < gop->Mode->MaxMode && !found; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        UINTN size;
        if (EFI_IS_ERROR(gop->QueryMode(gop, m, &size, &info))) continue;
        if (info->HorizontalResolution == 1024 &&
            info->VerticalResolution   == 768  &&
            (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor ||
             info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor)) {
            best  = m;
            found = 1;
        }
    }
    gop->SetMode(gop, best);   /* ignore failure: current mode still valid */

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = gop->Mode->Info;
    fb_base   = gop->Mode->FrameBufferBase;
    fb_width  = mi->HorizontalResolution;
    fb_height = mi->VerticalResolution;
    fb_pitch  = mi->PixelsPerScanLine * 4;   /* 32 bpp -> 4 bytes/pixel */
    fb_valid  = (mi->PixelFormat == PixelBlueGreenRedReserved8BitPerColor ||
                 mi->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) ? 1 : 0;

    print("[boot] GOP ");
    print_hex(fb_width); print(" x "); print_hex(fb_height);
    print(" @ "); print_hex(fb_base); print("\n");
}

/* ---- load \kernel.bin from the boot volume -------------------------------
 * Returns the file in a pool buffer; *out_size gets its length. */
static void load_kernel(EFI_HANDLE image, void **out_buf, UINTN *out_size) {
    EFI_GUID li_guid  = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fs_guid  = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID fi_guid  = EFI_FILE_INFO_GUID;

    EFI_LOADED_IMAGE_PROTOCOL       *li = 0;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    EFI_FILE_PROTOCOL               *root = 0, *file = 0;

    if (EFI_IS_ERROR(BS->HandleProtocol(image, &li_guid, (void **)&li)))
        die("LoadedImage protocol");
    if (EFI_IS_ERROR(BS->HandleProtocol(li->DeviceHandle, &fs_guid, (void **)&fs)))
        die("SimpleFileSystem protocol");
    if (EFI_IS_ERROR(fs->OpenVolume(fs, &root)))
        die("OpenVolume");

    CHAR16 name[16];
    to_wide("kernel.bin", name);
    if (EFI_IS_ERROR(root->Open(root, &file, name, EFI_FILE_MODE_READ, 0)))
        die("open \\kernel.bin");

    /* File size via GetInfo(EFI_FILE_INFO). */
    UINT8 info_buf[256];
    UINTN info_size = sizeof(info_buf);
    if (EFI_IS_ERROR(file->GetInfo(file, &fi_guid, &info_size, info_buf)))
        die("kernel GetInfo");
    UINT64 ksize = ((EFI_FILE_INFO *)info_buf)->FileSize;
    if (ksize == 0 || ksize > 0xE0000)     /* sanity: < ~896 KiB, fits below 1 MiB */
        die("kernel size out of range");

    void *buf = 0;
    if (EFI_IS_ERROR(BS->AllocatePool(EfiLoaderData, ksize, &buf)) || !buf)
        die("AllocatePool for kernel");

    UINTN read = ksize;
    if (EFI_IS_ERROR(file->Read(file, &read, buf)) || read != ksize)
        die("read kernel");
    file->Close(file);
    root->Close(root);

    /* Reserve the fixed load region so the firmware won't stage anything there
     * before we copy in (best effort: on QEMU/OVMF 0x10000 is free). */
    EFI_PHYSICAL_ADDRESS at = KERNEL_LOAD_ADDR;
    UINTN pages = (ksize + 0xFFF) / 0x1000;
    if (EFI_IS_ERROR(BS->AllocatePages(AllocateAddress, EfiLoaderData, pages, &at)))
        print("[boot] warn: could not reserve 0x10000 (copying anyway)\n");

    *out_buf  = buf;
    *out_size = ksize;
    print("[boot] kernel loaded, "); print_hex(ksize); print(" bytes\n");
}

/* ---- E820 classification ------------------------------------------------- */
static UINT32 usable_type(UINT32 t) {
    /* After ExitBootServices these all become free RAM for the kernel. */
    switch (t) {
    case EfiConventionalMemory:
    case EfiBootServicesCode:
    case EfiBootServicesData:
    case EfiLoaderCode:
    case EfiLoaderData:
        return 1;                 /* usable */
    default:
        return 2;                 /* reserved */
    }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab) {
    ST = systab;
    BS = systab->BootServices;

    if (ST->ConOut && ST->ConOut->ClearScreen) ST->ConOut->ClearScreen(ST->ConOut);
    print("NexusOS UEFI bootloader\n");

    setup_gop();

    void *kbuf = 0; UINTN ksize = 0;
    load_kernel(image, &kbuf, &ksize);

    /* Grab the memory map, then exit boot services. GetMemoryMap must be the
     * last allocation-sensitive call before ExitBootServices, so we size the
     * buffer with slack up front and never allocate inside the retry loop. */
    UINTN map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_ver = 0;
    BS->GetMemoryMap(&map_size, 0, &map_key, &desc_size, &desc_ver);
    map_size += 8 * desc_size;               /* room for growth between calls */
    EFI_MEMORY_DESCRIPTOR *map = 0;
    if (EFI_IS_ERROR(BS->AllocatePool(EfiLoaderData, map_size, (void **)&map)))
        die("AllocatePool for memory map");

    UINTN final_size = 0;
    for (int tries = 0; tries < 8; tries++) {
        final_size = map_size;
        EFI_STATUS s = BS->GetMemoryMap(&final_size, map, &map_key, &desc_size, &desc_ver);
        if (EFI_IS_ERROR(s)) die("GetMemoryMap");
        s = BS->ExitBootServices(image, map_key);
        if (!EFI_IS_ERROR(s)) break;         /* success */
        if (tries == 7) die("ExitBootServices");
        /* else: map changed under us; retry with a fresh key (no alloc). */
    }

    /* -------- firmware is gone; only CPU/memory operations from here -------- */
    __asm__ volatile ("cli");

    /* Build the E820 table the kernel's pmm expects, coalescing adjacent
     * same-class runs so we stay under MAX_E820_ENTRIES. */
    e820_entry_t *e = (e820_entry_t *)E820_ENTRIES_ADDR;
    UINT32 count = 0;
    for (UINTN off = 0; off + desc_size <= final_size; off += desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + off);
        UINT64 len = d->NumberOfPages * 0x1000ULL;
        if (len == 0) continue;
        UINT32 type = usable_type(d->Type);
        UINT64 base = d->PhysicalStart;
        if (count > 0 && e[count - 1].type == type &&
            e[count - 1].base + e[count - 1].length == base) {
            e[count - 1].length += len;      /* merge with previous run */
        } else if (count < MAX_E820_ENTRIES) {
            e[count].base       = base;
            e[count].length     = len;
            e[count].type       = type;
            e[count].acpi_attrs = 1;
            count++;
        }
    }
    *(volatile UINT32 *)E820_COUNT_ADDR = count;

    /* Framebuffer descriptor for fb.c. Layout at 0x9700:
     *   0x9700 : u64 physical LFB base   (64-bit: real UEFI may park it >4 GiB)
     *   0x9708 : u32 pitch (bytes/line)
     *   0x970C : u32 width
     *   0x9710 : u32 height
     *   0x9714 : u32 bpp
     *   0x9718 : u32 valid */
    *(volatile UINT64 *)FB_INFO_ADDR = fb_base;
    volatile UINT32 *fbinfo = (volatile UINT32 *)(FB_INFO_ADDR + 8);
    fbinfo[0] = fb_pitch;
    fbinfo[1] = fb_width;
    fbinfo[2] = fb_height;
    fbinfo[3] = 32;
    fbinfo[4] = fb_valid;

    /* Place the kernel at its linked load address and enter it. */
    memcpy((void *)KERNEL_LOAD_ADDR, kbuf, ksize);
    ((void (*)(void))KERNEL_LOAD_ADDR)();

    for (;;) __asm__ volatile ("cli; hlt");   /* never reached */
    return EFI_SUCCESS;
}
