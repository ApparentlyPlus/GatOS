/*
 * test_multitasking.c - Scheduler and Process/Thread Validation Suite
 *
 * Author: Claude Code
 */

#include <kernel/sys/process.h>
#include <kernel/sys/scheduler.h>
#include <kernel/sys/userspace.h>
#include <kernel/sys/syscall.h>
#include <kernel/memory/heap.h>
#include <kernel/memory/vmm.h>
#include <arch/x86_64/cpu/gdt.h>
#include <arch/x86_64/cpu/cpu.h>
#include <arch/x86_64/cpu/msr.h>
#include <arch/x86_64/memory/layout.h>
#include <arch/x86_64/memory/paging.h>
#include <kernel/drivers/tty.h>
#include <kernel/debug.h>
#include <tests/tests.h>
#include <klibc/string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

static suite_t suite;

#pragma region Helpers

static void kentry(void* arg) { (void)arg; sched_exit(); }

userspace void uentry(void* arg) {
    (void)arg;
    while (1) __asm__ volatile("pause");
}

static size_t thread_count(process_t* p) {
    size_t n = 0;
    for (thread_t* t = p->threads; t; t = t->next) n++;
    return n;
}

static bool proc_in_list(process_t* target) {
    for (process_t* p = process_get_all(); p; p = p->next)
        if (p == target) return true;
    return false;
}
#pragma endregion

#pragma region Scheduler Bootstrap

static bool t_sched_active(void) {
    TEST_ASSERT(sched_active() == true);
    return true;
}

static bool t_sched_nn(void) {
    TEST_ASSERT(sched_current() != NULL);
    return true;
}

static bool t_sched_name(void) {
    TEST_ASSERT(kstrcmp(sched_current()->name, "kernel_main") == 0);
    return true;
}

static bool t_sched_stk(void) {
    TEST_ASSERT(sched_current()->kstack != NULL);
    return true;
}
#pragma endregion

#pragma region Process Create

static bool t_proc_nn(void) {
    process_t* p = process_create("t_proc", NULL);
    TEST_ASSERT(p != NULL);
    process_destroy(p);
    return true;
}

// Lives in the user image's .data, so every process starts with its own copy of it
static volatile uint32_t data_probe __attribute__((section(".user_data"))) = 0xC0DE1234;

static bool t_proc_data_private(void) {
    uintptr_t off = (uintptr_t)&data_probe - (uintptr_t)&USER_DATA_START;
    TEST_ASSERT((uintptr_t)&data_probe >= (uintptr_t)&USER_DATA_START);
    TEST_ASSERT((uintptr_t)&data_probe < (uintptr_t)&USER_DATA_END);

    process_t* a = process_create("t_data_a", NULL);
    process_t* b = process_create("t_data_b", NULL);
    TEST_ASSERT(a && b);

    uint64_t pa, pb;
    TEST_ASSERT(vmm_get_physical(a->vmm, (void*)&data_probe, &pa));
    TEST_ASSERT(vmm_get_physical(b->vmm, (void*)&data_probe, &pb));
    TEST_ASSERT(pa != pb && pa != (uintptr_t)&USER_DATA_LOAD_ADDR + off);

    // Both copies start out with the image's value
    volatile uint32_t* va = (volatile uint32_t*)PHYSMAP_P2V(pa);
    volatile uint32_t* vb = (volatile uint32_t*)PHYSMAP_P2V(pb);
    TEST_ASSERT(*va == 0xC0DE1234 && *vb == 0xC0DE1234);

    // A write in one process stays in that process
    *va = 0xDEAD;
    TEST_ASSERT(*vb == 0xC0DE1234);

    process_destroy(a);
    process_destroy(b);
    return true;
}

static bool t_proc_pid(void) {
    process_t* p = process_create("t_pid", NULL);
    TEST_ASSERT(p->pid > 0);
    process_destroy(p);
    return true;
}

static bool t_proc_name(void) {
    process_t* p = process_create("t_name_proc", NULL);
    TEST_ASSERT(kstrcmp(p->name, "t_name_proc") == 0);
    process_destroy(p);
    return true;
}

static bool t_proc_vmm(void) {
    process_t* p = process_create("t_vmm", NULL);
    TEST_ASSERT(p->vmm != NULL);
    process_destroy(p);
    return true;
}

