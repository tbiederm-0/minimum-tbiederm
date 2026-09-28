#ifndef MINEMU_KPRINTF_H
#define MINEMU_KPRINTF_H

#include <stdarg.h>

/* Lets GCC type-check format strings at compile time. */
int kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vkprintf(const char *fmt, va_list ap);

/* Optional: tiny log-level wrappers. */
#define KLOG_INFO(fmt, ...) kprintf("[info] " fmt "\n", ##__VA_ARGS__)
#define KLOG_ERR(fmt, ...)  kprintf("[err ] " fmt "\n", ##__VA_ARGS__)

#endif