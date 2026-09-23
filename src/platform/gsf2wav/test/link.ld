SECTIONS { .text 0x08000000 : { *(.text.start) *(.text*) } /DISCARD/ : { *(.ARM.exidx*) } }
