#include <minemu/rxbuf.h>
#include <minemu/irq.h>

#define RXBUF_SIZE 256   /* power of two */

static volatile uint8_t  rx_buf[RXBUF_SIZE];
static volatile uint32_t rx_head = 0;
static volatile uint32_t rx_tail = 0;
volatile uint32_t g_rx_dropped = 0;

void rx_push(uint8_t b) {
    uint32_t next = (rx_head + 1) & (RXBUF_SIZE - 1);
    if (next == rx_tail) { g_rx_dropped++; return; }
    rx_buf[rx_head] = b;
    rx_head = next;
}

int rx_pop(void) {
    /* Only the ISR advances rx_head, so an empty read needs no masking. */
    if (rx_tail == rx_head) return -1;

    uint32_t saved = irq_save();
    int c = -1;
    if (rx_tail != rx_head) {
        c = rx_buf[rx_tail];
        rx_tail = (rx_tail + 1) & (RXBUF_SIZE - 1);
    }
    irq_restore(saved);
    return c;
}