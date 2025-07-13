OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
    . = 0x00000000;          /* Start address of the program */

    .text : {
        *(.init)
        *(.text*)
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss : {
        *(.bss*)
        *(COMMON)
    }

    . = ALIGN(16);
    _stack_top = . + 0x10000; /* 64 KB stack */
}