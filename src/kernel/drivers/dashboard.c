/*
 * dashboard.c - Live Kernel Dashboard
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/dashboard.h>
#include <kernel/drivers/tty.h>
#include <kernel/drivers/console.h>
#include <kernel/memory/pmm.h>
#include <kernel/memory/vmm.h>
#include <kernel/memory/heap.h>
#include <kernel/sys/process.h>
#include <kernel/sys/scheduler.h>
#include <kernel/sys/timers.h>
#include <arch/x86_64/cpu/cpu.h>
#include <klibc/stdio.h>
#include <klibc/string.h>

static tty_t* g_dtty;
static tty_t* g_prev;

#pragma region Low Level Helpers

/*
 * emit - Emits a string to the console without acquiring locks or refreshing
 */
static void emit(console_t* con, const char* s) {
    while (*s) con_putc(con, *s++);
}

/*
 * cl - Sets the color of the console
 */
static void cl(console_t* con, uint8_t fg, uint8_t bg) {
    con_set_color(con, fg, bg);
}

/*
 * section - Draws a section heading
 */
static void section(console_t* con, const char* name) {
    cl(con, CONSOLE_COLOR_LIGHT_CYAN, CONSOLE_COLOR_BLACK);
    emit(con, "  ");
    emit(con, name);
    con_putc(con, '\n');
    cl(con, CONSOLE_COLOR_LIGHT_GRAY, CONSOLE_COLOR_BLACK);
}

#pragma region Key Value Helpers

#define KW  "%-13s"
#define KV2W "%-20s"

/*
 * kv - Draws a key-value pair in the console
 */
static void kv(console_t* con, const char* k, const char* v) {
    char buf[256];
    ksnprintf(buf, sizeof(buf), "  " KW " : %s\n", k, v);
    emit(con, buf);
}

/*
 * kv2 - Draws two key-value pairs on the same line in the console
 */
static void kv2(console_t* con, const char* k1, const char* v1, const char* k2, const char* v2) {
    char buf[256];
    ksnprintf(buf, sizeof(buf), "  " KW " : " KV2W "  " KW " : %s\n",
              k1, v1, k2, v2);
    emit(con, buf);
}

/*
 * fmtsz - Formats a size in bytes into a human-readable string
 */
static char* fmtsz(char* buf, size_t n, uint64_t b) {
    if (b < (1ULL << 20)) ksnprintf(buf, n, "%lu KiB", b >> 10);
    else                   ksnprintf(buf, n, "%lu MiB", b >> 20);
    return buf;
}

#pragma region Dashboard Sections

/*
 * draw_cpu - Draws the CPU section in the console
 */
static void draw_cpu(console_t* con) {
    char b[32];
    const CPUInfo* ci = cpu_get_info();

    con_putc(con, '\n');
    section(con, "CPU");
    ksnprintf(b, sizeof(b), "%u", ci->core_count);
    kv2(con, "Vendor", ci->vendor, "Cores", b);
    kv(con, "Brand", ci->brand);

    static const struct { cpu_feature_t f; const char* n; } feats[] = {
        {CPU_FEAT_PAE,    "PAE"   }, {CPU_FEAT_NX,     "NX"    },
        {CPU_FEAT_SSE,    "SSE"   }, {CPU_FEAT_SSE2,   "SSE2"  },
        {CPU_FEAT_SSE3,   "SSE3"  }, {CPU_FEAT_SSSE3,  "SSSE3" },
        {CPU_FEAT_SSE4_1, "SSE4.1"}, {CPU_FEAT_SSE4_2, "SSE4.2"},
        {CPU_FEAT_AVX,    "AVX"   }, {CPU_FEAT_AVX2,   "AVX2"  },
        {CPU_FEAT_VMX,    "VMX"   }, {CPU_FEAT_SVM,    "SVM"   },
        {CPU_FEAT_64BIT,  "64BIT" }, {CPU_FEAT_SMEP,   "SMEP"  },
        {CPU_FEAT_SMAP,   "SMAP"  },
    };

    // Emit label in key style, then inline colored feature tokens
    cl(con, CONSOLE_COLOR_LIGHT_GRAY, CONSOLE_COLOR_BLACK);
    emit(con, "  Features      :");
    for (size_t i = 0; i < sizeof(feats)/sizeof(feats[0]); i++) {
        bool on = (ci->features & feats[i].f) != 0;
        cl(con, on ? CONSOLE_COLOR_LIGHT_GREEN : CONSOLE_COLOR_DARK_GRAY,
               CONSOLE_COLOR_BLACK);
        emit(con, " ");
        emit(con, feats[i].n);
    }
    cl(con, CONSOLE_COLOR_LIGHT_GRAY, CONSOLE_COLOR_BLACK);
    con_putc(con, '\n');
}

