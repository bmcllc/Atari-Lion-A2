/* Stage 0 (BOOT.LION): low in RAM, clear of the payload at 0x240000 and the boot ROM's buffers at 0xC00000+. */
ENTRY(_start)
SECTIONS {
  . = 0x00010000;
  .text : { *(.text.start) *(.text*) }
  .rodata : { *(.rodata*) *(.sdata2*) }
  .data : { *(.data*) *(.sdata*) }
  .bss : { *(.sbss*) *(.bss*) *(COMMON) }
  /DISCARD/ : { *(.eh_frame*) *(.gcc_except_table*) *(.comment) *(.init_array*) *(.ctors*) }
}
