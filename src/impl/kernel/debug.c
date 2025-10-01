/*
 * debug.c - Implementation of debugging utilities for GatOS kernel
 *
 * Implements all debugging related functions declared in debug.h
 * 
 * Author: u/ApparentlyPlus
 */

#include <memory/paging.h>
#include <libc/string.h>
#include <stddef.h>
#include <serial.h>
#include <misc.h>

static int dbg_counter = 0;

/*
 * DEBUG_LOG - Debug function to log messages to qemu serial with counter
 */
void DEBUG_LOG(const char* msg, int total) {
    char buf[128];
    char* ptr = buf;

    *ptr++ = '[';
    ptr += int_to_str(++dbg_counter, ptr);
    *ptr++ = '/';
    ptr += int_to_str(total, ptr);
    *ptr++ = ']';
    *ptr++ = ' ';

    size_t msg_len = strlen(msg);
    memcpy(ptr, msg, msg_len);
    ptr += msg_len;

    *ptr++ = '\n';
    *ptr = '\0';

    serial_write(buf);
}

/*
 * DEBUG_GENERIC_LOG - Debug function to log messages to qemu serial without counter
 */
void DEBUG_GENERIC_LOG(const char* msg) {
    char buf[128];
    char* ptr = buf;

    size_t msg_len = strlen(msg);
    memcpy(ptr, msg, msg_len);
    ptr += msg_len;

    *ptr++ = '\n';
    *ptr = '\0';

    serial_write(buf);
}

/*
 * DEBUG_DUMP_PMT - Debug function to print page table structure
 */
void DEBUG_DUMP_PMT(void) {
    uint64_t* PML4 = getPML4();
    serial_write("Page Tables:\n");

    for (int pml4_i = 0; pml4_i < PAGE_ENTRIES; pml4_i++) {
        uint64_t pml4e = PML4[pml4_i];
        if (!(pml4e & PAGE_PRESENT)) continue;

        serial_write("PML4[");
        serial_write_hex16(pml4_i);
        serial_write("]: ");
        serial_write_hex32((uint32_t)pml4e); // Print only lower 32 bits
        serial_write(" -> PDPT\n");

        // Convert physical address to virtual
        uint64_t* pdpt = (uint64_t*)(KERNEL_P2V(pml4e & PAGE_MASK));

        for (int pdpt_i = 0; pdpt_i < PAGE_ENTRIES; pdpt_i++) {
            uint64_t pdpte = pdpt[pdpt_i];
            if (!(pdpte & PAGE_PRESENT)) continue;

            serial_write("  PDPT[");
            serial_write_hex16(pdpt_i);
            serial_write("]: ");
            serial_write_hex32((uint32_t)pdpte); // Print only lower 32 bits
            serial_write(" -> PD\n");

            // Convert physical address to virtual
            uint64_t* pd = (uint64_t*)(KERNEL_P2V(pdpte & PAGE_MASK));

            for (int pd_i = 0; pd_i < PAGE_ENTRIES; pd_i++) {
                uint64_t pde = pd[pd_i];
                if (!(pde & PAGE_PRESENT)) continue;

                serial_write("    PD[");
                serial_write_hex16(pd_i);
                serial_write("]: ");
                serial_write_hex32((uint32_t)pde); // Print only lower 32 bits
                serial_write(" -> PT\n");

                // Convert physical address to virtual
                uint64_t* pt = (uint64_t*)(KERNEL_P2V(pde & PAGE_MASK));

                for (int pt_i = 0; pt_i < PAGE_ENTRIES; pt_i++) {
                    uint64_t pte = pt[pt_i];
                    if (!(pte & PAGE_PRESENT)) continue;

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
