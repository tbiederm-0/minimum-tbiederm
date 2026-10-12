#include <stddef.h>
#include <stdint.h>

#include "minemu/user_abi.h"

/* Bytes stored per line, excluding the '\n' terminator. */
#define LINE_CAP 128
#define PROMPT "msh> "

#define BS 0x08
#define DEL 0x7f

/*
 * Write up to the kernel's per-call transfer cap per syscall. The kernel
 * validates every user pointer, so buffers produced by the compiler (string
 * literals in .rodata, stack locals) are safe to hand across the boundary.
 */
static void write_bytes(const char *data, size_t length) {
    while (length > 0) {
        uint32_t chunk = length > (size_t)MINEMU_IOCTL_MAX_TRANSFER
                             ? (uint32_t)MINEMU_IOCTL_MAX_TRANSFER
                             : (uint32_t)length;
        int32_t written = ioctl(MINEMU_FD_UART_OUT, MINEMU_IOCTL_UART_WRITE,
                                (void *)data, chunk);
        if (written <= 0) return;
        data += (size_t)written;
        length -= (size_t)written;
    }
}

static void write_string(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') length++;
    write_bytes(text, length);
}

static void write_char(char c) { write_bytes(&c, 1); }

static int is_blank(const char *line, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (line[i] != ' ') return 0;
    }
    return 1;
}

/* Prints the space-delimited words starting at i, one space between them. */
static void run_echo(const char *line, size_t length, size_t i) {
    int first = 1;

    while (i < length) {
        while (i < length && line[i] == ' ') i++;
        size_t start = i;
        while (i < length && line[i] != ' ') i++;
        if (i > start) {
            if (!first) write_char(' ');
            write_bytes(&line[start], i - start);
            first = 0;
        }
    }
    write_char('\n');
}

static void run_command(const char *line, size_t length) {
    if (is_blank(line, length)) return;

    size_t i = 0;
    while (i < length && line[i] == ' ') i++;
    size_t start = i;
    while (i < length && line[i] != ' ') i++;
    size_t word_length = i - start;
    const char *word = &line[start];

    if (word_length == 4 && word[0] == 'e' && word[1] == 'c' &&
        word[2] == 'h' && word[3] == 'o') {
        run_echo(line, length, i);
    } else {
        write_string("command not found: ");
        write_bytes(word, word_length);
        write_char('\n');
    }
}

void minemu_user_main(void) {
    char line[LINE_CAP];
    size_t length = 0;
    int overflow = 0;

    write_string(PROMPT);
    for (;;) {
        /* The kernel owns the hardware FIFO and exposes it through ioctl;
         * poll a byte at a time so input never has to cross an IRQ boundary. */
        char c;
        int32_t received =
            ioctl(MINEMU_FD_UART_IN, MINEMU_IOCTL_UART_READ, (void *)&c, (uint32_t)1);
        if (received <= 0) continue;

        if (c == '\n') {
            /* An overlong line is dropped whole: nothing is executed. */
            if (!overflow) run_command(line, length);
            length = 0;
            overflow = 0;
            write_string(PROMPT);
        } else if (c == BS || c == DEL) {
            if (length > 0) length--;
        } else if (length < LINE_CAP) {
            line[length++] = c;
        } else {
            overflow = 1;
        }
    }
}
