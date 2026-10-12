#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "minemu/boot.h"
#include "minemu/mmu.h"
#include "minemu/platform.h"
#include "minemu/syscall.h"
#include "minemu/trace.h"
#include "minemu/trap.h"
#include "minemu/uart.h"
#include "minemu/kprintf.h"
#include "minemu/irq.h"
#include "minemu/msh.h"
#include "minemu/user_abi.h"

/* ---- user-mode loader state (adapted from examples/user-mode-hello) ---- */

#define BOOTSTRAP_PAGE_DIRECTORY_PADDR UINT32_C(0x40010000)
#define USER_IMAGE_BASE UINT32_C(0x00400000)
#define USER_STACK_BOTTOM UINT32_C(0x007ff000)
#define USER_STACK_TOP UINT32_C(0x00800000)
#define MAX_USER_IMAGE_PAGES 16U
#define SYSTEM_ROM_TABLE_COUNT 4U
#define CPSR_MODE_MASK UINT32_C(0x1f)
#define CPSR_MODE_USR UINT32_C(0x10)
#define CPSR_THUMB UINT32_C(0x20)
#define A32_SVC_ZERO UINT32_C(0xef000000)

/* Name used by the HW2/HW3 user-mode fixture images. */
#define USER_MODULE_NAME "minimum-user"

/* These buffers live in the .user_scratch section (see linker/kernel.ld) so the
 * boot ROM does not spend ticks copying/zeroing ~88 KiB before the shell runs.
 * The loader explicitly zeroes each buffer before use. */
#define USER_SCRATCH __attribute__((section(".user_scratch"), aligned(MINEMU_PAGE_SIZE)))

static uint32_t system_rom_tables[SYSTEM_ROM_TABLE_COUNT][1024] USER_SCRATCH;
static uint32_t user_page_table[1024] USER_SCRATCH;
static uint8_t user_image_pages[MAX_USER_IMAGE_PAGES][MINEMU_PAGE_SIZE] USER_SCRATCH;
static uint8_t user_stack[MINEMU_PAGE_SIZE] USER_SCRATCH;
static uint32_t user_page_vaddrs[MAX_USER_IMAGE_PAGES];
static uint32_t user_page_flags[MAX_USER_IMAGE_PAGES];
static size_t user_page_count;

void minemu_enter_user(uint32_t entry_vaddr, uint32_t stack_top) __attribute__((noreturn));

static void zero_bytes(void *destination, size_t length) {
    uint8_t *bytes = destination;
    for (size_t index = 0; index < length; ++index) bytes[index] = 0;
}

static void copy_bytes(void *destination, const void *source, size_t length) {
    uint8_t *out = destination;
    const uint8_t *in = source;
    for (size_t index = 0; index < length; ++index) out[index] = in[index];
}

static bool bytes_equal(const uint8_t *left, const char *right, size_t length) {
    for (size_t index = 0; index < length; ++index) {
        if (left[index] != (uint8_t)right[index]) return false;
    }
    return true;
}

static bool image_range_valid(uint32_t offset, uint32_t length, uint32_t image_size) {
    return offset <= image_size && length <= image_size - offset;
}

static bool image_records_valid(uint32_t offset, uint32_t count, uint32_t image_size) {
    return offset <= image_size && count <= ((image_size - offset) >> 5);
}

static uint32_t kernel_vaddr_to_paddr(const void *address) {
    return (uint32_t)(uintptr_t)address - MINEMU_KERNEL_DIRECT_BASE + MINEMU_RAM_BASE;
}

static volatile uint32_t *active_page_directory(void) {
    return (volatile uint32_t *)(uintptr_t)(
        MINEMU_KERNEL_DIRECT_BASE + BOOTSTRAP_PAGE_DIRECTORY_PADDR - MINEMU_RAM_BASE);
}

