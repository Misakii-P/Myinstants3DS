#pragma once

#include <3ds/types.h>
#include <stddef.h>

#define API_MAX_RESULTS 36
#define API_ID_LEN      128
#define API_TITLE_LEN   256
#define API_URL_LEN     512
#define API_DESC_LEN    1024

#define HTTP_UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "     \
                "AppleWebKit/537.36 (KHTML, like Gecko) "        \
                "Chrome/120.0.0.0 Safari/537.36"

typedef struct {
    char id[API_ID_LEN];
    char title[API_TITLE_LEN];
    char mp3[API_URL_LEN];
} Sound;

typedef struct {
    Sound items[API_MAX_RESULTS];
    int count;
} SoundList;

typedef struct {
    char id[API_ID_LEN];
    char title[API_TITLE_LEN];
    char url[API_URL_LEN];
    char mp3[API_URL_LEN];
    char description[API_DESC_LEN];
    char favorites[32];
    char views[32];
    char uploader_name[64];
} SoundDetail;

int api_search(const char *query, SoundList *out, char *errmsg, size_t errlen);
int api_trending(const char *region, SoundList *out, char *errmsg, size_t errlen);
int api_detail(const char *id, SoundDetail *out, char *errmsg, size_t errlen);

int api_http_get(const char *url, char **out_body, size_t *out_len);

void api_curl_setopts(void *curl);

Result api_soc_init(void);
void   api_soc_exit(void);

void api_curl_ensure(void);
void api_curl_cleanup(void);