/*
 * draw_mem - Draws the memory section in the console
 */
static void draw_mem(console_t* con) {
    char ta[24], ua[24], fa[24], xa[24];

    /* --- Physical memory --- */
    pmm_stats_t ps;
    pmm_get_stats(&ps);
    uint64_t min_blk    = pmm_min_block_size();
    uint64_t phys_total = pmm_managed_size();

    uint64_t phys_free = 0, largest = 0;
    for (int i = PMM_MAX_ORDERS - 1; i >= 0; i--) {
        uint64_t bsz = min_blk << (uint32_t)i;
        phys_free += ps.free_blocks[i] * bsz;
        if (!largest && ps.free_blocks[i]) largest = bsz;
    }
    uint64_t phys_used = phys_total - phys_free;
    uint64_t frag      = phys_free ? (100 - (largest * 100 / phys_free)) : 0;

    section(con, "PHYSICAL MEMORY");

    kv2(con, "Total",        fmtsz(ta, sizeof(ta), phys_total),
             "Used",         fmtsz(ua, sizeof(ua), phys_used));
    kv2(con, "Free",         fmtsz(fa, sizeof(fa), phys_free),
             "Frag",         (ksnprintf(xa, sizeof(xa), "%lu%%", frag), xa));

    // kheap
    size_t ht, hu, hf, ho;
    heap_stats(heap_kernel_get(), &ht, &hu, &hf, &ho);
    int pct  = ht ? (int)(hu * 100 / ht) : 0;
    int bars = pct / 5;

    con_putc(con, '\n');
    section(con, "KERNEL HEAP");

    kv2(con, "Arena",   fmtsz(ta, sizeof(ta), ht), "In use",  fmtsz(ua, sizeof(ua), hu));
    kv2(con, "Free",    fmtsz(fa, sizeof(fa), hf), "Overhead", (ksnprintf(xa, sizeof(xa), "%lu B", (uint64_t)ho), xa));

    // Arena pressure bar
    emit(con, "  Pressure      : [");
    cl(con, pct > 80 ? CONSOLE_COLOR_LIGHT_RED :
            pct > 50 ? CONSOLE_COLOR_YELLOW     :
                       CONSOLE_COLOR_LIGHT_GREEN,
           CONSOLE_COLOR_BLACK);
    for (int i = 0; i < 20; i++) con_putc(con, i < bars ? '#' : '.');
    cl(con, CONSOLE_COLOR_LIGHT_GRAY, CONSOLE_COLOR_BLACK);
    ksnprintf(ta, sizeof(ta), "] %d%%\n", pct);
    emit(con, ta);
}

#pragma region Process/Thread Section

/*
 * state_str - Returns the string representation of a thread state
 */
static const char* state_str(thread_state_t s) {
    switch (s) {
        case THREAD_STATE_READY:    return "READY";
        case THREAD_STATE_RUNNING:  return "RUN  ";
        case THREAD_STATE_BLOCKED:  return "BLOCK";
        case THREAD_STATE_SLEEPING: return "SLEEP";
        case THREAD_STATE_DEAD:     return "DEAD ";
        default:                    return "?    ";
    }
}

/*
 * draw_procs - Draws the processes section in the console
 */