static bool t_proc_tty(void) {
    process_t* p = process_create("t_tty", NULL);
    TEST_ASSERT(p->tty != NULL);
    process_destroy(p);
    return true;
}

static bool t_proc_nothr(void) {
    process_t* p = process_create("t_nothr", NULL);
    TEST_ASSERT(p->threads == NULL);
    process_destroy(p);
    return true;
}

static bool t_proc_inlist(void) {
    process_t* p = process_create("t_inlist", NULL);
    TEST_ASSERT(proc_in_list(p));
    process_destroy(p);
    return true;
}

static bool t_proc_rmlist(void) {
    process_t* p = process_create("t_rmlist", NULL);
    process_destroy(p);
    TEST_ASSERT(!proc_in_list(p));
    return true;
}
#pragma endregion

#pragma region PID Uniqueness

static bool t_pid_uniq(void) {
    #define NP 8
    process_t* procs[NP];
    for (int i = 0; i < NP; i++) procs[i] = process_create("t_pidu", NULL);
    for (int i = 0; i < NP; i++)
        for (int j = i+1; j < NP; j++)
            TEST_ASSERT(procs[i]->pid != procs[j]->pid);
    for (int i = 0; i < NP; i++) process_destroy(procs[i]);
    return true;
}

static bool t_pid_mono(void) {
    #define NM 8
    process_t* procs[NM];
    for (int i = 0; i < NM; i++) procs[i] = process_create("t_pidm", NULL);
    for (int i = 0; i < NM-1; i++)
        TEST_ASSERT(procs[i]->pid < procs[i+1]->pid);
    for (int i = 0; i < NM; i++) process_destroy(procs[i]);
    return true;
}
#pragma endregion

#pragma region Kernel Thread

static bool t_kt_nn(void) {
    process_t* p = process_create("t_ktnull", NULL);
    thread_t* t = thread_create(p, "kt", kentry, NULL, false, 0);
    TEST_ASSERT(t != NULL);
    process_destroy(p);
    return true;
}

static bool t_kt_state(void) {
    process_t* p = process_create("t_ktstate", NULL);
    thread_t* t = thread_create(p, "kt_s", kentry, NULL, false, 0);
    TEST_ASSERT(t->state == T_READY);
    process_destroy(p);
    return true;
}

static bool t_kt_link(void) {
    process_t* p = process_create("t_ktlink", NULL);
    thread_t* t = thread_create(p, "kt_l", kentry, NULL, false, 0);
    TEST_ASSERT(t->process == p);
    process_destroy(p);
    return true;
}

static bool t_kt_name(void) {
    process_t* p = process_create("t_ktname", NULL);
    thread_t* t = thread_create(p, "mythread", kentry, NULL, false, 0);
    TEST_ASSERT(kstrcmp(t->name, "mythread") == 0);
    process_destroy(p);
    return true;
}

static bool t_kt_stk(void) {
    process_t* p = process_create("t_ktstk", NULL);
    thread_t* t = thread_create(p, "kt_stk", kentry, NULL, false, 0);
    TEST_ASSERT(t->kstack != NULL);
    process_destroy(p);
    return true;
}

static bool t_kt_ctx(void) {
    process_t* p = process_create("t_ktctx", NULL);
    thread_t* t = thread_create(p, "kt_ctx", kentry, (void*)0xDEAD, false, 0);
    // context is now embedded in the thread struct, not on the kstack
    TEST_ASSERT(t->context.iret_cs == KERNEL_CS);
    uintptr_t ctx = (uintptr_t)&t->context;
    TEST_ASSERT(ctx >= (uintptr_t)t && ctx < (uintptr_t)t + sizeof(thread_t));
    process_destroy(p);
    return true;
}

static bool t_kt_cs(void) {
    process_t* p = process_create("t_ktcs", NULL);
    thread_t* t = thread_create(p, "kt_cs", kentry, NULL, false, 0);
    TEST_ASSERT(t->context.iret_cs == KERNEL_CS);
    process_destroy(p);
    return true;
}

static bool t_kt_ss(void) {
    process_t* p = process_create("t_ktss", NULL);
    thread_t* t = thread_create(p, "kt_ss", kentry, NULL, false, 0);
    TEST_ASSERT(t->context.iret_ss == KERNEL_DS);
    process_destroy(p);
    return true;
}

