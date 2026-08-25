#include "api.h"

#include <3ds.h>
#include <curl/curl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool g_curl_ready = false;
static char g_http_err[160] = "";

void api_curl_ensure(void) {
    if (!g_curl_ready) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        g_curl_ready = true;
    }
}

void api_curl_cleanup(void) {
    curl_global_cleanup();
    g_curl_ready = false;
}

typedef struct {
    char *buf;
    size_t len, cap;
    int too_big;
} MemBuf;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    MemBuf *m = (MemBuf *)userdata;
    size_t total = size * nmemb;
    if (total == 0)
        return 0;
    if (m->len + total + 1 > m->cap) {
        size_t ncap = m->cap ? m->cap * 2 : 65536;
        while (m->len + total + 1 > ncap)
            ncap *= 2;
        if (ncap > (size_t)(1 << 22))
            ncap = (size_t)(1 << 22);
        if (m->len + total + 1 > ncap) {
            m->too_big = 1;
            return 0;
        }
        char *nb = (char *)realloc(m->buf, ncap);
        if (!nb)
            return 0;
        m->buf = nb;
        m->cap = ncap;
    }
    memcpy(m->buf + m->len, ptr, total);
    m->len += total;
    return total;
}

void api_curl_setopts(void *curl_handle) {
    CURL *e = (CURL *)curl_handle;
    curl_easy_setopt(e, CURLOPT_USERAGENT, HTTP_UA);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(e, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(e, CURLOPT_IPRESOLVE, (long)CURL_IPRESOLVE_V4);
}

int api_http_get(const char *url, char **out_body, size_t *out_len) {
    api_curl_ensure();

    *out_body = NULL;
    *out_len = 0;

    CURL *e = curl_easy_init();
    if (!e)
        return -1;

    MemBuf m;
    memset(&m, 0, sizeof(m));
    m.cap = 65536;
    m.buf = (char *)malloc(m.cap);
    int ret = -1;

    if (!m.buf)
        goto done;

    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, &m);
    api_curl_setopts(e);

    CURLcode rc = curl_easy_perform(e);

    long code = 0;
    curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &code);

    if (rc != CURLE_OK)
        snprintf(g_http_err, sizeof(g_http_err), "curl %d (%s)", (int)rc,
                 curl_easy_strerror(rc));
    else
        snprintf(g_http_err, sizeof(g_http_err), "http %ld", code);

    if (rc == CURLE_OK && code == 200 && m.len > 0 && !m.too_big) {
        m.buf[m.len] = '\0';
        *out_body = m.buf;
        *out_len = m.len;
        ret = 0;
        m.buf = NULL;
    } else if (code != 0) {
        ret = -(int)code;
    } else if (rc == CURLE_OPERATION_TIMEDOUT || rc == CURLE_COULDNT_CONNECT ||
               rc == CURLE_COULDNT_RESOLVE_HOST || rc == CURLE_COULDNT_RESOLVE_PROXY) {
        ret = -1000 - (int)rc;
    }

done:
    free(m.buf);
    curl_easy_cleanup(e);
    return ret;
}

#define API_BASE "http://myinstants-api.vercel.app"

static u32 *g_soc_buffer = NULL;

Result api_soc_init(void) {
    if (g_soc_buffer)
        return 0;
    g_soc_buffer = (u32 *)memalign(0x1000, 0x100000);
    if (!g_soc_buffer)
        return -1;
    return socInit(g_soc_buffer, 0x100000);
}

