/*
 * slab.c - Slab Allocator Implementation
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/memory/paging.h>
#include <arch/x86_64/memory/layout.h>
#include <kernel/sys/spinlock.h>
#include <kernel/memory/slab.h>
#include <kernel/memory/pmm.h>
#include <kernel/debug.h>
#include <klibc/string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Magic numbers / constants
#define SLAB_MAGIC 0xC00151AB
#define SLAB_CACHE_MAGIC 0xCACE51AB
#define SLAB_FREE_MAGIC 0xFEEDF00D
#define SLAB_ALLOC_MAGIC 0xA110C8ED
#define SLAB_RED_ZONE 0xDEADFA11

// Objects larger than this should use PMM directly
#define SLAB_MAX_OBJ_SIZE (PAGE_SIZE / 8)

// Minimum object size must fit the freelist header we use when free.
#define SLAB_MIN_OBJ_SIZE (sizeof(slab_free_obj_t))

// Which list a slab is currently on
#define SLAB_LIST_NONE    0
#define SLAB_LIST_EMPTY   1
#define SLAB_LIST_PARTIAL 2
#define SLAB_LIST_FULL    3

// Slab metadata structures
typedef struct slab {
    uint32_t magic;
    uint32_t in_use;
    uint32_t capacity;
    uint32_t obj_size;
    void* freelist;
    struct slab* next;
    struct slab* prev;
    slab_cache_t* cache;
    uint64_t slab_phys;
    uint8_t list_id; // tracks which list this slab is on
} slab_t;

// free object header embedded inside the object when it's free
typedef struct slab_free_obj {
    uint32_t magic;
    uint32_t red_zone_pre;
    struct slab_free_obj* next;
    uint32_t red_zone_post;
} slab_free_obj_t;

// header stored before the user's pointer when object is allocated
typedef struct {
    uint32_t magic;
    uint32_t cache_id;
} slab_alloc_header_t;

// slab cache structure - minor reformatting but same fields
struct slab_cache {
    uint32_t magic;
    char name[SLAB_CACHE_NAME_LEN];
    size_t obj_size;
    size_t user_size;
    size_t align;

    uint32_t cache_id;

    slab_t* slabs_empty;
    slab_t* slabs_partial;
    slab_t* slabs_full;

    spinlock_t lock;

    cache_stats_t stats;
    slab_cache_t* next;
};

static bool slab_inited = false;
static slab_cache_t* caches = NULL;

// Cache structs live in BSS, a full page each from the PMM would be a waste
static slab_cache_t cache_pool[SLAB_MAX_CACHES];
static uint32_t next_cache_id = 1;
static slab_stats_t stats;

static spinlock_t slab_list_lock;

// Forward declarations
static slab_cache_t* cache_find_locked(const char* name);

#pragma region Validation Helpers

/*
 * slab_validate - Validate slab structure (does not panic; returns bool)
 */
static inline bool slab_validate(slab_t* slab) {
    if (!slab) return false;

    uint32_t m = slab->magic;
    if (m != SLAB_MAGIC) {
        LOGF("[SLAB ERROR] Invalid slab magic: 0x%x (expected 0x%x)\n", m,
             SLAB_MAGIC);
        stats.corruption_detected++;
        return false;
    }

    if (slab->in_use > slab->capacity) {
        LOGF("[SLAB ERROR] Slab in_use (%u) > capacity (%u)\n", slab->in_use,
             slab->capacity);
        stats.corruption_detected++;
        return false;
    }

    return true;
}

static inline bool cache_validate(slab_cache_t* cache) {
    if (!cache) return false;

    if (cache->magic != SLAB_CACHE_MAGIC) {
        LOGF("[SLAB ERROR] Invalid cache magic: 0x%x (expected 0x%x)\n",
             cache->magic, SLAB_CACHE_MAGIC);
        stats.corruption_detected++;
        return false;
    }

    return true;
}

/*
 * validate_free_obj - Validate freelist object header
 */
