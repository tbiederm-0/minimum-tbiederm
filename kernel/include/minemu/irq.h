#ifndef MINEMU_IRQ_H
#define MINEMU_IRQ_H

#include "minemu/trap.h"

/* Students implement this A32 vector trampoline and its IRQ dispatch policy. */
void minemu_irq_trampoline(void) __attribute__((noreturn));
struct minemu_trap_frame *minemu_irq_dispatch(struct minemu_trap_frame *frame);

static inline void minemu_irq_enable(void) {
    __asm__ volatile("cpsie i" : : : "memory");
}

static inline void minemu_irq_disable(void) {
    __asm__ volatile("cpsid i" : : : "memory");
}

typedef void (*irq_handler_t)(void);
void irq_register(uint32_t id, irq_handler_t fn);
void irq_init(void);

static inline uint32_t irq_save(void) {
    uint32_t cpsr;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    minemu_irq_disable();
    return cpsr & (1u << 7);
}

static inline void irq_restore(uint32_t saved) {
    if (!saved) minemu_irq_enable();
}

#endif
