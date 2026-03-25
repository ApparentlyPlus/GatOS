/*
 * apic.c - Local and I/O APIC Implementation (Production Grade)
 *
 * This module provides an implementation of the APIC interrupt controller.
 * It handles the transition from legacy PIC to APIC, parses the ACPI MADT for 
 * hardware topology, and manages both Local and I/O APIC configurations.
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/memory/paging.h>
#include <kernel/drivers/serial.h>
#include <arch/x86_64/cpu/io.h>
#include <arch/x86_64/cpu/cpu.h>
#include <kernel/memory/vmm.h>
#include <kernel/sys/panic.h>
#include <kernel/sys/apic.h>
#include <kernel/sys/acpi.h>
#include <kernel/debug.h>
#include <klibc/string.h>

#pragma region Internal Helpers & Globals

uint64_t lapic_base = 0;
static uint64_t ioapic_base = 0;
static MADT_IOAPIC* ioapic_rec = NULL;
static uint64_t ticks_per_ms = 0;

/*
 * disable_pic - Disable the legacy 8259 PICs
 */
void disable_pic(void) {

    // reinit the PIC to a known state before masking
    outb(PIC_COMMAND_MASTER, ICW_1);
    io_wait();
    outb(PIC_COMMAND_SLAVE, ICW_1);
    io_wait();

    // remap IRQs above the exception range (0-31)
    outb(PIC_DATA_MASTER, ICW_2_M);
    io_wait();
    outb(PIC_DATA_SLAVE, ICW_2_S);
    io_wait();

    outb(PIC_DATA_MASTER, ICW_3_M);
    io_wait();
    outb(PIC_DATA_SLAVE, ICW_3_S);
    io_wait();

    outb(PIC_DATA_MASTER, ICW_4);
    io_wait();
    outb(PIC_DATA_SLAVE, ICW_4);
    io_wait();

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
    if (lapic_base == 0) return;
    *(volatile uint32_t*)(lapic_base + reg) = value;
}

/*
 * lapic_read - Read a value from a LAPIC register
 */
uint32_t lapic_read(uint32_t reg) {
    if (lapic_base == 0) return 0;
    return *(volatile uint32_t*)(lapic_base + reg);
}

/*
 * lapic_eoi - Send End Of Interrupt signal to LAPIC
 */
void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

/*
 * lapic_init - Initialize the Local APIC
 */
void lapic_init(void) {

    // ensure we are not running on a potato
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & (1 << 9))) {
        panic("System does not support APIC!");
    }

    uint64_t apic_msr = read_msr(MSR_IA32_APIC_BASE);
    if (!(apic_msr & MSR_APIC_BASE_ENABLE)) {
        LOGF("[APIC] LAPIC hardware disabled in MSR. Enabling...\n");
        apic_msr |= MSR_APIC_BASE_ENABLE;
        write_msr(MSR_IA32_APIC_BASE, apic_msr);
    }

    uint64_t phys_base = apic_msr & 0xFFFFF000;
    if (lapic_base == 0) {
        void* virt_addr = NULL;
        if (vmm_alloc(NULL, PAGE_SIZE, VM_FLAG_WRITE | VM_FLAG_MMIO, (void*)phys_base, &virt_addr) != VMM_OK)
            panic("Failed to map LAPIC memory.");
        lapic_base = (uint64_t)virt_addr;
    }

    lapic_write(LAPIC_SPURIOUS, LAPIC_SW_ENABLE | LAPIC_SPURIOUS_IV);
    lapic_write(LAPIC_TPR, 0);

    if (c & (1 << 21)) {
        LOGF("[APIC] X2APIC support detected.\n");
        // we are NOT handling that lol, I got enough on my plate
    }

    // Parse MADT for LAPIC and I/O APIC info
    MADTHeader* madt = (MADTHeader*)acpi_find_table("APIC");
    if (madt) {
        uint8_t* start = (uint8_t*)(madt + 1);
        uint8_t* end = (uint8_t*)madt + madt->header.Length;
        uint32_t my_id = lapic_get_id();

        while (start < end) {
            MADTRecordHeader* header = (MADTRecordHeader*)start;
            if (header->type == MADT_TYPE_NMI) {
                MADT_NMI* nmi = (MADT_NMI*)header;
                // 0xFF targets all processors; otherwise match by local APIC ID
                if (nmi->acpi_processor_id == 0xFF || nmi->acpi_processor_id == my_id) {
                    uint32_t lvt_reg = (nmi->lint == 0) ? LAPIC_LVT_LINT0 : LAPIC_LVT_LINT1;
                    lapic_write(lvt_reg, (4 << 8));
                    LOGF("[APIC] Configured LINT%u as NMI\n", nmi->lint);
                }
            }
            start += header->length;
        }
    }

    LOGF("[APIC] LAPIC initialized. Local ID: %u, Version: 0x%X\n", lapic_get_id(), lapic_read(LAPIC_VER) & 0xFF);
}

/*
 * lapic_send_ipi - Send an Inter-Processor Interrupt to another core
 */
void lapic_send_ipi(uint32_t dest_id, uint8_t vector) {
    // spin until delivery bit clears before writing ICR
    while (lapic_read(LAPIC_ICR_LOW) & (1 << 12));

    lapic_write(LAPIC_ICR_HIGH, dest_id << 24);
    lapic_write(LAPIC_ICR_LOW, vector);
}