static inline bool validate_free_obj(slab_free_obj_t* obj) {
    if (!obj) return false;

    if (obj->magic != SLAB_FREE_MAGIC) {
        LOGF("[SLAB ERROR] Invalid free object magic: 0x%x\n", obj->magic);
        stats.corruption_detected++;
        return false;
    }
    if (obj->red_zone_pre != SLAB_RED_ZONE) {
        LOGF("[SLAB ERROR] Free object pre-red-zone corrupted: 0x%x\n",
             obj->red_zone_pre);
        stats.corruption_detected++;
        return false;
    }
    if (obj->red_zone_post != SLAB_RED_ZONE) {
        LOGF("[SLAB ERROR] Free object post-red-zone corrupted: 0x%x\n",
             obj->red_zone_post);
        stats.corruption_detected++;
        return false;
    }

    return true;
}
 
/*
 * b2mb - Convert bytes to megabytes with 2 decimal places
 */
static uint64_t b2mb(uint64_t bytes) {
    return (bytes * 100ULL) / (1024ULL * 1024ULL);
}

/*
 * ratio_to_tenths - Convert a ratio to tenths of a percent
 */
static uint64_t ratio_to_tenths(uint64_t num, uint64_t den) {
    return den ? ((num * 1000ULL) / den) : 0;
}

#pragma endregion

#pragma region Internal Functions

static void slab_remove_from_list(slab_t** list_head, slab_t* slab) {
    if (!slab) return;

    if (slab->prev) {
        slab->prev->next = slab->next;
    } else {
        *list_head = slab->next;
    }

    if (slab->next) {
        slab->next->prev = slab->prev;
    }

    slab->next = NULL;
    slab->prev = NULL;
    slab->list_id = SLAB_LIST_NONE;
}

/*
 * slab_add_to_list - add slab to head of list (LIFO style); records list_id
 */
static void slab_add_to_list(slab_t** list_head, slab_t* slab, uint8_t list_id) {
    if (!slab) return;

    slab->list_id = list_id;
    slab->next = *list_head;
    slab->prev = NULL;

    if (*list_head) {
        (*list_head)->prev = slab;
    }

    *list_head = slab;
}

/*
 * slab_relist - Move a slab to the list its fill level says it belongs on, counters included
 */
static void slab_relist(slab_cache_t* cache, slab_t* slab) {
    slab_t** lists[] = { NULL, &cache->slabs_empty, &cache->slabs_partial, &cache->slabs_full };
    uint64_t* counts[] = { NULL, &cache->stats.empty_slabs, &cache->stats.partial_slabs, &cache->stats.full_slabs };

    uint8_t to = slab->in_use == 0 ? SLAB_LIST_EMPTY
               : slab->in_use == slab->capacity ? SLAB_LIST_FULL
               : SLAB_LIST_PARTIAL;
    uint8_t from = slab->list_id;
    if (to == from) return;

    if (from != SLAB_LIST_NONE) {
        slab_remove_from_list(lists[from], slab);
        (*counts[from])--;
    }
    slab_add_to_list(lists[to], slab, to);
    (*counts[to])++;
}

static slab_t* slab_allocate_page(slab_cache_t* cache) {
    if (!cache_validate(cache)) return NULL;

    uint64_t phys = 0;
    pmm_status_t pmm_status = pmm_alloc(PAGE_SIZE, &phys);
    if (pmm_status != PMM_OK) {
        LOGF("[SLAB] Failed to allocate page from PMM: %d\n", pmm_status);
        return NULL;
    }

    slab_t* slab = (slab_t*)PHYSMAP_P2V(phys);

    kmemset(slab, 0, PAGE_SIZE);

    slab->magic = SLAB_MAGIC;
    slab->obj_size = cache->obj_size;
    slab->cache = cache;
    slab->slab_phys = phys;
    slab->next = NULL;
    slab->prev = NULL;

    // Determine where objects start so that user pointer (after alloc header)
    // is aligned to cache->align.
    size_t metadata_size = sizeof(slab_t);
    uintptr_t base = (uintptr_t)slab;
    uintptr_t first_obj_start = base + metadata_size;
    uintptr_t first_user_ptr = first_obj_start + sizeof(slab_alloc_header_t);

    uintptr_t aligned_user_ptr = align_up(first_user_ptr, cache->align);

    metadata_size = (size_t)(aligned_user_ptr - base - sizeof(slab_alloc_header_t));

    size_t available = PAGE_SIZE - metadata_size;

    slab->capacity = (uint32_t)(available / cache->obj_size);

    if (slab->capacity == 0) {
        LOGF(
            "[SLAB ERROR] Object size %zu too large for page (metadata=%zu, "
            "avail=%zu)\n",
            cache->obj_size, metadata_size, available);
        pmm_free(phys, PAGE_SIZE);
        return NULL;
    }

    slab->in_use = 0;

    uint8_t* obj_base = (uint8_t*)slab + metadata_size;
    slab->freelist = NULL;

    for (size_t i = 0; i < slab->capacity; i++) {
        slab_free_obj_t* obj = (slab_free_obj_t*)(obj_base + i * cache->obj_size);
        obj->magic = SLAB_FREE_MAGIC;
        obj->red_zone_pre = SLAB_RED_ZONE;
        obj->red_zone_post = SLAB_RED_ZONE;
        obj->next = (slab_free_obj_t*)slab->freelist;
        slab->freelist = obj;
    }

    stats.total_slabs++;
    stats.total_pmm_bytes += PAGE_SIZE;

    cache->stats.slab_count++;

    return slab;
}