static bool map_system_rom(const struct minemu_boot_info *boot_info) {
    if (boot_info->system_rom_base != MINEMU_SYSTEM_ROM_BASE || boot_info->image_size == 0 ||
        boot_info->image_size > MINEMU_SYSTEM_ROM_SIZE) {
        return false;
    }
    uint32_t page_count = (boot_info->image_size + MINEMU_PAGE_SIZE - 1U) / MINEMU_PAGE_SIZE;
    uint32_t table_count = (page_count + 1023U) / 1024U;
    if (table_count > SYSTEM_ROM_TABLE_COUNT) return false;

    volatile uint32_t *directory = active_page_directory();
    for (uint32_t table = 0; table < table_count; ++table) {
        zero_bytes(system_rom_tables[table], sizeof(system_rom_tables[table]));
        uint32_t remaining = page_count - table * 1024U;
        uint32_t entries = remaining < 1024U ? remaining : 1024U;
        for (uint32_t index = 0; index < entries; ++index) {
            uint32_t page = table * 1024U + index;
            system_rom_tables[table][index] =
                (boot_info->system_rom_base + page * MINEMU_PAGE_SIZE) |
                MINEMU_PTE_VALID | MINEMU_PTE_READABLE;
        }
        uint32_t directory_index = (boot_info->system_rom_base >> 22) + table;
        directory[directory_index] =
            kernel_vaddr_to_paddr(system_rom_tables[table]) | MINEMU_PDE_VALID;
    }
    minemu_mmu_tlbiall();
    return true;
}

static const struct minemu_module_record *find_module(const struct minemu_boot_info *boot_info,
                                                       const char *name, size_t name_length) {
    const uint8_t *image = (const uint8_t *)(uintptr_t)boot_info->system_rom_base;
    if (!image_records_valid(boot_info->module_table_offset, boot_info->module_count,
                             boot_info->image_size)) {
        return NULL;
    }
    const struct minemu_module_record *modules =
        (const struct minemu_module_record *)(image + boot_info->module_table_offset);
    for (uint32_t index = 0; index < boot_info->module_count; ++index) {
        const struct minemu_module_record *module = &modules[index];
        if (module->name_length == name_length &&
            image_range_valid(module->name_offset, module->name_length, boot_info->image_size) &&
            bytes_equal(image + module->name_offset, name, name_length)) {
            return module;
        }
    }
    return NULL;
}

static uint32_t pte_flags_from_segment(uint32_t segment_flags) {
    uint32_t pte_flags = 0;
    if ((segment_flags & MINEMU_SEGMENT_READABLE) != 0) pte_flags |= MINEMU_PTE_READABLE;
    if ((segment_flags & MINEMU_SEGMENT_WRITABLE) != 0) pte_flags |= MINEMU_PTE_WRITABLE;
    if ((segment_flags & MINEMU_SEGMENT_EXECUTABLE) != 0) pte_flags |= MINEMU_PTE_EXECUTABLE;
    return pte_flags;
}

static int user_page_index(uint32_t page_vaddr, uint32_t flags, bool create) {
    for (size_t index = 0; index < user_page_count; ++index) {
        if (user_page_vaddrs[index] == page_vaddr) {
            if (create && user_page_flags[index] != flags) return -1;
            return (int)index;
        }
    }
    if (!create || user_page_count == MAX_USER_IMAGE_PAGES) return -1;
    size_t index = user_page_count++;
    user_page_vaddrs[index] = page_vaddr;
    user_page_flags[index] = flags;
    zero_bytes(user_image_pages[index], MINEMU_PAGE_SIZE);
    return (int)index;
}

static bool prepare_user_range(uint32_t vaddr, uint32_t length, uint32_t flags) {
    uint32_t end = vaddr + length;
    uint32_t page = vaddr & MINEMU_PTE_PAGE_MASK;
    while (page < end) {
        if (user_page_index(page, flags, true) < 0) return false;
        page += MINEMU_PAGE_SIZE;
    }
    return true;
}

