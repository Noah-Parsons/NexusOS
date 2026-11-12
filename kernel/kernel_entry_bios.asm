; kernel_entry_bios.asm
;
; Entry stub for the legacy BIOS boot path (bootloader/boot.asm). The UEFI
; path uses kernel_entry.asm instead; both are linked at physical 0x10000
; and call the same kernel_main.
;
; Entered in 32-bit protected mode from boot.asm. Sets up identity-mapped
; paging for the first 4 GiB (2 MiB pages), switches to long mode, and calls
; kernel_main.
;
; 4 GiB rather than 2 MiB because kernel_main brings the framebuffer up
; before the VMM exists (see kernel.c), and the VBE linear framebuffer sits
; high in the 32-bit space (0xFD000000 under QEMU). Under UEFI the firmware's
; identity map already covers it; here this stub has to provide it.
;
; The VMM in kernel.c rebuilds proper 4-level page tables once the PMM is
; available. This stub only needs enough mapping to reach that point.

extern kernel_main

section .text
global _start

[BITS 32]
_start:
    cli

    ; Page tables at 0x70000: PML4, PDPT, then four PDs (one per GiB).
    ; Zero all six pages (24 KiB).
    mov edi, 0x70000
    xor eax, eax
    mov ecx, 0x6000 / 4
    rep stosd

    ; PML4[0] -> PDPT
    mov dword [0x70000], 0x71000 | 0x03
    ; PDPT[0..3] -> PD0..PD3
    mov dword [0x71000], 0x72000 | 0x03
    mov dword [0x71008], 0x73000 | 0x03
    mov dword [0x71010], 0x74000 | 0x03
    mov dword [0x71018], 0x75000 | 0x03

    ; PD entries: 2048 x 2 MiB pages covering 0..4 GiB (PS=1, RW, present).
    ; Entry i maps physical i * 2 MiB; the high dword stays 0.
    mov edi, 0x72000
    mov eax, 0x00000083
    mov ecx, 2048
.fill_pd:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop .fill_pd

    mov eax, 0x70000
    mov cr3, eax

    mov eax, cr4
    or  eax, 1 << 5             ; CR4.PAE
    mov cr4, eax

    mov ecx, 0xC0000080         ; IA32_EFER
    rdmsr
    or  eax, 1 << 8             ; LME
    wrmsr

    mov eax, cr0
    or  eax, 1 << 31            ; PG
    mov cr0, eax

    jmp 0x18:long_mode

[BITS 64]
long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, 0x90000

    ; Zero the .bss section. objcopy stripped it out of the binary,
    ; so the memory contents are undefined; clear them.
    extern __bss_start
    extern __bss_end
    mov   rdi, __bss_start
    mov   rcx, __bss_end
    sub   rcx, rdi
    shr   rcx, 3            ; bytes -> qwords
    xor   rax, rax
    cld
    rep stosq

    call kernel_main

.hang:
    cli
    hlt
    jmp .hang
