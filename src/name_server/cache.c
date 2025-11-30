#include "cache.h"
#include "logger.h"
#include <string.h>

static CacheEntry cache[CACHE_SIZE];
static pthread_mutex_t cache_mutex;

void init_cache() {
    pthread_mutex_init(&cache_mutex, NULL);
    memset(cache, 0, sizeof(cache));
    write_log("INIT", "File Cache (%d entries) initialized.", CACHE_SIZE);
}

/**
 * @brief Finds a file in the cache. Updates its timestamp on hit.
 */
int cache_lookup(const char* filename) {
    pthread_mutex_lock(&cache_mutex);

    int found_index = -1;
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (cache[i].is_valid && strcmp(cache[i].filename, filename) == 0) {
            // Cache Hit!
            cache[i].last_used_time = time(NULL);
            found_index = i;
            break;
        }
    }

    pthread_mutex_unlock(&cache_mutex);

    if (found_index != -1) {
        write_log("CACHE", "Cache HIT for '%s'", filename);
        return cache[found_index].ss_index;
    } else {
        write_log("CACHE", "Cache MISS for '%s'", filename);
        return -1;
    }
}

/**
 * @brief Adds a file to the cache, evicting the LRU entry if full.
 */
void cache_add(const char* filename, int ss_index) {
    pthread_mutex_lock(&cache_mutex);

    // 1. Find the slot to replace.
    // We look for either an empty slot or the oldest (LRU) slot.
    int lru_index = 0;
    time_t oldest_time = time(NULL); // Default to now
    int found_empty_slot = 0;

    for (int i = 0; i < CACHE_SIZE; i++) {
        if (!cache[i].is_valid) {
            // Found an empty slot, use it immediately.
            lru_index = i;
            found_empty_slot = 1;
            break;
        }
        if (cache[i].last_used_time < oldest_time) {
            // Found an older entry
            oldest_time = cache[i].last_used_time;
            lru_index = i;
        }
    }

    // 2. Overwrite the chosen slot (lru_index)
    if (found_empty_slot) {
        write_log("CACHE", "Adding '%s' to empty cache slot %d", filename, lru_index);
    } else {
        write_log("CACHE", "Evicting '%s' and adding '%s' to cache slot %d", 
                  cache[lru_index].filename, filename, lru_index);
    }

    strncpy(cache[lru_index].filename, filename, MAX_FILENAME - 1);
    cache[lru_index].ss_index = ss_index;
    cache[lru_index].last_used_time = time(NULL);
    cache[lru_index].is_valid = 1;

    pthread_mutex_unlock(&cache_mutex);
}

/**
 * @brief Invalidates a cache entry (e.g., on file delete).
 */
void cache_invalidate(const char* filename) {
    pthread_mutex_lock(&cache_mutex);
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (cache[i].is_valid && strcmp(cache[i].filename, filename) == 0) {
            cache[i].is_valid = 0;
            write_log("CACHE", "Invalidated '%s' from cache.", filename);
            break;
        }
    }
    pthread_mutex_unlock(&cache_mutex);
}