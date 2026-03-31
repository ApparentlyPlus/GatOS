/*
 * interrupts.c - Functions for managing CPU interrupts
 *
 * This file implements the Interrupt Descriptor Table (IDT) initialization,
 * interrupt dispatching, and handler registration.
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/cpu/interrupts.h>
#include <arch/x86_64/cpu/gdt.h>
#include <klibc/stdio.h>
#include <klibc/string.h>
#include <arch/x86_64/cpu/cpu.h>
#include <kernel/sys/panic.h>
#include <kernel/sys/apic.h>
#include <kernel/sys/scheduler.h>
#include <kernel/sys/process.h>
#include <kernel/debug.h>
#include <kernel/misc.h>
#include <kernel/memory/pmm.h>
#include <arch/x86_64/memory/paging.h>

#pragma region Types and Globals

idt_entry_t idt[IDT_SIZE] = {0};
static irq_handler_t irq_handlers[IDT_SIZE] = {0};
extern char interrupt_handler_0[];

#pragma endregion


#pragma region Interrupt Management API

/*
 * irq_register - Register a custom handler for a specific vector
 */
void irq_register(uint8_t vector, irq_handler_t handler)
{
    irq_handlers[vector] = handler;
}

/*
 * irq_unregister - Remove a custom handler for a specific vector
 */
void irq_unregister(uint8_t vector)
{
    irq_handlers[vector] = NULL;
}

#pragma endregion

#pragma region IDT Setup

/*
 * set_idt_entry - Populate a single IDT entry
 */
void set_idt_entry(uint8_t vector, void* handler, uint8_t dpl, uint8_t ist_index)
{
    uint64_t handler_addr = (uint64_t)handler;

    idt_entry_t* entry = &idt[vector];
    entry->address_low = handler_addr & 0xFFFF;
    entry->address_mid = (handler_addr >> 16) & 0xFFFF;
    entry->address_high = handler_addr >> 32;
    entry->selector = KERNEL_CS;
    entry->flags = INTERRUPT_GATE | ((dpl & 0b11) << 5) | (1 << 7);
    entry->ist = ist_index & 0x7; // 0 = no IST stack switching

    entry->reserved = 0;
}

/*
 * load_idt - Load the IDT pointer into the CPU (LIDT)
 */
void load_idt(void* idt_addr)
{
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr;

    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)idt_addr;

    __asm__ volatile("lidt %0" :: "m"(idtr));
}

/*
 * idt_init - Initialize the Interrupt Descriptor Table
 */
void idt_init(void)
{
    // imagine using the PIC in the big 2026, this is some museum stuff here
    // jokes aside, we'll use the modern APIC instead
    disable_pic();

    // The stubs are spaced 16 bytes apart in the interrupt_handler_0 block
    // This is a hacky trick, but it works, and the alternative is hardcoding them all, so this will do
    for (size_t i = 0; i < IDT_SIZE; i++)
    {
        void* handler = (void*)((uint64_t)interrupt_handler_0 + (i * 16));
        uint8_t ist = 0;
        uint8_t dpl = DPL_RING_0;

        if (i == INT_DOUBLE_FAULT) {
            ist = 1;
        }
        else if (i == INT_PAGE_FAULT) {
            ist = 2;
        }

        // Breakpoint and Debug can be triggered from Ring 3 for debugging
        // but this is untested and possibly out of scope for now
        else if (i == INT_BREAKPOINT || i == INT_DEBUG) {
            dpl = DPL_RING_3;
        }

        set_idt_entry(i, handler, dpl, ist);
    }

    load_idt((void*)idt);

    LOGF("[IDT] Interrupt Descriptor Table initialized and loaded.\n");
}

#pragma endregion

#pragma region Dispatcher

/*
 * interrupt_dispatcher - Central C handler called by assembly stubs
 */
