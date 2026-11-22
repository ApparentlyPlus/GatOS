/*
 * cpu.c - CPU Feature and Topology Detection
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/cpu/cpu.h>
#include <arch/x86_64/cpu/msr.h>
#include <kernel/debug.h>
#include <klibc/string.h>
#include <kernel/sys/panic.h>

static cpu_info_t cpuinfo;
cpu_local_t cpu_local = {0};

// Set once AVX is on
static bool use_xsave = false;

/*
 * cpuid - Execute the CPUID instruction with given EAX and ECX inputs
 */
void cpuid(uint32_t eax, uint32_t ecx, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(eax), "c"(ecx));
}

uint64_t read_msr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * rdmsr_safe - rdmsr for MSRs that might not exist
 */
__attribute__((noinline, noclone)) bool rdmsr_safe(uint32_t msr, uint64_t* out)
{
    uint32_t lo = 0, hi = 0;
    uint32_t ok;
    __asm__ volatile(
        "mov $1, %[ok]\n"
        ".global msr_safe_insn\n"
        "msr_safe_insn: rdmsr\n"
        "jmp 1f\n"
        ".global msr_safe_fail\n"
        "msr_safe_fail: xor %[ok], %[ok]\n"
        "1:\n"
        : [ok] "=&r"(ok), "+a"(lo), "+d"(hi)
        : "c"(msr)
        : "memory");
    if (ok && out) *out = ((uint64_t)hi << 32) | lo;
    return ok != 0;
}

void write_msr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

/*
 * read_cr0 - Read from Control Register CR0
 */
uint64_t read_cr0(void)
{ 
    uint64_t val; 
    __asm__ volatile("mov %%cr0, %0" : "=r"(val)); 
    return val; 
}

/*
 * write_cr0 - Write to Control Register CR0
 */
void write_cr0(uint64_t val) 
{ 
    __asm__ volatile("mov %0, %%cr0" :: "r"(val)); 
}

/*
 * read_cr4 - Read from Control Register CR4
 */
uint64_t read_cr4(void)
{ 
    uint64_t val;
    __asm__ volatile("mov %%cr4, %0" : "=r"(val)); 
    return val; 
}

/*
 * write_cr4 - Write to Control Register CR4
 */
void write_cr4(uint64_t val)
{ 
    __asm__ volatile("mov %0, %%cr4" :: "r"(val)); 
}

/*
 * read_xcr0 - Read from Extended Control Register XCR0
 */
