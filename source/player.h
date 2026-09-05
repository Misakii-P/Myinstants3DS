#pragma once

#include <stdbool.h>
#include <stddef.h>

struct SoundCache;

/* Attach a cache instance (NULL disables caching). */
void player_set_cache(struct SoundCache *cache);

/* Start background download of url.  id is used as cache key (may be NULL).
   Returns: 0 = download started (non-blocking), 1 = cache hit (playing now). */
int player_play_url(const char *url, const char *id,
                    char *errmsg, size_t errlen);

/* Check if background download finished and init playback.
   Returns: -1=still loading, 0=now playing, >0=error code */
int player_poll_play(char *errmsg, size_t errlen);

void player_stop(void);
bool player_is_playing(void);