static void slab_free_page(slab_t* slab) {
    if (!slab_validate(slab)) return;

    slab_cache_t* cache = slab->cache;
    if (!cache_validate(cache)) return;

    stats.total_slabs--;
    stats.total_pmm_bytes -= PAGE_SIZE;
    cache->stats.slab_count--;

    // Clear magic to help detect dangling references
    slab->magic = 0;

    pmm_free(slab->slab_phys, PAGE_SIZE);
}

/*
 * get_slab_from_obj - Given an object pointer (user or header), find slab
 */
static slab_t* get_slab_from_obj(void* obj) {
    if (!obj) return NULL;

    uintptr_t addr = (uintptr_t)obj;

    uintptr_t slab_addr = addr & ~(PAGE_SIZE - 1);
    slab_t* slab = (slab_t*)slab_addr;

    if (!slab_validate(slab)) {
        return NULL;
    }

    return slab;
}

/*
 * slab_alloc_cache_struct - Grab a free slot in the static cache table, NULL when it's full
 */
static slab_cache_t* slab_alloc_cache_struct(void) {
    for (int i = 0; i < SLAB_MAX_CACHES; i++) {
        if (cache_pool[i].magic == SLAB_CACHE_MAGIC) continue;
        kmemset(&cache_pool[i], 0, sizeof(slab_cache_t));
        return &cache_pool[i];
    }
    return NULL;
}

#pragma endregion

#pragma region Initialization and Shutdown

slab_status_t slab_init(void) {
    spinlock_init(&slab_list_lock, "slab_list");
    bool flags = spinlock_acquire(&slab_list_lock);

    if (slab_inited) {
        spinlock_release(&slab_list_lock, flags);
        return SLAB_ERR_ALREADY_INIT;
    }

    if (!pmm_is_initialized()) {
        LOGF("[SLAB] PMM must be initialized before slab allocator\n");
        spinlock_release(&slab_list_lock, flags);
        return SLAB_ERR_NOT_INIT;
    }

    caches = NULL;
    next_cache_id = 1;
    kmemset(&stats, 0, sizeof(slab_stats_t));

    slab_inited = true;

    LOGF("[SLAB] Slab (System Wide) Allocator initialized\n");

    spinlock_release(&slab_list_lock, flags);
    return SLAB_OK;
}

/*
 * slab_shutdown - Destroy all caches and mark allocator uninitialized
 */
void slab_shutdown(void) {
    bool flags = spinlock_acquire(&slab_list_lock);
    if (!slab_inited) {
        spinlock_release(&slab_list_lock, flags);
        return;
    }

    // slab_cache_destroy acquires the list lock, so we release before calling it
    while (caches) {
        slab_cache_t* cache = caches;
        spinlock_release(&slab_list_lock, flags);
        slab_cache_destroy(cache);
        flags = spinlock_acquire(&slab_list_lock);
    }

    slab_inited = false;
    caches = NULL;
    next_cache_id = 1;
    kmemset(&stats, 0, sizeof(slab_stats_t));

    LOGF("[SLAB] Slab (System Wide) Allocator shutdown\n");
    spinlock_release(&slab_list_lock, flags);
}

/*
 * slab_is_initialized - simple accessor
 */
bool slab_is_initialized(void) { return slab_inited; }

#pragma endregion

#pragma region Cache Management

