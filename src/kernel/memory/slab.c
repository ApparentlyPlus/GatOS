/* 
 * slab.c - Slab Allocator Implementation 
 * 
 * This implementation provides efficient allocation for small, fixed-size objects. 
 * Each cache manages a list of slabs (PMM pages) divided into equal-sized objects. 
 * Free objects are tracked using an embedded free-list within the objects themselves. 
 * 
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/memory/paging.h>
#include <kernel/memory/slab.h>
#include <kernel/memory/pmm.h>
#include <kernel/debug.h>
#include <libc/string.h>
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
    uint64_t alloc_timestamp;
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

    slab_cache_stats_t stats;
    slab_cache_t* next;
};

// Global state
static bool g_slab_initialized = false;
static slab_cache_t* g_caches = NULL;
static uint32_t g_next_cache_id = 1;
static slab_stats_t g_stats;

#pragma region Validation Helpers

/*
 * slab_validate - Validate slab structure (does not panic; returns bool)
 */
static inline bool slab_validate(slab_t* slab) {
    if (!slab) return false;

    uint32_t m = slab->magic; // small temp to make logging lines shorter
    if (m != SLAB_MAGIC) {
        LOGF("[SLAB ERROR] Invalid slab magic: 0x%x (expected 0x%x)\n", m,
             SLAB_MAGIC);
        g_stats.corruption_detected++;
        return false;
    }

    if (slab->in_use > slab->capacity) {
        LOGF("[SLAB ERROR] Slab in_use (%u) > capacity (%u)\n", slab->in_use,
             slab->capacity);
        g_stats.corruption_detected++;
        return false;
    }

    return true;
}

/*
 * cache_validate - Validate cache structure
 */
static inline bool cache_validate(slab_cache_t* cache) {
    if (!cache) return false;

    // a slightly verbose check to aid auditing
    if (cache->magic != SLAB_CACHE_MAGIC) {
        LOGF("[SLAB ERROR] Invalid cache magic: 0x%x (expected 0x%x)\n",
             cache->magic, SLAB_CACHE_MAGIC);
        g_stats.corruption_detected++;
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
        g_stats.corruption_detected++;
        return false;
    }
    if (obj->red_zone_pre != SLAB_RED_ZONE) {
        LOGF("[SLAB ERROR] Free object pre-red-zone corrupted: 0x%x\n",
             obj->red_zone_pre);
        g_stats.corruption_detected++;
        return false;
    }
    if (obj->red_zone_post != SLAB_RED_ZONE) {
        LOGF("[SLAB ERROR] Free object post-red-zone corrupted: 0x%x\n",
             obj->red_zone_post);
        g_stats.corruption_detected++;
        return false;
    }

    return true;
}

#pragma endregion

#pragma region Internal Functions

/*
 * is_pow2_u64 - check power-of-two (kept simple)
 */
static inline bool is_pow2_u64(uint64_t x) { return x && ((x & (x - 1)) == 0); }

/*
 * slab_remove_from_list - remove 'slab' from a doubly-linked list
 */
static void slab_remove_from_list(slab_t** list_head, slab_t* slab) {
    if (!slab) return;

    if (slab->prev) {
        slab->prev->next = slab->next;
    } else {
        // slab was head
        *list_head = slab->next;
    }

    if (slab->next) {
        slab->next->prev = slab->prev;
    }

    // clear links to avoid accidental reuse footguns
    slab->next = NULL;
    slab->prev = NULL;
}

/*
 * slab_add_to_list - add slab to head of list (LIFO style)
 */
static void slab_add_to_list(slab_t** list_head, slab_t* slab) {
    if (!slab) return;

    slab->next = *list_head;
    slab->prev = NULL;

    if (*list_head) {
        (*list_head)->prev = slab;
    }

    *list_head = slab;
}

/*
 * slab_move_to_list - convenience wrapper (remove then add)
 */
static void slab_move_to_list(slab_t** from_list, slab_t** to_list, slab_t* slab) {
    // simple wrapper to make callers shorter and intention clearer
    slab_remove_from_list(from_list, slab);
    slab_add_to_list(to_list, slab);
}

