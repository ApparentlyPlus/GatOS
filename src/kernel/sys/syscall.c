/*
 * syscall.c - Syscall initialization and dispatching
 *
 * Configures the MSRs for the syscall/sysret instructions and
 * dispatches syscalls from userspace.
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/caps.h>
#include <kernel/sys/syscall.h>
#include <arch/x86_64/cpu/cpu.h>
#include <arch/x86_64/cpu/gdt.h>
#include <arch/x86_64/cpu/msr.h>
#include <kernel/sys/scheduler.h>
#include <kernel/sys/process.h>
#include <klibc/stdio.h>
#include <kernel/memory/vmm.h>
#include <kernel/drivers/tty.h>
#include <kernel/drivers/serial.h>
#include <kernel/debug.h>
#include <kernel/memory/heap.h>
#include <kernel/sys/timers.h>
#include <kernel/sys/power.h>
#include <klibc/string.h>

#ifdef GATA_CAP_THREADS

extern void syscall_entry(void);

void syscall_init(void) {
    uint64_t efer = read_msr(MSR_EFER);
    efer |= EFER_SCE; 
    write_msr(MSR_EFER, efer);

    // STAR MSR layout:
    //   STAR[47:32] = kernel CS base (SYSCALL loads CS=base, SS=base+8)
    //   STAR[63:48] = user CS/SS base (SYSRET loads CS=base+16, SS=base+8)
    //
    // GDT order: 0x08=KCode, 0x10=KData, 0x18=UData(DPL3), 0x20=UCode(DPL3)
    //
    // We set the user base to 0x13 (= 0x10 | RPL3) so that SYSRETQ computes:
    //   CS = 0x13 + 16 = 0x23 = USER_CS
    //   SS = 0x13 + 8  = 0x1B = USER_DS
    //
    // This avoids reliance on the CPU forcing SS.RPL=3, which certain Intel
    // microarchitectures fail to do (producing SS=0x18 instead of 0x1B)
    // Author's Note: Oooooof dude, this was a pain to figure out, thanks google

    uint64_t star = ((uint64_t)0x13 << 48) | ((uint64_t)0x08 << 32);
    write_msr(MSR_STAR, star);

    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

    // FMASK MSR - RFLAGS to clear on syscall. We clear IF (bit 9), DF (bit 10).
    write_msr(MSR_FMASK, 0x200 | 0x400);

    LOGF("[SYSCALL] Syscall interface initialized.\n");
}

/*
 * syscall_dispatcher - Called from syscall_entry.S with a pointer to
 * the full cpu_context_t built on the per-thread kernel stack
 */
