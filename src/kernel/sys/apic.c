/*
 * apic.c - Local and I/O APIC Implementation (Production Grade)
 *
 * This module provides an implementation of the APIC interrupt controller.
 * It handles the transition from legacy PIC to APIC, parses the ACPI MADT for 
 * hardware topology, and manages both Local and I/O APIC configurations.
 *
 * Author: ApparentlyPlus
 */

#include <arch/x86_64/memory/paging.h>
#include <kernel/drivers/serial.h>
#include <arch/x86_64/cpu/cpu.h>
#include <kernel/memory/vmm.h>
#include <kernel/sys/panic.h>
#include <kernel/sys/apic.h>
#include <kernel/sys/acpi.h>
#include <kernel/debug.h>
#include <libc/string.h>

#pragma region Internal Helpers & Globals

static uint64_t g_lapic_base = 0;
static uint64_t g_ioapic_base = 0;
static MADT_IOAPIC* g_ioapic_rec = NULL; 

/*
 * io_wait - I/O wait by writing to an unused port
 */
static inline void io_wait(void) {
    outb(0x80, 0);
}

/*
 * disable_pic - Disable the legacy 8259 PICs
 */
void disable_pic(void) {

    // Standard PIC initialization sequence to ensure it's in a known state
    // before we mask everything
    outb(PIC_COMMAND_MASTER, ICW_1);
    io_wait();
    outb(PIC_COMMAND_SLAVE, ICW_1);
    io_wait();

    // Map PIC vectors out of the way of exceptions
    outb(PIC_DATA_MASTER, ICW_2_M);
    io_wait();
    outb(PIC_DATA_SLAVE, ICW_2_S);
    io_wait();

    // Cascading info
    outb(PIC_DATA_MASTER, ICW_3_M);
    io_wait();
    outb(PIC_DATA_SLAVE, ICW_3_S);
    io_wait();

    // 8086 mode
    outb(PIC_DATA_MASTER, ICW_4);
    io_wait();
    outb(PIC_DATA_SLAVE, ICW_4);
    io_wait();

    // Mask all interrupts on both PICs
    outb(PIC_DATA_MASTER, 0xFF);
    outb(PIC_DATA_SLAVE, 0xFF);
    
    LOGF("[APIC] Legacy PIC disabled and masked.\n");
}

#pragma endregion

#pragma region LAPIC

/*
 * lapic_write - Write a value to a LAPIC register
 */
void lapic_write(uint32_t reg, uint32_t value) {
    if (g_lapic_base == 0) return;
    *(volatile uint32_t*)(g_lapic_base + reg) = value;
}

/*
 * lapic_read - Read a value from a LAPIC register
 */
uint32_t lapic_read(uint32_t reg) {
    if (g_lapic_base == 0) return 0;
    return *(volatile uint32_t*)(g_lapic_base + reg);
}

/*
 * lapic_eoi - Send End Of Interrupt signal to LAPIC
 */
void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

/*
 * lapic_get_id - Get the Local APIC ID of the current processor
 */
uint32_t lapic_get_id(void) {
    return lapic_read(LAPIC_ID) >> 24;
}

/*
 * lapic_init - Initialize the Local APIC
 */
