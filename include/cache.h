#ifndef CACHE_H
#define CACHE_H

#include "protocol.h" // For MAX_FILENAME_LEN
#include "common.h"
#include <pthread.h>
#include <time.h>

#define CACHE_SIZE 16 // We can store 16 recent results

typedef struct {
    char filename[MAX_FILENAME];
    int ss_index;
    int is_valid;       // 0 = empty slot, 1 = valid data
    time_t last_used_time; // For LRU eviction
} CacheEntry;

// --- Cache API ---

/**
 * @brief Initializes the cache.
 */
void init_cache();

/**
 * @brief Tries to find a file in the cache.
 * @param filename The name of the file.
 * @return The ss_index if found (cache hit), or -1 if not found (cache miss).
 */
int cache_lookup(const char* filename);

/**
 * @brief Adds or updates an entry in the cache.
 * If the cache is full, it evicts the least recently used entry.
 * @param filename The name of the file.
 * @param ss_index The storage server index.
 */
void cache_add(const char* filename, int ss_index);

/**
 * @brief (Good to have later) Removes an entry from the cache.
 * Call this if a file is deleted or moved.
 * @param filename The name of the file to invalidate.
 */
void cache_invalidate(const char* filename);

#endif // CACHE_H