cpu_context_t* interrupt_dispatcher(cpu_context_t* context)
{
    uint64_t vec = context->vector_number;
   
    // Spurious interrupts do not require an EOI
    if (vec == INT_SPURIOUS_INTERRUPT) {
        return context;
    }

    if (irq_handlers[vec] != NULL) {
        context = irq_handlers[vec](context);
        if (!context) {
            panic("IRQ handler returned NULL context");
        }

        // Validate iret frame CS and SS to catch smashed frames before iretq faults
        // This helped me debug the sysretq microarhitecture implementation nightmare
        // And therefore can be useful to capture frame corruption early
        uint16_t cs = (uint16_t)context->iret_cs;
        uint16_t ss = (uint16_t)context->iret_ss;
        if ((cs & 3) == 0) {
            if (cs != KERNEL_CS)
                panicf_c(context, "Bad kernel CS in iret frame (cs=0x%04x ss=0x%04x vec=%lu)", cs, ss, vec);
            if (ss != 0 && ss != KERNEL_DS)
                panicf_c(context, "Bad kernel SS in iret frame (ss=0x%04x cs=0x%04x vec=%lu)", ss, cs, vec);
        } else {
            if (cs != USER_CS)
                panicf_c(context, "Bad user CS in iret frame (cs=0x%04x ss=0x%04x vec=%lu)", cs, ss, vec);
            if (ss != USER_DS)
                panicf_c(context, "Bad user SS in iret frame (ss=0x%04x cs=0x%04x vec=%lu)", ss, cs, vec);
        }

        // exceptions don't need EOI, only real hw interrupts
        if (vec >= INT_FIRST_INTERRUPT) {
            lapic_eoi();
        }
        return context;
    }

    if (vec < INT_FIRST_INTERRUPT) {
        const char* panic_msg = "Unknown Exception";
        
        switch (vec) {
            case INT_DIVIDE_ERROR:         panic_msg = "Divide by zero"; break;
            case INT_DEBUG:                panic_msg = "Debug trap"; break;
            case INT_NMI:                  panic_msg = "Non-maskable interrupt"; break;
            case INT_BREAKPOINT:           panic_msg = "Breakpoint"; break;
            case INT_OVERFLOW:             panic_msg = "Overflow"; break;
            case INT_BOUND_RANGE:          panic_msg = "Bound range exceeded"; break;
            case INT_INVALID_OPCODE:       panic_msg = "Invalid opcode"; break;
            case INT_DEVICE_NOT_AVAILABLE: panic_msg = "Device not available (FPU)"; break;
            case INT_DOUBLE_FAULT:         panic_msg = "Double Fault (Critical)"; break;
            case INT_INVALID_TSS:          panic_msg = "Invalid TSS"; break;
            case INT_SEGMENT_NOT_PRESENT:  panic_msg = "Segment not present"; break;
            case INT_STACK_SEGMENT_FAULT:  panic_msg = "Stack segment fault"; break;
            case INT_GENERAL_PROTECTION:   panic_msg = "General protection fault"; break;
            case INT_PAGE_FAULT:           panic_msg = "Page Fault"; break;
            case INT_X87_FPU_ERROR:        panic_msg = "x87 FPU error"; break;
            case INT_ALIGNMENT_CHECK:      panic_msg = "Alignment check"; break;
            case INT_MACHINE_CHECK:        panic_msg = "Machine check"; break;
            case INT_SIMD_ERROR:           panic_msg = "SIMD exception"; break;
        }

        // demand paging
        // try proc VMM first, fall back to kernel's
        if (vec == INT_PAGE_FAULT) {
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

            vmm_t* demand_vmm = NULL;
            vm_object* demand_obj = NULL;

            thread_t* current = sched_current();
            if (current && current->process && current->process->vmm) {
                demand_obj = vmm_find_mapped_object(current->process->vmm, (void*)cr2);
                if (demand_obj && (demand_obj->flags & VM_FLAG_LAZY)) {
                    demand_vmm = current->process->vmm;
                }
            }

            if (!demand_vmm) {
                demand_obj = vmm_find_mapped_object(vmm_kernel_get(), (void*)cr2);
                if (demand_obj && (demand_obj->flags & VM_FLAG_LAZY)) {
                    demand_vmm = vmm_kernel_get();
                }
            }

            if (demand_vmm && demand_obj) {
                uint64_t phys_base;
                if (pmm_alloc(PAGE_SIZE, &phys_base) == PMM_OK) {
                    uint64_t page_aligned_cr2 = cr2 & ~(PAGE_SIZE - 1);
                    kmemset((void*)PHYSMAP_P2V(phys_base), 0, PAGE_SIZE);
                    vmm_status_t status = vmm_map_page(demand_vmm, phys_base, (void*)page_aligned_cr2, demand_obj->flags);
                    if (status == VMM_OK) {
                        return context;
                    } else if (status == VMM_ERR_ALREADY_MAPPED) {
                        pmm_free(phys_base, PAGE_SIZE); // Race: already backed by another CPU
                        return context;
                    } else {
                        pmm_free(phys_base, PAGE_SIZE);
                    }
                }
            }
        }

        bool is_user = (context->iret_cs & 3) == 3;

        // If it's a user fault, we can kill the offending process instead of panicking the whole system
        if (is_user && sched_active()) {
            thread_t* current = sched_current();
            if (current && current->process) {
                char buf[256];
                int len;
                
                if (vec == INT_PAGE_FAULT) {
                    uint64_t cr2;
                    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
                    // null rip + instr fetch = thread fell off its entry point
                    if (cr2 == 0 && (context->error_code & 16)) {
                        len = snprintf_(buf, sizeof(buf), "\n[Thread %u from %s (PID %u)] Returned from entry point (RIP: 0x%lx)\n",
                                       current->tid, current->process->name, current->process->pid, context->iret_rip);
                    } else {
                        len = snprintf_(buf, sizeof(buf), "\n[Thread %u from %s (PID %u)] Segmentation Fault at 0x%lx (RIP: 0x%lx)\n",
                                       current->tid, current->process->name, current->process->pid, cr2, context->iret_rip);
                    }
                } else {
                    len = snprintf_(buf, sizeof(buf), "\n[Thread %u from %s (PID %u)] %s (Vector %lu) at RIP: 0x%lx\n",
                                   current->tid, current->process->name, current->process->pid, panic_msg, vec, context->iret_rip);
                }
                
                if (current->process->tty) {
                    tty_write(current->process->tty, buf, (size_t)len);
                }
                LOGF("[USERSPACE FAULT] %s", buf + 1);

                current->state = T_DEAD;
                return sched_schedule(context);
            }
        }

        // For kernel faults or if we can't find the process info, panic the whole system
        if (vec == INT_PAGE_FAULT) {
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

            LOGF("[EXCEPTION] Page Fault at address: 0x%lx\n", cr2);
            LOGF("CR2 (Fault Address): 0x%lx\n", cr2);
            LOGF("Error Code: 0x%lx (P:%d W:%d U:%d R:%d I:%d)\n",
                context->error_code,
                (context->error_code & 1)  ? 1 : 0,
                (context->error_code & 2)  ? 1 : 0,
                (context->error_code & 4)  ? 1 : 0,
                (context->error_code & 8)  ? 1 : 0,
                (context->error_code & 16) ? 1 : 0);
        }

        LOGF("[PANIC] %s (Vector %d)\n", panic_msg, (int)vec);
        panic_c(panic_msg, context);
    }

    // no handler, but still gotta EOI or we'll never get another interrupt
    else {
        LOGF("[INT] Unhandled interrupt vector: %d\n", vec);
        lapic_eoi();
    }

    return context;
}