slab_cache_t* slab_cache_create(const char* name, size_t obj_size,
                                size_t align) {
    bool list_flags = spinlock_acquire(&slab_list_lock);

    if (!slab_inited) {
        LOGF("[SLAB] Allocator not initialized\n");
        spinlock_release(&slab_list_lock, list_flags);
        return NULL;
    }

    if (!name || obj_size == 0) {
        LOGF("[SLAB] Invalid arguments\n");
        spinlock_release(&slab_list_lock, list_flags);
        return NULL;
    }

    if (obj_size > SLAB_MAX_OBJ_SIZE) {
        LOGF("[SLAB] Object size %zu exceeds max %zu\n", obj_size,
             SLAB_MAX_OBJ_SIZE);
        spinlock_release(&slab_list_lock, list_flags);
        return NULL;
    }

    if (align == 0) align = 8;

    if (!is_pow2_u64(align)) {
        LOGF("[SLAB] Alignment must be power of 2\n");
        spinlock_release(&slab_list_lock, list_flags);
        return NULL;
    }

    if (cache_find_locked(name)) {
        LOGF("[SLAB] Cache '%s' already exists\n", name);
        spinlock_release(&slab_list_lock, list_flags);
        return NULL;
    }

    slab_cache_t* cache = slab_alloc_cache_struct();
    if (!cache) {
        LOGF("[SLAB] Cache table full (max %d)\n", SLAB_MAX_CACHES);
        spinlock_release(&slab_list_lock, list_flags);
        return NULL;
    }

    cache->magic = SLAB_CACHE_MAGIC;
    cache->cache_id = next_cache_id++;
    kstrncpy(cache->name, name, SLAB_CACHE_NAME_LEN - 1);
    cache->name[SLAB_CACHE_NAME_LEN - 1] = '\0';

    cache->user_size = obj_size;

    size_t total_size = obj_size + sizeof(slab_alloc_header_t);

    // Must be large enough to hold the freelist header when the object is free
    if (total_size < SLAB_MIN_OBJ_SIZE) {
        total_size = SLAB_MIN_OBJ_SIZE;
    }

    cache->obj_size = align_up(total_size, align);
    cache->align = align;

    cache->slabs_empty = NULL;
    cache->slabs_partial = NULL;
    cache->slabs_full = NULL;

    spinlock_init(&cache->lock, "slab_cache");

    kmemset(&cache->stats, 0, sizeof(cache_stats_t));

    cache->next = caches;
    caches = cache;
    stats.cache_count++;

    spinlock_release(&slab_list_lock, list_flags);
    return cache;
}

void slab_cache_destroy(slab_cache_t* cache) {
    if (!cache_validate(cache)) return;

    bool list_flags = spinlock_acquire(&slab_list_lock);
    bool cache_flags = spinlock_acquire(&cache->lock);

    slab_t* lists[] = {cache->slabs_empty, cache->slabs_partial,
                       cache->slabs_full};

    for (int i = 0; i < 3; i++) {
        slab_t* slab = lists[i];
        while (slab) {
            slab_t* next = slab->next;
            slab_free_page(slab);
            slab = next;
        }
    }

    slab_cache_t** prev = &caches;
    while (*prev) {
        if (*prev == cache) {
            *prev = cache->next;
            break;
        }
        prev = &(*prev)->next;
    }

    stats.cache_count--;

    // Clear magic before freeing to detect dangling references
    cache->magic = 0;

    spinlock_release(&cache->lock, cache_flags);
    spinlock_release(&slab_list_lock, list_flags);

}

/*
 * cache_find_locked - find a cache by name (assumes lock is held)
 */
static slab_cache_t* cache_find_locked(const char* name) {
    slab_cache_t* cache = caches;
    while (cache) {
        if (!cache_validate(cache)) return NULL;
        if (kstrncmp(cache->name, name, SLAB_CACHE_NAME_LEN) == 0) {
            return cache;
        }
        cache = cache->next;
    }
    return NULL;
}

slab_cache_t* slab_cache_find(const char* name) {
    if (!slab_inited || !name) return NULL;

    bool flags = spinlock_acquire(&slab_list_lock);
    slab_cache_t* cache = cache_find_locked(name);
    spinlock_release(&slab_list_lock, flags);

    return cache;
}

#pragma endregion

#pragma region Allocation and Deallocation

/*
 * slab_alloc - Allocate an object from a cache
 */