void lapic_init(void) {

    // Safety Check: Verify APIC support via CPUID
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & (1 << 9))) {
        panic("System does not support APIC!");
    }

    // Get Base Address from MSR
    uint64_t apic_msr = read_msr(MSR_IA32_APIC_BASE);
    
    // Ensure Hardware Enable
    if (!(apic_msr & MSR_APIC_BASE_ENABLE)) {
        LOGF("[APIC] LAPIC hardware disabled in MSR. Enabling...\n");
        apic_msr |= MSR_APIC_BASE_ENABLE;
        write_msr(MSR_IA32_APIC_BASE, apic_msr);
    }

    // Map physical address to virtual space
    uint64_t phys_base = apic_msr & 0xFFFFF000;
    
    if (g_lapic_base == 0) {
        void* virt_addr = NULL;
        if (vmm_alloc(NULL, PAGE_SIZE, VM_FLAG_WRITE | VM_FLAG_MMIO, (void*)phys_base, &virt_addr) != VMM_OK) {
            panic("Failed to map LAPIC memory.");
        }
        g_lapic_base = (uint64_t)virt_addr;
    }

    // Software Enable & Spurious Vector
    lapic_write(LAPIC_SPURIOUS, LAPIC_SW_ENABLE | LAPIC_SPURIOUS_IV);
    
    // Set TPR to 0 to allow all interrupts
    lapic_write(LAPIC_TPR, 0);

    // Check for X2APIC Support
    if (c & (1 << 21)) {
        LOGF("[APIC] X2APIC support detected.\n");
        // we are NOT handling that lol, I got enough on my plate
    }

    // Configure NMIs if specified in MADT
    MADTHeader* madt = (MADTHeader*)acpi_find_table("APIC");
    if (madt) {
        uint8_t* start = (uint8_t*)(madt + 1);
        uint8_t* end = (uint8_t*)madt + madt->header.Length;
        uint32_t my_id = lapic_get_id();

        while (start < end) {
            MADTRecordHeader* header = (MADTRecordHeader*)start;
            if (header->type == MADT_TYPE_NMI) {
                MADT_NMI* nmi = (MADT_NMI*)header;
                // 0xFF means all processors, or match my ID
                if (nmi->acpi_processor_id == 0xFF || nmi->acpi_processor_id == my_id) {
                    uint32_t lvt_reg = (nmi->lint == 0) ? LAPIC_LVT_LINT0 : LAPIC_LVT_LINT1;
                    // Delivery Mode: NMI (100b = 4)
                    lapic_write(lvt_reg, (4 << 8)); 
                    LOGF("[APIC] Configured LINT%u as NMI\n", nmi->lint);
                }
            }
            start += header->length;
        }
    }

    LOGF("[APIC] LAPIC initialized. Local ID: %u, Version: 0x%X\n", 
         lapic_get_id(), lapic_read(LAPIC_VER) & 0xFF);
}

/*
 * lapic_send_ipi - Send an Inter-Processor Interrupt to another core
 */
void lapic_send_ipi(uint32_t dest_id, uint8_t vector) {
    // In xAPIC mode, wait for delivery status to be clear (Bit 12 of ICR Low)
    // prod note - busy waiting on ICR is standard for IPIs i think
    while (lapic_read(LAPIC_ICR_LOW) & (1 << 12));

    lapic_write(LAPIC_ICR_HIGH, dest_id << 24);
    lapic_write(LAPIC_ICR_LOW, vector);
}

#pragma endregion

#pragma region IOAPIC

/*
 * ioapic_read - Read a value from an I/O APIC register
 */
uint32_t ioapic_read(uint32_t reg) {
    if (g_ioapic_base == 0) return 0;
    *(volatile uint32_t*)(g_ioapic_base + IOAPIC_REGSEL) = reg;
    return *(volatile uint32_t*)(g_ioapic_base + IOAPIC_IOWIN);
}

/*
 * ioapic_write - Write a value to an I/O APIC register
 */
void ioapic_write(uint32_t reg, uint32_t value) {
    if (g_ioapic_base == 0) return;
    *(volatile uint32_t*)(g_ioapic_base + IOAPIC_REGSEL) = reg;
    *(volatile uint32_t*)(g_ioapic_base + IOAPIC_IOWIN) = value;
}

/*
 * ioapic_set_entry - Set a redirection table entry
 */
void ioapic_set_entry(uint8_t index, uint64_t data) {
    ioapic_write(IOAPIC_REDTBL + 2 * index, (uint32_t)(data & 0xFFFFFFFF));
    ioapic_write(IOAPIC_REDTBL + 2 * index + 1, (uint32_t)(data >> 32));
}

/*
 * ioapic_redirect - Configure a redirection entry for a specific IRQ
 */
void ioapic_redirect(uint8_t irq, uint8_t vector, uint32_t dest_core, uint16_t flags) {
    uint64_t entry = vector;

    // Delivery Mode - Fixed (000)
    // Destination Mode - Physical (0)
    // Polarity: bit 13 - 0=High, 1=Low
    // Trigger: bit 15 - 0=Edge, 1=Level

    // ACPI Flags (passed from ISO):
    // Polarity (bits 0-1): 01=High, 11=Low, 00=Default (High for ISA)
    // Trigger (bits 2-3): 01=Edge, 11=Level, 00=Default (Edge for ISA)

    uint8_t polarity = flags & 0x03;
    uint8_t trigger = (flags >> 2) & 0x03;

    if (polarity == 0x03) entry |= (1 << 13);
    if (trigger == 0x03)  entry |= (1 << 15);

    // Destination field
    entry |= ((uint64_t)dest_core << 56);
    
    // Default to MASKED (Bit 16 = 1). 
    // Drivers must explicitly call ioapic_unmask() when they are ready to handle it
    entry |= (1 << 16);
    
    ioapic_set_entry(irq, entry);
}

