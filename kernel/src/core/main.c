#include "minemu/boot.h"
#include "minemu/trap.h"
#include "minemu/trace.h"
#include "minemu/uart.h"
#include "minemu/kprintf.h"
#include "minemu/irq.h"
#include <minemu/msh.h>

void minemu_kernel_main(const struct minemu_boot_info *boot_info) {
    if ((uintptr_t)boot_info != MINEMU_BOOT_INFO_VADDR ||
        boot_info->magic != MINEMU_BOOT_INFO_MAGIC ||
        boot_info->version != MINEMU_ABI_VERSION ||
        boot_info->size != sizeof(*boot_info) ||
        boot_info->system_rom_base != UINT32_C(0x08000000) ||
        boot_info->direct_map_vaddr != UINT32_C(0xc0000000) ||
        boot_info->direct_map_paddr != UINT32_C(0x40000000) ||
        boot_info->direct_map_size != UINT32_C(0x04000000)) {
        minemu_trace_event(UINT32_C(0xb007bad0));
        minemu_fail_stop();
    }
    uart_init(UART_CONSOLE);
    uart_puts(UART_CONSOLE, "hello world\n");

    irq_init();
    UART_CONSOLE->control |= MINEMU_UART_CONTROL_RX_IRQ_ENABLE;
    MINEMU_INTERRUPT->enable |= (UINT32_C(1) << MINEMU_IRQ_UART0);
    minemu_irq_enable();

    msh_run();
}