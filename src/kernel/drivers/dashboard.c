/*
 * dashboard.c - Live Kernel Dashboard
 *
 * Provides a real-time dashboard with CPU, memory, and process information.
 *
 * Author: u/ApparentlyPlus, ChatGPT Codex 5.4 (look dude, there was no way I was writing all this by hand)
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
#include <kernel/sys/power.h>
#include <kernel/debug.h>
#include <arch/x86_64/cpu/cpu.h>
#include <klibc/stdio.h>
#include <klibc/string.h>

static tty_t* dashTTY;
static tty_t* lastTTY;

#pragma region Layout

// layout struct
typedef struct {
    int W;
    int indent;
    int gap;
    int key_gap;
    int header_gap;
    int section_gap;
    int badge_gap;
    int pid_w;
    int thr_w;
    int mem_w;
    int state_w;
    int bar_w;
    int lkey_w;
    int rkey_w;
    int value_col;
    int state_col;
    int thr_col;
    int rss_col;
    int virt_col;
    int name_w;
    int tnw;
} layout_t;

/*
 * mk_layout - Create a layout struct based on the given width
 */
static layout_t mk_layout(int width) {
    layout_t L;

    // lord have mercy because this is gonna be a nightmare to maintain

    // base everything on width and then apply sanity caps
    L.W = width;
    L.indent = width / 120;
    if (L.indent < 2) L.indent = 2;

    // gap is the base unit for spacing between columns
    L.gap = width / 32;
    if (L.gap < 2) L.gap = 2;

    // key_gap is the gap between keys and values in the kv sections
    L.key_gap = width / 72;
    if (L.key_gap < 1) L.key_gap = 1;

    // section_gap is the number of blank lines between sections
    // badge_gap is the gap between badges in the process list
    L.section_gap = 1;
    L.badge_gap = width / 120;
    if (L.badge_gap < 2) L.badge_gap = 2;

    // column widths for the process list, with sanity caps
    L.pid_w = width / 36;
    if (L.pid_w < 4) L.pid_w = 4;

    // state and mem are wider because they have bars
    L.thr_w = width / 56;
    if (L.thr_w < 3) L.thr_w = 3;

    // mem_w is also used for the memory section bars
    L.mem_w = width / 18;
    if (L.mem_w < 8) L.mem_w = 8;

    // state_w is wider to accommodate the longer state strings
    L.state_w = width / 10;
    if (L.state_w < 14) L.state_w = 14;

    // bar_w is the width of the bars in the process list, with a sanity cap
    L.bar_w = width / 10;
    if (L.bar_w < 12) L.bar_w = 12;

    // all zeros
    L.lkey_w = 0;
    L.rkey_w = 0;
    L.value_col = 0;
    L.state_col = 0;
    L.thr_col = 0;
    L.rss_col = 0;
    L.virt_col = 0;
    L.name_w = 0;
    L.tnw = 0;

    return L;
}

#pragma region Helpers

/*
 * print_str - Print a null terminated string to the console
 */
static void print_str(console_t* c, const char* s) {
    while (*s) {
        con_putc(c, *s);
        s++;
    }
}

/*
 * clip_text - Clip a string to a given width, adding "..." if it was clipped
 */
static void clip_text(char* out, size_t n, const char* s, int width) {
    int len = kstrlen(s);

    if (!out || !n) return;

    if (width <= 0) {
        out[0] = '\0';
        return;
    }

    if (len <= width) {
        ksnprintf(out, n, "%s", s ? s : "");
        return;
    }

    if (width <= 3) {
        ksnprintf(out, n, "%.*s", width, "...");
        return;
    }

    ksnprintf(out, n, "%.*s...", width - 3, s ? s : "");
}

/*
 * print_padded - Print a string padded to a given width
 */
static void print_padded(console_t* c, const char* s, int width) {
    int len = kstrlen(s);
    print_str(c, s);

    for (int i = len; i < width; i++) {
        con_putc(c, ' ');
    }
}

/*
 * print_spaces - Print a given number of spaces
 */
static void print_spaces(console_t* c, int count) {
    for (int i = 0; i < count; i++) {
        con_putc(c, ' ');
    }
}

/*
 * set_col - Set the foreground and background color of the console
 */
static void set_col(console_t* c, uint8_t fg, uint8_t bg) {
    con_set_color(c, fg, bg);
}

/*
 * set_defer - Toggle deferred rendering for a console
 */