/*
 * ioapic_mask - Mask an I/O APIC interrupt
 */
void ioapic_mask(uint8_t irq) {
    uint32_t low = ioapic_read(IOAPIC_REDTBL + 2 * irq);
    ioapic_write(IOAPIC_REDTBL + 2 * irq, low | (1 << 16));
}

/*
 * ioapic_unmask - Unmask an I/O APIC interrupt
 */
void ioapic_unmask(uint8_t irq) {
    uint32_t low = ioapic_read(IOAPIC_REDTBL + 2 * irq);
    ioapic_write(IOAPIC_REDTBL + 2 * irq, low & ~(1 << 16));
}

/*
 * ioapic_init - Initialize the I/O APIC
 */
void ioapic_init(void) {
    // Locate MADT Table
    MADTHeader* madt = (MADTHeader*)acpi_find_table("APIC");
    if (!madt) {
        panic("MADT (APIC) table not found!");
    }

    // Parse MADT for I/O APIC record
    uint8_t* start = (uint8_t*)(madt + 1);
    uint8_t* end = (uint8_t*)madt + madt->header.Length;

    while (start < end) {
        MADTRecordHeader* header = (MADTRecordHeader*)start;
        if (header->type == MADT_TYPE_IOAPIC) {
            g_ioapic_rec = (MADT_IOAPIC*)header;
            break;
        }
        start += header->length;
    }

    if (!g_ioapic_rec) {
        panic("No I/O APIC record found in MADT!");
    }

    // Map I/O APIC memory
    uint64_t phys = g_ioapic_rec->io_apic_address;
    void* virt_addr = NULL;
    if (vmm_alloc(NULL, PAGE_SIZE, VM_FLAG_WRITE | VM_FLAG_MMIO, (void*)phys, &virt_addr) != VMM_OK) {
        panic("Failed to map I/O APIC memory.");
    }
    g_ioapic_base = (uint64_t)virt_addr;

    // Get number of redirection entries
    uint32_t ver = ioapic_read(IOAPIC_VER);
    uint32_t count = ((ver >> 16) & 0xFF) + 1;

    // Initialize Redirection Table
    // First, mask all entries and set a default 1:1 mapping for ISA IRQs (offset 32)
    // The GSI base for this IOAPIC is g_ioapic_rec->global_system_interrupt_base
    uint32_t gsi_base = g_ioapic_rec->global_system_interrupt_base;
    uint32_t bsp_id = lapic_get_id();

    for (uint32_t i = 0; i < count; i++) {
        // Masked (bit 16), Vector 32+i, Fixed, Physical, Dest=BSP
        uint64_t entry = (1 << 16) | (32 + gsi_base + i);
        entry |= ((uint64_t)bsp_id << 56);
        ioapic_set_entry(i, entry);
    }

    // Apply Interrupt Source Overrides (ISOs)
    start = (uint8_t*)(madt + 1);
    while (start < end) {
        MADTRecordHeader* header = (MADTRecordHeader*)start;
        if (header->type == MADT_TYPE_ISO) {
            MADT_ISO* iso = (MADT_ISO*)header;
            
            // Check if this GSI belongs to this I/O APIC
            if (iso->global_system_interrupt >= gsi_base && 
                iso->global_system_interrupt < gsi_base + count) {
                
                uint32_t io_index = iso->global_system_interrupt - gsi_base;
                
                LOGF("[APIC] ISO: IRQ %u -> GSI %u (Flags: 0x%X)\n", 
                     iso->irq_source, iso->global_system_interrupt, iso->flags);
                
                // Unmask and redirect - Standard Vector = 32 + IRQ_Source
                ioapic_redirect(io_index, 32 + iso->irq_source, bsp_id, iso->flags);
            }
        }
        start += header->length;
    }

    // Note here: 

    // Unmask the standard ISA interrupts (1-15). IRQ 0 is usually PIT/Timer.
    // For production, we typically unmask them as drivers request them.
    // For now, let's just unmask the ones we redirected via ISO or identity.
    // NOTE: IRQ 0 is often overridden to GSI 2.
    
    LOGF("[APIC] I/O APIC initialized at 0x%lX. %u redirection entries.\n", g_ioapic_base, count);
}

#pragma endregion

/*
 * apic_init - High-level APIC initialization
 */
void apic_init(void) {
    LOGF("[APIC] Beginning hardware interrupt controller setup...\n");

    disable_pic();
    lapic_init();
    ioapic_init();

    LOGF("[APIC] Interrupt subsystem is online.\n");
}