#include <path/to/paging.h>
#include <stdint.h>
#include <stddef.h>

/*

Your paging.h should define at least the following macros and constants:

#define KERNEL_VIRTUAL_BASE     0xFFFFFFFF80000000
#define PRESENT                 (1ULL << 0)
#define PAGE_SIZE               0x1000UL
#define PAGE_ENTRIES            512
#define PAGE_MASK               0xFFFFF000

#ifdef __ASSEMBLER__

#define KERNEL_V2P(a) ((a) - KERNEL_VIRTUAL_BASE)
#define KERNEL_P2V(a) ((a) + KERNEL_VIRTUAL_BASE)

#else

#include <stdint.h>
#define KERNEL_V2P(a) ((uintptr_t)(a) & ~KERNEL_VIRTUAL_BASE)
#define KERNEL_P2V(a) ((uintptr_t)(a) | KERNEL_VIRTUAL_BASE)

#endif

*/


#define COM1_PORT 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/*
 * serial_init - Initializes COM1 port at 38400 baud
 */
void serial_init(void) {
    outb(COM1_PORT + 1, 0x00);    // Disable interrupts
    outb(COM1_PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(COM1_PORT + 0, 0x03);    // Set divisor to 3 (38400 baud)
    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(COM1_PORT + 2, 0xC7);    // Enable FIFO, clear them, 14-byte threshold
    outb(COM1_PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

/*
 * serial_is_ready - Checks if transmit buffer is empty
 */
int serial_is_ready(void) {
    return inb(COM1_PORT + 5) & 0x20;
}

/*
 * serial_write_char - Outputs single character to serial port
 */
void serial_write_char(char c) {
    while (!serial_is_ready()); // Wait until THR is empty
    outb(COM1_PORT, (uint8_t)c);
}

/*
 * serial_write - Outputs null-terminated string to serial
 */
void serial_write(const char* str) {
    while (*str) {
        if (*str == '\n') {
            serial_write_char('\r'); // Add carriage return for terminal compatibility
        }
        serial_write_char(*str++);
    }
}

/*
 * serial_write_len - Outputs fixed-length string to serial
 */
void serial_write_len(const char* str, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\n') {
            serial_write_char('\r');
        }
        serial_write_char(str[i]);
    }
}

/*
 * serial_write_hex_digit - Internal helper for hex digit output
 */
static void serial_write_hex_digit(uint8_t val) {
    val &= 0xF;
    if (val < 10)
        serial_write_char('0' + val);
    else
        serial_write_char('A' + (val - 10));
}

/*
 * serial_write_hex8 - Outputs 8-bit value in hexadecimal
 */
void serial_write_hex8(uint8_t value) {
    serial_write_hex_digit(value >> 4);
    serial_write_hex_digit(value & 0xF);
}

/*
 * serial_write_hex16 - Outputs 16-bit value in hexadecimal
 */
void serial_write_hex16(uint16_t value) {
    for (int i = 12; i >= 0; i -= 4)
        serial_write_hex_digit((value >> i) & 0xF);
}

/*
 * serial_write_hex32 - Outputs 32-bit value in hexadecimal
 */
void serial_write_hex32(uint32_t value) {
    for (int i = 28; i >= 0; i -= 4)
        serial_write_hex_digit((value >> i) & 0xF);
}

/*
 * serial_write_hex64 - Outputs 64-bit value in hexadecimal
 */
void serial_write_hex64(uint64_t value) {
    for (int i = 60; i >= 0; i -= 4)
        serial_write_hex_digit((value >> i) & 0xF);
}


/*
 * getPML4 - Retrieves current PML4 table address (virtual)
 */
static inline uint64_t* getPML4(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return (uint64_t*)KERNEL_P2V(cr3);
}

/*
 * dbg_dump_pmt - Debug function to print page table structure
 */
void dbg_dump_pmt(void) {
    uint64_t* PML4 = getPML4();
    serial_write("Page Tables:\n");

    for (int pml4_i = 0; pml4_i < PAGE_ENTRIES; pml4_i++) {
        uint64_t pml4e = PML4[pml4_i];
        if (!(pml4e & PRESENT)) continue;

        serial_write("PML4[");
        serial_write_hex16(pml4_i);
        serial_write("]: ");
        serial_write_hex32((uint32_t)pml4e); // Print only lower 32 bits
        serial_write(" -> PDPT\n");

        // Convert physical address to virtual
        uint64_t* pdpt = (uint64_t*)(KERNEL_P2V(pml4e & PAGE_MASK));

        for (int pdpt_i = 0; pdpt_i < PAGE_ENTRIES; pdpt_i++) {
            uint64_t pdpte = pdpt[pdpt_i];
            if (!(pdpte & PRESENT)) continue;

            serial_write("  PDPT[");
            serial_write_hex16(pdpt_i);
            serial_write("]: ");
            serial_write_hex32((uint32_t)pdpte); // Print only lower 32 bits
            serial_write(" -> PD\n");

            // Convert physical address to virtual
            uint64_t* pd = (uint64_t*)(KERNEL_P2V(pdpte & PAGE_MASK));

            for (int pd_i = 0; pd_i < PAGE_ENTRIES; pd_i++) {
                uint64_t pde = pd[pd_i];
                if (!(pde & PRESENT)) continue;

                serial_write("    PD[");
                serial_write_hex16(pd_i);
                serial_write("]: ");
                serial_write_hex32((uint32_t)pde); // Print only lower 32 bits
                serial_write(" -> PT\n");

                // Convert physical address to virtual
                uint64_t* pt = (uint64_t*)(KERNEL_P2V(pde & PAGE_MASK));

                for (int pt_i = 0; pt_i < PAGE_ENTRIES; pt_i++) {
                    uint64_t pte = pt[pt_i];
                    if (!(pte & PRESENT)) continue;

                    serial_write("      PT[");
                    serial_write_hex16(pt_i);
                    serial_write("]: ");
                    serial_write_hex32((uint32_t)pte); // Print only lower 32 bits
                    serial_write(" -> PHYS\n");
                }
            }
        }
    }
}
