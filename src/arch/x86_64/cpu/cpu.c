/*
 * cpu.c - CPU Feature and Topology Detection
 *
 * Gathers detailed CPU information (vendor, brand, features, core count)
 * using the CPUID instruction and related MSRs. Results are cached in
 * a global CPUInfo structure accessible to the rest of GatOS.
 *
 * Author: ApparentlyPlus
 */

#include <arch/x86_64/cpu/cpu.h>
#include <kernel/debug.h>
#include <libc/string.h>

static CPUInfo g_cpu;

/*
 * cpuid - Execute the CPUID instruction with given EAX and ECX inputs
 */
void cpuid(uint32_t eax, uint32_t ecx, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(eax), "c"(ecx));
}

/*
 * read_msr - Read a Model-Specific Register (MSR)
 */
uint64_t read_msr(uint32_t msr)
{
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

/*
 * write_msr - Write a Model-Specific Register (MSR)
 */
void write_msr(uint32_t msr, uint64_t value)
{
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(low), "d"(high));
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
    uint32_t low, high;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    return ((uint64_t)high << 32) | low;
}

/*
 * write_xcr0 - Write to Extended Control Register XCR0
 */
void write_xcr0(uint64_t value)
{
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile("xsetbv" :: "a"(low), "d"(high), "c"(0));
}

/*
 * cpu_init - Initialize CPU information by querying CPUID and MSRs
 */
void cpu_init(void)
{
    memset(&g_cpu, 0, sizeof(g_cpu));

    uint32_t a, b, c, d;

    // Vendor string
    cpuid(0, 0, &a, &b, &c, &d);
    *((uint32_t*)&g_cpu.vendor[0]) = b;
    *((uint32_t*)&g_cpu.vendor[4]) = d;
    *((uint32_t*)&g_cpu.vendor[8]) = c;
    g_cpu.vendor[12] = '\0';
    uint32_t max_basic = a;

    // Basic feature detection
    cpuid(1, 0, &a, &b, &c, &d);
    g_cpu.family   = ((a >> 8) & 0xF) + ((a >> 20) & 0xFF);
    g_cpu.model    = ((a >> 4) & 0xF) | ((a >> 12) & 0xF0);
    g_cpu.stepping = (a & 0xF);

    if (d & (1 << 6))  g_cpu.features |= CPU_FEAT_PAE;
    if (d & (1 << 25)) g_cpu.features |= CPU_FEAT_SSE;
    if (d & (1 << 26)) g_cpu.features |= CPU_FEAT_SSE2;
    if (c & (1 << 0))  g_cpu.features |= CPU_FEAT_SSE3;
    if (c & (1 << 9))  g_cpu.features |= CPU_FEAT_SSSE3;
    if (c & (1 << 19)) g_cpu.features |= CPU_FEAT_SSE4_1;
    if (c & (1 << 20)) g_cpu.features |= CPU_FEAT_SSE4_2;
    if (c & (1 << 28)) g_cpu.features |= CPU_FEAT_AVX;
    if (c & (1 << 5))  g_cpu.features |= CPU_FEAT_VMX;

    // Extended features (AMD/NX/64-bit/SVM)
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    uint32_t max_ext = a;

    if (max_ext >= 0x80000001) {
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        if (d & (1 << 20)) g_cpu.features |= CPU_FEAT_NX;
        if (d & (1 << 29)) g_cpu.features |= CPU_FEAT_64BIT;
        if (c & (1 << 2))  g_cpu.features |= CPU_FEAT_SVM;
    }

    // Brand string (0x80000002 - 0x80000004)
    if (max_ext >= 0x80000004) {
        uint32_t* brand_ptr = (uint32_t*)g_cpu.brand;
        for (uint32_t i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, 0, &brand_ptr[i * 4 + 0],
                                    &brand_ptr[i * 4 + 1],
                                    &brand_ptr[i * 4 + 2],
                                    &brand_ptr[i * 4 + 3]);
        }
        g_cpu.brand[48] = '\0';
    }

    // Core count detection
    g_cpu.core_count = 1;

    if (max_basic >= 0x0B) {
        cpuid(0x0B, 0, &a, &b, &c, &d);
        if (b) g_cpu.core_count = b;
    } else if (max_basic >= 0x04) {
        cpuid(0x04, 0, &a, &b, &c, &d);
        g_cpu.core_count = ((b >> 26) & 0x3F) + 1;
    }

    // Log gathered CPU information
    LOGF("[CPU] Vendor: %s\n", g_cpu.vendor);
    LOGF("[CPU] Brand:  %s\n", g_cpu.brand);
    LOGF("[CPU] Family: %u  Model: %u  Stepping: %u\n",
            g_cpu.family, g_cpu.model, g_cpu.stepping);
    LOGF("[CPU] Cores:  %u\n", g_cpu.core_count);
    LOGF("[CPU] Features: 0x%lX\n", g_cpu.features);
}