static bool write_user_range(uint32_t vaddr, const uint8_t *source, uint32_t length) {
    while (length != 0) {
        uint32_t page_vaddr = vaddr & MINEMU_PTE_PAGE_MASK;
        int index = user_page_index(page_vaddr, 0, false);
        if (index < 0) return false;
        uint32_t offset = vaddr - page_vaddr;
        uint32_t chunk = MINEMU_PAGE_SIZE - offset;
        if (chunk > length) chunk = length;
        if (source == NULL) {
            zero_bytes(&user_image_pages[index][offset], chunk);
        } else {
            copy_bytes(&user_image_pages[index][offset], source, chunk);
            source += chunk;
        }
        vaddr += chunk;
        length -= chunk;
    }
    return true;
}

static bool load_user_module(const struct minemu_boot_info *boot_info,
                             const struct minemu_module_record *module) {
    const uint8_t *image = (const uint8_t *)(uintptr_t)boot_info->system_rom_base;
    if (module->segment_count == 0 ||
        !image_records_valid(module->segment_table_offset, module->segment_count,
                             boot_info->image_size) ||
        (module->entry_vaddr & 3U) != 0) {
        return false;
    }

    const struct minemu_module_segment *segments =
        (const struct minemu_module_segment *)(image + module->segment_table_offset);
    bool entry_is_executable = false;
    zero_bytes(user_page_table, sizeof(user_page_table));
    zero_bytes(user_stack, sizeof(user_stack));
    user_page_count = 0;

    for (uint32_t index = 0; index < module->segment_count; ++index) {
        const struct minemu_module_segment *segment = &segments[index];
        if (segment->memory_size == 0 || segment->file_size > segment->memory_size ||
            (segment->flags & ~(MINEMU_SEGMENT_READABLE | MINEMU_SEGMENT_WRITABLE |
                                MINEMU_SEGMENT_EXECUTABLE)) != 0 ||
            ((segment->flags & MINEMU_SEGMENT_EXECUTABLE) != 0 &&
             (segment->flags & MINEMU_SEGMENT_READABLE) == 0) ||
            segment->virtual_address < USER_IMAGE_BASE ||
            segment->virtual_address >= USER_STACK_BOTTOM ||
            segment->memory_size > USER_STACK_BOTTOM - segment->virtual_address ||
            !image_range_valid(segment->data_offset, segment->file_size, boot_info->image_size)) {
            return false;
        }

        uint32_t pte_flags = pte_flags_from_segment(segment->flags);
        if (!prepare_user_range(segment->virtual_address, segment->memory_size, pte_flags) ||
            !write_user_range(segment->virtual_address, NULL, segment->memory_size) ||
            !write_user_range(segment->virtual_address, image + segment->data_offset,
                              segment->file_size)) {
            return false;
        }

        if ((segment->flags & MINEMU_SEGMENT_EXECUTABLE) != 0 &&
            segment->file_size >= 4U && module->entry_vaddr >= segment->virtual_address &&
            module->entry_vaddr - segment->virtual_address <= segment->file_size - 4U) {
            entry_is_executable = true;
        }
    }
    if (!entry_is_executable) return false;

    for (size_t index = 0; index < user_page_count; ++index) {
        uint32_t table_index = (user_page_vaddrs[index] >> 12) & 1023U;
        user_page_table[table_index] = kernel_vaddr_to_paddr(user_image_pages[index]) |
                                       MINEMU_PTE_VALID | MINEMU_PTE_USER |
                                       user_page_flags[index];
    }
    user_page_table[(USER_STACK_BOTTOM >> 12) & 1023U] =
        kernel_vaddr_to_paddr(user_stack) | MINEMU_PTE_VALID | MINEMU_PTE_USER |
        MINEMU_PTE_READABLE | MINEMU_PTE_WRITABLE;

    volatile uint32_t *directory = active_page_directory();
    directory[USER_IMAGE_BASE >> 22] = kernel_vaddr_to_paddr(user_page_table) | MINEMU_PDE_VALID;
    minemu_mmu_tlbiall();
    return true;
}

/* ---- ioctl syscall ---- */

/* Every user pointer is untrusted: it must lie inside the single mapped
 * 4MB user region and every touched page must carry MINEMU_PTE_USER plus
 * the permission bit the operation needs. */
