/*
 * syscall.c - Syscall initialization and dispatching
 *
 * Configures the MSRs for the syscall/sysret instructions and
 * dispatches syscalls from userspace.
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/sys/syscall.h>
#include <arch/x86_64/cpu/cpu.h>
#include <arch/x86_64/cpu/gdt.h>
#include <arch/x86_64/cpu/msr.h>
#include <kernel/sys/scheduler.h>
#include <kernel/sys/process.h>
#include <klibc/stdio.h>
#include <kernel/memory/vmm.h>
#include <kernel/drivers/tty.h>
#include <kernel/debug.h>
#include <kernel/memory/heap.h>
#include <klibc/string.h>

extern void syscall_entry(void);

void syscall_init(void) {
    uint64_t efer = read_msr(MSR_EFER);
    efer |= EFER_SCE; 
    write_msr(MSR_EFER, efer);

    // STAR MSR: 
    // Bits 63:48 - User CS/SS base. sysret uses STAR[63:48]+16 for CS, STAR[63:48]+8 for SS.
    // We want User CS = 0x20 (index 4), User SS = 0x18 (index 3).
    // So base should be 0x10.
    // Bits 47:32 - Kernel CS/SS base. syscall uses STAR[47:32] for CS, STAR[47:32]+8 for SS.
    // We want Kernel CS = 0x08, Kernel SS = 0x10.
    // So base should be 0x08.

    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
    write_msr(MSR_STAR, star);

    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

    // FMASK MSR - RFLAGS to clear on syscall. We clear IF (bit 9), DF (bit 10).
    write_msr(MSR_FMASK, 0x200 | 0x400);

    LOGF("[SYSCALL] Syscall interface initialized.\n");
}

/*
 * syscall_dispatcher - Called from assembly stub
 */