/*
 * cpu_get_info - Get a pointer to the cached CPUInfo structure
 */
const CPUInfo* cpu_get_info(void)
{
    return &g_cpu;
}

/*
 * cpu_has_feature - Check if a specific CPU feature is supported
 */
bool cpu_has_feature(cpu_feature_t feature)
{
    return (g_cpu.features & feature) != 0;
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
        case CPU_FEAT_PAE:
            cr4 = read_cr4();
            cr4 |= (1 << 5); // CR4.PAE
            write_cr4(cr4);
            return true;

        case CPU_FEAT_SSE:
        case CPU_FEAT_SSE2:
        case CPU_FEAT_SSE3:
        case CPU_FEAT_SSSE3:
        case CPU_FEAT_SSE4_1:
        case CPU_FEAT_SSE4_2:
            cr0 = read_cr0();
            cr4 = read_cr4();
            cr0 &= ~(1 << 2); // Clear EM (Emulation)
            cr0 |=  (1 << 1); // Set MP (Monitor Coprocessor)
            cr4 |=  (1 << 9); // Set OSFXSR
            cr4 |=  (1 << 10); // Set OSXMMEXCPT
            write_cr0(cr0);
            write_cr4(cr4);
            return true;

        case CPU_FEAT_AVX:
        case CPU_FEAT_AVX2:
            cr4 = read_cr4();
            cr4 |= (1 << 18); // CR4.OSXSAVE
            write_cr4(cr4);
            xcr0 = read_xcr0();
            xcr0 |= (1 << 0) | (1 << 1); // enable x87 + SSE
            xcr0 |= (1 << 2); // enable AVX state
            write_xcr0(xcr0);
            return true;

        case CPU_FEAT_NX:
            efer = read_msr(0xC0000080); // IA32_EFER
            efer |= (1 << 11); // EFER.NXE
            write_msr(0xC0000080, efer);
            return true;

        case CPU_FEAT_VMX:
            cr4 = read_cr4();
            cr4 |= (1 << 13); // CR4.VMXE
            write_cr4(cr4);
            return true;

        case CPU_FEAT_SVM:
            efer = read_msr(0xC0000080);
            efer |= (1 << 12); // EFER.SVME
            write_msr(0xC0000080, efer);
            return true;

        default:
            return false;
    }
}

/*
 * cpu_is_feature_enabled - Checks if a CPU feature is enabled
 */
bool cpu_is_feature_enabled(cpu_feature_t feature)
{
    uint64_t cr0, cr4, xcr0, efer;

    switch (feature) {
        case CPU_FEAT_PAE:
            cr4 = read_cr4();
            return (cr4 & (1 << 5)) != 0;

        case CPU_FEAT_SSE:
        case CPU_FEAT_SSE2:
        case CPU_FEAT_SSE3:
        case CPU_FEAT_SSSE3:
        case CPU_FEAT_SSE4_1:
        case CPU_FEAT_SSE4_2:
            cr0 = read_cr0();
            cr4 = read_cr4();
            return ((cr4 & (1 << 9)) && (cr4 & (1 << 10)) && !(cr0 & (1 << 2)));

        case CPU_FEAT_AVX:
        case CPU_FEAT_AVX2:
            cr4  = read_cr4();
            xcr0 = read_xcr0();
            return ((cr4 & (1 << 18)) &&
                    (xcr0 & (1 << 0)) && (xcr0 & (1 << 1)) && (xcr0 & (1 << 2)));

        case CPU_FEAT_NX:
            efer = read_msr(0xC0000080); // IA32_EFER
            return (efer & (1 << 11)) != 0;

        case CPU_FEAT_VMX:
            cr4 = read_cr4();
            return (cr4 & (1 << 13)) != 0;

        case CPU_FEAT_SVM:
            efer = read_msr(0xC0000080);
            return (efer & (1 << 12)) != 0;

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

/*
 * tsc_deadline_arm - Arms the TSC deadline timer
 */
void tsc_deadline_arm(uint64_t target_tsc) {
    write_msr(0x6E0, target_tsc);
}