void api_soc_exit(void) {
    if (g_soc_buffer) {
        socExit();
        free(g_soc_buffer);
        g_soc_buffer = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Tiny JSON helpers (tailored to myinstants-api responses)            */
/* ------------------------------------------------------------------ */

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
        p++;
    return p;
}

/* Find "key" : "<string>" inside [obj_start,obj_end); unescape into out. */
static bool json_obj_string(const char *obj_start, const char *obj_end,
                            const char *key, char *out, size_t outsz) {
    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = obj_start;
    size_t plen = strlen(pat);
    while ((p = (const char *)memchr(p, '"', (size_t)(obj_end - p))) != NULL) {
        if ((size_t)(obj_end - p) < plen)
            break;
        if (memcmp(p, pat, plen) != 0) {
            p++;
            continue;
        }
        const char *v = skip_ws(p + plen);
        if (*v != ':') {
            p++;
            continue;
        }
        v = skip_ws(v + 1);
        if (*v != '"') {
            p += plen;
            continue;
        }
        v++; /* past opening quote */
        size_t o = 0;
        while (v < obj_end && *v != '"' && o + 4 < outsz) {
            char c = *v;
            if (c == '\\') {
                v++;
                if (v >= obj_end)
                    break;
                switch (*v) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u': {
                    if (v + 4 < obj_end) {
                        unsigned cp = 0;
                        for (int i = 1; i <= 4; i++) {
                            char h = v[i];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        }
                        v += 4;
                        /* encode UTF-8 (BMP only, good enough here) */
                        if (cp < 0x80) {
                            out[o++] = (char)cp;
                        } else if (cp < 0x800) {
                            out[o++] = (char)(0xC0 | (cp >> 6));
                            out[o++] = (char)(0x80 | (cp & 0x3F));
                        } else {
                            out[o++] = (char)(0xE0 | (cp >> 12));
                            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            out[o++] = (char)(0x80 | (cp & 0x3F));
                        }
                        c = '\0';
                    }
                    break;
                }
                default: c = *v; break; /* \" \\ \/ */
                }
                v++;
            } else {
                v++;
            }
            if (c != '\0')
                out[o++] = c;
        }
        out[o] = '\0';
        return true;
    }
    return false;
}

/* Decode the HTML entities the scraper leaves in titles/descriptions. */
void api_html_decode(char *s) {
    static const struct { const char *ent; const char *rep; } named[] = {
        {"&amp;", "&"},  {"&lt;", "<"},   {"&gt;", ">"},
        {"&quot;", "\""},{"&apos;", "'"}, {"&#39;", "'"},
        {"&#x27;", "'"}, {"&nbsp;", " "},
    };
    for (char *p = s; *p; p++) {
        if (*p != '&')
            continue;
        int replaced = 0;
        for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
            size_t l = strlen(named[i].ent);
            if (strncmp(p, named[i].ent, l) == 0) {
                size_t rl = strlen(named[i].rep);
                memmove(p + rl, p + l, strlen(p + l) + 1);
                memcpy(p, named[i].rep, rl);
                replaced = 1;
                break;
            }
        }
        if (!replaced && p[1] == '#') {
            unsigned cp = 0;
            char *q = p + 2;
            int hex = (*q == 'x' || *q == 'X');
            if (hex)
                q++;
            while (*q >= '0' && *q <= '9')
                cp = cp * 10 + (unsigned)(*q++ - '0'), replaced = 1;
            if (hex) {
                replaced = 0;
                cp = 0;
                q = p + 3;
                while ((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'f') ||
                       (*q >= 'A' && *q <= 'F')) {
                    char h = *q++;
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else cp |= (unsigned)(h - 'A' + 10);
                    replaced = 1;
                }
            }
            if (replaced && *q == ';' && cp > 0 && cp < 0x800) {
                q++;
                char enc[3];
                int n;
                if (cp < 0x80) {
                    enc[0] = (char)cp;
                    n = 1;
                } else {
                    enc[0] = (char)(0xC0 | (cp >> 6));
                    enc[1] = (char)(0x80 | (cp & 0x3F));
                    n = 2;
                }
                memmove(p + n, q, strlen(q) + 1);
                memcpy(p, enc, (size_t)n);
            }
        }
    }
}

static void rewrite_https(char *url) {
    /* "https://..." -> "http://..." (Cloudflare blocks curl TLS via JA3 fingerprint) */
    if (strncmp(url, "https://", 8) == 0)
        memmove(url + 4, url + 5, strlen(url + 5) + 1);
}

/* Locate the data array: returns pointer at '[' or NULL. */
static const char *find_data_array(const char *body) {
    const char *d = strstr(body, "\"data\"");
    if (!d)
        return NULL;
    d = strchr(d, '[');
    return d ? d + 1 : NULL;
}

/* Iterate top-level objects of an array. Returns object end or NULL.
   Advances *cursor past the object. */
static const char *next_object(const char **cursor, const char **start) {
    const char *p = *cursor;
    while (*p && *p != '{' && *p != ']')
        p++;
    if (*p != '{')
        return NULL;
    *start = p;
    int depth = 0;
    bool in_str = false;
    while (*p) {
        if (in_str) {
            if (*p == '\\')
                p++;
            else if (*p == '"')
                in_str = false;
        } else if (*p == '"') {
            in_str = true;
        } else if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0) {
                *cursor = p + 1;
                return p;
            }
        }
        p++;
    }
    return NULL;
}

