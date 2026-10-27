[BITS 16]
[ORG 0x7C00]

; NexusOS BIOS bootloader - 512 byte MBR.
;
;   1. Collect the BIOS E820 memory map at 0x9000/0x9008.
;   2. Set a VESA linear-framebuffer mode (0x118); on failure the kernel
;      falls back to its text console.
;   3. Load the kernel to 0x10000 via int 13h LBA extensions.
;   4. Enable A20, install GDT, enter protected mode, jump to the kernel.

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl
    mov si, msg_loading
    call print_string

    ; ---------------- E820 memory map -------------------------------
    ;   dword [0x9000] = entry count, 24-byte entries from 0x9008.
    ;   (If firmware returns a map with no usable region, the kernel
    ;    substitutes a conservative default -- see pmm_init.)
    xor ax, ax
    mov es, ax
    mov di, 0x9008
    xor ebx, ebx
    xor bp, bp
.e820_loop:
    mov eax, 0xE820
    mov edx, 0x534D4150         ; "SMAP"
    mov ecx, 24
    mov dword [es:di + 20], 1   ; default ACPI 3.0 attr = "valid"
    int 0x15
    jc  .e820_done              ; CF -> end of list (or unsupported)
    cmp eax, 0x534D4150
    jne .e820_done
    add di, 24                  ; advance every entry -> loop must end
    inc bp
    test ebx, ebx               ; ebx == 0 -> last entry
    jz  .e820_done
    cmp bp, 64
    jb  .e820_loop
.e820_done:
    mov [0x9000], bp
    mov word [0x9002], 0
    mov word [0x9004], 0
    mov word [0x9006], 0

    ; ---------------- VBE linear-framebuffer mode ------------------
%ifdef TEXT_ONLY
    ; Build with -dTEXT_ONLY for firmware that hangs inside the VBE int 0x10
    ; calls under CSM. Skip VBE entirely; the kernel
    ; uses its text console.
    mov dword [0x9714], 0
%else
    ; Get mode info for 1024x768 (VBE 0x118), then set it with the LFB bit.
    ; On success, leave a descriptor at 0x9700 for the kernel; on failure,
    ; set the "valid" flag to 0 so the kernel uses its text console.
    ; es is still 0 from the E820 loop.
    mov ax, 0x4F01
    mov cx, 0x118
    mov di, 0x9800
    int 0x10
    cmp ax, 0x004F
    jne .vbe_fail
    mov ax, 0x4F02
    mov bx, 0x118
    or  bx, 0x4000             ; bit 14 = linear framebuffer
    int 0x10
    cmp ax, 0x004F
    jne .vbe_fail
    mov eax, [0x9800 + 40]     ; PhysBasePtr
    mov [0x9700], eax
    movzx eax, word [0x9800 + 16]   ; BytesPerScanLine
    mov [0x9704], eax
    movzx eax, word [0x9800 + 18]   ; XResolution
    mov [0x9708], eax
    movzx eax, word [0x9800 + 20]   ; YResolution
    mov [0x970C], eax
    movzx eax, byte [0x9800 + 25]   ; BitsPerPixel
    mov [0x9710], eax
    mov dword [0x9714], 1
    jmp .vbe_done
.vbe_fail:
    mov dword [0x9714], 0
.vbe_done:
%endif

    ; ---------------- Load kernel to 0x10000 (int 13h LBA) ----------
    ; CHS reads hang on many USB-boot BIOSes; the LBA extensions are
    ; universally available on anything that can boot from USB.
    mov si, kernel_dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc  disk_error

    ; ---------------- A20 + GDT + PM --------------------------------
    in  al, 0x92
    or  al, 2
    out 0x92, al

    cli
    lgdt [gdt_descriptor]

    mov eax, cr0
    or  eax, 1
    mov cr0, eax

    jmp 0x08:protected_mode

disk_error:
    mov si, msg_disk_err
    call print_string
.halt:
    cli
    hlt
    jmp .halt

print_string:
    mov ah, 0x0E
    mov bx, 0x0007             ; page 0, light-grey attribute
.loop:
    lodsb
    test al, al
    jz   .done
    int  0x10
    jmp  .loop
.done:
    ret

[BITS 32]
protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    jmp 0x08:0x10000

boot_drive:   db 0
%ifdef TEXT_ONLY
msg_loading:  db "NexusOS text-mode boot", 0x0D, 0x0A, 0
%else
msg_loading:  db "Booting NexusOS", 0x0D, 0x0A, 0
%endif
msg_disk_err: db "Disk read error", 0

; Disk Address Packet for the int 13h LBA read of the kernel.
align 4
kernel_dap:
    db 0x10                     ; packet size
    db 0                        ; reserved
    dw 127                      ; sectors to read (kernel is ~73)
    dw 0x0000                   ; destination offset
    dw 0x1000                   ; destination segment -> phys 0x10000
    dq 1                        ; start LBA (sector 0 is the MBR itself)

align 8
gdt_start:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF       ; 32-bit code
    dq 0x00CF92000000FFFF       ; 32-bit data
    dq 0x00AF9A000000FFFF       ; 64-bit code
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

times 510 - ($ - $$) db 0
dw 0xAA55

; Consider adding UEFI capabilities
