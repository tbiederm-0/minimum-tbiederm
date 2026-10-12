#include <minemu/msh.h>
#include <minemu/rxbuf.h>
#include <minemu/uart.h>
#include <minemu/kprintf.h>
#include <stddef.h>

/* Bytes stored per line, excluding the '\n' terminator. */
#define LINE_CAP 20
#define PROMPT "msh> "

#define BS 0x08
#define DEL 0x7f

static int is_blank(const char *line, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (line[i] != ' ') return 0;
    }
    return 1;
}

/* Prints the space-delimited words starting at i, one space between them. */
static void run_echo(const char *line, size_t len, size_t i) {
    int first = 1;

    while (i < len) {
        while (i < len && line[i] == ' ') i++;
        size_t start = i;
        while (i < len && line[i] != ' ') i++;
        if (i > start) {
            if (!first) uart_putc(UART_CONSOLE, ' ');
            uart_write(UART_CONSOLE, &line[start], i - start);
            first = 0;
        }
    }
    uart_putc(UART_CONSOLE, '\n');
}

static void run_command(const char *line, size_t len) {
    if (is_blank(line, len)) return;

    size_t i = 0;
    while (i < len && line[i] == ' ') i++;
    size_t start = i;
    while (i < len && line[i] != ' ') i++;
    size_t word_len = i - start;
    const char *word = &line[start];

    if (word_len == 4 && word[0] == 'e' && word[1] == 'c' &&
        word[2] == 'h' && word[3] == 'o') {
        run_echo(line, len, i);
    } else {
        uart_puts(UART_CONSOLE, "command not found: ");
        uart_write(UART_CONSOLE, word, word_len);
        uart_putc(UART_CONSOLE, '\n');
    }
}

void msh_run(void) {
    char line[LINE_CAP];
    size_t len = 0;
    int overflow = 0;

    kprintf(PROMPT);
    for (;;) {
        /* The UART0 ISR owns the hardware FIFO; drain the ring buffer. */
        int c = rx_pop();
        if (c < 0) continue;

        if (c == '\n') {
            /* An overlong line is dropped whole: nothing is executed. */
            if (!overflow) run_command(line, len);
            len = 0;
            overflow = 0;
            kprintf(PROMPT);
        } else if (c == BS || c == DEL) {
            if (len > 0) len--;
        } else if (len < LINE_CAP) {
            line[len++] = (char)c;
        } else {
            overflow = 1;
        }
    }
}