static void set_defer(console_t* c, bool enabled) {
    bool flags = spinlock_acquire(&c->lock);
    c->defer_render = enabled;
    spinlock_release(&c->lock, flags);
}

/*
 * pct_color - Pick a color based on a percentage, green is fine, yellow is sus, red is your problem now
 */
static uint8_t pct_color(int percent) {
    if (percent > 80) return CONSOLE_COLOR_LIGHT_RED;
    if (percent > 50) return CONSOLE_COLOR_YELLOW;
    return CONSOLE_COLOR_LIGHT_GREEN;
}

/*
 * draw_section - Draw a section header with a title and a line of dashes filling the rest
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

// enum for kv pair type, either plain text or a bar
typedef enum {
    PAIR_TEXT,
    PAIR_BAR,
} pkind_t;

// one side of a kv row
typedef struct {
    const char* key;
    pkind_t kind;
    const char* text;
    uint8_t color;
    int percent;
} ppart_t;

// precomputed column widths for a set of kv rows
typedef struct {
    int lkey_w;
    int rkey_w;
    int indent;
    int value_col;
    int lval_w;
    int bar_w;
    int scol;
} plout_t;

// two sided kv row, used to measure key widths
typedef struct {
    ppart_t left;
    ppart_t right;
} prow_t;

/*
 * mk_plout - Compute a plout_t from the widest keys across the given rows
 */
static plout_t mk_plout(const layout_t* L, const prow_t* rows, size_t count) {
    plout_t P = {0};

    for (size_t i = 0; i < count; i++) {
        const prow_t* row = &rows[i];

        if (kstrlen(row->left.key) > P.lkey_w)
            P.lkey_w = kstrlen(row->left.key);

        if (row->right.key && row->right.key[0]) {
            if (kstrlen(row->right.key) > P.rkey_w)
                P.rkey_w = kstrlen(row->right.key);
        }
    }

    return P;
}

/*
 * apply_pgrid - Apply the global layout grid to a plout_t, call after apply_lgrid
 */
static void apply_pgrid(plout_t* P, const layout_t* L) {
    P->indent = L->indent;
    P->lkey_w = L->lkey_w;
    P->rkey_w = L->rkey_w;
    P->value_col = L->value_col;
    P->scol = L->rss_col;
    P->lval_w = L->rss_col - (L->gap * 2) - L->value_col;
    if (P->lval_w < 8) P->lval_w = 8;
    P->bar_w = L->bar_w;
    if (P->bar_w > P->lval_w - 7) P->bar_w = P->lval_w - 7;
    if (P->bar_w < 3) P->bar_w = 3;
}

/*
 * draw_ppart - Draw one side of a kv row, key then separator then text or a bar
 */
static void draw_ppart(console_t* c, const plout_t* P, const ppart_t* part, int key_w, int value_w, bool indent) {
    int cur;

    if (indent) print_spaces(c, P->indent);

    set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    print_padded(c, part->key, key_w);
    print_str(c, " : ");

    if (indent) {
        cur = P->indent + key_w + 3;
        if (P->value_col > cur)
            print_spaces(c, P->value_col - cur);
    }

    if (part->kind == PAIR_BAR) {
        char tmp[16];
        int filled = (part->percent * P->bar_w) / 100;
        int pad = value_w - (P->bar_w + 7); // negative pad is fine, we just dont print

        set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
        con_putc(c, '[');

        set_col(c, pct_color(part->percent), CONSOLE_COLOR_BLACK);
        for (int i = 0; i < P->bar_w; i++) {
            con_putc(c, (i < filled) ? '#' : '.');
        }
        set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
        print_str(c, "] ");

        set_col(c, pct_color(part->percent), CONSOLE_COLOR_BLACK);
        ksnprintf(tmp, sizeof(tmp), "%3d%%", part->percent);
        print_str(c, tmp);

        if (value_w > 0) {
            for (int i = 0; i < pad; i++) {
                con_putc(c, ' ');
            }
        }
        return;
    }

    set_col(c, part->color, CONSOLE_COLOR_BLACK);
    if (value_w > 0) {
        print_padded(c, part->text, value_w);
    } else {
        print_str(c, part->text);
    }
}

/*
 * draw_pcol - Draw a left & right kv row, properly column aligned
 */
static void draw_pcol(console_t* c, const plout_t* P, const ppart_t* left, const ppart_t* right) {
    draw_ppart(c, P, left, P->lkey_w, P->lval_w, true);

    if (right && right->key[0]) {
        int cur = P->value_col + P->lval_w;
        int pad = P->scol - cur;
        print_spaces(c, pad);
        draw_ppart(c, P, right, P->rkey_w, 0, false);
    }

    con_putc(c, '\n');
}

