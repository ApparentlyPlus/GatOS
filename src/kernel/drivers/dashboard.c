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

static tty_t* dtty;
static tty_t* prev_tty;

#pragma region Layout

/*
 * layout_t - All column/field widths derived from con->width
 *
 *   gap     = W/55, min 2          - uniform inter-column spacing
 *   key_w   = W/10, min 12         - key label width (kv / kv2)
 *   val2_w  = W/2 - key_w - 5      - left-value field in kv2 (right pair starts at W/2)
 *   bar_w   = W/8,  min 10         - bar interior width
 *   pid_w   = 4
 *   name_w  = W/5,  min 14
 *   thr_w   = 3
 *   mem_w   = W/18, min 8
 *   tnw     = pid_w + name_w - 10
 *   state_w = W/14, min 14
 */
typedef struct {
    int W;
    int gap;
    int key_w;
    int val2_w;
    int bar_w;
    int pid_w;
    int name_w;
    int thr_w;
    int mem_w;
    int tnw;
    int state_w;
} layout_t;

static layout_t make_layout(int W) {
    layout_t L;
    L.W       = W;
    L.gap     = W / 55; if (L.gap     <  2) L.gap     =  2;
    L.key_w   = W / 10; if (L.key_w   < 12) L.key_w   = 12;
    L.val2_w  = W / 2 - L.key_w - 5;
                        if (L.val2_w  < 14) L.val2_w  = 14;
    L.bar_w   = W / 8;  if (L.bar_w   < 10) L.bar_w   = 10;
    L.pid_w   = 4;
    L.name_w  = W / 5;  if (L.name_w  < 14) L.name_w  = 14;
    L.thr_w   = 3;
    L.mem_w   = W / 18; if (L.mem_w   <  8) L.mem_w   =  8;
    L.tnw     = L.pid_w + L.name_w - 10;
                        if (L.tnw     <  8) L.tnw     =  8;
    L.state_w = W / 14; if (L.state_w < 14) L.state_w = 14;
    return L;
}

#pragma region Low Level Helpers

static void emit(console_t* con, const char* s) {
    while (*s) con_putc(con, *s++);
}

static void cl(console_t* con, uint8_t fg, uint8_t bg) {
    con_set_color(con, fg, bg);
}

/* pct_color - Traffic-light color for a 0–100 percentage */
static uint8_t pct_color(int pct) {
    if (pct > 80) return CONSOLE_COLOR_LIGHT_RED;
    if (pct > 50) return CONSOLE_COLOR_YELLOW;
    return CONSOLE_COLOR_LIGHT_GREEN;
}

/*
 * section - Colored title followed by a dim horizontal rule to end of line
 *   "  TITLE ─────────────────────────────────────"
 */
