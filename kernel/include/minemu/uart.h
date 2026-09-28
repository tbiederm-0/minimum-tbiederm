#ifndef MINEMU_UART_H
#define MINEMU_UART_H

#include <stddef.h>
#include <stdint.h>
#include <minemu/platform.h>

typedef volatile struct minemu_uart_regs *uart_t;

#define UART_CONSOLE MINEMU_UART0   /* wired to the TUI console */
#define UART_AUX     MINEMU_UART1

void uart_init(uart_t u);

/* Transmit (blocking) */
void uart_putc(uart_t u, char c);
void uart_write(uart_t u, const char *buf, size_t len);
void uart_puts(uart_t u, const char *s);

/* Receive */
int  uart_rx_ready(uart_t u);
int  uart_try_getc(uart_t u);   /* -1 if nothing available */
char uart_getc(uart_t u);       /* blocks */

#endif