/*
 * draw_kv - Draw a simple "key : value" line, key in dark gray
 */
static void draw_kv(console_t* c, int key_w, const char* key, const char* val) {
    set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    print_str(c, "  ");
    print_padded(c, key, key_w);
    print_str(c, " : ");
    set_col(c, CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    print_str(c, val);
    con_putc(c, '\n');
}

/*
 * draw_kvpair - Draw two kv pairs side by side, both values white
 */
static void draw_kvpair(console_t* c, const plout_t* P, const char* k1, const char* v1, const char* k2, const char* v2) {
    ppart_t left = { k1, PAIR_TEXT, v1, CONSOLE_COLOR_WHITE, 0 };
    ppart_t right = { k2, PAIR_TEXT, v2, CONSOLE_COLOR_WHITE, 0 };
    draw_pcol(c, P, &left, &right);
}

/*
 * draw_kvpcol - Draw two kv pairs side by side with custom value colors
 */
static void draw_kvpcol(console_t* c, const plout_t* P, const char* k1, const char* v1, uint8_t c1, const char* k2, const char* v2, uint8_t c2) {
    ppart_t left = { k1, PAIR_TEXT, v1, c1, 0 };
    ppart_t right = { k2, PAIR_TEXT, v2, c2, 0 };
    draw_pcol(c, P, &left, &right);
}

/*
 * draw_bar - Draw a standalone progress bar
 */
static void draw_bar(console_t* c, const plout_t* P, const char* label, int percent) {
    ppart_t left = { label, PAIR_BAR, NULL, 0, percent };
    draw_pcol(c, P, &left, NULL);
    set_col(c, CONSOLE_COLOR_LIGHT_GRAY, CONSOLE_COLOR_BLACK);
}

/*
 * draw_barpair - Draw a progress bar on the left and a text value on the right
 */
static void draw_barpair(console_t* c, const plout_t* P, const char* k1, int percent, const char* k2, const char* v2, uint8_t c2) {
    ppart_t left = { k1, PAIR_BAR, NULL, 0, percent };
    ppart_t right = { k2, PAIR_TEXT, v2, c2, 0 };
    draw_pcol(c, P, &left, &right);
    set_col(c, CONSOLE_COLOR_LIGHT_GRAY, CONSOLE_COLOR_BLACK);
}

// forward decl
static char* fmt_size(char* out, size_t n, uint64_t bytes);

// cpu section data
typedef struct {
    const cpu_info_t* info;
    char cores[32];
    char watts[16];
    char usage[16];
    int cpu_pct;
    plout_t pl;
} cpu_sec_t;

// mem section data
typedef struct {
    char phys_total[24], phys_used[24], phys_free[24], phys_frag[24], phys_mmio[24];
    char heap_arena[24], heap_used[24], heap_free[24], heap_overhead[24];
    int phys_used_pct;
    int phys_frag_pct;
    int heap_pct;
    plout_t phys_pl;
    plout_t heap_pl;
} mem_sec_t;

/*
 * mk_cpu - Gather CPU info and preformat everything we'll need to render it
 */
static cpu_sec_t mk_cpu(const layout_t* L) {
    cpu_sec_t S = {0};
    prow_t rows[3];
    static uint64_t s_idle_prev = 0, s_total_prev = 0;
    uint64_t idle_now, total_now;

    S.info = cpu_get_info();
    ksnprintf(S.cores, sizeof(S.cores), "%u", S.info->core_count);

    sched_cpu_usage(&idle_now, &total_now);
    uint64_t d_idle = idle_now - s_idle_prev;
    uint64_t d_total = total_now - s_total_prev;
    s_idle_prev = idle_now;
    s_total_prev = total_now;
    S.cpu_pct = (d_total > 0) ? (int)((d_total - d_idle) * 100 / d_total) : 0;
    if (S.cpu_pct > 100) S.cpu_pct = 100;

    // critical for pwr
    uint32_t wx10 = power_avg_watts();
    if (power_rapl_available())
        ksnprintf(S.watts, sizeof(S.watts), "%u.%u W", wx10 / 10, wx10 % 10);
    else
        ksnprintf(S.watts, sizeof(S.watts), "N/A");

    ksnprintf(S.usage, sizeof(S.usage), "%d%%", S.cpu_pct);

    rows[0] = (prow_t){
        { "Vendor", PAIR_TEXT, S.info->vendor, CONSOLE_COLOR_WHITE, 0 },
        { "Cores", PAIR_TEXT, S.cores, CONSOLE_COLOR_WHITE, 0 },
    };
    rows[1] = (prow_t){
        { "Brand", PAIR_TEXT, S.info->brand, CONSOLE_COLOR_WHITE, 0 },
        { "Power", PAIR_TEXT, S.watts, CONSOLE_COLOR_CYAN, 0 },
    };
    rows[2] = (prow_t){
        { "Usage", PAIR_TEXT, S.usage, pct_color(S.cpu_pct), 0 },
        { "", PAIR_TEXT, "", CONSOLE_COLOR_BLACK, 0 },
    };

    S.pl = mk_plout(L, rows, 3);
    return S;
}

/*
 * apply_lgrid - Unify column positions across all sections
 * Author's Note: this is a nightmare function, godspeed Codex
 */
static void apply_lgrid(layout_t* L, const cpu_sec_t* cpu, const mem_sec_t* mem) {
    int lkey_w = cpu->pl.lkey_w;
    int rkey_w = cpu->pl.rkey_w;
    int right_margin = L->indent;
    int min_first_col;
    int desired_second;

    if (mem->phys_pl.lkey_w > lkey_w) lkey_w = mem->phys_pl.lkey_w;
    if (mem->heap_pl.lkey_w > lkey_w) lkey_w = mem->heap_pl.lkey_w;
    if (kstrlen("Features") > lkey_w) lkey_w = kstrlen("Features");

    if (mem->phys_pl.rkey_w > rkey_w) rkey_w = mem->phys_pl.rkey_w;
    if (mem->heap_pl.rkey_w > rkey_w) rkey_w = mem->heap_pl.rkey_w;

    L->lkey_w = lkey_w;
    L->rkey_w = rkey_w;

    L->value_col = L->indent + L->lkey_w + 3 + L->key_gap;

    desired_second = (L->W * 3) / 5;
    min_first_col = L->bar_w + 7 + (L->gap * 2);
    if (min_first_col < L->W / 4) min_first_col = L->W / 4;
    if (desired_second < L->value_col + min_first_col)
        desired_second = L->value_col + min_first_col;

    L->virt_col = L->W - right_margin - L->mem_w;
    L->rss_col = desired_second;
    if (L->rss_col > L->virt_col - L->gap - L->mem_w)
        L->rss_col = L->virt_col - L->gap - L->mem_w;

    L->state_col = L->rss_col - L->gap - L->state_w;
    L->thr_col = L->state_col - L->gap - L->thr_w;

    if (L->state_col < L->value_col + L->W / 6)
        L->state_col = L->value_col + L->W / 6;
    if (L->thr_col < L->value_col + L->W / 8)
        L->thr_col = L->value_col + L->W / 8;

    if (L->thr_col > L->state_col - L->gap - L->thr_w)
        L->thr_col = L->state_col - L->gap - L->thr_w;

    if (L->rss_col <= L->state_col + L->state_w + L->gap)
        L->rss_col = L->state_col + L->state_w + L->gap;

    if (L->virt_col <= L->rss_col + L->mem_w + L->gap)
        L->virt_col = L->rss_col + L->mem_w + L->gap;

    if (L->virt_col > L->W - right_margin - L->mem_w)
        L->virt_col = L->W - right_margin - L->mem_w;

    if (L->rss_col > L->virt_col - L->gap - L->mem_w)
        L->rss_col = L->virt_col - L->gap - L->mem_w;

    if (L->state_col > L->rss_col - L->gap - L->state_w)
        L->state_col = L->rss_col - L->gap - L->state_w;

    if (L->thr_col > L->state_col - L->gap - L->thr_w)
        L->thr_col = L->state_col - L->gap - L->thr_w;

    L->name_w = L->thr_col - L->value_col - L->gap;
    if (L->name_w < 12) {
        L->name_w = 12;
        L->thr_col = L->value_col + L->name_w + L->gap;
        L->state_col = L->thr_col + L->thr_w + L->gap;
        L->rss_col = L->state_col + L->state_w + L->gap;
    }

    if (L->virt_col + L->mem_w > L->W - right_margin) {
        L->virt_col = L->W - right_margin - L->mem_w;
        L->rss_col = L->virt_col - L->gap - L->mem_w;
        L->state_col = L->rss_col - L->gap - L->state_w;
        L->thr_col = L->state_col - L->thr_w - L->gap;
        L->name_w = L->thr_col - L->value_col - L->gap;
    }

    L->tnw = L->name_w - (4 + (L->thr_w + 1) + L->gap);
    if (L->tnw < 8) L->tnw = 8;

    // this looks like the matrix
}

/*
 * mk_mem - Gather memory stats and preformat everything we'll need to render both sections
 */
static mem_sec_t mk_mem(const layout_t* L) {
    mem_sec_t S = {0};
    prow_t phys_rows[3];
    prow_t heap_rows[3];
    pmm_stats_t ps;
    uint64_t min_blk;
    uint64_t phys_total;
    uint64_t phys_free = 0;
    uint64_t largest = 0;
    size_t ht, hu, hf, ho;

    pmm_get_stats(&ps);
    min_blk = pmm_min_block_size();
    phys_total = pmm_managed_size();

    for (int i = PMM_MAX_ORDERS - 1; i >= 0; i--) {
        uint64_t bsz = min_blk << (uint32_t)i;
        phys_free += ps.free_blocks[i] * bsz;
        if (!largest && ps.free_blocks[i]) largest = bsz;
    }

    uint64_t phys_used = phys_total - phys_free;
    uint64_t frag = phys_free ? (100 - (largest * 100 / phys_free)) : 0; // fun fact: a fully defragged buddy allocator can still show 1% here
    S.phys_used_pct = phys_total ? (int)(phys_used * 100 / phys_total) : 0;
    S.phys_frag_pct = (int)frag;

    fmt_size(S.phys_total, sizeof(S.phys_total), phys_total);
    fmt_size(S.phys_used, sizeof(S.phys_used), phys_used);
    fmt_size(S.phys_free, sizeof(S.phys_free), phys_free);
    fmt_size(S.phys_mmio, sizeof(S.phys_mmio), vmm_mmio_total());
    ksnprintf(S.phys_frag, sizeof(S.phys_frag), "%lu%%", frag);

    phys_rows[0] = (prow_t){
        { "Total", PAIR_TEXT, S.phys_total, CONSOLE_COLOR_WHITE, 0 },
        { "Used", PAIR_TEXT, S.phys_used, pct_color(S.phys_used_pct), 0 },
    };
    phys_rows[1] = (prow_t){
        { "Free", PAIR_TEXT, S.phys_free, CONSOLE_COLOR_WHITE, 0 },
        { "Frag", PAIR_TEXT, S.phys_frag, pct_color((int)frag), 0 },
    };
    phys_rows[2] = (prow_t){
        { "Usage", PAIR_BAR, NULL, CONSOLE_COLOR_WHITE, S.phys_used_pct },
        { "MMIO", PAIR_TEXT, S.phys_mmio, CONSOLE_COLOR_CYAN, 0 },
    };
    S.phys_pl = mk_plout(L, phys_rows, 3);

    heap_stats(heap_kernel_get(), &ht, &hu, &hf, &ho);
    S.heap_pct = ht ? (int)(hu * 100 / ht) : 0;

    fmt_size(S.heap_arena, sizeof(S.heap_arena), ht);
    fmt_size(S.heap_used, sizeof(S.heap_used), hu);
    fmt_size(S.heap_free, sizeof(S.heap_free), hf);
    ksnprintf(S.heap_overhead, sizeof(S.heap_overhead), "%lu B", (uint64_t)ho);

    heap_rows[0] = (prow_t){
        { "Arena", PAIR_TEXT, S.heap_arena, CONSOLE_COLOR_WHITE, 0 },
        { "In use", PAIR_TEXT, S.heap_used,  pct_color(S.heap_pct), 0 },
    };
    heap_rows[1] = (prow_t){
        { "Free", PAIR_TEXT, S.heap_free, CONSOLE_COLOR_WHITE, 0 },
        { "Overhead", PAIR_TEXT, S.heap_overhead, CONSOLE_COLOR_WHITE, 0 },
    };
    heap_rows[2] = (prow_t){
        { "Pressure", PAIR_BAR, NULL, CONSOLE_COLOR_WHITE, S.heap_pct },
        { "", PAIR_TEXT, "", CONSOLE_COLOR_BLACK, 0 },
    };
    S.heap_pl = mk_plout(L, heap_rows, 3);

    return S;
}

/*
 * fmt_size - Format a byte count into a human readable string, because nobody wants to read "134217728"
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
 * render_cpu - Render CPU info and features
 */
static void render_cpu(console_t* c, const layout_t* L, const cpu_sec_t* S) {
    int cur;

    draw_section(c, L, "CPU");
    for (int i = 0; i < L->header_gap; i++) con_putc(c, '\n');

    draw_kvpair(c, &S->pl, "Vendor", S->info->vendor, "Cores", S->cores);
    draw_kvpcol(c, &S->pl, "Brand", S->info->brand, CONSOLE_COLOR_WHITE, "Power", S->watts, CONSOLE_COLOR_CYAN);
    draw_kvpcol(c, &S->pl, "Usage", S->usage, pct_color(S->cpu_pct), "", "", CONSOLE_COLOR_BLACK);

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

    set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    print_spaces(c, S->pl.indent);
    print_padded(c, "Features", S->pl.lkey_w);
    print_str(c, " : ");
    cur = S->pl.indent + S->pl.lkey_w + 3;
    if (S->pl.value_col > cur)
        print_spaces(c, S->pl.value_col - cur);

    // green means supported, gray means your cpu skipped leg day
    for (size_t i = 0; i < sizeof(feats)/sizeof(feats[0]); i++) {
        bool on = (S->info->features & feats[i].f) != 0;
        set_col(c, on ? CONSOLE_COLOR_LIGHT_GREEN : CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
        print_str(c, feats[i].n);
        print_str(c, " ");
    }
    con_putc(c, '\n');
}

#pragma region Memory

/*
 * render_mem - Render physical memory stats and kernel heap stats
 */
static void render_mem(console_t* c, const layout_t* L, const mem_sec_t* S) {
    draw_section(c, L, "PHYSICAL MEMORY");
    for (int i = 0; i < L->header_gap; i++) con_putc(c, '\n');

    draw_kvpcol(c, &S->phys_pl, "Total", S->phys_total, CONSOLE_COLOR_WHITE, "Used", S->phys_used, pct_color(S->phys_used_pct));
    draw_kvpcol(c, &S->phys_pl, "Free", S->phys_free, CONSOLE_COLOR_WHITE, "Frag", S->phys_frag, pct_color(S->phys_frag_pct));
    draw_barpair(c, &S->phys_pl, "Usage", S->phys_used_pct, "MMIO", S->phys_mmio, CONSOLE_COLOR_CYAN);

    for (int i = 0; i < L->section_gap; i++) con_putc(c, '\n');
    draw_section(c, L, "KERNEL HEAP");
    for (int i = 0; i < L->header_gap; i++) con_putc(c, '\n');

    draw_kvpcol(c, &S->heap_pl, "Arena", S->heap_arena, CONSOLE_COLOR_WHITE, "In use", S->heap_used, pct_color(S->heap_pct));
    draw_kvpcol(c, &S->heap_pl, "Free", S->heap_free, CONSOLE_COLOR_WHITE, "Overhead", S->heap_overhead, CONSOLE_COLOR_WHITE);

    draw_bar(c, &S->heap_pl, "Pressure", S->heap_pct);
}

#pragma region Processes

/*
 * state_str - Turn a thread state into something a human can read
 */
static const char* state_str(thread_state_t s) {
    switch (s) {
        case T_READY:    return "READY";
        case T_RUNNING:  return "RUNNING";
        case T_BLOCKED:  return "BLOCKED";
        case T_SLEEPING: return "SLEEPING";
        case T_DEAD:     return "DEAD";
        default:         return "?";
    }
}

/*
 * state_color - Pick a color for a thread state, dead threads get gray because that's what they deserve
 */
static uint8_t state_color(thread_state_t s) {
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
 * fmt_tdetail - Format the state detail string for a thread row
 */
static void fmt_tdetail(char* out, size_t n, const thread_t* t) {
    if (t->state == T_SLEEPING) {
        ksnprintf(out, n, "SLEEPING");
        return;
    }

    ksnprintf(out, n, "%s", state_str(t->state));
}

/*
 * proc_rows - Count how many rows the process table will need, one per process plus one per thread
 */
static int proc_rows(void) {
    int rows = 0;

    for (process_t* proc = process_get_all(); proc; proc = proc->next) {
        rows++;
        for (thread_t* t = proc->threads; t; t = t->next) rows++;
    }

    return rows;
}

/*
 * fixed_rows - How many non-process rows the dashboard occupies
 */
static int fixed_rows(const layout_t* L) {
    return 15 + (L->header_gap * 3) + (L->section_gap * 3);
}

/*
 * condense_for_h - Squish spacing when the terminal is too short to fit everything
 */
static void condense_for_h(layout_t* L, int height) {
    int safety_rows = 1;
    int max_rows = height - 2;

    // if it fits, it sits
    if (fixed_rows(L) + proc_rows() + safety_rows <= max_rows)
        return;

    L->header_gap = 0;
    L->section_gap = 1;
}

/*
 * render_procs - Scaled process and thread table
 */
static void render_procs(console_t* c, const layout_t* L) {
    char mem[16], virt_s[16], detail[48], tid_buf[16], name_buf[128];
    int col;
    int tid_w = L->thr_w + 1;
    int thread_tree_w = 4;
    int name_col = L->value_col + thread_tree_w + tid_w + L->gap;
    int name_w = L->tnw;

    draw_section(c, L, "PROCESSES");
    for (int i = 0; i < L->header_gap; i++) con_putc(c, '\n');

    set_col(c, CONSOLE_COLOR_LIGHT_CYAN, CONSOLE_COLOR_BLACK);
    col = 0;
    print_spaces(c, L->indent); col += L->indent;
    print_padded(c, "PID", L->pid_w); col += L->pid_w;
    if (L->value_col > col) { print_spaces(c, L->value_col - col); col = L->value_col; }
    print_padded(c, "NAME", L->name_w); col += L->name_w;
    if (L->thr_col > col) { print_spaces(c, L->thr_col - col); col = L->thr_col; }
    print_padded(c, "THR", L->thr_w); col += L->thr_w;
    if (L->state_col > col) { print_spaces(c, L->state_col - col); col = L->state_col; }
    print_padded(c, "STATE", L->state_w); col += L->state_w;
    if (L->rss_col > col) { print_spaces(c, L->rss_col - col); col = L->rss_col; }
    print_padded(c, "RSS", L->mem_w); col += L->mem_w;
    if (L->virt_col > col) { print_spaces(c, L->virt_col - col); col = L->virt_col; }
    print_padded(c, "VIRT", L->mem_w);
    con_putc(c, '\n');

    process_t* proc = process_get_all();
    while (proc) {
        if (c->cy >= c->height - 2) break; // out of screen, not out of processes

        int nth = 0;
        for (thread_t* t = proc->threads; t; t = t->next) nth++;

        size_t vt = 0, res = 0;
        if (proc->vmm) vmm_stats(proc->vmm, &vt, &res);

        col = 0;
        set_col(c, CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_BLACK);
        print_spaces(c, L->indent); col += L->indent;
        ksnprintf(tid_buf, sizeof(tid_buf), "%u", proc->pid);
        print_padded(c, tid_buf, L->pid_w); col += L->pid_w;

        set_col(c, CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
        if (L->value_col > col) { print_spaces(c, L->value_col - col); col = L->value_col; }
        print_padded(c, proc->name, L->name_w); col += L->name_w;
        if (L->thr_col > col) { print_spaces(c, L->thr_col - col); col = L->thr_col; }
        ksnprintf(tid_buf, sizeof(tid_buf), "%d", nth);
        print_padded(c, tid_buf, L->thr_w); col += L->thr_w;
        if (L->state_col > col) { print_spaces(c, L->state_col - col); col = L->state_col; }
        print_padded(c, "", L->state_w); col += L->state_w;
        if (L->rss_col > col) { print_spaces(c, L->rss_col - col); col = L->rss_col; }
        print_padded(c, fmt_size(mem, sizeof(mem), (uint64_t)res), L->mem_w); col += L->mem_w;
        if (L->virt_col > col) { print_spaces(c, L->virt_col - col); col = L->virt_col; }
        print_padded(c, fmt_size(virt_s, sizeof(virt_s), (uint64_t)vt), L->mem_w);
        con_putc(c, '\n');

        for (thread_t* t = proc->threads; t; t = t->next) {
            int tid_len;

            if (c->cy >= c->height - 2) break;

            fmt_tdetail(detail, sizeof(detail), t);

            set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
            print_spaces(c, L->value_col);
            print_str(c, "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 ");

            ksnprintf(tid_buf, sizeof(tid_buf), "%u", t->tid);
            tid_len = kstrlen(tid_buf);
            print_str(c, tid_buf);
            con_putc(c, ' ');

            clip_text(name_buf, sizeof(name_buf), t->name, name_w);
            set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
            print_str(c, name_buf);

            print_spaces(c, L->gap);

            for (int i = tid_len + 1; i < tid_w; i++) {
                con_putc(c, ' ');
            }

            col = name_col + kstrlen(name_buf);
            if (L->state_col > col) print_spaces(c, L->state_col - col);

            set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
            print_str(c, "[");
            set_col(c, state_color(t->state), CONSOLE_COLOR_BLACK);
            print_str(c, detail);
            set_col(c, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
            print_str(c, "]\n");
        }

        proc = proc->next;
    }
}

#pragma region Main Draw

/*
 * dash_draw - Render the full dashboard, build, measure, draw, done
 */
static void dash_draw(void) {
    console_t* con = dashTTY->console;
    uint64_t ms = get_uptime_ms();

    // kinda verbose but easier to read than clever math
    uint64_t totalSec = ms / 1000;
    uint64_t mins = totalSec / 60;
    uint64_t hrs = mins / 60;

    totalSec %= 60;
    mins %= 60;

    char header[80];

    ksnprintf(header, sizeof(header), "  GatOS Kernel Dashboard  |  Uptime: %02lu:%02lu:%02lu", hrs, mins, totalSec);

    set_defer(con, true);
    con_header_write(con, 0, header, CONSOLE_COLOR_WHITE, CONSOLE_COLOR_MAGENTA);

    con_clear(con, CONSOLE_COLOR_BLACK);
    set_col(con, CONSOLE_COLOR_LIGHT_GRAY, CONSOLE_COLOR_BLACK);

    layout_t L = mk_layout((int)con->width);
    condense_for_h(&L, (int)con->height);
    cpu_sec_t cpu = mk_cpu(&L);
    mem_sec_t mem = mk_mem(&L);

    // unify column positions across all sections, then bake them in
    apply_lgrid(&L, &cpu, &mem);
    apply_pgrid(&cpu.pl, &L);
    apply_pgrid(&mem.phys_pl, &L);
    apply_pgrid(&mem.heap_pl, &L);

    render_cpu(con, &L, &cpu);
    for (int i = 0; i < L.section_gap; i++) con_putc(con, '\n');

    render_mem(con, &L, &mem);
    for (int i = 0; i < L.section_gap; i++) con_putc(con, '\n');

    render_procs(con, &L);

    // pad remaining rows to clear artifacts and anchor bottom text
    set_col(con, CONSOLE_COLOR_BLACK, CONSOLE_COLOR_BLACK);
    while (con->cy < con->height - 1) con_putc(con, '\n');

    set_col(con, CONSOLE_COLOR_DARK_GRAY, CONSOLE_COLOR_BLACK);
    print_str(con, " CTRL+SHIFT+ESC to close or ALT+TAB to cycle");

    con_refresh(con);
    set_defer(con, false);
}

#pragma region Thread

/*
 * dash_thread - Dashboard thread, redraws every second when visible and idles otherwise
 */
static void dash_thread(void* arg) {
    (void)arg;

    while (1) {
        if (active_tty == dashTTY) {
            dash_draw();
            // refresh every second while active, otherwise idle to save resources
            sleep_ms(1000);
        } else {
            sleep_ms(100); // idle-ish
        }
    }
}

#pragma region API

/*
 * dash_active - Returns true if the dashboard TTY is currently in the foreground
 */
bool dash_active(void) {
    return active_tty == dashTTY;
}

/*
 * dash_toggle - Toggle the dashboard on or off, restoring the previous TTY on close
 */
void dash_toggle(void) {
    if (active_tty == dashTTY) {
        tty_switch(lastTTY ? lastTTY : kernel_tty);
        if (active_tty == dashTTY && kernel_tty) {
            tty_switch(kernel_tty);
        }
        lastTTY = NULL;
    } else {
        lastTTY = active_tty;
        tty_switch(dashTTY);
    }
}

/*
 * dash_init - Create the dashboard TTY, process, and thread, if anything fails we just log and bail
 */
void dash_init(void) {
    dashTTY = tty_create();
    if (!dashTTY) {
        LOGF("Failed to create dashboard TTY\n");
        return;
    }

    dashTTY->hidden = true;

    con_header_init(dashTTY->console, 1);
    con_header_write(dashTTY->console, 0, "  GatOS Kernel Dashboard", CONSOLE_COLOR_WHITE, CONSOLE_COLOR_MAGENTA);

    con_enable_cursor(dashTTY->console, false);

    process_t* p = process_create("dashboard", dashTTY);
    if (!p) { LOGF("Failed to create dashboard process\n"); return; }

    thread_t* t = thread_create(p, "dash", dash_thread, NULL, false, 0);
    if (!t) { LOGF("Failed to create dashboard thread\n"); return; }

    sched_add(t);
}
