PHDRS
{
  text PT_LOAD FILEHDR PHDRS FLAGS(5);
  dynamic PT_DYNAMIC FLAGS(4);
  eh_frame PT_GNU_EH_FRAME FLAGS(4);
}

SECTIONS
{
  . = SIZEOF_HEADERS;

  .hash           : { *(.hash) } :text
  .gnu.hash       : { *(.gnu.hash) } :text
  .dynsym         : { *(.dynsym) } :text
  .dynstr         : { *(.dynstr) } :text
  .gnu.version    : { *(.gnu.version) } :text
  .gnu.version_d  : { *(.gnu.version_d) } :text
  .dynamic        : { *(.dynamic) } :text :dynamic
  .rodata         : { *(.rodata*) } :text
  .eh_frame_hdr   : { *(.eh_frame_hdr) } :text :eh_frame
  .eh_frame       : { *(.eh_frame) } :text
  .text           : { *(.text*) } :text
  .got            : { *(.got*) } :text
  .got.plt        : { *(.got.plt*) } :text
  /DISCARD/       : { *(.note*) *(.comment*) }
}