slab_status_t slab_alloc(slab_cache_t* cache, void** out_obj) {
    if (!out_obj) return SLAB_ERR_INVALID;
    *out_obj = NULL;

    if (!cache_validate(cache)) return SLAB_ERR_INVALID;

    bool flags = spinlock_acquire(&cache->lock);

    slab_t* slab = NULL;

    // Prefer partial slabs for better locality
    // If nada, try empty slabs to possibly avoid fragmentation
    if (cache->slabs_partial) {
        slab = cache->slabs_partial;
    } else if (cache->slabs_empty) {
        slab = cache->slabs_empty;
    } else {
        slab = slab_allocate_page(cache);
        if (!slab) {
            spinlock_release(&cache->lock, flags);
            return SLAB_ERR_NO_MEMORY;
        }
    }

    if (!slab_validate(slab)) {
        spinlock_release(&cache->lock, flags);
        return SLAB_ERR_CORRUPTION;
    }

    if (!slab->freelist) {
        LOGF("[SLAB ERROR] Slab has no free objects but in_use=%u capacity=%u\n",
            slab->in_use, slab->capacity);
        spinlock_release(&cache->lock, flags);
        return SLAB_ERR_CORRUPTION;
    }

    slab_free_obj_t* obj = (slab_free_obj_t*)slab->freelist;
    if (!validate_free_obj(obj)) {
        LOGF("[SLAB ERROR] Corrupted free object in cache '%s'\n", cache->name);
        spinlock_release(&cache->lock, flags);
        return SLAB_ERR_CORRUPTION;
    }

    slab->freelist = obj->next;
    slab->in_use++;

    kmemset(obj, 0, cache->obj_size);

    slab_alloc_header_t* header = (slab_alloc_header_t*)obj;
    header->magic = SLAB_ALLOC_MAGIC;
    header->cache_id = cache->cache_id;

    cache->stats.total_allocs++;
    cache->stats.active_objects++;

    slab_relist(cache, slab);

    *out_obj = (void*)((uint8_t*)obj + sizeof(slab_alloc_header_t));
    
    spinlock_release(&cache->lock, flags);
    return SLAB_OK;
}

/*
 * slab_free - Free an object back to its cache
 */
slab_status_t slab_free(slab_cache_t* cache, void* obj) {
    if (!cache_validate(cache) || !obj) {
        return SLAB_ERR_INVALID;
    }

    bool flags = spinlock_acquire(&cache->lock);

    void* obj_start = (void*)((uint8_t*)obj - sizeof(slab_alloc_header_t));

    slab_t* slab = get_slab_from_obj(obj_start);
    if (!slab_validate(slab)) {
        LOGF("[SLAB ERROR] Object %p does not belong to a valid slab\n", obj);
        spinlock_release(&cache->lock, flags);
        return SLAB_ERR_NOT_FOUND;
    }

    if (slab->cache != cache) {
        LOGF("[SLAB ERROR] Object belongs to different cache\n");
        spinlock_release(&cache->lock, flags);
        return SLAB_ERR_NOT_FOUND;
    }

    slab_alloc_header_t* header = (slab_alloc_header_t*)obj_start;
    if (header->magic != SLAB_ALLOC_MAGIC) {
        LOGF(
            "[SLAB ERROR] Invalid allocation magic (double-free or "
            "corruption)\n");
        stats.corruption_detected++;
        spinlock_release(&cache->lock, flags);
        return SLAB_ERR_CORRUPTION;
    }
    if (header->cache_id != cache->cache_id) {
        LOGF("[SLAB ERROR] Cache ID mismatch\n");
        spinlock_release(&cache->lock, flags);
        return SLAB_ERR_CORRUPTION;
    }

    // free object by adding it back to the slab's freelist
    slab_free_obj_t* free_obj = (slab_free_obj_t*)obj_start;
    free_obj->magic = SLAB_FREE_MAGIC;
    free_obj->red_zone_pre = SLAB_RED_ZONE;
    free_obj->red_zone_post = SLAB_RED_ZONE;
    free_obj->next = (slab_free_obj_t*)slab->freelist;
    slab->freelist = free_obj;

    slab->in_use--;

    cache->stats.total_frees++;
    cache->stats.active_objects--;

    // An empty slab goes back to the empty list
    slab_relist(cache, slab);
    if (slab->in_use == 0 && cache->stats.empty_slabs > 1) {
        slab_remove_from_list(&cache->slabs_empty, slab);
        cache->stats.empty_slabs--;
        slab_free_page(slab);
    }

    spinlock_release(&cache->lock, flags);
    return SLAB_OK;
}

