; kernel_entry.asm
;
; Entered in 64-bit long mode by the UEFI bootloader (boot.c), which jumps
; here at physical 0x10000 after ExitBootServices. The firmware already put
; us in long mode with identity-mapped low memory, so unlike the BIOS path
; (boot.asm + kernel_entry_bios.asm) there is no real->protected->long
; transition to perform.
;
; What this stub still must do:
;   1. Install the kernel's own GDT. The firmware's GDT is about to be
;      irrelevant, and the kernel hard-codes selector 0x18 as its 64-bit
;      code segment (see idt.c / sched.c), so we reload GDTR and refresh
;      every segment register -- CS via a long far-return -- to match.
;   2. Set the boot stack, zero .bss (objcopy strips it from the image),
;      and call kernel_main.
;
; The VMM in kernel.c rebuilds proper 4-level page tables once the PMM is
; up; until then we run on the firmware's identity map, which is enough.

extern kernel_main
extern __bss_start
extern __bss_end

section .text
global _start

[BITS 64]
_start:
    cli
    lgdt [rel gdt_descriptor]

    ; Reload the data segments from the new GDT (0x10 = data).
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Reload CS to the new 64-bit code selector (0x18) via a far return.
    push qword 0x18
    lea  rax, [rel .reload_cs]
    push rax
    o64 retf

.reload_cs:
    mov rsp, 0x90000

    ; Zero .bss (undefined after objcopy -O binary drops it).
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    add rcx, 7                  ; round up to whole qwords
    shr rcx, 3
    xor rax, rax
    cld
    rep stosq

    call kernel_main

.hang:
    cli
    hlt
    jmp .hang

; Same layout boot.asm installs on the BIOS path:
;   0x00 null, 0x08 32-bit code, 0x10 data, 0x18 64-bit code.
align 8
gdt_start:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF       ; 0x08 32-bit code
    dq 0x00CF92000000FFFF       ; 0x10 data
    dq 0x00AF9A000000FFFF       ; 0x18 64-bit code
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dq gdt_start                ; 64-bit base for lgdt in long mode