static bool validate_user_range(uint32_t vaddr, uint32_t length, bool need_write) {
    if (length == 0) return true;
    uint32_t end = vaddr + length;
    if (end < vaddr) return false;                       /* overflow */
    if (vaddr < USER_IMAGE_BASE || end > USER_STACK_TOP) return false;

    uint32_t page = vaddr & MINEMU_PTE_PAGE_MASK;
    uint32_t last_page = (end - 1U) & MINEMU_PTE_PAGE_MASK;
    for (;;) {
        uint32_t index = (page >> 12) & 1023U;
        uint32_t pte = user_page_table[index];
        if ((pte & MINEMU_PTE_VALID) == 0 || (pte & MINEMU_PTE_USER) == 0) return false;
        if (need_write) {
            if ((pte & MINEMU_PTE_WRITABLE) == 0) return false;
        } else {
            if ((pte & MINEMU_PTE_READABLE) == 0) return false;
        }
        if (page == last_page) break;
        page += MINEMU_PAGE_SIZE;
    }
    return true;
}

static volatile struct minemu_uart_regs *uart_for_fd(int fd) {
    switch (fd) {
    case MINEMU_FD_UART_IN:
    case MINEMU_FD_UART_OUT:
        return MINEMU_UART0;
    case MINEMU_FD_UART_DIAG:   /* ASSUMPTION: diag = UART1; confirm against the devices fixture */
        return MINEMU_UART1;
    default:
        return NULL;
    }
}

static int32_t ioctl_dispatch(int fd, uint32_t request, uint32_t arg0, uint32_t arg1) {
    switch (request) {
    case MINEMU_IOCTL_UART_READ: {
        volatile struct minemu_uart_regs *u = uart_for_fd(fd);
        if (u == NULL) return MINEMU_IOCTL_ERROR;
        int32_t n = (int32_t)arg1;
        if (n < 0) return MINEMU_IOCTL_ERROR;
        if (n > MINEMU_IOCTL_MAX_TRANSFER) n = MINEMU_IOCTL_MAX_TRANSFER;
        if (!validate_user_range(arg0, (uint32_t)n, true)) return MINEMU_IOCTL_ERROR;
        uint8_t *dst = (uint8_t *)(uintptr_t)arg0;
        int32_t count = 0;
        while (count < n && (u->status & MINEMU_UART_STATUS_RX_READY) != 0) {
            dst[count++] = (uint8_t)(u->rx_data & 0xFFU);
        }
        return count;
    }
    case MINEMU_IOCTL_UART_WRITE: {
        volatile struct minemu_uart_regs *u = uart_for_fd(fd);
        if (u == NULL) return MINEMU_IOCTL_ERROR;
        int32_t n = (int32_t)arg1;
        if (n < 0) return MINEMU_IOCTL_ERROR;
        if (n > MINEMU_IOCTL_MAX_TRANSFER) n = MINEMU_IOCTL_MAX_TRANSFER;
        if (!validate_user_range(arg0, (uint32_t)n, false)) return MINEMU_IOCTL_ERROR;
        const uint8_t *src = (const uint8_t *)(uintptr_t)arg0;
        for (int32_t i = 0; i < n; i++) {
            while ((u->status & MINEMU_UART_STATUS_TX_READY) == 0) { /* spin */ }
            u->tx_data = (uint32_t)src[i];
        }
        return n;
    }
    case MINEMU_IOCTL_UART_STATUS: {
        volatile struct minemu_uart_regs *u = uart_for_fd(fd);
        if (u == NULL) return MINEMU_IOCTL_ERROR;
        return (int32_t)u->status;
    }
    case MINEMU_IOCTL_RNG_SEED:
        if (fd != MINEMU_FD_RNG) return MINEMU_IOCTL_ERROR;
        MINEMU_RNG->seed = arg0;
        return 0;
    case MINEMU_IOCTL_RNG_NEXT: {
        if (fd != MINEMU_FD_RNG) return MINEMU_IOCTL_ERROR;
        if (!validate_user_range(arg0, sizeof(uint32_t), true)) return MINEMU_IOCTL_ERROR;
        *(uint32_t *)(uintptr_t)arg0 = MINEMU_RNG->data;
        return 0;
    }
    case MINEMU_IOCTL_RNG_STATE: {
        if (fd != MINEMU_FD_RNG) return MINEMU_IOCTL_ERROR;
        if (!validate_user_range(arg0, sizeof(uint32_t), true)) return MINEMU_IOCTL_ERROR;
        *(uint32_t *)(uintptr_t)arg0 = MINEMU_RNG->state;
        return 0;
    }
    case MINEMU_IOCTL_TRACE_EVENT:
        if (fd != MINEMU_FD_TRACE) return MINEMU_IOCTL_ERROR;
        MINEMU_TRACE->event = arg0;
        return 0;
    default:
        return MINEMU_IOCTL_ERROR;
    }
}

