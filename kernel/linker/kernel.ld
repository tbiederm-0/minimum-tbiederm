OUTPUT_ARCH(arm)
ENTRY(minemu_bootstrap_high)

PHDRS
{
  bootstrap PT_LOAD FLAGS(5);
  kernel PT_LOAD FLAGS(7);
}

SECTIONS
{
  . = 0x40008000;
  .bootstrap :
  {
    KEEP(*(.bootstrap.text))
  } :bootstrap

  . = 0xc0030000;
  .vectors : AT(0x40030000)
  {
    KEEP(*(.vectors))
  } :kernel

  .text : AT(ADDR(.text) - 0x80000000)
  {
    *(.text.startup)
    *(.text*)
  } :kernel

  .rodata : AT(ADDR(.rodata) - 0x80000000)
  {
    *(.rodata*)
  } :kernel

  .data : AT(ADDR(.data) - 0x80000000)
  {
    *(.data*)
  } :kernel

  .bss (NOLOAD) :
  {
    *(.bss*)
    *(COMMON)
  } :kernel

  . = ALIGN(8);
  .stacks (NOLOAD) :
  {
    . += 4096;
    __minemu_svc_stack_top = .;
    . += 4096;
    __minemu_irq_stack_top = .;
    . += 4096;
    __minemu_abt_stack_top = .;
    . += 4096;
    __minemu_und_stack_top = .;
  } :kernel

  /*
   * Scratch space for the user-mode loader (system-ROM shadow tables, the user
   * page table, staged user image pages, and the user stack). This section is
   * deliberately left out of every PHDR: the boot ROM only copies and zeroes
   * the PT_LOAD segments, so these ~88 KiB are not touched during boot. That
   * keeps the HW1 shell image inside its tight tick budget. The loader zeroes
   * every buffer itself before use, so no zero-initialisation is required.
   */
  . = ALIGN(4096);
  .user_scratch (NOLOAD) :
  {
    *(.user_scratch)
    *(.user_scratch.*)
  }

  __minemu_svc_stack_top_paddr = __minemu_svc_stack_top - 0x80000000;
  __minemu_irq_stack_top_paddr = __minemu_irq_stack_top - 0x80000000;
  __minemu_abt_stack_top_paddr = __minemu_abt_stack_top - 0x80000000;
  __minemu_und_stack_top_paddr = __minemu_und_stack_top - 0x80000000;
}