static bool t_kt_arg(void) {
    process_t* p = process_create("t_ktarg", NULL);
    thread_t* t = thread_create(p, "kt_arg", kentry, (void*)0xBEEF, false, 0);
    /* thread_wrap(entry, arg): entry→rdi, arg→rsi */
    TEST_ASSERT(t->context.rsi == 0xBEEF);
    process_destroy(p);
    return true;
}

static bool t_kt_fpu_align(void) {
    process_t* p = process_create("t_ktfpu", NULL);
    thread_t* t = thread_create(p, "kt_fpu", kentry, NULL, false, 0);
    TEST_ASSERT(((uintptr_t)t->fpu & (FPU_AREA_ALIGN - 1)) == 0);
    TEST_ASSERT(t->fpu >= t->fpu_raw);
    TEST_ASSERT(t->fpu + FPU_AREA_SIZE <= t->fpu_raw + sizeof(t->fpu_raw));
    process_destroy(p);
    return true;
}

#pragma endregion

#pragma region FPU

static uint8_t fpu_live[FPU_AREA_SIZE] __attribute__((aligned(FPU_AREA_ALIGN)));
static uint8_t fpu_test[FPU_AREA_SIZE] __attribute__((aligned(FPU_AREA_ALIGN)));

/*
 * The live registers belong to whoever owns the FPU, so each test saves them first, borrows the
 * FPU with interrupts off and puts everything back, TS included
 */
static bool fpu_borrow(uint64_t* cr0) {
    bool irq = intr_save();
    *cr0 = read_cr0();
    __asm__ volatile("clts" ::: "memory");
    fpu_save(fpu_live);
    return irq;
}

static void fpu_return(bool irq, uint64_t cr0) {
    fpu_restore(fpu_live);
    write_cr0(cr0);
    intr_restore(irq);
}

static bool t_fpu_fresh(void) {
    process_t* p = process_create("t_fpufresh", NULL);
    thread_t* t = thread_create(p, "kt_fresh", kentry, NULL, false, 0);

    uint64_t cr0;
    bool irq = fpu_borrow(&cr0);
    uint32_t mxcsr = 0;
    uint16_t fcw = 0;
    fpu_restore(t->fpu);
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    __asm__ volatile("fnstcw %0" : "=m"(fcw));
    fpu_return(irq, cr0);

    TEST_ASSERT(mxcsr == 0x1F80);
    TEST_ASSERT(fcw == 0x037F);
    process_destroy(p);
    return true;
}

static bool t_fpu_roundtrip(void) {
    // With AVX on, the top 16 bytes live in the YMM upper half that fxsave never saw
    bool avx = cpu_is_feature_enabled(CF_AVX);
    uint64_t in[4] = { 0x1111111111111111, 0x2222222222222222, 0x3333333333333333, 0x4444444444444444 };
    uint64_t out[4] = { 0 };

    uint64_t cr0;
    bool irq = fpu_borrow(&cr0);
    if (avx) {
        __asm__ volatile("vmovdqu %0, %%ymm0" :: "m"(in) : "memory");
        fpu_save(fpu_test);
        __asm__ volatile("vpxor %%ymm0, %%ymm0, %%ymm0" ::: "memory");
        fpu_restore(fpu_test);
        __asm__ volatile("vmovdqu %%ymm0, %0" : "=m"(out) :: "memory");
    } else {
        __asm__ volatile("movdqu %0, %%xmm0" :: "m"(in) : "memory");
        fpu_save(fpu_test);
        __asm__ volatile("pxor %%xmm0, %%xmm0" ::: "memory");
        fpu_restore(fpu_test);
        __asm__ volatile("movdqu %%xmm0, %0" : "=m"(out) :: "memory");
        out[2] = in[2];
        out[3] = in[3];
    }
    fpu_return(irq, cr0);

    for (int i = 0; i < 4; i++) TEST_ASSERT(out[i] == in[i]);
    return true;
}

#pragma endregion

#pragma region User Thread

static bool t_ut_nn(void) {
    process_t* p = process_create("t_utnull", NULL);
    thread_t* t = thread_create(p, "ut", uentry, NULL, true, 0);
    TEST_ASSERT(t != NULL);
    process_destroy(p);
    return true;
}

static bool t_ut_stk(void) {
    process_t* p = process_create("t_utstk", NULL);
    thread_t* t = thread_create(p, "ut_stk", uentry, NULL, true, 0);
    TEST_ASSERT(t->ustack != NULL);
    process_destroy(p);
    return true;
}