void syscall_dispatcher(cpu_context_t* regs) {
    thread_t* current = sched_current();
    if (!current) return;

    uint64_t syscall_num = regs->rax;

    switch (syscall_num) {
        case SYS_EXIT:
            sched_exit();
            break;
            
        case SYS_WRITE: {
            const char* buf = (const char*)regs->rdi;
            size_t len = (size_t)regs->rsi;

            if (!buf || len == 0) {
                regs->rax = (uint64_t)-1;
                break;
            }

            if (len > 65536) len = 65536;

            // Copy through a fixed stack buffer instead of a per-call kmalloc.
            // The user buffer is validated and copied in small sub-chunks with
            // interrupts disabled (so it cannot be remapped mid-copy and the
            // IRQ-off window stays bounded), but each filled buffer is handed
            // to the TTY in a single call so the console flushes once per
            // buffer rather than once per copy chunk.
            char kbuf[4096];
            size_t done = 0;
            while (done < len) {
                size_t block = len - done;
                if (block > sizeof(kbuf)) block = sizeof(kbuf);

                size_t filled = 0;
                while (filled < block) {
                    size_t n = block - filled;
                    if (n > 1024) n = 1024;

                    bool ints = intr_save();
                    if (!vmm_check_buffer(current->process->vmm, buf + done + filled, n, VM_FLAG_USER)) {
                        intr_restore(ints);
                        LOGF("[SYSCALL] SYS_WRITE: Invalid buffer pointer 0x%lx (len: %zu) from thread '%s' (PID %u)\n", (uintptr_t)buf, len, current->name, current->process ? current->process->pid : 0);
                        sched_exit();
                    }

                    // SMAP must be relaxed while touching user memory
                    smap_allow();
                    kmemcpy(kbuf + filled, buf + done + filled, n);
                    smap_deny();
                    intr_restore(ints);
                    filled += n;
                }

                // A userspace thread's stdout goes through this syscall
                // regardless of output mode - GATA_OUTPUT_SERIAL only rewires
                // the kernel realm's own _putchar (klibc/stdio.c), so without
                // this, "serial output" builds would still send userspace
                // program output to the (headless, invisible) framebuffer TTY.
                #ifdef GATA_OUTPUT_SERIAL
                serial_write_len_port(SERIAL_COM1, kbuf, block);
                #else
                if (current->process && current->process->tty) {
                    tty_write(current->process->tty, kbuf, block);
                }
                #endif
                done += block;
            }
            regs->rax = (uint64_t)len;
            break;
        }
            
        case SYS_MMAP: {
            void* addr = (void*)regs->rdi;
            size_t length = (size_t)regs->rsi;
            size_t vm_flags = (size_t)regs->rdx;
            
            // Don't allow userspace to set flags other than these
            size_t user_allowed_flags = VM_FLAG_WRITE | VM_FLAG_EXEC | VM_FLAG_LAZY;
            vm_flags &= user_allowed_flags;

            // Enforce W^X: no mapping may be both writable and executable
            if ((vm_flags & VM_FLAG_WRITE) && (vm_flags & VM_FLAG_EXEC)) {
                regs->rax = (uint64_t)-1;
                break;
            }

            void* out_addr = NULL;
            vmm_status_t status;
            
            if (addr) {
                status = vmm_alloc_at(current->process->vmm, addr, length, vm_flags | VM_FLAG_USER, NULL, &out_addr);
            } else {
                status = vmm_alloc(current->process->vmm, length, vm_flags | VM_FLAG_USER, NULL, &out_addr);
            }
            
            if (status == VMM_OK) {
                regs->rax = (uint64_t)out_addr;
            } else {
                regs->rax = (uint64_t)-1;
            }
            break;
        }
        
        case SYS_MUNMAP: {
            void* addr = (void*)regs->rdi;
            vmm_free(current->process->vmm, addr);
            regs->rax = 0;
            break;
        }
        
        case SYS_YIELD:
            sched_yield();
            break;

        case SYS_SLEEP_MS: {
            uint64_t ms = regs->rdi;
            sched_sleep(ms);
            break;
        }

        case SYS_READ: {
            char* buf = (char*)regs->rdi;
            size_t count = (size_t)regs->rsi;

            if (!buf || count == 0) {
                regs->rax = (uint64_t)-1;
                break;
            }

            // Don't allow reading more than 4096 bytes at once to prevent abuse
            // The TTY buffer is only 1024 bytes anyway, so this is more than enough
            if (count > 4096) count = 4096;

            if (!vmm_check_buffer(current->process->vmm, buf, count, VM_FLAG_USER | VM_FLAG_WRITE)) {
                LOGF("[SYSCALL] SYS_READ: invalid buffer 0x%lx (len: %zu) from '%s'\n", (uintptr_t)buf, count, current->name);
                regs->rax = (uint64_t)-1;
                break;
            }

            tty_t* tty = current->process->tty;
            if (!tty) {
                regs->rax = (uint64_t)-1;
                break;
            }

            // Read into a stack buffer instead of a per-call kmalloc, then
            // copy out in bounded chunks with interrupts disabled per chunk
            char kbuf[4096];
            size_t n = tty_read(tty, kbuf, count);

            size_t done = 0;
            while (done < n) {
                size_t c = n - done;
                if (c > 1024) c = 1024;

                bool ints = intr_save();
                if (!vmm_check_buffer(current->process->vmm, buf + done, c, VM_FLAG_USER | VM_FLAG_WRITE)) {
                    intr_restore(ints);
                    sched_exit();
                }

                smap_allow();
                kmemcpy(buf + done, kbuf + done, c);
                smap_deny();
                intr_restore(ints);
                done += c;
            }

            regs->rax = (uint64_t)n;
            break;
        }
        
        case SYS_TTY_CTRL: {
            uint64_t cmd = regs->rdi;
            uint64_t arg2 = regs->rsi;
            
            #ifdef GATA_OUTPUT_SERIAL

            (void)current;
            switch (cmd) {
                case TTY_CTRL_CLEAR:
                    serial_write_port(SERIAL_COM1, "\x1b[2J\x1b[H");
                    regs->rax = 0;
                    break;
                case TTY_CTRL_CURSOR:
                    serial_write_port(SERIAL_COM1, (arg2 & 0xFF) ? "\x1b[?25h" : "\x1b[?25l");
                    regs->rax = 0;
                    break;
                case TTY_CTRL_GET_DIMS:
                    regs->rax = ((uint64_t)24 << 32) | (uint64_t)80;
                    break;
                case TTY_CTRL_SET_COLOR: {

                    static const int a[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
                    char seq[24];
                    int fg = (int)(arg2 & 0xF), bg = (int)((arg2 >> 8) & 0xF);
                    ksnprintf(seq, sizeof(seq), "\x1b[%d;%dm",
                              (fg & 8) ? 90 + a[fg & 7] : 30 + a[fg & 7],
                              (bg & 8) ? 100 + a[bg & 7] : 40 + a[bg & 7]);
                    serial_write_port(SERIAL_COM1, seq);
                    regs->rax = 0;
                    break;
                }
                default:
                    regs->rax = (uint64_t)-1;
                    break;
            }
            #else
            tty_t* tty = current->process->tty;
            if (!tty || !tty->console) {
                regs->rax = (uint64_t)-1;
                break;
            }
            
            switch (cmd) {
                case TTY_CTRL_CLEAR:
                    con_clear(tty->console, CONSOLE_COLOR_BLACK);
                    regs->rax = 0;
                    break;
                case TTY_CTRL_CURSOR: {
                    uint8_t enabled = arg2 & 0xFF;
                    con_enable_cursor(tty->console, enabled);
                    regs->rax = 0;
                    break;
                }
                case TTY_CTRL_GET_DIMS: {
                    // height in high 32 bits, width in low 32
                    uint32_t width = (uint32_t)tty->console->width;
                    uint32_t height = (uint32_t)(tty->console->height - tty->console->header_rows);
                    regs->rax = ((uint64_t)height << 32) | (uint64_t)width;
                    break;
                }
                case TTY_CTRL_SET_COLOR: {
                    // bg in the next byte, fg in the low byte
                    uint8_t fg = (uint8_t)(arg2 & 0xFF);
                    uint8_t bg = (uint8_t)((arg2 >> 8) & 0xFF);
                    con_set_color(tty->console, fg, bg);
                    regs->rax = 0;
                    break;
                }
                default:
                    regs->rax = (uint64_t)-1;
                    break;
            }
            #endif
            break;
        }

        case SYS_POWEROFF:
            power_off();
            break;

        case SYS_REBOOT:
            reboot();
            break;

        case SYS_SET_FS_BASE: {
            uint64_t base = regs->rdi;
            // don't let userspace point FS into kernel memory
            if (base >= 0x0000800000000000ULL) {
                regs->rax = (uint64_t)-1;
                break;
            }
            current->fs_base = base;
            write_msr(MSR_FS_BASE, base);
            regs->rax = 0;
            break;
        }

        case SYS_DEBUG_WRITE: {
            const char* buf = (const char*)regs->rdi;
            size_t len = (size_t)regs->rsi;

            if (!buf || len == 0) {
                regs->rax = (uint64_t)-1;
                break;
            }
            if (len > 4096) len = 4096;

            // Validate and copy per chunk with interrupts disabled, so the
            // user can't remap the buffer between check and copy
            char kbuf[1024];
            size_t done = 0;
            while (done < len) {
                size_t n = len - done;
                if (n > sizeof(kbuf)) n = sizeof(kbuf);

                bool ints = intr_save();
                if (!vmm_check_buffer(current->process->vmm, buf + done, n, VM_FLAG_USER)) {
                    intr_restore(ints);
                    LOGF("[SYSCALL] SYS_DEBUG_WRITE: invalid buffer 0x%lx (len: %zu) from '%s'\n", (uintptr_t)buf, len, current->name);
                    regs->rax = (uint64_t)-1;
                    goto debug_write_done;
                }

                smap_allow();
                kmemcpy(kbuf, buf + done, n);
                smap_deny();
                intr_restore(ints);

                serial_write_len_port(SERIAL_COM3, kbuf, n);
                done += n;
            }
            regs->rax = (uint64_t)len;
            debug_write_done:
            break;
        }

        case SYS_TIME_NS:
            regs->rax = get_uptime_ns();
            break;

        default:
            LOGF("[SYSCALL] Unknown syscall: %lu from thread '%s' (PID %u)\n", syscall_num, current->name, current->process ? current->process->pid : 0);

            sched_exit();
            break;
    }
}

#endif // GATA_CAP_THREADS
