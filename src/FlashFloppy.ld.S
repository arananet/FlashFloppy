ENTRY(vector_table)

#if BUILD_GOTEK
MEMORY
{
  FLASH (rx)      : ORIGIN = 0x08008000, LENGTH = 96K
  RAM (rwx)       : ORIGIN = 0x20000000, LENGTH = 64K
}
REGION_ALIAS("RO", FLASH);
REGION_ALIAS("RW", RAM);
#elif BUILD_TOUCH
MEMORY
{
  FLASH (rx)      : ORIGIN = 0x08008000, LENGTH = 96K
  RAM (rwx)       : ORIGIN = 0x20000000, LENGTH = 20K
}
REGION_ALIAS("RO", FLASH);
REGION_ALIAS("RW", RAM);
#else
MEMORY
{
  RAM (rwx)       : ORIGIN = 0x20001000, LENGTH = 48K
}
REGION_ALIAS("RO", RAM);
REGION_ALIAS("RW", RAM);
#endif

SECTIONS
{
  .text : {
    _stext = .;
    *(.vector_table)
    *(.text)
    *(.text*)
    *(.rodata)
    *(.rodata*)
    KEEP (*(.init))
    KEEP (*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >RO

  .data : AT (_etext) {
    . = ALIGN(4);
    _sdat = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edat = .;
    _ldat = LOADADDR(.data);
  } >RW

  .bss : {
    . = ALIGN(8);
    _sbss = .;
    _irq_stackbottom = .;
    . = . + 512;
    _irq_stacktop = .;
    _thread_stackbottom = .;
    . = . + 1024;
    _thread_stacktop = .;
    *(.bss)
    *(.bss*)
    . = ALIGN(4);
    _ebss = .;
  } >RW

  /DISCARD/ : {
    *(.eh_frame)
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
