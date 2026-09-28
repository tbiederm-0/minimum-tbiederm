#include <minemu/uart.h>

/* Set to 1 if the TUI shows staircased output. */
#define UART_CRLF 0

void uart_init(uart_t u) {
    /* Clear the only defined control bit: RX interrupts off for now. */
    u->control = 0;
}

void uart_putc(uart_t u, char c) {
    while (!(u->status & MINEMU_UART_STATUS_TX_READY)) {
        /* spin */
    }
    u->tx_data = (uint32_t)(uint8_t)c;
}

void uart_write(uart_t u, const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
#if UART_CRLF
        if (buf[i] == '\n') uart_putc(u, '\r');
#endif
        uart_putc(u, buf[i]);
    }
}

void uart_puts(uart_t u, const char *s) {
    while (*s) {
#if UART_CRLF
        if (*s == '\n') uart_putc(u, '\r');
#endif
        uart_putc(u, *s++);
    }
}

int uart_rx_ready(uart_t u) {
    return (u->status & MINEMU_UART_STATUS_RX_READY) != 0;
}

int uart_try_getc(uart_t u) {
    if (!uart_rx_ready(u)) return -1;
    return (int)(u->rx_data & 0xFF);
}

char uart_getc(uart_t u) {
    while (!uart_rx_ready(u)) {
        /* spin */
    }
    return (char)(u->rx_data & 0xFF);
}