#pragma endregion

#pragma region Statistics and Debugging

void slab_cache_stats(slab_cache_t* cache, cache_stats_t* out_stats) {
    if (!cache_validate(cache) || !out_stats) return;
    bool flags = spinlock_acquire(&cache->lock);
    *out_stats = cache->stats;
    spinlock_release(&cache->lock, flags);
}

/*
 * slab_get_stats - copy global stats out
 */
void slab_get_stats(slab_stats_t* out_stats) {
    if (!out_stats) return;
    bool flags = spinlock_acquire(&slab_list_lock);
    *out_stats = stats;
    spinlock_release(&slab_list_lock, flags);
}

/*
 * stats_locked - The allocator wide numbers (list lock held)
 */
static void stats_locked(void) {
    uint64_t mib = b2mb(stats.total_pmm_bytes);
    LOGF("Slab allocator\n");
    log_indent(1);
    LOGF("caches    %lu of %d\n", stats.cache_count, SLAB_MAX_CACHES);
    LOGF("slabs     %lu (%lu.%02lu MiB from the PMM)\n", stats.total_slabs, mib / 100ULL, mib % 100ULL);
    if (stats.corruption_detected) LOGF("errors    %lu corruptions caught\n", stats.corruption_detected);
    log_indent(-1);
}

/*
 * slab_dump_stats - print global stats to klog
 */
void slab_dump_stats(void) {
    bool flags = spinlock_acquire(&slab_list_lock);
    if (!slab_inited) {
        LOGF("[SLAB] Not initialized\n");
        spinlock_release(&slab_list_lock, flags);
        return;
    }
    stats_locked();
    spinlock_release(&slab_list_lock, flags);
}

/*
 * slab_cache_dump - print detailed info about a specific cache
 */
void slab_cache_dump(slab_cache_t* cache) {
    if (!cache_validate(cache)) return;

    bool flags = spinlock_acquire(&cache->lock);

    uint64_t total = cache->stats.slab_count * PAGE_SIZE;
    uint64_t used = cache->stats.active_objects * cache->obj_size;
    uint64_t pct = ratio_to_tenths(used, total);

    LOGF("cache %s (id %u)\n", cache->name, cache->cache_id);
    log_indent(1);
    LOGF("objects   %zu bytes, %zu with header (align %zu)\n", cache->user_size, cache->obj_size, cache->align);
    LOGF("calls     %lu allocs, %lu frees, %lu live\n",
         cache->stats.total_allocs, cache->stats.total_frees, cache->stats.active_objects);
    LOGF("slabs     %lu (%lu empty, %lu partial, %lu full)\n", cache->stats.slab_count,
         cache->stats.empty_slabs, cache->stats.partial_slabs, cache->stats.full_slabs);
    LOGF("memory    %lu of %lu bytes used (%lu.%01lu%%)\n", used, total, pct / 10ULL, pct % 10ULL);
    log_indent(-1);

    spinlock_release(&cache->lock, flags);
}

void slab_dump_all_caches(void) {
    bool flags = spinlock_acquire(&slab_list_lock);
    if (!slab_inited) {
        LOGF("[SLAB] Not initialized\n");
        spinlock_release(&slab_list_lock, flags);
        return;
    }

    stats_locked();
    log_indent(1);
    if (!caches) LOGF("no caches\n");

    for (slab_cache_t* cache = caches; cache; cache = cache->next) {
        if (!cache_validate(cache)) {
            LOGF("[SLAB ERROR] Corrupted cache in list\n");
            break;
        }
        // list lock is held and dump takes the cache lock, safe because the order is list -> cache
        slab_cache_dump(cache);
    }

    log_indent(-1);
    spinlock_release(&slab_list_lock, flags);
}

/*
 * slab_verify_integrity - Deep check of all caches and slabs
 */
