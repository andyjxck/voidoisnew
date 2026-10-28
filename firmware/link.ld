/* nRF52832 — MCUBoot app slot on PineTime.
   Slot base 0x8000 + 32-byte image header = vector table at 0x8020. */
OUTPUT_FORMAT("elf32-littlearm")
ENTRY(Reset_Handler)

MEMORY
{
    /* same budget as InfiniTime's gcc_nrf52-mcuboot.ld: slot 0x8000..
       0x7C000 minus the 32-byte MCUBoot header, the TLV area (0x1b0)
       and the image trailer (0x28). The old 0x73FC0 let a large image
       run into the trailer, where the image-valid word at 0x7BFE8
       lives. */
    FLASH (rx) : ORIGIN = 0x08020, LENGTH = 0x73E08
    RAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 0x10000
}

SECTIONS
{
    .text :
    {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
    } > FLASH

    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    .data : AT(_etext)
    {
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM

    .bss :
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = .;
        end = .;          /* newlib _sbrk heap start (unused: no malloc) */
        __end__ = .;
    } > RAM

    . = ALIGN(4);
    _stack_top = ORIGIN(RAM) + LENGTH(RAM);
    __StackTop = _stack_top;
}