static void section(console_t* con, const layout_t* L, const char* name) {
    cl(con, CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    emit(con, "  ");
    emit(con, name);
    emit(con, " ");

    /* measure name length to fill the rest of the line */
    int name_len = 0;
    for (const char* p = name; *p; p++) name_len++;
    int fill = L->W - 3 - name_len;
    cl(con, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    for (int i = 0; i < fill && i < 256; i++) emit(con, "\xe2\x94\x80"); /* U+2500 ─ */
    con_putc(con, '\n');
    cl(con, CONSOLE_COLOR_LIGHT_GRAY, CONSOLE_COLOR_BLACK);
}

#pragma region Key Value Helpers

/*
 * kv - Key in dim gray, value in white
 *   "  key          : value"
 */
static void kv(console_t* con, const layout_t* L, const char* k, const char* v) {
    char buf[64];
    ksnprintf(buf, sizeof(buf), "  %-*s : ", L->key_w, k);
    cl(con, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    emit(con, buf);
    cl(con, CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    emit(con, v);
    con_putc(con, '\n');
}

/*
 * kv2 - Two key-value pairs on one line; right pair starts at column W/2
 */
static void kv2(console_t* con, const layout_t* L,
                const char* k1, const char* v1,
                const char* k2, const char* v2) {
    char buf[64];
    ksnprintf(buf, sizeof(buf), "  %-*s : ", L->key_w, k1);
    cl(con, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    emit(con, buf);
    cl(con, CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    ksnprintf(buf, sizeof(buf), "%-*s", L->val2_w, v1);
    emit(con, buf);
    ksnprintf(buf, sizeof(buf), "  %-*s : ", L->key_w, k2);
    cl(con, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    emit(con, buf);
    cl(con, CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    emit(con, v2);
    con_putc(con, '\n');
}

/*
 * kv2c - kv2 with explicit colors for each value (for traffic-light feedback)
 */
static void kv2c(console_t* con, const layout_t* L,
                 const char* k1, const char* v1, uint8_t c1,
                 const char* k2, const char* v2, uint8_t c2) {
    char buf[64];
    ksnprintf(buf, sizeof(buf), "  %-*s : ", L->key_w, k1);
    cl(con, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    emit(con, buf);
    cl(con, c1, CONSOLE_COLOR_BLACK);
    ksnprintf(buf, sizeof(buf), "%-*s", L->val2_w, v1);
    emit(con, buf);
    ksnprintf(buf, sizeof(buf), "  %-*s : ", L->key_w, k2);
    cl(con, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    emit(con, buf);
    cl(con, c2, CONSOLE_COLOR_BLACK);
    emit(con, v2);
    con_putc(con, '\n');
}

/*
 * bar - Draws a labeled progress bar
 *   "  label        : [####..........] pct%"
 */
static void bar(console_t* con, const layout_t* L,
                const char* label, int pct) {
    char buf[64];
    int  filled = pct * L->bar_w / 100;

    ksnprintf(buf, sizeof(buf), "  %-*s : [", L->key_w, label);
    cl(con, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    emit(con, buf);
    cl(con, pct_color(pct), CONSOLE_COLOR_BLACK);
    for (int i = 0; i < L->bar_w; i++) con_putc(con, i < filled ? '#' : '.');
    cl(con, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    emit(con, "] ");
    cl(con, pct_color(pct), CONSOLE_COLOR_BLACK);
    ksnprintf(buf, sizeof(buf), "%d%%\n", pct);
    emit(con, buf);
    cl(con, CONSOLE_COLOR_LIGHT_GRAY, CONSOLE_COLOR_BLACK);
}

/*
 * fmtsz - Formats a size in bytes to a human-readable string
 */
static char* fmtsz(char* buf, size_t n, uint64_t b) {
    if      (b < (1ULL << 10)) ksnprintf(buf, n, "%lu B",   b);
    else if (b < (1ULL << 20)) ksnprintf(buf, n, "%lu KiB", b >> 10);
    else                       ksnprintf(buf, n, "%lu MiB", b >> 20);
    return buf;
}

#pragma region Dashboard Sections

/*
 * draw_cpu - CPU vendor, brand, core count, and feature flags
 */
static void draw_cpu(console_t* con, const layout_t* L) {
    char b[32];
    const CPUInfo* ci = cpu_get_info();

    section(con, L, "CPU");

    ksnprintf(b, sizeof(b), "%u", ci->core_count);
    kv2(con, L, "Vendor", ci->vendor, "Cores", b);
    kv(con,  L, "Brand",  ci->brand);

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

    /* Label aligned with kv key column */
    ksnprintf(b, sizeof(b), "  %-*s : ", L->key_w, "Features");
    cl(con, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    emit(con, b);
    for (size_t i = 0; i < sizeof(feats)/sizeof(feats[0]); i++) {
        bool on = (ci->features & feats[i].f) != 0;
        cl(con, on ? CONSOLE_COLOR_LIGHT_GREEN : CONSOLE_COLOR_DARK_GRAY,
               CONSOLE_COLOR_BLACK);
        emit(con, feats[i].n);
        emit(con, " ");
    }
    con_putc(con, '\n');
}

/*
 * draw_mem - Physical memory stats + kernel heap stats with pressure bar
 */
static void draw_mem(console_t* con, const layout_t* L) {
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
    int      used_pct  = phys_total ? (int)(phys_used * 100 / phys_total) : 0;
    uint64_t frag      = phys_free  ? (100 - (largest * 100 / phys_free)) : 0;

    section(con, L, "PHYSICAL MEMORY");

    kv2c(con, L,
         "Total", fmtsz(ta, sizeof(ta), phys_total), CONSOLE_COLOR_WHITE,
         "Used",  fmtsz(ua, sizeof(ua), phys_used),  pct_color(used_pct));
    kv2c(con, L,
         "Free",  fmtsz(fa, sizeof(fa), phys_free),  CONSOLE_COLOR_WHITE,
         "Frag",  (ksnprintf(xa, sizeof(xa), "%lu%%", frag), xa),
                  pct_color((int)frag));
    bar(con, L, "Usage", used_pct);

    /* --- Kernel heap --- */
    size_t ht, hu, hf, ho;
    heap_stats(heap_kernel_get(), &ht, &hu, &hf, &ho);
    int heap_pct = ht ? (int)(hu * 100 / ht) : 0;

    con_putc(con, '\n');
    section(con, L, "KERNEL HEAP");

    kv2c(con, L,
         "Arena",    fmtsz(ta, sizeof(ta), ht), CONSOLE_COLOR_WHITE,
         "In use",   fmtsz(ua, sizeof(ua), hu), pct_color(heap_pct));
    kv2c(con, L,
         "Free",     fmtsz(fa, sizeof(fa), hf), CONSOLE_COLOR_WHITE,
         "Overhead", (ksnprintf(xa, sizeof(xa), "%lu B", (uint64_t)ho), xa),
                     CONSOLE_COLOR_WHITE);
    bar(con, L, "Pressure", heap_pct);
}

#pragma region Process/Thread Section

/*
 * state_str / state_color - Human-readable label and traffic-light color per state
 */
static const char* state_str(thread_state_t s) {
    switch (s) {
        case THREAD_STATE_READY:    return "READY";
        case THREAD_STATE_RUNNING:  return "RUNNING";
        case THREAD_STATE_BLOCKED:  return "BLOCKED";
        case THREAD_STATE_SLEEPING: return "SLEEPING";
        case THREAD_STATE_DEAD:     return "DEAD";
        default:                    return "?";
    }
}

static uint8_t state_color(thread_state_t s) {
    switch (s) {
        case THREAD_STATE_RUNNING:  return CONSOLE_COLOR_LIGHT_GREEN;
        case THREAD_STATE_READY:    return CONSOLE_COLOR_GREEN;
        case THREAD_STATE_SLEEPING: return CONSOLE_COLOR_YELLOW;
        case THREAD_STATE_BLOCKED:  return CONSOLE_COLOR_LIGHT_CYAN;
        case THREAD_STATE_DEAD:     return CONSOLE_COLOR_DARK_GRAY;
        default:                    return CONSOLE_COLOR_LIGHT_GRAY;
    }
}

/*
 * draw_procs - Scaled process + thread table
 */
static void draw_procs(console_t* con, const layout_t* L) {
    char buf[512], mem[16], virt_s[16], detail[48];

    section(con, L, "PROCESSES");

    /* Header row */
    cl(con, CONSOLE_COLOR_LIGHT_CYAN, CONSOLE_COLOR_BLACK);
    ksnprintf(buf, sizeof(buf), "  %-*s%*s%-*s%*s%*s%*s\n",
              L->pid_w,  "PID",
              L->gap,    "",
              L->name_w, "NAME",
              L->gap + L->thr_w, "THR",
              L->gap + L->mem_w, "RSS",
              L->gap + L->mem_w, "VIRT");
    emit(con, buf);

    process_t* proc = process_get_all();
    while (proc) {
        if (con->cursor_y >= con->height - 2) break;

        int nth = 0;
        for (thread_t* t = proc->threads; t; t = t->next) nth++;

        size_t vt = 0, res = 0;
        if (proc->vmm) vmm_stats(proc->vmm, &vt, &res);

        /* Process row: PID in yellow, rest in white */
        cl(con, CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_BLACK);
        ksnprintf(buf, sizeof(buf), "  %-*u", L->pid_w, proc->pid);
        emit(con, buf);
        cl(con, CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
        ksnprintf(buf, sizeof(buf), "%*s%-*.*s%*d%*s%*s\n",
                  L->gap,    "",
                  L->name_w, L->name_w, proc->name,
                  L->gap + L->thr_w, nth,
                  L->gap + L->mem_w, fmtsz(mem,    sizeof(mem),    (uint64_t)res),
                  L->gap + L->mem_w, fmtsz(virt_s, sizeof(virt_s), (uint64_t)vt));
        emit(con, buf);

        /* Thread sub-rows: prefix in dim, state bracket right-aligned in state color */
        for (thread_t* t = proc->threads; t; t = t->next) {
            if (con->cursor_y >= con->height - 2) break;

            if (t->state == THREAD_STATE_SLEEPING) {
                int64_t left = (int64_t)t->sleep_until - (int64_t)get_uptime_ms();
                ksnprintf(detail, sizeof(detail), "SLEEPING %ldms", left > 0 ? left : 0);
            } else {
                ksnprintf(detail, sizeof(detail), "%s", state_str(t->state));
            }

            int badge_w = 20;
            int left_len = 16 + L->gap + L->tnw;
            int pad = (L->W - 4) - badge_w - left_len;
            if (pad < 1) pad = 1;

            cl(con, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
            ksnprintf(buf, sizeof(buf), "        \xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 %-*u%*s%-*.*s%*s",
                      4,       t->tid,
                      L->gap,  "",
                      L->tnw,  L->tnw, t->name,
                      pad,     "");
            emit(con, buf);

            int dlen = 0;
            for (char* p = detail; *p; p++) dlen++;
            if (dlen > badge_w - 2) dlen = badge_w - 2;
            int lpad = (badge_w - 2 - dlen) / 2;
            int rpad = badge_w - 2 - dlen - lpad;

            cl(con, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
            emit(con, "[");
            cl(con, state_color(t->state), CONSOLE_COLOR_BLACK);
            
            ksnprintf(buf, sizeof(buf), "%*s%.*s%*s", 
                      lpad ? lpad : 0, "", 
                      dlen, detail, 
                      rpad ? rpad : 0, "");
            emit(con, buf);
            
            cl(con, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
            emit(con, "]\n");
        }

        proc = proc->next;
    }
}

#pragma region Main Draw

static void dash_draw(void) {
    console_t* con = dtty->console;

    /* Update sticky header with current uptime */
    uint64_t ms = get_uptime_ms();
    uint64_t s  = ms / 1000, m = s / 60, h = m / 60;
    s %= 60; m %= 60;
    char hdr[80];
    ksnprintf(hdr, sizeof(hdr), "  GatOS Kernel Dashboard  |  Uptime: %02lu:%02lu:%02lu",
              h, m, s);
    con_header_write(con, 0, hdr, CONSOLE_COLOR_WHITE, CONSOLE_COLOR_MAGENTA);

    tty_t* active = active_tty;
    active_tty  = NULL;

    con_clear(con, CONSOLE_COLOR_BLACK);
    cl(con, CONSOLE_COLOR_LIGHT_GRAY, CONSOLE_COLOR_BLACK);

    layout_t L = make_layout((int)con->width);

    draw_cpu(con, &L);
    con_putc(con, '\n');
    draw_mem(con, &L);
    con_putc(con, '\n');
    draw_procs(con, &L);

    /* Pad remaining rows */
    cl(con, CONSOLE_COLOR_BLACK, CONSOLE_COLOR_BLACK);
    while (con->cursor_y < con->height - 1) con_putc(con, '\n');

    cl(con, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    emit(con, " CTRL+SHIFT+ESC to close or ALT+TAB to cycle");

    active_tty = active;
    con_refresh(con);
}

#pragma region Dashboard Thread

static void dash_thread_fn(void* arg) {
    (void)arg;
    while (1) {
        if (active_tty == dtty) {
            dash_draw();
            sleep_ms(2000);
        } else {
            sleep_ms(100);
        }
    }
}

#pragma region Dashboard API

bool is_dash_tty(void) {
    return active_tty == dtty;
}

void dash_toggle(void) {
    if (active_tty == dtty) {
        tty_switch(prev_tty ? prev_tty : kernel_tty);
        prev_tty = NULL;
    } else {
        prev_tty = active_tty;
        tty_switch(dtty);
    }
}

void dash_init(void) {
    dtty = tty_create();
    if (!dtty) return;

    dtty->hidden = true;

    con_header_init(dtty->console, 1);
    con_header_write(dtty->console, 0,
                     "  GatOS Kernel Dashboard",
                     CONSOLE_COLOR_WHITE, CONSOLE_COLOR_MAGENTA);
    con_enable_cursor(dtty->console, false);

    process_t* proc = process_create("dashboard", dtty);
    if (!proc) return;

    thread_t* t = thread_create(proc, "dash", dash_thread_fn, NULL, false, 0);
    if (!t) return;

    sched_add(t);
}