/*
 * slab_allocate_page - Allocate a new slab (one PMM page) and initialize it
 */
static slab_t* slab_allocate_page(slab_cache_t* cache) {
    if (!cache_validate(cache)) return NULL;

    uint64_t phys = 0;
    pmm_status_t pmm_status = pmm_alloc(PAGE_SIZE, &phys);
    if (pmm_status != PMM_OK) {
        LOGF("[SLAB] Failed to allocate page from PMM: %d\n", pmm_status);
        return NULL;
    }

    // map physical page into kernel virtual space
    slab_t* slab = (slab_t*)PHYSMAP_P2V(phys);

    // zero the whole page to start cleanly
    memset(slab, 0, PAGE_SIZE);

    // init metadata
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

    // compute the effective metadata size to satisfy alignment
    metadata_size = (size_t)(aligned_user_ptr - base - sizeof(slab_alloc_header_t));

    // available bytes in page for object storage
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

    // initialize freelist: push each object on the list
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

    // update counters (globals and cache-local)
    g_stats.total_slabs++;
    g_stats.total_pmm_bytes += PAGE_SIZE;

    cache->stats.slab_count++;
    cache->stats.empty_slabs++;

    return slab;
}

/*
 * slab_free_page - Free a slab (page) back to PMM
 */
static void slab_free_page(slab_t* slab) {
    if (!slab_validate(slab)) return;

    slab_cache_t* cache = slab->cache;
    if (!cache_validate(cache)) return;

    // update stats first so we keep accounting right away
    g_stats.total_slabs--;
    g_stats.total_pmm_bytes -= PAGE_SIZE;
    cache->stats.slab_count--;

    // clear magic to help detect dangling references
    slab->magic = 0;

    pmm_free(slab->slab_phys, PAGE_SIZE);
}

/*
 * get_slab_from_obj - Given an object pointer (user or header), find slab
 */
static slab_t* get_slab_from_obj(void* obj) {
    if (!obj) return NULL;

    uintptr_t addr = (uintptr_t)obj;

    // round down to page boundary to recover slab header
    uintptr_t slab_addr = addr & ~(PAGE_SIZE - 1);
    slab_t* slab = (slab_t*)slab_addr;

    if (!slab_validate(slab)) {
        return NULL;
    }

    return slab;
}

/*
 * slab_alloc_cache_struct - Allocate the cache structure itself from PMM
 */
static slab_cache_t* slab_alloc_cache_struct(void) {
    uint64_t phys = 0;
    pmm_status_t status = pmm_alloc(sizeof(slab_cache_t), &phys);
    if (status != PMM_OK) {
        LOGF("[SLAB] Failed to allocate cache structure from PMM\n");
        return NULL;
    }

    slab_cache_t* cache = (slab_cache_t*)PHYSMAP_P2V(phys);

    // zero initialize
    memset(cache, 0, sizeof(slab_cache_t));

    return cache;
}

/*
 * slab_free_cache_struct - Free the cache structure back to PMM
 */
static void slab_free_cache_struct(slab_cache_t* cache) {
    if (!cache) return;

    uint64_t phys = PHYSMAP_V2P((uint64_t)cache);

    // align physically and by size to pmm_min_block_size
    uint64_t aligned_phys = align_down(phys, pmm_min_block_size());
    size_t aligned_size = align_up(sizeof(slab_cache_t), pmm_min_block_size());

    pmm_free(aligned_phys, aligned_size);
}

#pragma endregion

#pragma region Initialization and Shutdown

/*
 * slab_init - Initialize the slab allocator
 */
slab_status_t slab_init(void) {
    if (g_slab_initialized) {
        return SLAB_ERR_ALREADY_INIT;
    }

    if (!pmm_is_initialized()) {
        LOGF("[SLAB] PMM must be initialized before slab allocator\n");
        return SLAB_ERR_NOT_INIT;
    }

    g_caches = NULL;
    g_next_cache_id = 1;
    memset(&g_stats, 0, sizeof(slab_stats_t));

    g_slab_initialized = true;

    LOGF("[SLAB] Slab (System Wide) Allocator initialized\n");

    return SLAB_OK;
}

