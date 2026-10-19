/* vita-elf-create appends its SCE module data at the end of the code segment, in the address
   space before the data segment; the data segment starts at the next 4 KiB page, so the room
   left depends on the code's size and was too small for v0.6.15 (1728 of 2524 bytes). Leave
   4 KiB more before the data segment, whatever the code's size. */
SECTIONS
{
  . = . + 0x1000;
}
INSERT AFTER .eh_frame;
