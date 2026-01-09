/*
 * apic.c - Local APIC Implementation
 * Author: ApparentlyPlus
 */

#include <arch/x86_64/cpu/cpu.h>
#include <kernel/memory/vmm.h>
#include <kernel/sys/apic.h>
#include <kernel/debug.h>

// This global holds the VIRTUAL address of the LAPIC base
static uint64_t g_lapic_base = 0;

/*
 * lapic_write - Write to a Local APIC register
 */
void lapic_write(uint32_t reg, uint32_t value)
{
    if (g_lapic_base == 0) {
        LOGF("[APIC] Panic: Attempted write before mapping!\n");
        return;
    }
    *(volatile uint32_t*)(g_lapic_base + reg) = value;
}

/*
 * lapic_eoi - Signal End of Interrupt
 */
void lapic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

/*
 * lapic_init - Initialize the Local APIC
 * MSR Enable -> VMM Map -> Software Enable
 */
void lapic_init(void)
{
    // Read the MSR to get the Physical Address and status
    uint64_t apic_msr = read_msr(MSR_IA32_APIC_BASE);
    
    // If the MSR says it's off, turn it on
    if (!(apic_msr & MSR_APIC_BASE_ENABLE)) {
        LOGF("[APIC] Hardware disabled in MSR. Enabling...\n");
        apic_msr |= MSR_APIC_BASE_ENABLE;
        write_msr(MSR_IA32_APIC_BASE, apic_msr);
    }

    // Extract the Physical Base Address
    uint64_t phys_base = apic_msr & 0xFFFFF000;

    if (vmm_map_page(NULL, phys_base, (void*)phys_base, VM_FLAG_WRITE | VM_FLAG_MMIO) != VMM_OK) {
        LOGF("[APIC] CRITICAL: Failed to map LAPIC physical address 0x%lX\n", phys_base);
        return;
    }

    // Save the Virtual Address
    g_lapic_base = phys_base;
    LOGF("[APIC] Mapped and accessible at 0x%lX\n", g_lapic_base);

    // Software Enable (Spurious Vector)
    lapic_write(LAPIC_SPURIOUS, LAPIC_SW_ENABLE | LAPIC_SPURIOUS_IV);
    
    LOGF("[APIC] Software Enabled. Spurious Vector set to 0x%X\n", LAPIC_SPURIOUS_IV);
}

/*
 * lapic_send_ipi - Send an Inter Processor Interrupt
 */
void lapic_send_ipi(uint32_t dest_id, uint8_t vector)
{
    // Set the destination in the High register (Bits 56-63)
    // In xAPIC mode, the destination ID is in the top 8 bits of the high register
    lapic_write(LAPIC_ICR_HIGH, dest_id << 24);

    // Write the vector to the Low register to SEND the interrupt.
    // We leave other bits (delivery mode, etc) as 0 for "fixed" mode.
    lapic_write(LAPIC_ICR_LOW, vector);
}