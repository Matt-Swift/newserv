.meta name="Decoction"
.meta description="Make the Decoction\nitem reset your\nmaterial usage"
# Original code by Ralf @ GC-Forever and Aleron Ives
# https://www.gc-forever.com/forums/viewtopic.php?t=2050
# https://www.gc-forever.com/forums/viewtopic.php?t=2049
# Xbox port by fuzziqersoftware

entry_ptr:
reloc0:
  .offsetof start
start:
  .include  WriteCodeBlocksXB
  .data     0x001842D0
  .deltaof  code_start, code_end
code_start:
  .include  DecoctionXB
code_end:
  .data     0x001842D1
  .data     0x00000004
  .data     0x001FD670
  .data     0x00000000
  .data     0x00000000
