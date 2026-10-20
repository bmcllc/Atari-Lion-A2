/* The PS1 image sits above the debug log ring (0x1F0000..0x200000) that the Lion emulator can dump. */
ENTRY(_start)
SECTIONS {
  . = 0x00240000;
  .text : { *(.text.start) *(.text*) }
  .rodata : { *(.rodata*) *(.sdata2*) }
  .data : { *(.data*) *(.sdata*) }
  .bss : { __bss_start = .; *(.sbss*) *(.bss*) *(COMMON) __bss_end = .; }
  .wintrace 0x220000 (NOLOAD) : { *(.wintrace) }
  /DISCARD/ : { *(.eh_frame*) *(.gcc_except_table*) *(.comment) *(.init_array*) *(.ctors*) }
}