struct minemu_trap_frame *minemu_svc_dispatch(struct minemu_trap_frame *frame) {
    uint32_t mode = frame->spsr & CPSR_MODE_MASK;
    if (frame->exception_id != MINEMU_EXCEPTION_SVC || mode != CPSR_MODE_USR ||
        (frame->spsr & CPSR_THUMB) != 0) {
        frame->r[0] = (uint32_t)MINEMU_IOCTL_ERROR;
        return frame;
    }

    uint32_t instruction = *(const uint32_t *)(uintptr_t)frame->fault_pc;
    if (instruction != A32_SVC_ZERO) {
        frame->r[0] = (uint32_t)MINEMU_IOCTL_ERROR;
        return frame;
    }

    if (frame->r[7] != MINEMU_SYSCALL_IOCTL) {
        frame->r[0] = (uint32_t)MINEMU_IOCTL_ERROR;
        return frame;
    }

    int32_t result = ioctl_dispatch((int)frame->r[0], frame->r[1], frame->r[2], frame->r[3]);
    frame->r[0] = (uint32_t)result;
    return frame;
}

/* ---- boot ---- */

void minemu_kernel_main(const struct minemu_boot_info *boot_info) {
    /* Identical to HW1 — do not change, so HW1 test images still pass. */
    if ((uintptr_t)boot_info != MINEMU_BOOT_INFO_VADDR ||
        boot_info->magic != MINEMU_BOOT_INFO_MAGIC ||
        boot_info->version != MINEMU_ABI_VERSION ||
        boot_info->size != sizeof(*boot_info) ||
        boot_info->system_rom_base != UINT32_C(0x08000000) ||
        boot_info->direct_map_vaddr != UINT32_C(0xc0000000) ||
        boot_info->direct_map_paddr != UINT32_C(0x40000000) ||
        boot_info->direct_map_size != UINT32_C(0x04000000)) {
        minemu_trace_event(UINT32_C(0xb007bad0));
        minemu_fail_stop();
    }

    /* Enter user mode whenever the image ships a "minimum-user" module. The
     * HW2 shell image (image/minimum.toml) ships msh this way, and user-mode
     * fixture images (HW2/HW3) declare their own. Images without a module,
     * such as the HW1 shell image, fall through to the built-in msh below.
     * Never guess from the module's instructions. */
    static const char module_name[] = USER_MODULE_NAME;
    if (boot_info->module_count != 0 && map_system_rom(boot_info)) {
        const struct minemu_module_record *module =
            find_module(boot_info, module_name, sizeof(module_name) - 1U);
        if (module != NULL && load_user_module(boot_info, module)) {
            minemu_enter_user(module->entry_vaddr, USER_STACK_TOP);
            /* noreturn */
        }
    }

    /* Fallback: HW1 kernel shell. */
    uart_init(UART_CONSOLE);
    uart_puts(UART_CONSOLE, "hello world\n");

    irq_init();
    UART_CONSOLE->control |= MINEMU_UART_CONTROL_RX_IRQ_ENABLE;
    MINEMU_INTERRUPT->enable |= (UINT32_C(1) << MINEMU_IRQ_UART0);
    minemu_irq_enable();

    msh_run();
}