/*
 * slab_shutdown - Destroy all caches and mark allocator uninitialized
 */
void slab_shutdown(void) {
    if (!g_slab_initialized) return;

    // walk the cache list and destroy them one by one
    slab_cache_t* cache = g_caches;
    while (cache) {
        slab_cache_t* next = cache->next;
        slab_cache_destroy(cache);
        cache = next;
    }

    // reset globals
    g_slab_initialized = false;
    g_caches = NULL;
    g_next_cache_id = 1;
    memset(&g_stats, 0, sizeof(slab_stats_t));

    LOGF("[SLAB] Slab (System Wide) Allocator shutdown\n");
}

/*
 * slab_is_initialized - simple accessor
 */
bool slab_is_initialized(void) { return g_slab_initialized; }

#pragma endregion

#pragma region Cache Management

/*
 * slab_cache_create - Create a new cache for fixed-size allocations
 */
slab_cache_t* slab_cache_create(const char* name, size_t obj_size,
                                size_t align) {
    if (!g_slab_initialized) {
        LOGF("[SLAB] Allocator not initialized\n");
        return NULL;
    }

    if (!name || obj_size == 0) {
        LOGF("[SLAB] Invalid arguments\n");
        return NULL;
    }

    if (obj_size > SLAB_MAX_OBJ_SIZE) {
        LOGF("[SLAB] Object size %zu exceeds max %zu\n", obj_size,
             SLAB_MAX_OBJ_SIZE);
        return NULL;
    }

    if (align == 0) align = 8; // eeeh, default alignment

    if (!is_pow2_u64(align)) {
        LOGF("[SLAB] Alignment must be power of 2\n");
        return NULL;
    }

    // prevent duplicate cache names (keeps things sane)
    if (slab_cache_find(name)) {
        LOGF("[SLAB] Cache '%s' already exists\n", name);
        return NULL;
    }

    // allocate the cache struct from PMM (bootstrapping path)
    slab_cache_t* cache = slab_alloc_cache_struct();
    if (!cache) {
        LOGF("[SLAB] Failed to allocate cache structure\n");
        return NULL;
    }

    // initialize fields
    cache->magic = SLAB_CACHE_MAGIC;
    cache->cache_id = g_next_cache_id++;
    strncpy(cache->name, name, SLAB_CACHE_NAME_LEN - 1);
    cache->name[SLAB_CACHE_NAME_LEN - 1] = '\0';

    cache->user_size = obj_size;

    // include allocation header in per object accounting
    size_t total_size = obj_size + sizeof(slab_alloc_header_t);

    // make sure freelist header fits when object is free
    if (total_size < SLAB_MIN_OBJ_SIZE) {
        total_size = SLAB_MIN_OBJ_SIZE;
    }

    // align total size up to align
    cache->obj_size = align_up(total_size, align);
    cache->align = align;

    cache->slabs_empty = NULL;
    cache->slabs_partial = NULL;
    cache->slabs_full = NULL;

    memset(&cache->stats, 0, sizeof(slab_cache_stats_t));

    // insert into global cache list (LIFO)
    cache->next = g_caches;
    g_caches = cache;
    g_stats.cache_count++;

    return cache;
}

/*
 * slab_cache_destroy - tear down a cache and free its slabs
 */
void slab_cache_destroy(slab_cache_t* cache) {
    if (!cache_validate(cache)) return;

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

    // remove from global cache list
    slab_cache_t** prev = &g_caches;
    while (*prev) {
        if (*prev == cache) {
            *prev = cache->next;
            break;
        }
        prev = &(*prev)->next;
    }

    g_stats.cache_count--;

    // clear magic before freeing structure back to PMM
    cache->magic = 0;

    slab_free_cache_struct(cache);
}

/*
 * slab_cache_find - find a cache by name (simple linear search)
 */
