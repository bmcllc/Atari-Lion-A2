ENTRY(_start)
SECTIONS {
  . = 0x00010000;
  .text : { *(.text.start) *(.text*) }
  .rodata : { *(.rodata*) *(.sdata2*) }
  .init_array : { __init_array_start = .; KEEP(*(.init_array*)) KEEP(*(.ctors*)) __init_array_end = .; }
  .data : { *(.data*) *(.sdata*) }
  .bss : { *(.sbss*) *(.bss*) *(COMMON) }
  /DISCARD/ : { *(.eh_frame*) *(.gcc_except_table*) *(.comment) }
}
