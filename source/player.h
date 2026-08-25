#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Start background download of url. Returns 0 immediately (non-blocking). */
int player_play_url(const char *url, char *errmsg, size_t errlen);

/* Check if background download finished and init playback.
   Returns: -1=still loading, 0=now playing, >0=error code */
int player_poll_play(char *errmsg, size_t errlen);

void player_stop(void);
bool player_is_playing(void);