slab_cache_t* slab_cache_find(const char* name) {
    if (!g_slab_initialized || !name) return NULL;

    slab_cache_t* cache = g_caches;
    while (cache) {
        if (!cache_validate(cache)) {
            LOGF("[SLAB] Corrupted cache in list\n");
            return NULL;
        }
        if (strncmp(cache->name, name, SLAB_CACHE_NAME_LEN) == 0) {
            return cache;
        }
        cache = cache->next;
    }

    return NULL;
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

    slab_t* slab = NULL;

    // prefer partial slabs (already have some allocations) -> better locality
    if (cache->slabs_partial) {
        slab = cache->slabs_partial;
    } else if (cache->slabs_empty) {
        slab = cache->slabs_empty;
    } else {
        // need to allocate a new slab page
        slab = slab_allocate_page(cache);
        if (!slab) return SLAB_ERR_NO_MEMORY;
        slab_add_to_list(&cache->slabs_empty, slab);
    }

    if (!slab_validate(slab)) return SLAB_ERR_CORRUPTION;

    // pop from freelist
    if (!slab->freelist) {
        LOGF("[SLAB ERROR] Slab has no free objects but in_use=%u capacity=%u\n",
            slab->in_use, slab->capacity);
        return SLAB_ERR_CORRUPTION;
    }

    slab_free_obj_t* obj = (slab_free_obj_t*)slab->freelist;
    if (!validate_free_obj(obj)) {
        LOGF("[SLAB ERROR] Corrupted free object in cache '%s'\n", cache->name);
        return SLAB_ERR_CORRUPTION;
    }

    // unlink first free
    slab->freelist = obj->next;
    slab->in_use++;

    // clear object memory before giving to user
    memset(obj, 0, cache->obj_size);

    // write allocation header at the object start
    slab_alloc_header_t* header = (slab_alloc_header_t*)obj;
    header->magic = SLAB_ALLOC_MAGIC;
    header->cache_id = cache->cache_id;
    header->alloc_timestamp = 0; // prolly need TSC here

    // stats
    cache->stats.total_allocs++;
    cache->stats.active_objects++;

    // move slab between lists if its fullness changed
    if (slab->in_use == slab->capacity) {
        // slab became full
        if (slab == cache->slabs_partial) {
            slab_move_to_list(&cache->slabs_partial, &cache->slabs_full, slab);
            cache->stats.partial_slabs--;
            cache->stats.full_slabs++;
        } else if (slab == cache->slabs_empty) {
            slab_move_to_list(&cache->slabs_empty, &cache->slabs_full, slab);
            cache->stats.empty_slabs--;
            cache->stats.full_slabs++;
        }
    } else if (slab->in_use == 1) {
        // slab transitioned from empty -> partial
        if (slab == cache->slabs_empty) {
            slab_move_to_list(&cache->slabs_empty, &cache->slabs_partial, slab);
            cache->stats.empty_slabs--;
            cache->stats.partial_slabs++;
        }
    }

    // return pointer to user space (right after header)
    *out_obj = (void*)((uint8_t*)obj + sizeof(slab_alloc_header_t));
    return SLAB_OK;
}

/*
 * slab_free - Free an object back to its cache
 */
