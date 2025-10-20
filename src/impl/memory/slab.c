/*
 * slab.c - Slab Allocator Implementation
 * 
 * This implementation provides efficient allocation for small, fixed-size objects.
 * Each cache manages a list of slabs (PMM pages) divided into equal-sized objects.
 * Free objects are tracked using an embedded free-list within the objects themselves.
 * 
 * Author: u/ApparentlyPlus
 */

#include <memory/slab.h>
#include <memory/pmm.h>
#include <memory/paging.h>
#include <libc/string.h>
#include <debug.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Magic numbers for validation
#define SLAB_MAGIC           0xC00151AB
#define SLAB_CACHE_MAGIC     0xCACE51AB
#define SLAB_FREE_MAGIC      0xFEEDF00D
#define SLAB_ALLOC_MAGIC     0xA110C8ED
#define SLAB_RED_ZONE        0xDEADFA11

// Objects larger than this should be using the PMM directly
#define SLAB_MAX_OBJ_SIZE (PAGE_SIZE / 8)

// Minimum object size (must fit free-list pointer + magic + red zones)
#define SLAB_MIN_OBJ_SIZE (sizeof(slab_free_obj_t))

// Slab metadata, which we will store at the beginning of PMM pages
typedef struct slab {
    uint32_t magic;              // SLAB_MAGIC
    uint32_t in_use;             // Number of allocated objects
    uint32_t capacity;           // Total objects in this slab
    uint32_t obj_size;           // Size of each object (including user header)
    void* freelist;              // First free object (or NULL)
    struct slab* next;           // Next slab in list
    struct slab* prev;           // Previous slab in list
    slab_cache_t* cache;         // Parent cache
    uint64_t slab_phys;          // Physical address of this slab
} slab_t;

// Free object header (embedded in free objects)
typedef struct slab_free_obj {
    uint32_t magic;              // SLAB_FREE_MAGIC
    uint32_t red_zone_pre;       // SLAB_RED_ZONE
    struct slab_free_obj* next;  // Next free object
    uint32_t red_zone_post;      // SLAB_RED_ZONE
} slab_free_obj_t;

// Allocated object header (stored before user data)
typedef struct {
    uint32_t magic;              // SLAB_ALLOC_MAGIC
    uint32_t cache_id;           // For validation
    uint64_t alloc_timestamp;    // For debugging
} slab_alloc_header_t;

// Slab cache structure
struct slab_cache {
    uint32_t magic;              // SLAB_CACHE_MAGIC
    char name[SLAB_CACHE_NAME_LEN];
    size_t obj_size;             // Total size per object (including header)
    size_t user_size;            // User-visible object size
    size_t align;                // Alignment requirement
    uint32_t cache_id;           // Unique cache identifier
    
    slab_t* slabs_empty;         // Slabs with all objects free
    slab_t* slabs_partial;       // Slabs with some objects free
    slab_t* slabs_full;          // Slabs with no free objects
    
    slab_cache_stats_t stats;
    slab_cache_t* next;          // Next cache in global list
};

// Global state
static bool g_slab_initialized = false;
static slab_cache_t* g_caches = NULL;
static uint32_t g_next_cache_id = 1;
static slab_stats_t g_stats;

#pragma region Validation Helpers

/*
 * slab_validate - Validate slab structure
 */
static inline bool slab_validate(slab_t* slab) {
    if (!slab) return false;
    if (slab->magic != SLAB_MAGIC) {
        LOGF("[SLAB ERROR] Invalid slab magic: 0x%x (expected 0x%x)\n",
               slab->magic, SLAB_MAGIC);
        g_stats.corruption_detected++;
        return false;
    }
    if (slab->in_use > slab->capacity) {
        LOGF("[SLAB ERROR] Slab in_use (%u) > capacity (%u)\n",
               slab->in_use, slab->capacity);
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
    if (cache->magic != SLAB_CACHE_MAGIC) {
        LOGF("[SLAB ERROR] Invalid cache magic: 0x%x (expected 0x%x)\n",
               cache->magic, SLAB_CACHE_MAGIC);
        g_stats.corruption_detected++;
        return false;
    }
    return true;
}

/*
 * validate_free_obj - Validate free object header
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
 * is_pow2_u64 - check if x is a power of two
 */
static inline bool is_pow2_u64(uint64_t x) { 
    return x && ((x & (x - 1)) == 0); 
}

/*
 * slab_remove_from_list - Remove slab from its current list
 */
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
}