bool slab_verify_integrity(void) {
    bool list_flags = spinlock_acquire(&slab_list_lock);
    if (!slab_inited) {
        LOGF("[SLAB VERIFY] Not initialized\n");
        spinlock_release(&slab_list_lock, list_flags);
        return false;
    }

    bool all_ok = true;

    slab_cache_t* cache = caches;
    int cache_count = 0;

    while (cache) {
        cache_count++;

        bool cache_flags = spinlock_acquire(&cache->lock);

        if (!cache_validate(cache)) {
            LOGF("[SLAB VERIFY] Cache %d: validation failed\n", cache_count);
            all_ok = false;
            spinlock_release(&cache->lock, cache_flags);
            break;
        }

        slab_t* lists[] = {cache->slabs_empty, cache->slabs_partial,
                           cache->slabs_full};
        const char* list_names[] = {"empty", "partial", "full"};

        for (int i = 0; i < 3; i++) {
            slab_t* slab = lists[i];
            int slab_num = 0;

            while (slab) {
                slab_num++;

                if (!slab_validate(slab)) {
                    LOGF("[SLAB VERIFY] Cache '%s': %s list slab %d invalid\n",
                         cache->name, list_names[i], slab_num);
                    all_ok = false;
                    break;
                }

                if (slab->cache != cache) {
                    LOGF(
                        "[SLAB VERIFY] Cache '%s': slab %d belongs to wrong "
                        "cache\n",
                        cache->name, slab_num);
                    all_ok = false;
                }

                if (i == 0 && slab->in_use != 0) {
                    LOGF(
                        "[SLAB VERIFY] Cache '%s': empty list has slab with "
                        "in_use=%u\n",
                        cache->name, slab->in_use);
                    all_ok = false;
                }
                if (i == 1 &&
                    (slab->in_use == 0 || slab->in_use >= slab->capacity)) {
                    LOGF(
                        "[SLAB VERIFY] Cache '%s': partial list has slab with "
                        "in_use=%u/%u\n",
                        cache->name, slab->in_use, slab->capacity);
                    all_ok = false;
                }
                if (i == 2 && slab->in_use != slab->capacity) {
                    LOGF(
                        "[SLAB VERIFY] Cache '%s': full list has slab with "
                        "in_use=%u/%u\n",
                        cache->name, slab->in_use, slab->capacity);
                    all_ok = false;
                }

                uint32_t free_count = 0;
                slab_free_obj_t* free_obj = (slab_free_obj_t*)slab->freelist;

                while (free_obj && free_count < slab->capacity) {
                    if (!validate_free_obj(free_obj)) {
                        LOGF(
                            "[SLAB VERIFY] Cache '%s': slab %d has corrupted "
                            "free object\n",
                            cache->name, slab_num);
                        all_ok = false;
                        break;
                    }
                    free_count++;
                    free_obj = free_obj->next;
                }

                if (free_count > slab->capacity) {
                    LOGF(
                        "[SLAB VERIFY] Cache '%s': slab %d freelist has too "
                        "many objects\n",
                        cache->name, slab_num);
                    all_ok = false;
                }

                uint32_t expected_free = slab->capacity - slab->in_use;
                if (free_count != expected_free) {
                    LOGF(
                        "[SLAB VERIFY] Cache '%s': slab %d free count mismatch "
                        "(got %u, expected %u)\n",
                        cache->name, slab_num, free_count, expected_free);
                    all_ok = false;
                }

                slab = slab->next;

                if (slab_num > 10000) {
                    LOGF(
                        "[SLAB VERIFY] Cache '%s': %s list has too many slabs "
                        "(loop?)\n",
                        cache->name, list_names[i]);
                    all_ok = false;
                    break;
                }
            }
        }

        spinlock_release(&cache->lock, cache_flags);
        cache = cache->next;

        if (cache_count > 1000) {
            LOGF("[SLAB VERIFY] Too many caches (loop?)\n");
            all_ok = false;
            break;
        }
    }

    if (!all_ok) LOGF("[SLAB VERIFY] Integrity check failed\n");

    spinlock_release(&slab_list_lock, list_flags);
    return all_ok;
}

#pragma endregion

#pragma region Introspection

/*
 * slab_cache_obj_size - return user-visible object size (not internal)
 */
size_t slab_cache_obj_size(slab_cache_t* cache) {
    if (!cache_validate(cache)) return 0;
    return cache->user_size;
}

const char* slab_cache_name(slab_cache_t* cache) {
    if (!cache_validate(cache)) return NULL;
    return cache->name;
}

#pragma endregion
