#include "cache.h"

#include <3ds.h>
#include <stdlib.h>
#include <string.h>

void cache_init(SoundCache *cache) {
    memset(cache, 0, sizeof(*cache));
    cache->tick = 1;
}

void cache_free(SoundCache *cache) {
    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        if (cache->entries[i].occupied && cache->entries[i].data) {
            linearFree(cache->entries[i].data);
            cache->entries[i].data = NULL;
        }
    }
    memset(cache, 0, sizeof(*cache));
}

static int find_entry(SoundCache *cache, const char *id) {
    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        if (cache->entries[i].occupied &&
            strcmp(cache->entries[i].id, id) == 0)
            return i;
    }
    return -1;
}

static int find_victim(SoundCache *cache) {
    int oldest = -1;
    unsigned long long oldest_tick = 0;
    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        if (!cache->entries[i].occupied)
            return i;
        if (oldest == -1 || cache->entries[i].last_used < oldest_tick) {
            oldest = i;
            oldest_tick = cache->entries[i].last_used;
        }
    }
    return oldest;
}

static void evict(SoundCache *cache, int idx) {
    if (!cache->entries[idx].occupied)
        return;
    cache->total_size -= cache->entries[idx].size;
    linearFree(cache->entries[idx].data);
    cache->entries[idx].data = NULL;
    cache->entries[idx].occupied = false;
    cache->count--;
}

bool cache_get(SoundCache *cache, const char *id,
               const unsigned char **out_data, size_t *out_size) {
    int idx = find_entry(cache, id);
    if (idx < 0)
        return false;

    cache->entries[idx].last_used = cache->tick++;
    *out_data = cache->entries[idx].data;
    *out_size = cache->entries[idx].size;
    return true;
}

void cache_put(SoundCache *cache, const char *id,
               const unsigned char *data, size_t size) {
    if (size == 0 || !id[0])
        return;

    /* Already cached — just update LRU */
    int idx = find_entry(cache, id);
    if (idx >= 0) {
        cache->entries[idx].last_used = cache->tick++;
        return;
    }

    /* Evict until we have room */
    while (cache->total_size + size > CACHE_MAX_SIZE ||
           cache->count >= CACHE_MAX_ENTRIES) {
        int victim = find_victim(cache);
        if (victim < 0)
            break;
        evict(cache, victim);
    }

    /* Find a free slot */
    idx = -1;
    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        if (!cache->entries[i].occupied) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return;

    unsigned char *buf = (unsigned char *)linearAlloc(size);
    if (!buf)
        return;

    memcpy(buf, data, size);
    strncpy(cache->entries[idx].id, id, API_ID_LEN - 1);
    cache->entries[idx].id[API_ID_LEN - 1] = '\0';
    cache->entries[idx].data = buf;
    cache->entries[idx].size = size;
    cache->entries[idx].last_used = cache->tick++;
    cache->entries[idx].occupied = true;
    cache->count++;
    cache->total_size += size;
}
