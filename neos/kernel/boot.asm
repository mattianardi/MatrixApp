; Multiboot1 entry (32-bit, physical addresses) -> long mode -> higher-half kernel.
;   0xFFFFFFFF80000000 + phys : kernel image          (PML4[511])
;   0xFFFF800000000000 + phys : direct map of 4 GiB   (PML4[256]; RAM cached, above 1 GiB uncached = PCI MMIO)
;   PML4[0] is identity-mapped only until the kernel is running in the higher half, then cleared:
;   the whole lower half belongs to user processes.
MB_MAGIC equ 0x1BADB002
MB_FLAGS equ 0x7            ; page-align modules + memory map + ask the loader for a linear video mode
KERNEL_VMA equ 0xFFFFFFFF80000000

section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd -(MB_MAGIC + MB_FLAGS)
    dd 0, 0, 0, 0, 0        ; address fields (unused for ELF images)
    dd 0, 1024, 768, 32     ; video: linear framebuffer 1024x768x32 (GRUB sets it; QEMU -kernel ignores it)

section .boot.bss nobits alloc write align=4096
global boot_pml4
boot_pml4:  resb 4096
pdpt_low:   resb 4096
pdpt_high:  resb 4096
pd:         resb 4096 * 4
align 16
boot_stack: resb 4096
boot_stack_top:

section .boot.rodata alloc align=8
gdt64:
    dq 0
    dq 0x00AF9A000000FFFF   ; 0x08 kernel code (64-bit)
    dq 0x00CF92000000FFFF   ; 0x10 kernel data
gdt64_ptr:
    dw $ - gdt64 - 1
    dd gdt64
    dd 0

section .boot.text exec align=16
bits 32
global _start
_start:
    cli
    mov word [0xB8000 + (24*80+50)*2], 0x4F41     ; 'A' boot breadcrumb (visible in text mode)
    mov esp, boot_stack_top
    cmp eax, 0x2BADB002
    jne .halt
    mov edi, eax
    mov esi, ebx

    push edi
    mov edi, boot_pml4
    xor eax, eax
    mov ecx, 7 * 1024       ; pml4 + pdpt_low + pdpt_high + 4 pd
    rep stosd
    pop edi

    mov dword [boot_pml4], pdpt_low + 3
    mov dword [boot_pml4 + 256 * 8], pdpt_low + 3
    mov dword [boot_pml4 + 511 * 8], pdpt_high + 3
    mov dword [pdpt_high + 510 * 8], pd + 3          ; -2 GiB .. -1 GiB -> phys 0 .. 1 GiB
    xor ecx, ecx
.pdpt:
    mov eax, ecx
    shl eax, 12
    add eax, pd + 3
    mov [pdpt_low + ecx * 8], eax
    inc ecx
    cmp ecx, 4
    jne .pdpt

    xor ecx, ecx
.map:
    mov eax, ecx
    shl eax, 21
    or  eax, 0x83           ; present | writable | 2 MiB page (supervisor only)
    cmp ecx, 1536           ; RAM below 3 GiB is cached; the PCI window above it (frame buffer, xHCI...) is not
    jb  .cached
    or  eax, 0x18           ; PWT | PCD
.cached:
    mov [pd + ecx * 8], eax
    inc ecx
    cmp ecx, 2048
    jne .map

    mov eax, cr4
    or  eax, 1 << 5         ; PAE
    mov cr4, eax
    mov eax, boot_pml4
    mov cr3, eax
    mov ecx, 0xC0000080     ; EFER
    rdmsr
    or  eax, (1 << 8) | (1 << 11)     ; LME | NXE
    wrmsr
    mov eax, cr0
    or  eax, 0x80000001     ; PG | PE
    mov cr0, eax
    lgdt [gdt64_ptr]
    mov word [0xB8000 + (24*80+51)*2], 0x4F42     ; 'B' paging on
    jmp 0x08:long_mode
.halt:
    hlt
    jmp .halt

bits 64
long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov word [0xB8000 + (24*80+52)*2], 0x4F43     ; 'C' long mode
    mov edi, edi            ; zero-extend multiboot magic / info pointer (physical)
    mov esi, esi
    mov rax, higher_half
    jmp rax

section .bss
align 16
kstack_bottom: resb 16384
kstack_top:

section .text
bits 64
extern kmain
higher_half:
    mov rsp, kstack_top
    mov rax, 0xFFFFFFFF800B8000 + (24*80+53)*2
    mov word [rax], 0x4F44                        ; 'D' higher half
    xor ebp, ebp
    call kmain
.hang:
    cli
    hlt
    jmp .hang
