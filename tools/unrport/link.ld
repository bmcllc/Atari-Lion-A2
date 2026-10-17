ENTRY(_start)
SECTIONS {
  . = 0x00010000;
  .text : { *(.text.start) *(.text*) }
  .rodata : { *(.rodata*) *(.sdata2*) }
  .data : { *(.data*) *(.sdata*) }
  .bss : { *(.sbss*) *(.bss*) *(COMMON) }
}