uint64_t read_xcr0(void)
{
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * write_xcr0 - Write to Extended Control Register XCR0
 */
void write_xcr0(uint64_t value)
{
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("xsetbv" :: "a"(lo), "d"(hi), "c"(0));
}

/*
 * fpu_save - Saves the FPU/SSE/AVX registers into a FPU_AREA_ALIGN aligned area
 */
void fpu_save(void* area)
{
    if (use_xsave)
        __asm__ volatile("xsave (%0)" :: "r"(area), "a"(0xFFFFFFFF), "d"(0xFFFFFFFF) : "memory");
    else
        __asm__ volatile("fxsave (%0)" :: "r"(area) : "memory");
}

/*
 * fpu_restore - Loads registers saved by fpu_save
 */
void fpu_restore(const void* area)
{
    if (use_xsave)
        __asm__ volatile("xrstor (%0)" :: "r"(area), "a"(0xFFFFFFFF), "d"(0xFFFFFFFF) : "memory");
    else
        __asm__ volatile("fxrstor (%0)" :: "r"(area) : "memory");
}

/*
 * cpu_init - Initialize CPU information by querying CPUID and MSRs
 */
void cpu_init(void)
{
    // Init the struct
    kmemset(&cpuinfo, 0, sizeof(cpuinfo));

    uint32_t a, b, c, d;

    // 4b segments
    cpuid(0, 0, &a, &b, &c, &d);
    *((uint32_t*)&cpuinfo.vendor[0]) = b;
    *((uint32_t*)&cpuinfo.vendor[4]) = d;
    *((uint32_t*)&cpuinfo.vendor[8]) = c;
    cpuinfo.vendor[12] = '\0';
    uint32_t max_basic = a;

    // parse model info
    cpuid(1, 0, &a, &b, &c, &d);
    cpuinfo.family = ((a >> 8) & 0xF) + ((a >> 20) & 0xFF);
    cpuinfo.model = ((a >> 4) & 0xF) | ((a >> 12) & 0xF0);
    cpuinfo.stepping = (a & 0xF);

    // feats
    if (d & (1 << 6)) cpuinfo.features |= CF_PAE;
    if (d & (1 << 25)) cpuinfo.features |= CF_SSE;
    if (d & (1 << 26)) cpuinfo.features |= CF_SSE2;
    if (c & (1 << 0)) cpuinfo.features |= CF_SSE3;
    if (c & (1 << 9)) cpuinfo.features |= CF_SSSE3;
    if (c & (1 << 19)) cpuinfo.features |= CF_SSE4_1;
    if (c & (1 << 20)) cpuinfo.features |= CF_SSE4_2;
    if (c & (1 << 28)) cpuinfo.features |= CF_AVX;
    if (c & (1 << 5)) cpuinfo.features |= CF_VMX;

    if (max_basic >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        if (b & (1 << 5)) cpuinfo.features |= CF_AVX2;
        if (b & (1 << 7)) cpuinfo.features |= CF_SMEP;
        if (b & (1 << 20)) cpuinfo.features |= CF_SMAP;
    }

    // NX support hacky check, blame x86 for this atrocity
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    uint32_t max_ext = a;

    if (max_ext >= 0x80000001) {
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        if (d & (1 << 20)) cpuinfo.features |= CF_NX;
        if (d & (1 << 29)) cpuinfo.features |= CF_64BIT;
        if (c & (1 << 2)) cpuinfo.features |= CF_SVM;
    }

    if (max_ext >= 0x80000004) {
        uint32_t* brand_ptr = (uint32_t*)cpuinfo.brand;
        for (uint32_t i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, 0, &brand_ptr[i * 4 + 0], &brand_ptr[i * 4 + 1], &brand_ptr[i * 4 + 2], &brand_ptr[i * 4 + 3]);
        }
        cpuinfo.brand[48] = '\0';
    }

    cpuinfo.core_count = 1;

    // Leaf 0xB lists the topology levels (SMT, then core). EBX of the core level counts every
    // logical processor in the package. Subleaf 0 is the SMT level, which only counts one core's.
    if (max_basic >= 0x0B) {
        for (uint32_t lvl = 0; lvl < 8; lvl++) {
            cpuid(0x0B, lvl, &a, &b, &c, &d);
            uint32_t type = (c >> 8) & 0xFF;
            if (type == 0) break;
            if (type == 2 && b) cpuinfo.core_count = b;
        }
    } else if (max_basic >= 0x04) {
        cpuid(0x04, 0, &a, &b, &c, &d);
        cpuinfo.core_count = ((a >> 26) & 0x3F) + 1;
    }

    // SSE is enabled at boot time (it's a requirement for GatOS to work)
    // but it's good to do it here too
    if (cpu_has_feature(CF_SSE)) {
        cpu_enable_feature(CF_SSE);
    }

    if (cpu_has_feature(CF_AVX)) {
        cpu_enable_feature(CF_AVX);
    }

    if (cpu_has_feature(CF_SMEP)) {
        cpu_enable_feature(CF_SMEP);
    }

    if (cpu_has_feature(CF_SMAP)) {
        cpu_enable_feature(CF_SMAP);
    }

    // GS_BASE -> cpu_local in ring 0, KERNEL_GS_BASE holds user GS on entry
    write_msr(MSR_GS_BASE, (uint64_t)&cpu_local);
    write_msr(MSR_KERNEL_GS_BASE, 0);

    cpu_idle_init();

    LOGF("[CPU] Vendor: %s\n", cpuinfo.vendor);
    LOGF("[CPU] Brand:  %s\n", cpuinfo.brand);
    LOGF("[CPU] Family: %u  Model: %u  Stepping: %u\n", cpuinfo.family, cpuinfo.model, cpuinfo.stepping);
    LOGF("[CPU] Cores:  %u\n", cpuinfo.core_count);
    LOGF("[CPU] Features: 0x%lX\n", cpuinfo.features);
}

