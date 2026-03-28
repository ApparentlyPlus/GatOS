/*
 * dashboard.c - Live Kernel Dashboard
 *
 * Provides a real-time dashboard with CPU, memory, and process information.
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
#include <kernel/debug.h>
#include <arch/x86_64/cpu/cpu.h>
#include <klibc/stdio.h>
#include <klibc/string.h>

static tty_t* dashTTY;
static tty_t* lastTTY;

#pragma region Layout

// layout struct
typedef struct {
    int W;      // total width in chars, used for calculating section sizes
    int gap;    // gap between sections (also used as min width for some sections)
    int key_w;  // width of the "key" part of kv pairs
    int val2_w; // width of the "value" part of two-column kv pairs
    int bar_w;  // width of progress bars
    int pid_w;  // width of PID column in process table (fixed width since numeric)
    int name_w; // width of Name column in process table
    int thr_w;  // width of Threads column in process table (fixed width since numeric)
    int mem_w;  // width of Memory column in process table
    int tnw;    // width of truncated name column (name_w minus some for ellipsis if needed)
    int state_w;// width of State column in process table
} layout_t;

/*
 * build_layout - Create a layout struct based on the given width
 */
static layout_t build_layout(int width) {
    layout_t L;

    // Calculate section widths based on total width, with some minimums to prevent collapse
    L.W = width;
    L.gap = width / 55;
    if (L.gap < 2) L.gap = 2;

    // Key/value widths for stats sections, with some minimums
    L.key_w = width / 10;
    if (L.key_w < 12) L.key_w = 12;

    // For two-column kv pairs, value width is a bit more flexible since it can truncate if needed
    L.val2_w = width / 2 - L.key_w - 5;
    if (L.val2_w < 14) L.val2_w = 14;

    // Progress bar width, with a minimum to ensure visibility
    L.bar_w = width / 8;
    if (L.bar_w < 10) L.bar_w = 10;

    // hardcoded widths, maybe should rethink later
    L.pid_w = 4;
    L.thr_w = 3;

    // Name column gets whatever is left, but enforce a minimum and calculate truncated name width for process table
    L.name_w = width / 5;
    if (L.name_w < 14) L.name_w = 14;

    // Memory column width, with a minimum to prevent collapse
    L.mem_w = width / 18;
    if (L.mem_w < 8) L.mem_w = 8;

    // Truncated name width for process table, if name exceeds this it will be truncated with ellipsis
    L.tnw = L.pid_w + L.name_w - 10;
    if (L.tnw < 8) L.tnw = 8;

    // State column gets whatever is left after accounting for other columns and gaps, with a minimum to prevent collapse
    L.state_w = width / 14;
    if (L.state_w < 14) L.state_w = 14;

    return L;
}

#pragma region Helpers

/*
 * print_str - Simple helper to print a null-terminated string to the console
 */
static void print_str(console_t* c, const char* s) {
    while (*s) {
        con_putc(c, *s);
        s++;
    }
}

/*
 * set_col - Set the color of the console
 */
static void set_col(console_t* c, uint8_t fg, uint8_t bg) {
    con_set_color(c, fg, bg);
}

/*
 * pick_pct_color - Pick a color based on percentage
 */
static uint8_t pick_pct_color(int percent) {
    if (percent > 80) return CONSOLE_COLOR_LIGHT_RED;
    if (percent > 50) return CONSOLE_COLOR_YELLOW;
    return CONSOLE_COLOR_LIGHT_GREEN;
}

/*
 * draw_section - Draw a section header with a title and underline
 */
static void draw_section(console_t* c, const layout_t* L, const char* title) {
    set_col(c, CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    print_str(c, "  ");
    print_str(c, title);
    print_str(c, " ");

    int len = 0;
    const char* tmp = title;
    while (*tmp++) len++;

    int remaining = L->W - 3 - len;

    set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);

    // capped at 256 just in case
    for (int i = 0; i < remaining && i < 256; i++) {
        print_str(c, "\xe2\x94\x80");
    }

    con_putc(c, '\n');
    set_col(c, CONSOLE_COLOR_LIGHT_GRAY, CONSOLE_COLOR_BLACK);
}