static bool t_ut_ctx(void) {
    process_t* p = process_create("t_utctx", NULL);
    thread_t* t = thread_create(p, "ut_ctx", uentry, NULL, true, 0);
    TEST_ASSERT(t->context.iret_cs == USER_CS);
    process_destroy(p);
    return true;
}

static bool t_ut_align(void) {
    /* x86-64 SysV ABI: RSP % 16 == 8 at function entry point */
    process_t* p = process_create("t_utalign", NULL);
    thread_t* t = thread_create(p, "ut_align", uentry, NULL, true, 0);
    uintptr_t rsp = (uintptr_t)t->context.iret_rsp;
    TEST_ASSERT((rsp % 16) == 8);
    process_destroy(p);
    return true;
}
#pragma endregion

#pragma region TID Uniqueness

static bool t_tid_same(void) {
    process_t* p = process_create("t_tidu", NULL);
    thread_t* t1 = thread_create(p, "t1", kentry, NULL, false, 0);
    thread_t* t2 = thread_create(p, "t2", kentry, NULL, false, 0);
    TEST_ASSERT(t1->tid != t2->tid);
    process_destroy(p);
    return true;
}

static bool t_tid_cross(void) {
    process_t* p1 = process_create("t_tid_p1", NULL);
    process_t* p2 = process_create("t_tid_p2", NULL);
    thread_t* t1 = thread_create(p1, "t1", kentry, NULL, false, 0);
    thread_t* t2 = thread_create(p2, "t2", kentry, NULL, false, 0);
    thread_t* t3 = thread_create(p2, "t3", kentry, NULL, false, 0);
    TEST_ASSERT(t1->tid != t2->tid);
    TEST_ASSERT(t1->tid != t3->tid);
    TEST_ASSERT(t2->tid != t3->tid);
    process_destroy(p1);
    process_destroy(p2);
    return true;
}
#pragma endregion

#pragma region Thread List

static bool t_thr_grows(void) {
    process_t* p = process_create("t_tgrow", NULL);
    TEST_ASSERT(thread_count(p) == 0);
    thread_create(p, "t1", kentry, NULL, false, 0);
    TEST_ASSERT(thread_count(p) == 1);
    thread_create(p, "t2", kentry, NULL, false, 0);
    TEST_ASSERT(thread_count(p) == 2);
    thread_create(p, "t3", kentry, NULL, false, 0);
    TEST_ASSERT(thread_count(p) == 3);
    process_destroy(p);
    return true;
}

static bool t_thr_inlist(void) {
    process_t* p = process_create("t_tinlist", NULL);
    thread_t* t = thread_create(p, "t_check", kentry, NULL, false, 0);
    bool found = false;
    for (thread_t* it = p->threads; it; it = it->next)
        if (it == t) { found = true; break; }
    TEST_ASSERT(found);
    process_destroy(p);
    return true;
}

static bool t_multi_5(void) {
    process_t* p = process_create("t_multi5", NULL);
    for (int i = 0; i < 5; i++) {
        thread_t* t = thread_create(p, "w", kentry, NULL, false, 0);
        TEST_ASSERT(t != NULL);
        TEST_ASSERT(t->process == p);
    }
    TEST_ASSERT(thread_count(p) == 5);
    process_destroy(p);
    return true;
}
#pragma endregion

#pragma region Name Truncation

