#ifndef MINEMU_RXBUF_H
#define MINEMU_RXBUF_H
#include <stdint.h>

void rx_push(uint8_t b);   /* called from IRQ context */
int  rx_pop(void);         /* called from main context; -1 if empty */

#endif