#pragma region KV

/*
 * draw_kv - Draw a single key-value pair, with the key in a different color
 */
static void draw_kv(console_t* c, const layout_t* L, const char* key, const char* val) {
    char tmp[64];
    ksnprintf(tmp, sizeof(tmp), "  %-*s : ", L->key_w, key);
    set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    print_str(c, tmp);
    set_col(c, CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    print_str(c, val);
    con_putc(c, '\n');
}

/*
 * draw_kv_pair - Draw a pair of kv pairs side by side
 */
static void draw_kv_pair(console_t* c, const layout_t* L, const char* k1, const char* v1, const char* k2, const char* v2) {
    char tmp[64];
    ksnprintf(tmp, sizeof(tmp), "  %-*s : ", L->key_w, k1);
    set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    print_str(c, tmp);

    set_col(c, CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    ksnprintf(tmp, sizeof(tmp), "%-*s", L->val2_w, v1);
    print_str(c, tmp);

    ksnprintf(tmp, sizeof(tmp), "  %-*s : ", L->key_w, k2);
    set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    print_str(c, tmp);

    set_col(c, CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    print_str(c, v2);

    con_putc(c, '\n');
}

/*
 * draw_kv_pair_col - Draw a pair of kv pairs side by side with custom value colors
 */
static void draw_kv_pair_col(console_t* c, const layout_t* L, 
                             const char* k1, const char* v1, uint8_t c1, 
                             const char* k2, const char* v2, uint8_t c2) {
    char tmp[64];
    ksnprintf(tmp, sizeof(tmp), "  %-*s : ", L->key_w, k1);
    set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    print_str(c, tmp);

    set_col(c, c1, CONSOLE_COLOR_BLACK);
    ksnprintf(tmp, sizeof(tmp), "%-*s", L->val2_w, v1);
    print_str(c, tmp);

    ksnprintf(tmp, sizeof(tmp), "  %-*s : ", L->key_w, k2);
    set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    print_str(c, tmp);

    set_col(c, c2, CONSOLE_COLOR_BLACK);
    print_str(c, v2);

    con_putc(c, '\n');
}

/*
 * draw_bar - Draw a progress bar with a label and percentage
 */
static void draw_bar(console_t* c, const layout_t* L, const char* label, int percent) {
    char tmp[64];

    int filled = (percent * L->bar_w) / 100;

    ksnprintf(tmp, sizeof(tmp), "  %-*s : [", L->key_w, label);

    set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    print_str(c, tmp);

    set_col(c, pick_pct_color(percent), CONSOLE_COLOR_BLACK);

    for (int i = 0; i < L->bar_w; i++) {
        // using . instead of space so it's easier to see the empty part
        con_putc(c, (i < filled) ? '#' : '.');
    }

    set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    print_str(c, "] ");

    set_col(c, pick_pct_color(percent), CONSOLE_COLOR_BLACK);

    ksnprintf(tmp, sizeof(tmp), "%d%%\n", percent);
    print_str(c, tmp);

    set_col(c, CONSOLE_COLOR_LIGHT_GRAY, CONSOLE_COLOR_BLACK);
}

/*
 * fmt_size - Format a size in bytes into a human-readable string
 */
static char* fmt_size(char* out, size_t n, uint64_t bytes) {
    if (bytes < (1ULL << 10)) {
        ksnprintf(out, n, "%lu B", bytes);
    } else if (bytes < (1ULL << 20)) {
        ksnprintf(out, n, "%lu KiB", bytes >> 10);
    } else {
        ksnprintf(out, n, "%lu MiB", bytes >> 20);
    }
    return out;
}

#pragma region CPU

/*
 * render_cpu - Render CPU information
 */
static void render_cpu(console_t* c, const layout_t* L) {
    char buf[32];
    const cpu_info_t* info = cpu_get_info();
    draw_section(c, L, "CPU");
    ksnprintf(buf, sizeof(buf), "%u", info->core_count);
    draw_kv_pair(c, L, "Vendor", info->vendor, "Cores", buf);
    draw_kv(c, L, "Brand", info->brand);

    static const struct { cpu_feature_t f; const char* n; } feats[] = {
        {CF_PAE,    "PAE"   }, {CF_NX,     "NX"    },
        {CF_SSE,    "SSE"   }, {CF_SSE2,   "SSE2"  },
        {CF_SSE3,   "SSE3"  }, {CF_SSSE3,  "SSSE3" },
        {CF_SSE4_1, "SSE4.1"}, {CF_SSE4_2, "SSE4.2"},
        {CF_AVX,    "AVX"   }, {CF_AVX2,   "AVX2"  },
        {CF_VMX,    "VMX"   }, {CF_SVM,    "SVM"   },
        {CF_64BIT,  "64BIT" }, {CF_SMEP,   "SMEP"  },
        {CF_SMAP,   "SMAP"  },
    };

    ksnprintf(buf, sizeof(buf), "  %-*s : ", L->key_w, "Features");
    set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    print_str(c, buf);
    
    for (size_t i = 0; i < sizeof(feats)/sizeof(feats[0]); i++) {
        bool on = (info->features & feats[i].f) != 0;
        set_col(c, on ? CONSOLE_COLOR_LIGHT_GREEN : CONSOLE_COLOR_DARK_GRAY,
               CONSOLE_COLOR_BLACK);
        print_str(c, feats[i].n);
        print_str(c, " ");
    }
    con_putc(c, '\n');
}

#pragma region Memory

/*
 * render_mem - Render Physical memory stats and kernel heap stats
 */
static void render_mem(console_t* c, const layout_t* L) {
    char ta[24], ua[24], fa[24], xa[24];

    // Physical memory
    pmm_stats_t ps;
    pmm_get_stats(&ps);
    uint64_t min_blk = pmm_min_block_size();
    uint64_t phys_total = pmm_managed_size();

    uint64_t phys_free = 0, largest = 0;
    for (int i = PMM_MAX_ORDERS - 1; i >= 0; i--) {
        uint64_t bsz = min_blk << (uint32_t)i;
        phys_free += ps.free_blocks[i] * bsz;
        if (!largest && ps.free_blocks[i]) largest = bsz;
    }
    
    uint64_t phys_used = phys_total - phys_free;
    int used_pct = phys_total ? (int)(phys_used * 100 / phys_total) : 0;
    uint64_t frag = phys_free ? (100 - (largest * 100 / phys_free)) : 0;

    draw_section(c, L, "PHYSICAL MEMORY");

    draw_kv_pair_col(c, L,
         "Total", fmt_size(ta, sizeof(ta), phys_total), CONSOLE_COLOR_WHITE,
         "Used",  fmt_size(ua, sizeof(ua), phys_used),  pick_pct_color(used_pct));
         
    draw_kv_pair_col(c, L,
         "Free",  fmt_size(fa, sizeof(fa), phys_free),  CONSOLE_COLOR_WHITE,
         "Frag",  (ksnprintf(xa, sizeof(xa), "%lu%%", frag), xa), pick_pct_color((int)frag));
         
    draw_bar(c, L, "Usage", used_pct);

    // Kernel heap
    size_t ht, hu, hf, ho;
    heap_stats(heap_kernel_get(), &ht, &hu, &hf, &ho);
    int heap_pct = ht ? (int)(hu * 100 / ht) : 0;

    con_putc(c, '\n');
    draw_section(c, L, "KERNEL HEAP");

    draw_kv_pair_col(c, L,
         "Arena",    fmt_size(ta, sizeof(ta), ht), CONSOLE_COLOR_WHITE,
         "In use",   fmt_size(ua, sizeof(ua), hu), pick_pct_color(heap_pct));
         
    draw_kv_pair_col(c, L,
         "Free",     fmt_size(fa, sizeof(fa), hf), CONSOLE_COLOR_WHITE,
         "Overhead", (ksnprintf(xa, sizeof(xa), "%lu B", (uint64_t)ho), xa), CONSOLE_COLOR_WHITE);
         
    draw_bar(c, L, "Pressure", heap_pct);
}

#pragma region Processes

static const char* get_state_str(thread_state_t s) {
    switch (s) {
        case T_READY:    return "READY";
        case T_RUNNING:  return "RUNNING";
        case T_BLOCKED:  return "BLOCKED";
        case T_SLEEPING: return "SLEEPING";
        case T_DEAD:     return "DEAD";
        default:         return "?";
    }
}

static uint8_t get_state_color(thread_state_t s) {
    switch (s) {
        case T_RUNNING:  return CONSOLE_COLOR_LIGHT_GREEN;
        case T_READY:    return CONSOLE_COLOR_GREEN;
        case T_SLEEPING: return CONSOLE_COLOR_YELLOW;
        case T_BLOCKED:  return CONSOLE_COLOR_LIGHT_CYAN;
        case T_DEAD:     return CONSOLE_COLOR_DARK_GRAY;
        default:         return CONSOLE_COLOR_LIGHT_GRAY;
    }
}

/*
 * render_procs - Scaled process and thread table
 */
static void render_procs(console_t* c, const layout_t* L) {
    char buf[512], mem[16], virt_s[16], detail[48];

    draw_section(c, L, "PROCESSES");

    set_col(c, CONSOLE_COLOR_LIGHT_CYAN, CONSOLE_COLOR_BLACK);
    ksnprintf(buf, sizeof(buf), "  %-*s%*s%-*s%*s%*s%*s\n",
              L->pid_w,  "PID",
              L->gap,    "",
              L->name_w, "NAME",
              L->gap + L->thr_w, "THR",
              L->gap + L->mem_w, "RSS",
              L->gap + L->mem_w, "VIRT");
    print_str(c, buf);

    process_t* proc = process_get_all();
    while (proc) {
        if (c->cy >= c->height - 2) break;

        int nth = 0;
        for (thread_t* t = proc->threads; t; t = t->next) nth++;

        size_t vt = 0, res = 0;
        if (proc->vmm) vmm_stats(proc->vmm, &vt, &res);

        // Process row, we got PID in yellow, rest in white
        set_col(c, CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_BLACK);
        ksnprintf(buf, sizeof(buf), "  %-*u", L->pid_w, proc->pid);
        print_str(c, buf);
        
        set_col(c, CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
        ksnprintf(buf, sizeof(buf), "%*s%-*.*s%*d%*s%*s\n",
                  L->gap,    "",
                  L->name_w, L->name_w, proc->name,
                  L->gap + L->thr_w, nth,
                  L->gap + L->mem_w, fmt_size(mem,    sizeof(mem),    (uint64_t)res),
                  L->gap + L->mem_w, fmt_size(virt_s, sizeof(virt_s), (uint64_t)vt));
        print_str(c, buf);

        // Thread subrows
        for (thread_t* t = proc->threads; t; t = t->next) {
            if (c->cy >= c->height - 2) break;

            if (t->state == T_SLEEPING) {
                int64_t left = (int64_t)t->wake_at - (int64_t)get_uptime_ms();
                ksnprintf(detail, sizeof(detail), "SLEEPING %ldms", left > 0 ? left : 0);
            } else {
                ksnprintf(detail, sizeof(detail), "%s", get_state_str(t->state));
            }

            int badge_w = 20;
            int left_len = 16 + L->gap + L->tnw;
            int pad = (L->W - 4) - badge_w - left_len;
            if (pad < 1) pad = 1;

            set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
            ksnprintf(buf, sizeof(buf), "        \xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 %-*u%*s%-*.*s%*s",
                      4,       t->tid,
                      L->gap,  "",
                      L->tnw,  L->tnw, t->name,
                      pad,     "");
            print_str(c, buf);

            int dlen = 0;
            for (char* p = detail; *p; p++) dlen++;
            if (dlen > badge_w - 2) dlen = badge_w - 2;
            int lpad = (badge_w - 2 - dlen) / 2;
            int rpad = badge_w - 2 - dlen - lpad;

            set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
            print_str(c, "[");
            
            set_col(c, get_state_color(t->state), CONSOLE_COLOR_BLACK);
            ksnprintf(buf, sizeof(buf), "%*s%.*s%*s", 
                      lpad ? lpad : 0, "", 
                      dlen, detail, 
                      rpad ? rpad : 0, "");
            print_str(c, buf);
            
            set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
            print_str(c, "]\n");
        }

        proc = proc->next;
    }
}

#pragma region Main Draw

/*
 * dashboard_draw - Draw the main dashboard
 */
static void dashboard_draw(void) {
    console_t* con = dashTTY->console;
    uint64_t ms = get_uptime_ms();

    // kinda verbose but easier to read than clever math
    uint64_t totalSec = ms / 1000;
    uint64_t mins = totalSec / 60;
    uint64_t hrs = mins / 60;

    totalSec %= 60;
    mins %= 60;

    char header[80];

    ksnprintf(header, sizeof(header), "  GatOS Kernel Dashboard  |  Uptime: %02lu:%02lu:%02lu",
              hrs, mins, totalSec);

    con_header_write(con, 0, header, CONSOLE_COLOR_WHITE, CONSOLE_COLOR_MAGENTA);

    // temporarily detach active tty
    tty_t* current = active_tty;
    active_tty = NULL;

    con_clear(con, CONSOLE_COLOR_BLACK);
    set_col(con, CONSOLE_COLOR_LIGHT_GRAY, CONSOLE_COLOR_BLACK);

    layout_t L = build_layout((int)con->width);

    render_cpu(con, &L);
    con_putc(con, '\n');
    
    render_mem(con, &L);
    con_putc(con, '\n');
    
    render_procs(con, &L);

    // Pad remaining rows to clear artifacts and anchor bottom text
    set_col(con, CONSOLE_COLOR_BLACK, CONSOLE_COLOR_BLACK);
    while (con->cy < con->height - 1) con_putc(con, '\n');

    set_col(con, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    print_str(con, " CTRL+SHIFT+ESC to close or ALT+TAB to cycle");

    // reattach active tty
    active_tty = current;

    con_refresh(con);
}

#pragma region Thread

/*
 * dashboard_thread - Thread function for the dashboard
 */
static void dashboard_thread(void* arg) {
    (void)arg;

    while (1) {
        if (active_tty == dashTTY) {
            dashboard_draw();
            // refresh every second while active, otherwise idle to save resources
            sleep_ms(1000);
        } else {
            sleep_ms(100); // idle-ish 
        }
    }
}

#pragma region API

/*
 * dash_active - Returns true if the dashboard is currently active
 */
bool dash_active(void) {
    return active_tty == dashTTY;
}

/*
 * dashboard_toggle - Toggle the visibility of the dashboard
 */
void dash_toggle(void) {
    if (active_tty == dashTTY) {
        tty_switch(lastTTY ? lastTTY : kernel_tty);
        lastTTY = NULL;
    } else {
        lastTTY = active_tty;
        tty_switch(dashTTY);
    }
}

void dash_init(void) {
    dashTTY = tty_create();
    if (!dashTTY){
        LOGF("Failed to create dashboard TTY\n");
        return;
    }

    dashTTY->hidden = true;

    con_header_init(dashTTY->console, 1);
    con_header_write(dashTTY->console, 0, "  GatOS Kernel Dashboard",
                     CONSOLE_COLOR_WHITE, CONSOLE_COLOR_MAGENTA);

    con_enable_cursor(dashTTY->console, false);

    process_t* p = process_create("dashboard", dashTTY);
    if (!p){ LOGF("Failed to create dashboard process\n"); return; }

    thread_t* t = thread_create(p, "dash", dashboard_thread, NULL, false, 0);
    if (!t) { LOGF("Failed to create dashboard thread\n"); return; }

    sched_add(t);
}