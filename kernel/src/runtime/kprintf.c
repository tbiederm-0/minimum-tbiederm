#include <minemu/kprintf.h>
#include <minemu/uart.h>
#include <stddef.h>
#include <stdint.h>

/* The single output sink. Change this to redirect all kernel output. */
static int emit(char c) {
    uart_putc(UART_CONSOLE, c);
    return 1;
}

static int pad(char c, int count) {
    int n = 0;
    while (count-- > 0) n += emit(c);
    return n;
}

static size_t kstrlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* Writes digits of v in the given base into buf (most significant first). */
static size_t utoa_buf(char *buf, uint32_t v, uint32_t base, int upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[32];
    size_t n = 0;
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v != 0);
    for (size_t i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

static int emit_number(uint32_t mag, int neg, uint32_t base, int upper,
                       int width, int left, int zero) {
    char digits[32];
    size_t len = utoa_buf(digits, mag, base, upper);
    int total = (int)len + (neg ? 1 : 0);
    int fill = width > total ? width - total : 0;
    int n = 0;

    if (left) zero = 0;                       /* '-' overrides '0' */
    if (!left && !zero) n += pad(' ', fill);
    if (neg) n += emit('-');                  /* sign goes before zero padding */
    if (!left && zero) n += pad('0', fill);
    for (size_t i = 0; i < len; i++) n += emit(digits[i]);
    if (left) n += pad(' ', fill);
    return n;
}

static int emit_string(const char *s, int width, int left) {
    int len = (int)kstrlen(s);
    int fill = width > len ? width - len : 0;
    int n = 0;
    if (!left) n += pad(' ', fill);
    for (int i = 0; i < len; i++) n += emit(s[i]);
    if (left) n += pad(' ', fill);
    return n;
}

int vkprintf(const char *fmt, va_list ap) {
    int n = 0;

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            n += emit(*fmt);
            continue;
        }
        fmt++;

        /* flags */
        int left = 0, zero = 0, width = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else break;
        }
        /* width */
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        /* length modifier: long/size_t are 32-bit here, same as int */
        if (*fmt == 'l' || *fmt == 'z') fmt++;

        switch (*fmt) {
        case 'd':
        case 'i': {
            int v = va_arg(ap, int);
            uint32_t mag = v < 0 ? (uint32_t)0 - (uint32_t)v : (uint32_t)v;
            n += emit_number(mag, v < 0, 10, 0, width, left, zero);
            break;
        }
        case 'u':
            n += emit_number(va_arg(ap, unsigned int), 0, 10, 0, width, left, zero);
            break;
        case 'x':
            n += emit_number(va_arg(ap, unsigned int), 0, 16, 0, width, left, zero);
            break;
        case 'X':
            n += emit_number(va_arg(ap, unsigned int), 0, 16, 1, width, left, zero);
            break;
        case 'p':
            n += emit('0');
            n += emit('x');
            n += emit_number((uint32_t)(uintptr_t)va_arg(ap, void *), 0, 16, 0, 8, 0, 1);
            break;
        case 'c': {
            char c[2] = { (char)va_arg(ap, int), '\0' };
            n += emit_string(c, width, left);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            n += emit_string(s ? s : "(null)", width, left);
            break;
        }
        case '%':
            n += emit('%');
            break;
        case '\0':
            return n;                 /* format string ended after a lone '%' */
        default:
            n += emit('%');           /* unknown conversion: print it verbatim */
            n += emit(*fmt);
            break;
        }
    }
    return n;
}

int kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vkprintf(fmt, ap);
    va_end(ap);
    return n;
}