// MONITOR/MWAIT idle state, probed once by cpu_idle_init
static bool mwait_ok = false;
static uint32_t mwait_cstate_hint = 0;
static uint32_t mwait_ecx_ext = 0;
static void* volatile idle_dummy = 0;

/*
 * cpu_idle_init - Detect MONITOR/MWAIT and pick the deepest C-state hint
 */
void cpu_idle_init(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(c & (1u << 3))) {
        LOGF("[CPU] MONITOR/MWAIT not available; idle will use HLT.\n");
        return;
    }

    mwait_ok = true;
    cpuid(5, 0, &a, &b, &c, &d);
    mwait_ecx_ext = (c & 1u);

    if ((d >> 28) & 0xF) mwait_cstate_hint = 0x60;
    else if ((d >> 24) & 0xF) mwait_cstate_hint = 0x50;
    else if ((d >> 20) & 0xF) mwait_cstate_hint = 0x40;
    else if ((d >> 16) & 0xF) mwait_cstate_hint = 0x30;
    else if ((d >> 12) & 0xF) mwait_cstate_hint = 0x20;
    else if ((d >> 8) & 0xF) mwait_cstate_hint = 0x10;
    else mwait_cstate_hint = 0x00;

    // Without ARAT (Always Running APIC Timer), the LAPIC timer stops in C2 and deeper, and
    // the deadline that should wake us never fires. C1 is the only safe choice then.
    cpuid(0, 0, &a, &b, &c, &d);
    bool arat = false;
    if (a >= 6) {
        cpuid(6, 0, &a, &b, &c, &d);
        arat = a & (1u << 2);
    }
    if (!arat) mwait_cstate_hint = 0x00;

    LOGF("[CPU] MONITOR/MWAIT: C-state hint=0x%02x IBE=%u ARAT=%u\n",
         mwait_cstate_hint, mwait_ecx_ext, arat);
}

/*
 * cpu_idle_on - Deepest available C-state until an interrupt, or until something writes *watch.
 * Returns right away if *watch is already set. The HLT fallback only wakes on interrupts
 */
void cpu_idle_on(void* volatile* watch) {
    if (mwait_ok) {
        __asm__ volatile("monitor" :: "a"(watch), "c"(0), "d"(0) : "memory");
        if (*watch) return;
        __asm__ volatile("mwait" :: "a"(mwait_cstate_hint), "c"(mwait_ecx_ext) : "memory");
    } else {
        __asm__ volatile("hlt");
    }
}

/*
 * cpu_idle - Idle until the next interrupt (watches an address nothing ever writes)
 */
void cpu_idle(void) {
    cpu_idle_on(&idle_dummy);
}

const cpu_info_t* cpu_get_info(void)
{
    return &cpuinfo;
}

bool cpu_has_feature(cpu_feature_t feature)
{
    return (cpuinfo.features & feature) != 0;
}

/*
 * cpu_enable_feature - Enable a specific CPU feature if supported
 */
