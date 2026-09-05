#include "player.h"

#include <3ds.h>
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

#include "api.h"
#include "cache.h"

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"

#define MAX_MP3_BYTES   (2u << 20) /* 2 MiB compressed (most sounds <200KB) */
#define CHUNK_FRAMES    4096       /* per wave buffer frames (~16 KiB stereo s16) */
#define NUM_BUFFERS     2

static u8 *g_src = NULL;
static size_t g_src_len = 0;
static bool g_src_is_cache = false;
static drmp3 g_dec;
static bool g_dec_open = false;

static s16 *g_buf[NUM_BUFFERS];
static ndspWaveBuf g_wb[NUM_BUFFERS];

static bool g_active = false;
static bool g_eof = false;
static u32 g_rate = 44100;
static int g_channels = 2;

static SoundCache *g_cache = NULL;

void player_set_cache(SoundCache *cache) {
    g_cache = cache;
}

/* Background download state */
typedef enum {
    DL_IDLE, DL_LOADING, DL_READY, DL_ERROR
} DlState;
static volatile DlState g_dl_state = DL_IDLE;
static Thread g_dl_thread = NULL;
static int g_dl_rc = 0;
static char g_dl_err[160] = "";
static char g_dl_url[512] = "";
static char g_dl_id[API_ID_LEN] = "";

static void free_all(void) {
    if (g_dec_open) {
        drmp3_uninit(&g_dec);
        g_dec_open = false;
    }
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (g_buf[i]) {
            linearFree(g_buf[i]);
            g_buf[i] = NULL;
        }
    }
    memset(g_wb, 0, sizeof(g_wb));
    if (g_src_is_cache) {
        /* g_src points to cache-owned memory — do not free */
        g_src = NULL;
        g_src_is_cache = false;
    } else if (g_src) {
        linearFree(g_src);
        g_src = NULL;
    }
    g_src_len = 0;
    g_active = false;
    g_eof = false;
    g_channels = 2;
}

void player_stop(void) {
    /* Join any pending download thread to prevent use-after-free on g_src */
    if (g_dl_thread) {
        threadJoin(g_dl_thread, U64_MAX);
        threadFree(g_dl_thread);
        g_dl_thread = NULL;
        g_dl_state = DL_IDLE;
    }
    if (!g_active && !g_src)
        return;
    ndspChnWaveBufClear(0);
    ndspChnReset(0);
    free_all();
}

bool player_is_playing(void) {
    if (!g_active)
        return false;

    bool pending = false;
    for (int i = 0; i < NUM_BUFFERS; i++) {
        ndspWaveBuf *wb = &g_wb[i];
        bool finished = (wb->status == NDSP_WBUF_DONE ||
                         wb->status == NDSP_WBUF_FREE);
        if (!g_eof && finished) {
            u64 got =
                drmp3_read_pcm_frames_s16(&g_dec, CHUNK_FRAMES, g_buf[i]);
            if (got == 0) {
                g_eof = true;
            } else {
                /* Zero remaining buffer to avoid stale/noise data */
                size_t frame_bytes = g_channels * sizeof(s16);
                if (got < CHUNK_FRAMES) {
                    memset(g_buf[i] + got * g_channels, 0,
                           (CHUNK_FRAMES - got) * frame_bytes);
                }
                wb->data_vaddr = g_buf[i];
                wb->nsamples = (u32)got;
                DSP_FlushDataCache(g_buf[i],
                                   (u32)(CHUNK_FRAMES * frame_bytes));
                ndspChnWaveBufAdd(0, wb);
                pending = true;
                continue;
            }
        }
        if (!finished || !g_eof)
            pending = true;
    }

    if (!pending) {
        player_stop();
        return false;
    }
    return true;
}

/* curl writes into a fixed linear-memory buffer */
typedef struct {
    u8 *dst;
    size_t have, cap;
} FixedBuf;

static size_t fixed_write_cb(char *ptr, size_t size, size_t nmemb,
                             void *userdata) {
    FixedBuf *fb = (FixedBuf *)userdata;
    size_t total = size * nmemb;
    if (total == 0)
        return 0;
    if (fb->have + total > fb->cap)
        return 0;
    memcpy(fb->dst + fb->have, ptr, total);
    fb->have += total;
    return total;
}

static void dl_thread_func(void *arg) {
    (void)arg;

    api_curl_ensure();

    g_src = (u8 *)linearAlloc(MAX_MP3_BYTES);
    if (!g_src) {
        snprintf(g_dl_err, sizeof(g_dl_err), "out of memory");
        g_dl_rc = -2;
        g_dl_state = DL_ERROR;
        return;
    }
    FixedBuf fb = { g_src, 0, MAX_MP3_BYTES };

    CURL *e = curl_easy_init();
    if (!e) {
        linearFree(g_src);
        g_src = NULL;
        snprintf(g_dl_err, sizeof(g_dl_err), "init failed");
        g_dl_rc = -3;
        g_dl_state = DL_ERROR;
        return;
    }
    curl_easy_setopt(e, CURLOPT_URL, g_dl_url);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, fixed_write_cb);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, &fb);
    api_curl_setopts(e);

    CURLcode rc = curl_easy_perform(e);
    long code = 0;
    curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(e);

    if (rc != CURLE_OK) {
        linearFree(g_src);
        g_src = NULL;
        snprintf(g_dl_err, sizeof(g_dl_err), "download failed");
        g_dl_rc = -(100 + (int)rc);
        g_dl_state = DL_ERROR;
        return;
    }
    if (code != 200 && code != 206) {
        linearFree(g_src);
        g_src = NULL;
        snprintf(g_dl_err, sizeof(g_dl_err), "audio HTTP %ld", code);
        g_dl_rc = -(int)code;
        g_dl_state = DL_ERROR;
        return;
    }
    g_src_len = fb.have;
    if (g_src_len == 0) {
        linearFree(g_src);
        g_src = NULL;
        snprintf(g_dl_err, sizeof(g_dl_err), "empty download");
        g_dl_rc = -6;
        g_dl_state = DL_ERROR;
        return;
    }

    /* Cache the downloaded MP3 for future instant playback */
    if (g_cache && g_dl_id[0]) {
        cache_put(g_cache, g_dl_id, g_src, g_src_len);
    }

    /* Download complete — main thread will init playback */
    g_dl_rc = 0;
    g_dl_state = DL_READY;
}