/*
 * lapic_set_tpm - Sets the calibrated tick rate
 */
void lapic_set_tpm(uint64_t tpm) {
    ticks_per_ms = tpm;
}

/*
 * lapic_timer_oneshot - Arms the LAPIC timer in one-shot mode
 */
void lapic_timer_oneshot(uint32_t us, uint8_t vector) {
    if (ticks_per_ms == 0) return;
    uint32_t ticks = (uint32_t)(((uint64_t)us * ticks_per_ms) / 1000);
    
    lapic_write(LAPIC_TDCR, 0x03); // Divide by 16
    lapic_write(LAPIC_LVT_TIMER, vector);
    lapic_write(LAPIC_TICR, ticks);
}

/*
 * lapic_timer_periodic - Arms the LAPIC timer in periodic mode
 */
void lapic_timer_periodic(uint32_t us, uint8_t vector) {
    if (ticks_per_ms == 0) return;
    uint32_t ticks = (uint32_t)(((uint64_t)us * ticks_per_ms) / 1000);

    lapic_write(LAPIC_TDCR, 0x03); // Divide by 16
    lapic_write(LAPIC_LVT_TIMER, vector | LVT_TIMER_PERIODIC);
    lapic_write(LAPIC_TICR, ticks);
}

/*
 * lapic_timer_stop - Stops the Local APIC timer
 */
void lapic_timer_stop(void) {
    lapic_write(LAPIC_LVT_TIMER, LVT_MASK);
    lapic_write(LAPIC_TICR, 0);
}

#pragma endregion

#pragma region IOAPIC

/*
 * ioapic_read - Read a value from an I/O APIC register
 */
uint32_t ioapic_read(uint32_t reg) {
    if (ioapic_base == 0) return 0;
    *(volatile uint32_t*)(ioapic_base + IOAPIC_REGSEL) = reg;
    return *(volatile uint32_t*)(ioapic_base + IOAPIC_IOWIN);
}

/*
 * ioapic_write - Write a value to an I/O APIC register
 */
void ioapic_write(uint32_t reg, uint32_t value) {
    if (ioapic_base == 0) return;
    *(volatile uint32_t*)(ioapic_base + IOAPIC_REGSEL) = reg;
    *(volatile uint32_t*)(ioapic_base + IOAPIC_IOWIN) = value;
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

    // Author notes:
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

    entry |= ((uint64_t)dest_core << 56);
    
    // start masked, drivers call ioapic_unmask() when ready
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
    MADTHeader* madt = (MADTHeader*)acpi_find_table("APIC");
    if (!madt) {
        panic("MADT (APIC) table not found!");
    }

    uint8_t* start = (uint8_t*)(madt + 1);
    uint8_t* end = (uint8_t*)madt + madt->header.Length;

    while (start < end) {
        MADTRecordHeader* header = (MADTRecordHeader*)start;
        if (header->type == MADT_TYPE_IOAPIC) {
            ioapic_rec = (MADT_IOAPIC*)header;
            break;
        }
        start += header->length;
    }

    if (!ioapic_rec) {
        panic("No I/O APIC record found in MADT!");
    }

    uint64_t phys = ioapic_rec->io_apic_address;
    void* virt_addr = NULL;
    if (vmm_alloc(NULL, PAGE_SIZE, VM_FLAG_WRITE | VM_FLAG_MMIO, (void*)phys, &virt_addr) != VMM_OK)
        panic("Failed to map I/O APIC memory.");
    ioapic_base = (uint64_t)virt_addr;

    uint32_t ver = ioapic_read(IOAPIC_VER);
    uint32_t count = ((ver >> 16) & 0xFF) + 1;

    // default 1:1 ISA mapping, everything masked
    uint32_t gsi_base = ioapic_rec->global_system_interrupt_base;
    uint32_t bsp_id = lapic_get_id();

    for (uint32_t i = 0; i < count; i++) {
        uint64_t entry = (1 << 16) | (32 + gsi_base + i);
        entry |= ((uint64_t)bsp_id << 56);
        ioapic_set_entry(i, entry);
    }

    start = (uint8_t*)(madt + 1);
    while (start < end) {
        MADTRecordHeader* header = (MADTRecordHeader*)start;
        if (header->type == MADT_TYPE_ISO) {
            MADT_ISO* iso = (MADT_ISO*)header;
            if (iso->global_system_interrupt >= gsi_base &&
                iso->global_system_interrupt < gsi_base + count) {
                uint32_t io_index = iso->global_system_interrupt - gsi_base;
                LOGF("[APIC] ISO: IRQ %u -> GSI %u (Flags: 0x%X)\n",
                     iso->irq_source, iso->global_system_interrupt, iso->flags);
                ioapic_redirect(io_index, 32 + iso->irq_source, bsp_id, iso->flags);
            }
        }
        start += header->length;
    }

    LOGF("[APIC] I/O APIC initialized at 0x%lX. %u redirection entries.\n", ioapic_base, count);
}

#pragma endregion

/*
 * apic_init - High-level APIC initialization
 */
void apic_init(void) {
    LOGF("[APIC] Beginning hardware interrupt controller setup...\n");

    // disable museum mode
    disable_pic();
    // enable lapic
    lapic_init();
    //enable ioapic for external interrupts
    ioapic_init();

    LOGF("[APIC] Interrupt subsystem is online.\n");
}