slab_status_t slab_free(slab_cache_t* cache, void* obj) {
    if (!cache_validate(cache) || !obj) {
        return SLAB_ERR_INVALID;
    }

    // compute pointer to header (start of internal object)
    void* obj_start = (void*)((uint8_t*)obj - sizeof(slab_alloc_header_t));

    // find owning slab by rounding down to page
    slab_t* slab = get_slab_from_obj(obj_start);
    if (!slab_validate(slab)) {
        LOGF("[SLAB ERROR] Object %p does not belong to a valid slab\n", obj);
        return SLAB_ERR_NOT_FOUND;
    }

    // object must belong to the cache passed in
    if (slab->cache != cache) {
        LOGF("[SLAB ERROR] Object belongs to different cache\n");
        return SLAB_ERR_NOT_FOUND;
    }

    // verify allocation header - detect double-free or corruption
    slab_alloc_header_t* header = (slab_alloc_header_t*)obj_start;
    if (header->magic != SLAB_ALLOC_MAGIC) {
        LOGF(
            "[SLAB ERROR] Invalid allocation magic (double-free or "
            "corruption)\n");
        g_stats.corruption_detected++;
        return SLAB_ERR_CORRUPTION;
    }
    if (header->cache_id != cache->cache_id) {
        LOGF("[SLAB ERROR] Cache ID mismatch\n");
        return SLAB_ERR_CORRUPTION;
    }

    // convert object into a free object and push on freelist
    slab_free_obj_t* free_obj = (slab_free_obj_t*)obj_start;
    free_obj->magic = SLAB_FREE_MAGIC;
    free_obj->red_zone_pre = SLAB_RED_ZONE;
    free_obj->red_zone_post = SLAB_RED_ZONE;
    free_obj->next = (slab_free_obj_t*)slab->freelist;
    slab->freelist = free_obj;

    slab->in_use--;

    // update stats
    cache->stats.total_frees++;
    cache->stats.active_objects--;

    // move slab to correct list depending on new in_use
    if (slab->in_use == 0) {
        // slab became empty
        if (slab == cache->slabs_partial) {
            slab_move_to_list(&cache->slabs_partial, &cache->slabs_empty, slab);
            cache->stats.partial_slabs--;
            cache->stats.empty_slabs++;
        } else if (slab == cache->slabs_full) {
            slab_move_to_list(&cache->slabs_full, &cache->slabs_empty, slab);
            cache->stats.full_slabs--;
            cache->stats.empty_slabs++;
        }

        // free empty slab if we have too many empties
        if (cache->stats.empty_slabs > 1) {
            slab_remove_from_list(&cache->slabs_empty, slab);
            slab_free_page(slab);
            cache->stats.empty_slabs--;
        }

    } else if (slab->in_use == slab->capacity - 1) {
        // slab went from full -> partial
        if (slab == cache->slabs_full) {
            slab_move_to_list(&cache->slabs_full, &cache->slabs_partial, slab);
            cache->stats.full_slabs--;
            cache->stats.partial_slabs++;
        }
    }

    return SLAB_OK;
}

#pragma endregion

#pragma region Statistics and Debugging

/*
 * slab_cache_stats - copy cache stats out
 */
void slab_cache_stats(slab_cache_t* cache, slab_cache_stats_t* out_stats) {
    if (!cache_validate(cache) || !out_stats) return;
    *out_stats = cache->stats;
}

/*
 * slab_get_stats - copy global stats out
 */
void slab_get_stats(slab_stats_t* out_stats) {
    if (!out_stats) return;
    *out_stats = g_stats;
}

/*
 * slab_dump_stats - print global stats to log
 */
void slab_dump_stats(void) {
    if (!g_slab_initialized) {
        LOGF("[SLAB] Not initialized\n");
        return;
    }

    LOGF("=== Slab Allocator Statistics ===\n");
    LOGF("Total slabs: %lu\n", g_stats.total_slabs);
    LOGF("Total PMM bytes: %lu (%.2f MiB)\n", g_stats.total_pmm_bytes,
         g_stats.total_pmm_bytes / (1024.0 * 1024.0));
    LOGF("Active caches: %lu (dynamic allocation)\n", g_stats.cache_count);
    LOGF("Corruption events: %lu\n", g_stats.corruption_detected);
    LOGF("=================================\n");
}

/*
 * slab_cache_dump - print detailed info about a specific cache
 */
