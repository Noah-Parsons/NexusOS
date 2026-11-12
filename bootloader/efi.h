/* efi.h -- a minimal, self-contained subset of the UEFI interfaces the
 * NexusOS bootloader needs. No gnu-efi, no EDK2: just the handful of types,
 * GUIDs, protocols and boot-service entry points used by boot.c.
 *
 * Only the members actually called are given real prototypes; the rest are
 * kept as opaque pointers so that every struct still matches the byte layout
 * the firmware expects (the offsets are what matter, not the names).
 *
 * All firmware calls use the Microsoft x64 calling convention, so every
 * function-pointer type is tagged EFIAPI. When built with the mingw-w64
 * cross-compiler that is already the default, but tagging keeps the header
 * correct under a SysV gcc too (used for a compile-only type check).
 */
#ifndef NEXUS_EFI_H
#define NEXUS_EFI_H

#define EFIAPI __attribute__((ms_abi))

typedef unsigned char       UINT8;
typedef unsigned short      UINT16;
typedef unsigned int        UINT32;
typedef unsigned long long  UINT64;
typedef signed   long long  INT64;
typedef UINT8               BOOLEAN;
typedef UINT16              CHAR16;
typedef UINT64              UINTN;      /* natural width on x86-64 */
typedef void               *EFI_HANDLE;
typedef void               *EFI_EVENT;
typedef UINTN               EFI_STATUS;
typedef UINT64              EFI_PHYSICAL_ADDRESS;
typedef UINT64              EFI_VIRTUAL_ADDRESS;

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

/* ---- status codes ---------------------------------------------------- */
#define EFI_ERR(x)              (0x8000000000000000ULL | (x))
#define EFI_SUCCESS             0ULL
#define EFI_INVALID_PARAMETER   EFI_ERR(2)
#define EFI_UNSUPPORTED         EFI_ERR(3)
#define EFI_BUFFER_TOO_SMALL    EFI_ERR(5)
#define EFI_NOT_FOUND           EFI_ERR(14)
#define EFI_IS_ERROR(s)         (((UINT64)(s)) & 0x8000000000000000ULL)

/* ---- memory services ------------------------------------------------- */
typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef struct {
    UINT32               Type;
    UINT32               Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    UINT64               NumberOfPages;
    UINT64               Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ---- simple text output --------------------------------------------- */
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void      *Reset;
    EFI_STATUS (EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                      CHAR16 *String);
    void      *TestString;
    void      *QueryMode;
    void      *SetMode;
    void      *SetAttribute;
    EFI_STATUS (EFIAPI *ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
    void      *SetCursorPosition;
    void      *EnableCursor;
    void      *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* ---- graphics output protocol --------------------------------------- */
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32                    Version;
    UINT32                    HorizontalResolution;
    UINT32                    VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK         PixelInformation;
    UINT32                    PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32                                MaxMode;
    UINT32                                Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN                                 SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                  FrameBufferBase;
    UINTN                                 FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *QueryMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                                   UINT32 ModeNumber, UINTN *SizeOfInfo,
                                   EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
    EFI_STATUS (EFIAPI *SetMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                                 UINT32 ModeNumber);
    void                              *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ---- loaded image protocol ------------------------------------------ */
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct {
    UINT32       Revision;
    EFI_HANDLE   ParentHandle;
    void        *SystemTable;
    EFI_HANDLE   DeviceHandle;      /* the volume we were loaded from */
    void        *FilePath;
    void        *Reserved;
    UINT32       LoadOptionsSize;
    void        *LoadOptions;
    void        *ImageBase;
    UINT64       ImageSize;
    EFI_MEMORY_TYPE ImageCodeType;
    EFI_MEMORY_TYPE ImageDataType;
    void        *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* ---- simple file system + file protocol ----------------------------- */
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_FILE_INFO_GUID \
    { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_FILE_MODE_READ  0x0000000000000001ULL

typedef struct { UINT16 Y; UINT8 Mo, D, H, Mi, S, Pad1; UINT32 Ns;
                 INT64 Pad2; } EFI_TIME;   /* 16 bytes; fields unused here */

typedef struct {
    UINT64   Size;
    UINT64   FileSize;
    UINT64   PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    UINT64   Attribute;
    CHAR16   FileName[1];
} EFI_FILE_INFO;

typedef struct EFI_FILE_PROTOCOL {
    UINT64      Revision;
    EFI_STATUS (EFIAPI *Open)(struct EFI_FILE_PROTOCOL *This,
                              struct EFI_FILE_PROTOCOL **NewHandle,
                              CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);
    EFI_STATUS (EFIAPI *Close)(struct EFI_FILE_PROTOCOL *This);
    void       *Delete;
    EFI_STATUS (EFIAPI *Read)(struct EFI_FILE_PROTOCOL *This,
                              UINTN *BufferSize, void *Buffer);
    void       *Write;
    EFI_STATUS (EFIAPI *GetPosition)(struct EFI_FILE_PROTOCOL *This, UINT64 *Position);
    EFI_STATUS (EFIAPI *SetPosition)(struct EFI_FILE_PROTOCOL *This, UINT64 Position);
    EFI_STATUS (EFIAPI *GetInfo)(struct EFI_FILE_PROTOCOL *This,
                                 EFI_GUID *InformationType,
                                 UINTN *BufferSize, void *Buffer);
    void       *SetInfo;
    void       *Flush;
} EFI_FILE_PROTOCOL;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64      Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                    EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* ---- boot services --------------------------------------------------- */
typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct {
    EFI_TABLE_HEADER Hdr;

    /* Task priority */
    void *RaiseTPL;
    void *RestoreTPL;

    /* Memory services */
    EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType,
                                       UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
    void       *FreePages;
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                      UINTN *MapKey, UINTN *DescriptorSize,
                                      UINT32 *DescriptorVersion);
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE PoolType, UINTN Size, void **Buffer);
    EFI_STATUS (EFIAPI *FreePool)(void *Buffer);

    /* Event & timer */
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;

    /* Protocol handler */
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;

    /* Image services */
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);

    /* Misc */
    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);
    void *SetWatchdogTimer;

    /* Driver support */
    void *ConnectController;
    void *DisconnectController;

    /* Open/close protocol */
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;

    /* Library services */
    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol, void *Registration, void **Interface);
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;

    /* CRC */
    void *CalculateCrc32;

    /* Misc */
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER                 Hdr;
    CHAR16                          *FirmwareVendor;
    UINT32                           FirmwareRevision;
    EFI_HANDLE                       ConsoleInHandle;
    void                            *ConIn;
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    void                            *RuntimeServices;
    EFI_BOOT_SERVICES               *BootServices;
    UINTN                            NumberOfTableEntries;
    void                            *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif /* NEXUS_EFI_H */