/*
 * slab_add_to_list - Add slab to the head of a list
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
 * slab_move_to_list - Move slab from one list to another
 */
static void slab_move_to_list(slab_t** from_list, slab_t** to_list, slab_t* slab) {
    slab_remove_from_list(from_list, slab);
    slab_add_to_list(to_list, slab);
}

/*
 * slab_allocate_page - Allocate a new slab from PMM
 */
static slab_t* slab_allocate_page(slab_cache_t* cache) {
    if (!cache_validate(cache)) return NULL;
    
    // Allocate one page from PMM
    uint64_t phys = 0;
    pmm_status_t pmm_status = pmm_alloc(PAGE_SIZE, &phys);
    if (pmm_status != PMM_OK) {
        LOGF("[SLAB] Failed to allocate page from PMM: %d\n", pmm_status);
        return NULL;
    }
    
    // Write to it
    slab_t* slab = (slab_t*)PHYSMAP_P2V(phys);
    memset(slab, 0, PAGE_SIZE);
    
    // Initialize slab metadata
    slab->magic = SLAB_MAGIC;
    slab->obj_size = cache->obj_size;
    slab->cache = cache;
    slab->slab_phys = phys;
    slab->next = NULL;
    slab->prev = NULL;
    
    // Calculate object layout
    // The user pointer (after allocation header) must be aligned to cache->align
    size_t metadata_size = sizeof(slab_t);
    
    // Calculate where first user pointer would be
    uintptr_t first_obj_start = (uintptr_t)slab + metadata_size;
    uintptr_t first_user_ptr = first_obj_start + sizeof(slab_alloc_header_t);
    
    // Align the user pointer
    uintptr_t aligned_user_ptr = align_up(first_user_ptr, cache->align);
    
    // Calculate required metadata size to achieve this alignment
    metadata_size = aligned_user_ptr - (uintptr_t)slab - sizeof(slab_alloc_header_t);
    
    size_t available = PAGE_SIZE - metadata_size;
    slab->capacity = available / cache->obj_size;
    
    if (slab->capacity == 0) {
        LOGF("[SLAB ERROR] Object size %zu too large for page (metadata=%zu, avail=%zu)\n",
               cache->obj_size, metadata_size, available);
        pmm_free(phys, PAGE_SIZE);
        return NULL;
    }
    
    slab->in_use = 0;
    
    // Initialize freelist - link all objects
    uint8_t* obj_base = (uint8_t*)slab + metadata_size;
    slab->freelist = NULL;
    
    for (size_t i = 0; i < slab->capacity; i++) {
        slab_free_obj_t* obj = (slab_free_obj_t*)(obj_base + i * cache->obj_size);
        obj->magic = SLAB_FREE_MAGIC;
        obj->red_zone_pre = SLAB_RED_ZONE;
        obj->red_zone_post = SLAB_RED_ZONE;
        obj->next = slab->freelist;
        slab->freelist = obj;
    }
    
    // Update statistics
    g_stats.total_slabs++;
    g_stats.total_pmm_bytes += PAGE_SIZE;
    cache->stats.slab_count++;
    cache->stats.empty_slabs++;
    
    return slab;
}

/*
 * slab_free_page - Free a slab back to PMM
 */
static void slab_free_page(slab_t* slab) {
    if (!slab_validate(slab)) return;
    
    slab_cache_t* cache = slab->cache;
    if (!cache_validate(cache)) return;
    
    // Update statistics
    g_stats.total_slabs--;
    g_stats.total_pmm_bytes -= PAGE_SIZE;
    cache->stats.slab_count--;
    
    // Clear magic to detect use-after-free
    slab->magic = 0;
    
    // Free to PMM
    pmm_free(slab->slab_phys, PAGE_SIZE);
}