static void fill_sound(Sound *s, const char *os, const char *oe) {
    json_obj_string(os, oe, "id", s->id, sizeof(s->id));
    json_obj_string(os, oe, "title", s->title, sizeof(s->title));
    json_obj_string(os, oe, "mp3", s->mp3, sizeof(s->mp3));
    api_html_decode(s->title);
    rewrite_https(s->mp3);
}

static int fetch_list(const char *url, SoundList *out, char *errmsg,
                      size_t errlen) {
    memset(out, 0, sizeof(*out));
    char *body = NULL;
    size_t blen = 0;
    int rc = api_http_get(url, &body, &blen);
    if (rc != 0) {
        snprintf(errmsg, errlen, "HTTP error %d [%s]", rc, g_http_err);
        return rc;
    }
    const char *arr = find_data_array(body);
    if (!arr) {
        snprintf(errmsg, errlen, "Bad response");
        free(body);
        return -5;
    }
    const char *cur = arr;
    const char *os, *oe;
    while (out->count < API_MAX_RESULTS &&
           (oe = next_object(&cur, &os)) != NULL) {
        fill_sound(&out->items[out->count], os, oe);
        if (out->items[out->count].mp3[0])
            out->count++;
    }
    free(body);
    if (out->count == 0) {
        snprintf(errmsg, errlen, "No results");
        return -6;
    }
    return 0;
}

int api_search(const char *query, SoundList *out, char *errmsg, size_t errlen) {
    char url[API_URL_LEN + 64];
    char esc[API_URL_LEN];
    size_t j = 0;
    for (size_t i = 0; query[i] && j + 4 < sizeof(esc); i++) {
        unsigned char c = (unsigned char)query[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~')
            esc[j++] = (char)c;
        else
            j += (size_t)snprintf(esc + j, 4, "%%%02X", c);
    }
    esc[j] = '\0';
    snprintf(url, sizeof(url), "%s/search?q=%s", API_BASE, esc);
    return fetch_list(url, out, errmsg, errlen);
}

int api_trending(const char *region, SoundList *out, char *errmsg,
                 size_t errlen) {
    char url[API_URL_LEN];
    snprintf(url, sizeof(url), "%s/trending?q=%s", API_BASE,
             region && region[0] ? region : "us");
    return fetch_list(url, out, errmsg, errlen);
}

static void url_encode_id(const char *id, char *dst, size_t dstsz) {
    size_t j = 0;
    for (size_t i = 0; id[i] && j + 4 < dstsz; i++) {
        unsigned char c = (unsigned char)id[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-')
            dst[j++] = (char)c;
        else
            j += (size_t)snprintf(dst + j, 4, "%%%02X", c);
    }
    dst[j] = '\0';
}

int api_detail(const char *id, SoundDetail *out, char *errmsg, size_t errlen) {
    memset(out, 0, sizeof(*out));
    char eid[160];
    url_encode_id(id, eid, sizeof(eid));
    char url[API_URL_LEN];
    snprintf(url, sizeof(url), "%s/detail?id=%s", API_BASE, eid);

    char *body = NULL;
    size_t blen = 0;
    int rc = api_http_get(url, &body, &blen);
    if (rc != 0) {
        snprintf(errmsg, errlen, "Fetch failed (%d) [%s]", rc, g_http_err);
        return rc;
    }
    /* data is a single object */
    const char *d = strstr(body, "\"data\"");
    if (!d) {
        snprintf(errmsg, errlen, "Sound not found");
        free(body);
        return -6;
    }
    d = strchr(d, '{');
    if (!d) {
        free(body);
        return -5;
    }
    const char *cur = d;
    const char *os, *oe;
    if (!(oe = next_object(&cur, &os))) {
        free(body);
        return -5;
    }
    json_obj_string(os, oe, "id", out->id, sizeof(out->id));
    json_obj_string(os, oe, "title", out->title, sizeof(out->title));
    json_obj_string(os, oe, "url", out->url, sizeof(out->url));
    json_obj_string(os, oe, "mp3", out->mp3, sizeof(out->mp3));
    json_obj_string(os, oe, "description", out->description,
                    sizeof(out->description));
    json_obj_string(os, oe, "favorites", out->favorites, sizeof(out->favorites));
    json_obj_string(os, oe, "views", out->views, sizeof(out->views));
    json_obj_string(os, oe, "username", out->uploader_name,
                    sizeof(out->uploader_name));
    api_html_decode(out->title);
    api_html_decode(out->description);
    rewrite_https(out->mp3);
    free(body);
    if (!out->mp3[0]) {
        snprintf(errmsg, errlen, "No audio in response");
        return -7;
    }
    return 0;
}
