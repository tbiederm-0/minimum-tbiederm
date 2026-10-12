#include <minemu/irq.h>
#include <minemu/platform.h>
#include <minemu/uart.h>
#include <minemu/rxbuf.h>

static irq_handler_t handlers[MINEMU_IRQ_BLOCK + 1] = {0};

/* Bytes drained out of the UART0 hardware FIFO by the ISR. */
volatile uint32_t g_uart_byte_count = 0;

void irq_register(uint32_t id, irq_handler_t fn) {
    if (id <= MINEMU_IRQ_BLOCK) handlers[id] = fn;
}

/* Draining every unread byte is what clears the RX interrupt condition. */
static void uart0_irq_handler(void) {
    while (uart_rx_ready(UART_CONSOLE)) {
        uint8_t b = (uint8_t)(UART_CONSOLE->rx_data & 0xFF);
        rx_push(b);
        g_uart_byte_count++;
    }
}

void irq_init(void) {
    irq_register(MINEMU_IRQ_UART0, uart0_irq_handler);
}

struct minemu_trap_frame *minemu_irq_dispatch(struct minemu_trap_frame *frame) {
    uint32_t source = (uint32_t)frame->exception_id;

    if (source <= MINEMU_IRQ_BLOCK && handlers[source]) {
        handlers[source]();
    }
    /* EOI does not clear the source's reason; the handler must do that. */
    if (source != MINEMU_IRQ_NONE) {
        MINEMU_INTERRUPT->eoi = source;
    }
    return frame;
}
