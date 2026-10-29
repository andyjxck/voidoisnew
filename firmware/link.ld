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
    /* The vector table gets its own output section, pinned to the start
       of FLASH. MCUBoot jumps through the table at slot + header size
       (0x8020), and imgtool puts the first byte of the .bin there.
       With the table inside .text, any 64-byte-aligned input section
       (newlib's strlen in arm-none-eabi-gcc 14 is .p2align 6) raised
       .text's alignment and moved the whole section to 0x8040. The
       .bin then started at 0x8040 but was flashed at 0x8020, so every
       address in the image was 0x20 off and the watch faulted before
       main(): the boot loop in 2.0.0. */
    .vectors ORIGIN(FLASH) :
    {
        KEEP(*(.vectors))
    } > FLASH

    ASSERT(ADDR(.vectors) == ORIGIN(FLASH), "vector table must sit at 0x8020")

    .text :
    {
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
