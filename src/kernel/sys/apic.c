#include <arch/x86_64/cpu/cpu.h>
#include <kernel/memory/vmm.h>
#include <kernel/debug.h>
#include <kernel/sys/apic.h>

uint64_t get_lapic_address(bool mapped){
    uint64_t lapic_bitfield = read_msr(IA32_APIC_BASE);
    uint64_t lapic_addy = lapic_bitfield & 0xFFFFF000;
    if (lapic_bitfield & APIC_GLOBAL_ENABLE_BIT) { // check if enabled
        if (mapped) {
            if(vmm_map_page(NULL, lapic_addy, (void*)lapic_addy, VM_FLAG_WRITE) == VMM_OK){
                return lapic_addy;
            }
            else{
                LOGF("Failed to map LAPIC address");
                return 0;
            }
        }
        return lapic_addy; // return the address
    }
    return 0;
}