static void draw_procs(console_t* con) {
    char buf[160], rss[24];

    section(con, "PROCESSES");

    cl(con, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    ksnprintf(buf, sizeof(buf), "  %-4s  %-22s  %7s  %s\n",
              "PID", "NAME", "THREADS", "RSS");
    emit(con, buf);
    cl(con, CONSOLE_COLOR_LIGHT_GRAY, CONSOLE_COLOR_BLACK);

    process_t* proc = process_get_all();
    while (proc) {
        if (con->cursor_y >= con->height - 2) {
            break;
        }

        int nth = 0;
        for (thread_t* t = proc->threads; t; t = t->next) nth++;

        size_t vt = 0, res = 0;
        if (proc->vmm) vmm_stats(proc->vmm, &vt, &res);

        cl(con, CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
        ksnprintf(buf, sizeof(buf), "  %-4u  %-22.22s  %7d  %s\n",
                  proc->pid, proc->name, nth,
                  fmtsz(rss, sizeof(rss), res));
        emit(con, buf);

        cl(con, CONSOLE_COLOR_LIGHT_GRAY, CONSOLE_COLOR_BLACK);
        if (con->cursor_y + nth < con->height - 2) {
            for (thread_t* t = proc->threads; t; t = t->next) {
                ksnprintf(buf, sizeof(buf),
                          "        +-- TID %-4u  %-17.22s  [%s]\n",
                          t->tid, t->name, state_str(t->state));
                emit(con, buf);
            }
        }

        proc = proc->next;
    }
}

#pragma region Main Draw

/*
 * dash_draw - Draws the main dashboard content
 */
static void dash_draw(void) {
    console_t* con = g_dtty->console;

    // Update sticky header with current uptime
    uint64_t ms = get_uptime_ms();
    uint64_t s  = ms / 1000, m = s / 60, h = m / 60;
    s %= 60; m %= 60;
    char hdr[80];
    ksnprintf(hdr, sizeof(hdr), "GatOS Kernel Dashboard | Uptime: %02lu:%02lu:%02lu", h, m, s);
    con_header_write(con, 0, hdr, CONSOLE_COLOR_WHITE, CONSOLE_COLOR_MAGENTA);

    tty_t* active = g_active_tty;
    g_active_tty  = NULL;

    con_clear(con, CONSOLE_COLOR_BLACK);
    cl(con, CONSOLE_COLOR_LIGHT_GRAY, CONSOLE_COLOR_BLACK);

    draw_cpu(con);
    con_putc(con, '\n');
    draw_mem(con);
    con_putc(con, '\n');
    draw_procs(con);

    cl(con, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    while (con->cursor_y < con->height - 1) {
        con_putc(con, '\n');
    }
    emit(con, "  CTRL+SHIFT+ESC to close or ALT+TAB to cycle");

    g_active_tty = active;
    con_refresh(con);
}

#pragma region Dashboard Thread

/*
 * dash_thread_fn - Background thread function for the dashboard
 */
static void dash_thread_fn(void* arg) {
    (void)arg;
    while (1) {
        if (g_active_tty == g_dtty) {
            dash_draw();
            sleep_ms(2000);
        } else {
            sleep_ms(100); /* poll; dashboard opens within 100ms */
        }
    }
}

#pragma region Dashboard API

bool is_dash_tty(void) {
    return g_active_tty == g_dtty;
}

/*
 * dash_toggle - Toggles the visibility of the dashboard
 */
void dash_toggle(void) {
    if (g_active_tty == g_dtty) {
        tty_switch(g_prev ? g_prev : g_kernel_tty);
        g_prev = NULL;
    } else {
        g_prev = g_active_tty;
        tty_switch(g_dtty);
    }
}

/*
 * dash_init - Initializes the dashboard
 */
void dash_init(void) {
    g_dtty = tty_create();
    if (!g_dtty) return;

    g_dtty->hidden = true; // exclude from Alt+Tab

    con_header_init(g_dtty->console, 1);
    con_header_write(g_dtty->console, 0,
                     "  GatOS Kernel Dashboard",
                     CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    con_enable_cursor(g_dtty->console, false);

    process_t* proc = process_create("dashboard", g_dtty);
    if (!proc) return;

    thread_t* t = thread_create(proc, "dash", dash_thread_fn, NULL, false, 0);
    if (!t) return;

    sched_add(t);
}