static bool t_proc_ntrunc(void) {
    char name[MAX_PROCESS_NAME * 2];
    kmemset(name, 'A', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    process_t* p = process_create(name, NULL);
    TEST_ASSERT(kstrlen(p->name) < MAX_PROCESS_NAME);
    TEST_ASSERT(p->name[MAX_PROCESS_NAME - 1] == '\0');
    process_destroy(p);
    return true;
}

static bool t_thr_ntrunc(void) {
    char name[MAX_THREAD_NAME * 2];
    kmemset(name, 'T', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    process_t* p = process_create("t_tntrunc", NULL);
    thread_t* t = thread_create(p, name, kentry, NULL, false, 0);
    TEST_ASSERT(kstrlen(t->name) < MAX_THREAD_NAME);
    TEST_ASSERT(t->name[MAX_THREAD_NAME - 1] == '\0');
    process_destroy(p);
    return true;
}
#pragma endregion

#pragma region Shared TTY

static bool t_shared_tty(void) {
    process_t* owner = process_create("t_owner", NULL);
    tty_t* shared = owner->tty;
    TEST_ASSERT(shared != NULL);
    process_t* guest = process_create("t_guest", shared);
    TEST_ASSERT(guest->tty == shared);
    /* Detach before destroy so guest doesn't free the shared TTY */
    guest->tty = NULL;
    process_destroy(guest);
    TEST_ASSERT(owner->tty == shared); /* owner's TTY still intact */
    process_destroy(owner);
    return true;
}
#pragma endregion

#pragma region Header Update

static bool t_hdr_update(void) {
    process_t* p = process_create("t_hdr", NULL);
    proc_hdr_update(p); /* must not crash */
    process_destroy(p);
    return true;
}

static bool t_hdr_update_thr(void) {
    process_t* p = process_create("t_hdrth", NULL);
    thread_create(p, "th", kentry, NULL, false, 0);
    proc_hdr_update(p); /* must not crash with 1 thread */
    process_destroy(p);
    return true;
}
#pragma endregion

#pragma region procs_kill_tty

static volatile int out_done = 0;

static void out_writer(void* arg) {
    tty_write((tty_t*)arg, "C", 1);
    out_done = 1;
    sched_exit();
}

static bool t_out_contend(void) {
    /* A second writer blocks until the owner lets go, then lands after it */
    tty_t* t = tty_create();
    process_t* p = process_create("t_out", NULL);
    thread_t* w = thread_create(p, "t_out_w", out_writer, t, false, 0);
    out_done = 0;

    bool held = tty_lock_output(t);
    TEST_ASSERT(held);
    tty_write(t, "A", 1);
    sched_add(w);
    for (int i = 0; i < 10; i++) sched_yield();
    TEST_ASSERT(!out_done);
    TEST_ASSERT(t->out->wait == w);

    tty_write(t, "B", 1);
    tty_unlock_output(t, held);
    for (int i = 0; i < 100 && !out_done; i++) sched_yield();
    TEST_ASSERT(out_done);

    console_char_t* row = t->console->buffer + t->console->header_rows * t->console->width;
    TEST_ASSERT(row[0].codepoint == 'A' && row[1].codepoint == 'B' && row[2].codepoint == 'C');
    tty_destroy(t);
    return true;
}

static bool t_term_tty(void) {
    process_t* p = process_create("t_termtty", NULL);
    tty_t* t = p->tty;
    /* Must not crash; process may or may not be destroyed here */
    procs_kill_tty(t);
    return true;
}
#pragma endregion

#pragma region thread_create_bootstrap

static bool t_bootstrap(void) {
    process_t* p = process_create("t_boot", NULL);
    thread_t* t = thread_create_bootstrap(p, "boot_t");
    TEST_ASSERT(t != NULL);
    TEST_ASSERT(t->process == p);
    TEST_ASSERT(kstrcmp(t->name, "boot_t") == 0);
    process_destroy(p);
    return true;
}
#pragma endregion

#pragma region Syscall ABI

/* Bit 63 = probe ran, low bits = registers that didn't survive the syscall */
userspace_bss static volatile uint64_t abi_res;

userspace void abi_probe(void* arg) {
    (void)arg;
    uint64_t bad;
    /* Everything but RAX, RCX and R11 must come back untouched */
    __asm__ volatile(
        "push %%rbp\n\t"
        "mov $0x11, %%rbx\n\t"
        "mov $0x12, %%rdx\n\t"
        "mov $0x13, %%rsi\n\t"
        "mov $0x14, %%rdi\n\t"
        "mov $0x15, %%rbp\n\t"
        "mov $0x16, %%r8\n\t"
        "mov $0x17, %%r9\n\t"
        "mov $0x18, %%r10\n\t"
        "mov $0x19, %%r12\n\t"
        "mov $0x1a, %%r13\n\t"
        "mov $0x1b, %%r14\n\t"
        "mov $0x1c, %%r15\n\t"
        "mov %[nr], %%eax\n\t"
        "syscall\n\t"
        "xor %%eax, %%eax\n\t"
        "cmp $0x11, %%rbx\n\t setne %%cl\n\t or %%cl, %%al\n\t"
        "cmp $0x12, %%rdx\n\t setne %%cl\n\t shl $1, %%cl\n\t or %%cl, %%al\n\t"
        "cmp $0x13, %%rsi\n\t setne %%cl\n\t shl $2, %%cl\n\t or %%cl, %%al\n\t"
        "cmp $0x14, %%rdi\n\t setne %%cl\n\t shl $3, %%cl\n\t or %%cl, %%al\n\t"
        "cmp $0x15, %%rbp\n\t setne %%cl\n\t shl $4, %%cl\n\t or %%cl, %%al\n\t"
        "cmp $0x16, %%r8\n\t  setne %%cl\n\t shl $5, %%cl\n\t or %%cl, %%al\n\t"
        "cmp $0x17, %%r9\n\t  setne %%cl\n\t shl $6, %%cl\n\t or %%cl, %%al\n\t"
        "cmp $0x18, %%r10\n\t setne %%cl\n\t shl $7, %%cl\n\t or %%cl, %%al\n\t"
        "cmp $0x19, %%r12\n\t setne %%cl\n\t or %%cl, %%ah\n\t"
        "cmp $0x1a, %%r13\n\t setne %%cl\n\t shl $1, %%cl\n\t or %%cl, %%ah\n\t"
        "cmp $0x1b, %%r14\n\t setne %%cl\n\t shl $2, %%cl\n\t or %%cl, %%ah\n\t"
        "cmp $0x1c, %%r15\n\t setne %%cl\n\t shl $3, %%cl\n\t or %%cl, %%ah\n\t"
        "pop %%rbp"
        : "=a"(bad)
        : [nr] "i"(SYS_YIELD)
        : "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
          "r12", "r13", "r14", "r15", "cc", "memory");
    abi_res = bad | (1ull << 63);
    /* The test kernel doesn't preempt ring 3, so hand the CPU back until we're torn down */
    while (1) __asm__ volatile("syscall" : : "a"(SYS_YIELD) : "rcx", "r11", "memory");
}

static bool t_sys_abi(void) {
    process_t* p = process_create("t_abi", NULL);
    thread_t* t = thread_create(p, "t_abi", abi_probe, NULL, true, 0);
    TEST_ASSERT(t != NULL);

    /* abi_res lives in p's own bss, so read it through the physmap */
    uint64_t phys;
    TEST_ASSERT(vmm_get_physical(p->vmm, (void*)&abi_res, &phys));
    volatile uint64_t* res = (volatile uint64_t*)PHYSMAP_P2V(phys);

    /* A probe that faults gets its process reaped, so stop looking at its memory then */
    sched_add(t);
    uint64_t r = 0;
    for (int i = 0; i < 100 && !r; i++) {
        sched_yield();
        if (!proc_in_list(p)) break;
        r = *res;
    }
    if (proc_in_list(p)) process_destroy(p);

    if (r != (1ull << 63)) LOGF("(res=0x%lx) ", r);
    TEST_ASSERT(r == (1ull << 63));
    return true;
}

/* Bit 63 = probe ran, low byte = what SYS_YIELD returned, next byte = what a bogus SYS_MUNMAP returned */
userspace_bss static volatile uint64_t ret_res;

userspace void ret_probe(void* arg) {
    (void)arg;
    uint64_t y, m;
    __asm__ volatile("syscall" : "=a"(y) : "a"(SYS_YIELD) : "rcx", "r11", "memory");
    __asm__ volatile("syscall" : "=a"(m) : "a"(SYS_MUNMAP), "D"(0x1000) : "rcx", "r11", "memory");
    ret_res = (1ull << 63) | (y & 0xFF) | ((m & 0xFF) << 8);
    while (1) __asm__ volatile("syscall" : : "a"(SYS_YIELD) : "rcx", "r11", "memory");
}

extern volatile uint8_t syscall_frame_mod16;

static bool t_sys_ret(void) {
    process_t* p = process_create("t_ret", NULL);
    thread_t* t = thread_create(p, "t_ret", ret_probe, NULL, true, 0);
    TEST_ASSERT(t != NULL);

    uint64_t phys;
    TEST_ASSERT(vmm_get_physical(p->vmm, (void*)&ret_res, &phys));
    volatile uint64_t* res = (volatile uint64_t*)PHYSMAP_P2V(phys);

    syscall_frame_mod16 = 0xFF;
    sched_add(t);
    uint64_t r = 0;
    for (int i = 0; i < 100 && !r; i++) {
        sched_yield();
        if (!proc_in_list(p)) break;
        r = *res;
    }
    if (proc_in_list(p)) process_destroy(p);

    if (r != ((1ull << 63) | 0xFF00)) LOGF("(res=0x%lx) ", r);
    TEST_ASSERT(r & (1ull << 63));
    TEST_ASSERT((r & 0xFF) == 0);           // yield returns 0, not its own number
    TEST_ASSERT(((r >> 8) & 0xFF) == 0xFF); // munmap of nothing returns -1
    return true;
}

static bool t_sys_align(void) {
    // t_sys_ret just ran syscalls, so the dispatcher recorded its frame. SysV wants it 16 aligned
    if (syscall_frame_mod16 != 0) LOGF("(mod16=%u) ", syscall_frame_mod16);
    TEST_ASSERT(syscall_frame_mod16 == 0);
    return true;
}

static bool t_sys_fmask(void) {
    // ring 3 must not be able to carry TF or AC into the syscall handler
    uint64_t mask = read_msr(MSR_FMASK);
    TEST_ASSERT(mask & (1 << 8));
    TEST_ASSERT(mask & (1 << 9));
    TEST_ASSERT(mask & (1 << 18));
    return true;
}
#pragma endregion

#pragma region Runner

static void run_test(const char* name, bool (*fn)(void)) {
    test_begin(name);
    bool pass = fn();
    test_end(&suite, pass);
}

void test_multitasking(void) {
    suite_begin(&suite, "Multitasking");

    run_test("sched_active is true",          t_sched_active);
    run_test("sched_current not null",        t_sched_nn);
    run_test("sched_current name=kernel_main",t_sched_name);
    run_test("sched_current has stack",       t_sched_stk);
    run_test("process: not null",             t_proc_nn);
    run_test("process: private .data",        t_proc_data_private);
    run_test("process: pid > 0",              t_proc_pid);
    run_test("process: name stored",          t_proc_name);
    run_test("process: has vmm",              t_proc_vmm);
    run_test("process: has tty",              t_proc_tty);
    run_test("process: no threads at create", t_proc_nothr);
    run_test("process: in global list",       t_proc_inlist);
    run_test("process: removed after destroy",t_proc_rmlist);
    run_test("pid: unique across 8 procs",    t_pid_uniq);
    run_test("pid: strictly monotone",        t_pid_mono);
    run_test("kthread: not null",             t_kt_nn);
    run_test("kthread: state READY",          t_kt_state);
    run_test("kthread: process link",         t_kt_link);
    run_test("kthread: name stored",          t_kt_name);
    run_test("kthread: stack not null",       t_kt_stk);
    run_test("kthread: context in stack",     t_kt_ctx);
    run_test("kthread: CS = KERNEL_CS",       t_kt_cs);
    run_test("kthread: SS = KERNEL_DS",       t_kt_ss);
    run_test("kthread: RDI = arg",            t_kt_arg);
    run_test("kthread: FPU area 64B aligned", t_kt_fpu_align);
    run_test("fpu: fresh area is reset state",t_fpu_fresh);
    run_test("fpu: save/restore round trip",  t_fpu_roundtrip);
    run_test("uthread: not null",             t_ut_nn);
    run_test("uthread: ustack not null",      t_ut_stk);
    run_test("uthread: context not null",     t_ut_ctx);
    run_test("uthread: RSP % 16 == 8 (ABI)", t_ut_align);
    run_test("tid: unique same process",      t_tid_same);
    run_test("tid: unique cross process",     t_tid_cross);
    run_test("thread_count grows",            t_thr_grows);
    run_test("thread in proc->threads",       t_thr_inlist);
    run_test("5 threads in one process",      t_multi_5);
    run_test("process name truncation",       t_proc_ntrunc);
    run_test("thread name truncation",        t_thr_ntrunc);
    run_test("shared tty between processes",  t_shared_tty);
    run_test("proc_hdr_update no crash",t_hdr_update);
    run_test("proc_hdr_update+thread",  t_hdr_update_thr);
    run_test("terminate_by_tty no crash",     t_term_tty);
    run_test("tty output: writer waits turn", t_out_contend);
    run_test("thread_create_bootstrap",       t_bootstrap);
    run_test("syscall keeps non-ABI regs",    t_sys_abi);
    run_test("syscall masks TF, IF and AC",   t_sys_fmask);
    run_test("syscall: yield/munmap results", t_sys_ret);
    run_test("syscall: dispatcher aligned",   t_sys_align);

    suite_end(&suite);
}
#pragma endregion
