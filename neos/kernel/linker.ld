ENTRY(_start)
KERNEL_VMA = 0xFFFFFFFF80000000;
SECTIONS
{
    . = 1M;
    .boot : { *(.multiboot) *(.boot.text) *(.boot.rodata) }
    .boot.bss (NOLOAD) : ALIGN(4K) { *(.boot.bss) }

    . = ALIGN(4K) + KERNEL_VMA;
    .text   : AT(ADDR(.text) - KERNEL_VMA)   { *(.text .text.*) }
    .rodata : AT(ADDR(.rodata) - KERNEL_VMA) ALIGN(4K) { *(.rodata .rodata.*) }
    .data   : AT(ADDR(.data) - KERNEL_VMA) ALIGN(4K)   { *(.data .data.*) }
    .bss    : AT(ADDR(.bss) - KERNEL_VMA) ALIGN(4K)    { *(COMMON) *(.bss .bss.*) }
    . = ALIGN(4K);
    _kernel_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
