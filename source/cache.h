#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "api.h"

#define CACHE_MAX_ENTRIES 32
#define CACHE_MAX_SIZE    (4u * 1024u * 1024u) /* 4 MiB total */

typedef struct {
    char id[API_ID_LEN];
    unsigned char *data;
    size_t size;
    unsigned long long last_used;
    bool occupied;
} CacheEntry;

typedef struct SoundCache {
    CacheEntry entries[CACHE_MAX_ENTRIES];
    int count;
    size_t total_size;
    unsigned long long tick;
} SoundCache;

void cache_init(SoundCache *cache);
void cache_free(SoundCache *cache);

/* Returns true if found; sets *out_data / *out_size to the cached MP3 bytes. */
bool cache_get(SoundCache *cache, const char *id,
               const unsigned char **out_data, size_t *out_size);

/* Store MP3 data under id. Evicts LRU entries if needed. */
void cache_put(SoundCache *cache, const char *id,
               const unsigned char *data, size_t size);