bool cpu_enable_feature(cpu_feature_t feature)
{
    if (!cpu_has_feature(feature))
        return false;

    uint64_t cr0, cr4, xcr0, efer;

    switch (feature) {
        case CF_PAE:
            cr4 = read_cr4();
            cr4 |= (1 << 5); // CR4.PAE
            write_cr4(cr4);
            return true;

        case CF_SSE:
        case CF_SSE2:
        case CF_SSE3:
        case CF_SSSE3:
        case CF_SSE4_1:
        case CF_SSE4_2:
            cr0 = read_cr0();
            cr4 = read_cr4();
            cr0 &= ~(1 << 2); // Clear EM (Emulation)
            cr0 |= (1 << 1); // Set MP (Monitor Coprocessor)
            cr4 |= (1 << 9); // Set OSFXSR
            cr4 |= (1 << 10); // Set OSXMMEXCPT
            write_cr0(cr0);
            write_cr4(cr4);
            return true;

        case CF_AVX:
        case CF_AVX2:
            cr4 = read_cr4();
            cr4 |= (1 << 18); // CR4.OSXSAVE
            write_cr4(cr4);
            xcr0 = read_xcr0();
            xcr0 |= (1 << 0) | (1 << 1); // enable x87 and SSE
            xcr0 |= (1 << 2); // enable AVX state
            write_xcr0(xcr0);

            // With XCR0 set, CPUID 0xD reports how big an xsave of the enabled state is
            uint32_t a, b, c, d;
            cpuid(0x0D, 0, &a, &b, &c, &d);
            if (b > FPU_AREA_SIZE) panic("FPU state doesn't fit FPU_AREA_SIZE");
            use_xsave = true;
            return true;

        case CF_NX:
            efer = read_msr(MSR_EFER);
            efer |= EFER_NXE;
            write_msr(MSR_EFER, efer);
            return true;

        case CF_VMX:
            cr4 = read_cr4();
            cr4 |= (1 << 13); // CR4.VMXE
            write_cr4(cr4);
            return true;

        case CF_SVM:
            efer = read_msr(0xC0000080);
            efer |= (1 << 12); // EFER.SVME
            write_msr(0xC0000080, efer);
            return true;

        case CF_SMEP:
            cr4 = read_cr4();
            cr4 |= (1 << 20); // CR4.SMEP
            write_cr4(cr4);
            return true;

        case CF_SMAP:
            cr4 = read_cr4();
            cr4 |= (1 << 21); // CR4.SMAP
            write_cr4(cr4);
            return true;

        default:
            return false;
    }
}

bool cpu_is_feature_enabled(cpu_feature_t feature)
{
    uint64_t cr0, cr4, xcr0, efer;

    switch (feature) {
        case CF_PAE:
            cr4 = read_cr4();
            return (cr4 & (1 << 5)) != 0;

        case CF_SSE:
        case CF_SSE2:
        case CF_SSE3:
        case CF_SSSE3:
        case CF_SSE4_1:
        case CF_SSE4_2:
            cr0 = read_cr0();
            cr4 = read_cr4();
            return ((cr4 & (1 << 9)) && (cr4 & (1 << 10)) && !(cr0 & (1 << 2)));

        case CF_AVX:
        case CF_AVX2:
            // xgetbv is #UD until CR4.OSXSAVE is set, so that has to be checked first
            cr4 = read_cr4();
            if (!(cr4 & (1 << 18))) return false;
            xcr0 = read_xcr0();
            return (xcr0 & (1 << 0)) && (xcr0 & (1 << 1)) && (xcr0 & (1 << 2));

        case CF_NX:
            efer = read_msr(MSR_EFER);
            return (efer & EFER_NXE) != 0;

        case CF_VMX:
            cr4 = read_cr4();
            return (cr4 & (1 << 13)) != 0;

        case CF_SVM:
            efer = read_msr(0xC0000080);
            return (efer & (1 << 12)) != 0;

        case CF_SMEP:
            cr4 = read_cr4();
            return (cr4 & (1 << 20)) != 0;

        case CF_SMAP:
            cr4 = read_cr4();
            return (cr4 & (1 << 21)) != 0;

        default:
            return false;
    }
}

/*
 * tsc_read - Reads the Time Stamp Counter
 */
uint64_t tsc_read(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void tsc_deadline_arm(uint64_t target_tsc) {
    write_msr(0x6E0, target_tsc);
}