/* Init ndsp playback from downloaded data. Called from main thread. */
static int init_playback(char *errmsg, size_t errlen) {
    if (!drmp3_init_memory(&g_dec, g_src, g_src_len, NULL)) {
        snprintf(errmsg, errlen, "not a valid MP3");
        return -7;
    }
    g_dec_open = true;

    g_rate = g_dec.sampleRate ? g_dec.sampleRate : 44100;
    /* Clamp to ndsp-supported range */
    if (g_rate < 8000) g_rate = 8000;
    if (g_rate > 140000) g_rate = 44100;

    g_channels = g_dec.channels ? g_dec.channels : 2;
    if (g_channels < 1 || g_channels > 2) g_channels = 2;

    for (int i = 0; i < NUM_BUFFERS; i++) {
        g_buf[i] = (s16 *)linearAlloc(CHUNK_FRAMES * g_channels * sizeof(s16));
        if (!g_buf[i]) {
            snprintf(errmsg, errlen, "out of memory");
            return -9;
        }
        memset(g_buf[i], 0, CHUNK_FRAMES * g_channels * sizeof(s16));
    }

    ndspChnReset(0);
    ndspSetOutputMode(g_channels == 1 ? NDSP_OUTPUT_MONO : NDSP_OUTPUT_STEREO);
    ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(0, (float)g_rate);
    ndspChnSetFormat(0, g_channels == 1 ? NDSP_FORMAT_MONO_PCM16
                                        : NDSP_FORMAT_STEREO_PCM16);
    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0f;
    if (g_channels == 2) mix[1] = 1.0f;
    ndspChnSetMix(0, mix);

    memset(g_wb, 0, sizeof(g_wb));
    for (int i = 0; i < NUM_BUFFERS; i++)
        g_wb[i].status = NDSP_WBUF_DONE;

    g_active = true;
    player_is_playing(); /* prime both buffers */
    return 0;
}

/* Start background download. Returns 0 (non-blocking) or 1 (cache hit, playing). */
int player_play_url(const char *url, const char *id,
                    char *errmsg, size_t errlen) {
    errmsg[0] = '\0';

    /* Stop any current playback or pending download */
    player_stop();

    /* Try cache first — use pointer directly, no copy */
    if (g_cache && id && id[0]) {
        const unsigned char *cdata;
        size_t csize;
        if (cache_get(g_cache, id, &cdata, &csize)) {
            g_src = (u8 *)cdata;
            g_src_len = csize;
            g_src_is_cache = true;
            int rc = init_playback(errmsg, errlen);
            if (rc == 0)
                return 1; /* cache hit — playing now */
            /* init failed — fall through to normal download */
            g_src = NULL;
            g_src_is_cache = false;
        }
    }

    /* Copy URL for the background thread */
    strncpy(g_dl_url, url, sizeof(g_dl_url) - 1);
    g_dl_url[sizeof(g_dl_url) - 1] = '\0';
    if (id) {
        strncpy(g_dl_id, id, sizeof(g_dl_id) - 1);
        g_dl_id[sizeof(g_dl_id) - 1] = '\0';
    } else {
        g_dl_id[0] = '\0';
    }
    g_dl_err[0] = '\0';
    g_dl_rc = 0;
    g_dl_state = DL_LOADING;

    g_dl_thread = threadCreate(dl_thread_func, NULL, 0x10000, 0x30, -1, false);
    if (!g_dl_thread) {
        g_dl_state = DL_IDLE;
        snprintf(errmsg, errlen, "thread failed");
        return -1;
    }

    return 0; /* non-blocking — caller should show loading indicator */
}

/* Check if background download finished and init playback.
   Returns: -1=still loading, 0=now playing, >0=error code */
int player_poll_play(char *errmsg, size_t errlen) {
    if (g_dl_state == DL_LOADING)
        return -1; /* still downloading */

    if (g_dl_state == DL_READY) {
        /* Download done — join thread, init playback on main thread */
        if (g_dl_thread) {
            threadJoin(g_dl_thread, U64_MAX);
            threadFree(g_dl_thread);
            g_dl_thread = NULL;
        }
        g_dl_state = DL_IDLE;

        int rc = init_playback(errmsg, errlen);
        if (rc != 0) {
            player_stop();
            return rc;
        }
        return 0;
    }

    if (g_dl_state == DL_ERROR) {
        if (g_dl_thread) {
            threadJoin(g_dl_thread, U64_MAX);
            threadFree(g_dl_thread);
            g_dl_thread = NULL;
        }
        if (errmsg && errlen)
            snprintf(errmsg, errlen, "%s", g_dl_err[0] ? g_dl_err : "download failed");
        int rc = g_dl_rc;
        g_dl_state = DL_IDLE;
        return rc;
    }

    return 0; /* DL_IDLE — nothing happening */
}