void syscall_dispatcher(uint64_t syscall_num, uint64_t* registers) {
    // registers[0] = r15 ... registers[14] = rax
    // According to our push order:
    // rdi = registers[9]
    // rsi = registers[10]
    // rdx = registers[11]
    
    thread_t* current = sched_current();
    if (!current) return;

    switch (syscall_num) {
        case SYS_EXIT:
            sched_exit();
            break;
            
        case SYS_WRITE: {
            const char* buf = (const char*)registers[9];
            size_t len = (size_t)registers[10];

            if (!buf || len == 0) {
                registers[14] = (uint64_t)-1;
                break;
            }

            if (len > 65536) len = 65536;
            char* kbuf = kmalloc(len);
            if (!kbuf) {
                registers[14] = (uint64_t)-1;
                break;
            }

            bool ints = intr_save();
            if (!vmm_check_buffer(current->process->vmm, buf, len, VM_FLAG_USER)) {
                intr_restore(ints);
                kfree(kbuf);
                LOGF("[SYSCALL] SYS_WRITE: Invalid buffer pointer 0x%lx (len: %zu) from thread '%s' (PID %u)\n", (uintptr_t)buf, len, current->name, current->process ? current->process->pid : 0);
                sched_exit();
                break;
            }
            smap_allow();
            kmemcpy(kbuf, buf, len);
            smap_deny();
            intr_restore(ints);

            if (current->process && current->process->tty) {
                tty_write(current->process->tty, kbuf, len);
            }
            kfree(kbuf);
            registers[14] = (uint64_t)len;
            break;
        }
            
        case SYS_MMAP: {
            void* addr = (void*)registers[9];
            size_t length = (size_t)registers[10];
            size_t vm_flags = (size_t)registers[11];
            
            size_t user_allowed_flags = VM_FLAG_WRITE | VM_FLAG_EXEC | VM_FLAG_LAZY;
            vm_flags &= user_allowed_flags;

            void* out_addr = NULL;
            vmm_status_t status;
            
            if (addr) {
                status = vmm_alloc_at(current->process->vmm, addr, length, vm_flags | VM_FLAG_USER, NULL, &out_addr);
            } else {
                status = vmm_alloc(current->process->vmm, length, vm_flags | VM_FLAG_USER, NULL, &out_addr);
            }
            
            if (status == VMM_OK) {
                registers[14] = (uint64_t)out_addr; // rax
            } else {
                registers[14] = (uint64_t)-1;
            }
            break;
        }
        
        case SYS_MUNMAP: {
            void* addr = (void*)registers[9];
            vmm_free(current->process->vmm, addr);
            registers[14] = 0;
            break;
        }
        
        case SYS_YIELD:
            sched_yield();
            break;

        case SYS_SLEEP_MS: {
            uint64_t ms = registers[9];
            sched_sleep(ms);
            break;
        }

        case SYS_READ: {
            char*  buf   = (char*)registers[9];
            size_t count = (size_t)registers[10];

            if (!buf || count == 0) {
                registers[14] = (uint64_t)-1;
                break;
            }

            if (count > 4096) count = 4096;

            if (!vmm_check_buffer(current->process->vmm, buf, count, VM_FLAG_USER | VM_FLAG_WRITE)) {
                LOGF("[SYSCALL] SYS_READ: invalid buffer 0x%lx (len: %zu) from '%s'\n", (uintptr_t)buf, count, current->name);
                registers[14] = (uint64_t)-1;
                break;
            }

            tty_t* tty = current->process->tty;
            if (!tty) {
                registers[14] = (uint64_t)-1;
                break;
            }

            char* kbuf = kmalloc(count);
            if (!kbuf) {
                registers[14] = (uint64_t)-1;
                break;
            }

            size_t n = tty_read(tty, kbuf, count);

            bool ints = intr_save();
            if (!vmm_check_buffer(current->process->vmm, buf, n, VM_FLAG_USER | VM_FLAG_WRITE)) {
                intr_restore(ints);
                kfree(kbuf);
                sched_exit();
                break;
            }

            smap_allow();
            kmemcpy(buf, kbuf, n);
            smap_deny();
            intr_restore(ints);
            kfree(kbuf);

            registers[14] = (uint64_t)n;
            break;
        }
        
        case SYS_TTY_CTRL: {
            uint64_t cmd  = registers[9];
            uint64_t arg2 = registers[10];
            
            tty_t* tty = current->process->tty;
            if (!tty || !tty->console) {
                registers[14] = (uint64_t)-1;
                break;
            }
            
            switch (cmd) {
                case TTY_CTRL_CLEAR:
                    con_clear(tty->console, CONSOLE_COLOR_BLACK);
                    registers[14] = 0;
                    break;
                case TTY_CTRL_CURSOR: {
                    uint8_t enabled = arg2 & 0xFF;
                    con_enable_cursor(tty->console, enabled);
                    registers[14] = 0;
                    break;
                }
                case TTY_CTRL_GET_DIMS: {
                    // Pack width and height into a single 64-bit return value
                    // High 32 bits is height (excluding sticky header rows)
                    // Low 32 bits is width
                    uint32_t width = (uint32_t)tty->console->width;
                    uint32_t height = (uint32_t)(tty->console->height - tty->console->header_rows);
                    registers[14] = ((uint64_t)height << 32) | (uint64_t)width;
                    break;
                }
                default:
                    registers[14] = (uint64_t)-1;
                    break;
            }
            break;
        }

        case SYS_SET_FS_BASE: {
            uint64_t base = registers[9];
            // Disallow setting FS base to values in the kernel space range to prevent abuse
            if (base >= 0x0000800000000000ULL) {
                registers[14] = (uint64_t)-1;
                break;
            }
            current->fs_base = base;
            write_msr(MSR_FS_BASE, base);
            registers[14] = 0;
            break;
        }

        default:
            LOGF("[SYSCALL] Unknown syscall: %lu from thread '%s' (PID %u)\n", syscall_num, current->name, current->process ? current->process->pid : 0);

            sched_exit();
            break;
    }
}