void slab_cache_dump(slab_cache_t* cache) {
    if (!cache_validate(cache)) return;

    LOGF("=== Slab Cache: %s ===\n", cache->name);
    LOGF("User object size: %zu bytes\n", cache->user_size);
    LOGF("Total object size: %zu bytes (align: %zu)\n", cache->obj_size,
         cache->align);
    LOGF("Cache ID: %u\n", cache->cache_id);
    LOGF("\nStatistics:\n");
    LOGF("  Total allocations: %lu\n", cache->stats.total_allocs);
    LOGF("  Total frees:       %lu\n", cache->stats.total_frees);
    LOGF("  Active objects:    %lu\n", cache->stats.active_objects);
    LOGF("  Slab count:        %lu\n", cache->stats.slab_count);
    LOGF("  Empty slabs:       %lu\n", cache->stats.empty_slabs);
    LOGF("  Partial slabs:     %lu\n", cache->stats.partial_slabs);
    LOGF("  Full slabs:        %lu\n", cache->stats.full_slabs);

    // memory usage calc (kept explicit)
    uint64_t total_bytes = cache->stats.slab_count * PAGE_SIZE;
    uint64_t used_bytes = cache->stats.active_objects * cache->obj_size;
    double utilization = 0.0;
    if (total_bytes > 0) utilization = (double)used_bytes / total_bytes * 100.0;

    LOGF("\nMemory usage:\n");
    LOGF("  Total:        %lu bytes (%.2f KiB)\n", total_bytes,
         total_bytes / 1024.0);
    LOGF("  Used:         %lu bytes (%.2f KiB)\n", used_bytes,
         used_bytes / 1024.0);
    LOGF("  Utilization:  %.1f%%\n", utilization);
    LOGF("========================\n");
}

/*
 * slab_dump_all_caches - dump stats for all caches
 */
void slab_dump_all_caches(void) {
    if (!g_slab_initialized) {
        LOGF("[SLAB] Not initialized\n");
        return;
    }

    slab_dump_stats();
    LOGF("\n");

    slab_cache_t* cache = g_caches;
    if (!cache) {
        LOGF("No caches created\n");
        return;
    }

    while (cache) {
        if (!cache_validate(cache)) {
            LOGF("[SLAB ERROR] Corrupted cache in list\n");
            break;
        }
        slab_cache_dump(cache);
        LOGF("\n");
        cache = cache->next;
    }
}

/*
 * slab_verify_integrity - Deep check of all caches and slabs
 */
bool slab_verify_integrity(void) {
    if (!g_slab_initialized) {
        LOGF("[SLAB VERIFY] Not initialized\n");
        return false;
    }

    LOGF("[SLAB VERIFY] Checking slab allocator integrity...\n");
    bool all_ok = true;

    slab_cache_t* cache = g_caches;
    int cache_count = 0;

    while (cache) {
        cache_count++;

        if (!cache_validate(cache)) {
            LOGF("[SLAB VERIFY] Cache %d: validation failed\n", cache_count);
            all_ok = false;
            break;
        }

        // verify the three lists
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

                // slab must point back to this cache
                if (slab->cache != cache) {
                    LOGF(
                        "[SLAB VERIFY] Cache '%s': slab %d belongs to wrong "
                        "cache\n",
                        cache->name, slab_num);
                    all_ok = false;
                }

                // check in_use vs list membership
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

                // verify freelist consistency: count free objects and validate headers
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

                // guard against accidental infinite loops
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

        cache = cache->next;

        // guard for accidental loops in cache list
        if (cache_count > 1000) {
            LOGF("[SLAB VERIFY] Too many caches (loop?)\n");
            all_ok = false;
            break;
        }
    }

    if (all_ok) {
        LOGF("[SLAB VERIFY] All checks passed (%d caches)\n", cache_count);
    } else {
        LOGF("[SLAB VERIFY] FAILED - integrity compromised!\n");
    }

    return all_ok;
}

#pragma endregion

#pragma region Introspection

/*
 * slab_cache_obj_size - return user-visible object size (not internal)
 */
size_t slab_cache_obj_size(slab_cache_t* cache) {
    if (!cache_validate(cache)) return 0;
    // return the size callers expect
    return cache->user_size;
}

/*
 * slab_cache_name - return cache name
 */
const char* slab_cache_name(slab_cache_t* cache) {
    if (!cache_validate(cache)) return NULL;
    return cache->name;
}

#pragma endregion