/*
 * get_slab_from_obj - Find which slab an object belongs to
 */
static slab_t* get_slab_from_obj(void* obj) {
    if (!obj) return NULL;
    
    // Round down to page boundary to find slab header
    uintptr_t addr = (uintptr_t)obj;
    uintptr_t slab_addr = addr & ~(PAGE_SIZE - 1);
    slab_t* slab = (slab_t*)slab_addr;
    
    if (!slab_validate(slab)) {
        return NULL;
    }
    
    return slab;
}

/*
 * slab_alloc_cache_struct - Allocate a cache structure from PMM
 * This is used during bootstrapping before we have slab caches
 */
static slab_cache_t* slab_alloc_cache_struct(void) {
    uint64_t phys;
    pmm_status_t status = pmm_alloc(sizeof(slab_cache_t), &phys);
    if (status != PMM_OK) {
        LOGF("[SLAB] Failed to allocate cache structure from PMM\n");
        return NULL;
    }
    
    slab_cache_t* cache = (slab_cache_t*)PHYSMAP_P2V(phys);
    memset(cache, 0, sizeof(slab_cache_t));
    
    return cache;
}

/*
 * slab_free_cache_struct - Free a cache structure back to PMM
 */
static void slab_free_cache_struct(slab_cache_t* cache) {
    if (!cache) return;
    
    uint64_t phys = PHYSMAP_V2P((uint64_t)cache);
    
    // Align down to PMM's allocation granularity
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
 * slab_shutdown - Shutdown the slab allocator
 */
void slab_shutdown(void) {
    if (!g_slab_initialized) return;
    
    // Destroy all caches
    slab_cache_t* cache = g_caches;
    while (cache) {
        slab_cache_t* next = cache->next;
        slab_cache_destroy(cache);
        cache = next;
    }
    
    g_slab_initialized = false;
    g_caches = NULL;
    g_next_cache_id = 1;
    memset(&g_stats, 0, sizeof(slab_stats_t));

    LOGF("[SLAB] Slab (System Wide) Allocator shutdown\n");
}

/*
 * slab_is_initialized - Check if slab allocator is initialized
 */
bool slab_is_initialized(void) {
    return g_slab_initialized;
}

#pragma endregion

#pragma region Cache Management

/*
 * slab_cache_create - Create a new slab cache
 */
slab_cache_t* slab_cache_create(const char* name, size_t obj_size, size_t align) {
    if (!g_slab_initialized) {
        LOGF("[SLAB] Allocator not initialized\n");
        return NULL;
    }
    
    if (!name || obj_size == 0) {
        LOGF("[SLAB] Invalid arguments\n");
        return NULL;
    }
    
    if (obj_size > SLAB_MAX_OBJ_SIZE) {
        LOGF("[SLAB] Object size %zu exceeds max %zu\n", obj_size, SLAB_MAX_OBJ_SIZE);
        return NULL;
    }
    
    if (align == 0)
        align = 8;  // Eh, default alignment I guess
    
    if (!is_pow2_u64(align)) {
        LOGF("[SLAB] Alignment must be power of 2\n");
        return NULL;
    }
    
    // Check for duplicate cache name
    if (slab_cache_find(name)) {
        LOGF("[SLAB] Cache '%s' already exists\n", name);
        return NULL;
    }
    
    // Allocate cache structure from PMM
    slab_cache_t* cache = slab_alloc_cache_struct();
    if (!cache) {
        LOGF("[SLAB] Failed to allocate cache structure\n");
        return NULL;
    }
    
    // Initialize cache
    cache->magic = SLAB_CACHE_MAGIC;
    cache->cache_id = g_next_cache_id++;
    strncpy(cache->name, name, SLAB_CACHE_NAME_LEN - 1);
    cache->name[SLAB_CACHE_NAME_LEN - 1] = '\0';
    
    // Calculate total object size (user size + allocation header)
    cache->user_size = obj_size;
    size_t total_size = obj_size + sizeof(slab_alloc_header_t);
    
    // Ensure we can fit free-list header when object is free
    if (total_size < SLAB_MIN_OBJ_SIZE) {
        total_size = SLAB_MIN_OBJ_SIZE;
    }
    
    // Align total size
    cache->obj_size = align_up(total_size, align);
    cache->align = align;
    
    cache->slabs_empty = NULL;
    cache->slabs_partial = NULL;
    cache->slabs_full = NULL;
    
    memset(&cache->stats, 0, sizeof(slab_cache_stats_t));
    
    // Add to global cache list
    cache->next = g_caches;
    g_caches = cache;
    g_stats.cache_count++;
    
    return cache;
}

/*
 * slab_cache_destroy - Destroy a slab cache
 */
void slab_cache_destroy(slab_cache_t* cache) {
    if (!cache_validate(cache)) return;
    
    // Free all slabs in all lists
    slab_t* lists[] = {cache->slabs_empty, cache->slabs_partial, cache->slabs_full};
    
    for (int i = 0; i < 3; i++) {
        slab_t* slab = lists[i];
        while (slab) {
            slab_t* next = slab->next;
            slab_free_page(slab);
            slab = next;
        }
    }
    
    // Remove from global cache list
    slab_cache_t** prev = &g_caches;
    while (*prev) {
        if (*prev == cache) {
            *prev = cache->next;
            break;
        }
        prev = &(*prev)->next;
    }
    
    g_stats.cache_count--;
    
    // Clear magic before freeing
    cache->magic = 0;
    
    // Free cache structure back to PMM
    slab_free_cache_struct(cache);
}

/*
 * slab_cache_find - Find a cache by name
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
    
    // Try partial slabs first (best case it already has some allocations)
    if (cache->slabs_partial) {
        slab = cache->slabs_partial;
    }
    // Try empty slabs next
    else if (cache->slabs_empty) {
        slab = cache->slabs_empty;
    }
    // Need to allocate a new slab
    else {
        slab = slab_allocate_page(cache);
        if (!slab) return SLAB_ERR_NO_MEMORY;
        slab_add_to_list(&cache->slabs_empty, slab);
    }
    
    if (!slab_validate(slab)) return SLAB_ERR_CORRUPTION;
    
    // Pop object from freelist
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
    
    slab->freelist = obj->next;
    slab->in_use++;
    
    // Clear object memory
    memset(obj, 0, cache->obj_size);
    
    // Write allocation header
    slab_alloc_header_t* header = (slab_alloc_header_t*)obj;
    header->magic = SLAB_ALLOC_MAGIC;
    header->cache_id = cache->cache_id;
    header->alloc_timestamp = 0; // Could use TSC here
    
    // Update statistics
    cache->stats.total_allocs++;
    cache->stats.active_objects++;
    
    // Move slab to appropriate list if needed
    if (slab->in_use == slab->capacity) {
        // Slab is now full
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
        // Slab was empty, now partial
        if (slab == cache->slabs_empty) {
            slab_move_to_list(&cache->slabs_empty, &cache->slabs_partial, slab);
            cache->stats.empty_slabs--;
            cache->stats.partial_slabs++;
        }
    }
    
    // Return pointer after header
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
    
    // Get actual object start (before user pointer)
    void* obj_start = (void*)((uint8_t*)obj - sizeof(slab_alloc_header_t));
    
    // Find which slab owns this object
    slab_t* slab = get_slab_from_obj(obj_start);
    if (!slab_validate(slab)) {
        LOGF("[SLAB ERROR] Object %p does not belong to a valid slab\n", obj);
        return SLAB_ERR_NOT_FOUND;
    }
    
    // Verify object belongs to this cache
    if (slab->cache != cache) {
        LOGF("[SLAB ERROR] Object belongs to different cache\n");
        return SLAB_ERR_NOT_FOUND;
    }
    
    // Validate allocation header
    slab_alloc_header_t* header = (slab_alloc_header_t*)obj_start;
    if (header->magic != SLAB_ALLOC_MAGIC) {
        LOGF("[SLAB ERROR] Invalid allocation magic (double-free or corruption)\n");
        g_stats.corruption_detected++;
        return SLAB_ERR_CORRUPTION;
    }
    if (header->cache_id != cache->cache_id) {
        LOGF("[SLAB ERROR] Cache ID mismatch\n");
        return SLAB_ERR_CORRUPTION;
    }
    
    // Convert to free object
    slab_free_obj_t* free_obj = (slab_free_obj_t*)obj_start;
    free_obj->magic = SLAB_FREE_MAGIC;
    free_obj->red_zone_pre = SLAB_RED_ZONE;
    free_obj->red_zone_post = SLAB_RED_ZONE;
    free_obj->next = slab->freelist;
    slab->freelist = free_obj;
    
    slab->in_use--;
    
    // Update statistics
    cache->stats.total_frees++;
    cache->stats.active_objects--;
    
    // Move slab to appropriate list if needed
    if (slab->in_use == 0) {
        // Slab is now empty
        if (slab == cache->slabs_partial) {
            slab_move_to_list(&cache->slabs_partial, &cache->slabs_empty, slab);
            cache->stats.partial_slabs--;
            cache->stats.empty_slabs++;
        } else if (slab == cache->slabs_full) {
            slab_move_to_list(&cache->slabs_full, &cache->slabs_empty, slab);
            cache->stats.full_slabs--;
            cache->stats.empty_slabs++;
        }
        
        // Here, technically we free empty slabs if we have too many
        // and keep at least one empty slab for quick allocations, BUT
        // this could fragment memory. Eh, it is what it is, can't be 
        // perfect everywhere, I am one person.
        if (cache->stats.empty_slabs > 1) {
            slab_remove_from_list(&cache->slabs_empty, slab);
            slab_free_page(slab);
            cache->stats.empty_slabs--;
        }
    } else if (slab->in_use == slab->capacity - 1) {
        // Slab was full, now partial
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
 * slab_cache_stats - Get statistics for a specific cache
 */
void slab_cache_stats(slab_cache_t* cache, slab_cache_stats_t* out_stats) {
    if (!cache_validate(cache) || !out_stats) return;
    *out_stats = cache->stats;
}

/*
 * slab_get_stats - Get global slab allocator statistics
 */
void slab_get_stats(slab_stats_t* out_stats) {
    if (!out_stats) return;
    *out_stats = g_stats;
}

/*
 * slab_dump_stats - Print global slab allocator statistics
 */
void slab_dump_stats(void) {
    if (!g_slab_initialized) {
        LOGF("[SLAB] Not initialized\n");
        return;
    }
    
    LOGF("=== Slab Allocator Statistics ===\n");
    LOGF("Total slabs: %lu\n", g_stats.total_slabs);
    LOGF("Total PMM bytes: %lu (%.2f MiB)\n",
           g_stats.total_pmm_bytes,
           g_stats.total_pmm_bytes / (1024.0 * 1024.0));
    LOGF("Active caches: %lu (dynamic allocation)\n", g_stats.cache_count);
    LOGF("Corruption events: %lu\n", g_stats.corruption_detected);
    LOGF("=================================\n");
}

/*
 * slab_cache_dump - Print detailed statistics for a specific cache
 */
void slab_cache_dump(slab_cache_t* cache) {
    if (!cache_validate(cache)) return;
    
    LOGF("=== Slab Cache: %s ===\n", cache->name);
    LOGF("User object size: %zu bytes\n", cache->user_size);
    LOGF("Total object size: %zu bytes (align: %zu)\n", cache->obj_size, cache->align);
    LOGF("Cache ID: %u\n", cache->cache_id);
    LOGF("\nStatistics:\n");
    LOGF("  Total allocations: %lu\n", cache->stats.total_allocs);
    LOGF("  Total frees:       %lu\n", cache->stats.total_frees);
    LOGF("  Active objects:    %lu\n", cache->stats.active_objects);
    LOGF("  Slab count:        %lu\n", cache->stats.slab_count);
    LOGF("  Empty slabs:       %lu\n", cache->stats.empty_slabs);
    LOGF("  Partial slabs:     %lu\n", cache->stats.partial_slabs);
    LOGF("  Full slabs:        %lu\n", cache->stats.full_slabs);
    
    // Calculate memory usage
    uint64_t total_bytes = cache->stats.slab_count * PAGE_SIZE;
    uint64_t used_bytes = cache->stats.active_objects * cache->obj_size;
    double utilization = total_bytes > 0 ? (double)used_bytes / total_bytes * 100.0 : 0.0;
    
    LOGF("\nMemory usage:\n");
    LOGF("  Total:        %lu bytes (%.2f KiB)\n",
           total_bytes, total_bytes / 1024.0);
    LOGF("  Used:         %lu bytes (%.2f KiB)\n",
           used_bytes, used_bytes / 1024.0);
    LOGF("  Utilization:  %.1f%%\n", utilization);
    LOGF("========================\n");
}

/*
 * slab_dump_all_caches - Print statistics for all caches
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
 * slab_verify_integrity - Verify integrity of all caches and slabs
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
        
        // Verify each slab list
        slab_t* lists[] = {cache->slabs_empty, cache->slabs_partial, cache->slabs_full};
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
                
                // Verify slab belongs to this cache
                if (slab->cache != cache) {
                    LOGF("[SLAB VERIFY] Cache '%s': slab %d belongs to wrong cache\n",
                           cache->name, slab_num);
                    all_ok = false;
                }
                
                // Verify in_use count makes sense for list type
                if (i == 0 && slab->in_use != 0) {
                    LOGF("[SLAB VERIFY] Cache '%s': empty list has slab with in_use=%u\n",
                           cache->name, slab->in_use);
                    all_ok = false;
                }
                if (i == 1 && (slab->in_use == 0 || slab->in_use >= slab->capacity)) {
                    LOGF("[SLAB VERIFY] Cache '%s': partial list has slab with in_use=%u/%u\n",
                           cache->name, slab->in_use, slab->capacity);
                    all_ok = false;
                }
                if (i == 2 && slab->in_use != slab->capacity) {
                    LOGF("[SLAB VERIFY] Cache '%s': full list has slab with in_use=%u/%u\n",
                           cache->name, slab->in_use, slab->capacity);
                    all_ok = false;
                }
                
                // Verify freelist consistency
                uint32_t free_count = 0;
                slab_free_obj_t* free_obj = (slab_free_obj_t*)slab->freelist;
                
                while (free_obj && free_count < slab->capacity) {
                    if (!validate_free_obj(free_obj)) {
                        LOGF("[SLAB VERIFY] Cache '%s': slab %d has corrupted free object\n",
                               cache->name, slab_num);
                        all_ok = false;
                        break;
                    }
                    free_count++;
                    free_obj = free_obj->next;
                }
                
                if (free_count > slab->capacity) {
                    LOGF("[SLAB VERIFY] Cache '%s': slab %d freelist has too many objects\n",
                           cache->name, slab_num);
                    all_ok = false;
                }
                
                uint32_t expected_free = slab->capacity - slab->in_use;
                if (free_count != expected_free) {
                    LOGF("[SLAB VERIFY] Cache '%s': slab %d free count mismatch (got %u, expected %u)\n",
                           cache->name, slab_num, free_count, expected_free);
                    all_ok = false;
                }
                
                slab = slab->next;
                
                // Prevent infinite loops
                if (slab_num > 10000) {
                    LOGF("[SLAB VERIFY] Cache '%s': %s list has too many slabs (loop?)\n",
                           cache->name, list_names[i]);
                    all_ok = false;
                    break;
                }
            }
        }
        
        cache = cache->next;
        
        // Prevent infinite loops
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
 * slab_cache_obj_size - Get object size for a cache
 */
size_t slab_cache_obj_size(slab_cache_t* cache) {
    if (!cache_validate(cache)) return 0;
    return cache->user_size;  // Return user-visible size, not internal (imagine that)
}

/*
 * slab_cache_name - Get name of a cache
 */
const char* slab_cache_name(slab_cache_t* cache) {
    if (!cache_validate(cache)) return NULL;
    return cache->name;
}

#pragma endregion