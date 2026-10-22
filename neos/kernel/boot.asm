; Multiboot1 entry: 32-bit protected mode -> identity-mapped 1 GiB -> long mode.
MB_MAGIC equ 0x1BADB002
MB_FLAGS equ 0x3            ; page-align modules + memory map

section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd -(MB_MAGIC + MB_FLAGS)

section .bss
align 4096
pml4:  resb 4096
pdpt:  resb 4096
pd:    resb 4096 * 4          ; 4 page directories = 4 GiB
align 16
stack_bottom: resb 16384
stack_top:

section .rodata
align 8
gdt64:
    dq 0
    dq 0x00AF9A000000FFFF   ; 0x08 kernel code (64-bit)
    dq 0x00CF92000000FFFF   ; 0x10 kernel data
gdt64_ptr:
    dw $ - gdt64 - 1
    dd gdt64
    dd 0

section .text
bits 32
global _start
extern kmain
_start:
    cli
    mov esp, stack_top
    cmp eax, 0x2BADB002
    jne .halt
    mov edi, eax
    mov esi, ebx

    ; zero the three page-table pages
    push edi
    mov edi, pml4
    xor eax, eax
    mov ecx, 6 * 1024
    rep stosd
    pop edi

    mov dword [pml4], pdpt + 3
    xor ecx, ecx
.pdpt:
    mov eax, ecx
    shl eax, 12
    add eax, pd + 3
    mov [pdpt + ecx * 8], eax
    inc ecx
    cmp ecx, 4
    jne .pdpt

    ; 2 MiB pages: RAM (first GiB) cached, everything above uncached (PCI MMIO)
    xor ecx, ecx
.map:
    mov eax, ecx
    shl eax, 21
    or  eax, 0x83           ; present | writable | 2 MiB page
    cmp ecx, 512
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
    mov eax, pml4
    mov cr3, eax
    mov ecx, 0xC0000080     ; EFER
    rdmsr
    or  eax, 1 << 8         ; LME
    wrmsr
    mov eax, cr0
    or  eax, 0x80000001     ; PG | PE
    mov cr0, eax
    lgdt [gdt64_ptr]
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
    mov rsp, stack_top
    mov edi, edi            ; zero-extend multiboot magic / info pointer
    mov esi, esi
    call kmain
.hang:
    cli
    hlt
    jmp .hang
