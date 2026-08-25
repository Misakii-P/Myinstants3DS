#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "api.h"
#include "player.h"

#define CLR_BG     C2D_Color32(21, 32, 43, 255)
#define CLR_WHITE  C2D_Color32(255, 255, 255, 255)
#define CLR_DIM    C2D_Color32(255, 255, 255, 150)
#define CLR_CURSOR C2D_Color32(255, 255, 255, 160)
#define CLR_PANEL  C2D_Color32(30, 42, 56, 240)
#define CLR_CYAN   C2D_Color32(0, 230, 255, 255)
#define CLR_CYAN_D C2D_Color32(0, 230, 255, 60)
#define CLR_BTN    C2D_Color32(40, 55, 75, 255)
#define CLR_BTN_HI C2D_Color32(55, 75, 100, 255)

#define COLS       5
#define ROWS       2
#define PAGE_SIZE  (COLS * ROWS)
#define CELL_W     80.0f
#define ROW_H      120.0f
#define BTN_HALF   57.0f
#define CPAD_DEAD  30
#define CPAD_MAX   150

static C3D_RenderTarget *g_top, *g_bot;
static C2D_TextBuf g_tbuf;
static C2D_SpriteSheet g_sheet;
static C2D_Image g_btn;
static C2D_Image g_btn_pressed_img;
static C2D_Image g_bm_empty;
static C2D_Image g_bm_filled;
static C2D_Image g_ahint;

static SoundList g_list;
static SoundDetail g_detail;
static int g_sel = 0;
static bool g_show_info = false;
static bool g_have_detail = false;
static int g_page = 0;
static int g_anim = 0;
static bool g_btn_pressed = false;
static bool g_dl_loading = false;
static int g_toast_timer = 0;
static char g_toast_msg[64] = "";

typedef enum { VIEW_HOME, VIEW_BOOKMARKS } View;
static View g_view = VIEW_HOME;

static SoundList g_bookmarks;
static bool g_bm_dirty = false;

#define BOOKMARK_FILE "bookmarks.txt"

typedef enum { NET_IDLE, NET_LOADING, NET_DONE, NET_ERROR } NetState;
static volatile NetState g_net_state = NET_IDLE;
static SoundList g_net_list;
static int g_net_rc = 0;
static char g_net_err[160] = "";
static Thread g_net_thread = NULL;
static bool g_net_is_search = false;
static char g_net_query[128] = "";

static volatile NetState g_info_state = NET_IDLE;
static Thread g_info_thread = NULL;
static int g_info_rc = 0;
static char g_info_err[160] = "";
static char g_info_id[API_ID_LEN];

static void net_thread_func(void *arg) {
    (void)arg;
    if (g_net_is_search)
        g_net_rc = api_search(g_net_query, &g_net_list, g_net_err, sizeof(g_net_err));
    else
        g_net_rc = api_trending("us", &g_net_list, g_net_err, sizeof(g_net_err));
    g_net_state = g_net_rc == 0 ? NET_DONE : NET_ERROR;
}

static void net_start_trending(void) {
    g_net_is_search = false;
    g_net_state = NET_LOADING;
    g_net_thread = threadCreate(net_thread_func, NULL, 0x10000, 0x30, -1, false);
}

static void net_start_search(const char *query) {
    g_net_is_search = true;
    strncpy(g_net_query, query, sizeof(g_net_query) - 1);
    g_net_query[sizeof(g_net_query) - 1] = '\0';
    g_net_state = NET_LOADING;
    g_net_thread = threadCreate(net_thread_func, NULL, 0x10000, 0x30, -1, false);
}

static bool net_poll(void) {
    if (g_net_state == NET_DONE) {
        if (g_net_thread) { threadJoin(g_net_thread, U64_MAX); threadFree(g_net_thread); g_net_thread = NULL; }
        g_list = g_net_list;
        g_sel = 0;
        g_page = 0;
        g_show_info = false;
        g_have_detail = false;
        g_net_state = NET_IDLE;
        return true;
    }
    if (g_net_state == NET_ERROR) {
        if (g_net_thread) { threadJoin(g_net_thread, U64_MAX); threadFree(g_net_thread); g_net_thread = NULL; }
        g_net_state = NET_IDLE;
        return true;
    }
    return false;
}

static void info_thread_func(void *arg) {
    (void)arg;
    char err[160] = "";
    g_info_rc = api_detail(g_info_id, &g_detail, err, sizeof(err));
    if (g_info_rc != 0)
        strncpy(g_info_err, err, sizeof(g_info_err));
    g_info_state = g_info_rc == 0 ? NET_DONE : NET_ERROR;
}

static int max_page(void) {
    if (g_list.count == 0) return 0;
    return (g_list.count - 1) / PAGE_SIZE;
}

static int page_end(void) {
    int e = (g_page + 1) * PAGE_SIZE;
    if (e > g_list.count) e = g_list.count;
    return e;
}

static int items_on_page(void) {
    return page_end() - g_page * PAGE_SIZE;
}

static void truncate_utf8(char *s, size_t maxlen) {
    size_t len = strlen(s);
    if (len <= maxlen)
        return;
    size_t cut = maxlen;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80)
        cut--;
    if (cut > 3) {
        s[cut - 3] = '.';
        s[cut - 2] = '.';
        s[cut - 1] = '.';
        s[cut]     = '\0';
    } else {
        s[cut] = '\0';
    }
}

/* ---- Bookmark persistence ---- */

static void bookmark_load(void) {
    g_bookmarks.count = 0;
    FILE *f = fopen(BOOKMARK_FILE, "r");
    if (!f) return;
    char line[API_URL_LEN + API_TITLE_LEN + API_ID_LEN + 16];
    while (fgets(line, sizeof(line), f) && g_bookmarks.count < API_MAX_RESULTS) {
        Sound *s = &g_bookmarks.items[g_bookmarks.count];
        char *p = line;
        char *id = strsep(&p, "|");
        char *title = strsep(&p, "|");
        char *mp3 = strsep(&p, "\n");
        if (!id || !title || !mp3) continue;
        strncpy(s->id, id, sizeof(s->id) - 1);
        strncpy(s->title, title, sizeof(s->title) - 1);
        strncpy(s->mp3, mp3, sizeof(s->mp3) - 1);
        g_bookmarks.count++;
    }
    fclose(f);
}

static void bookmark_save(void) {
    FILE *f = fopen(BOOKMARK_FILE, "w");
    if (!f) return;
    for (int i = 0; i < g_bookmarks.count; i++) {
        Sound *s = &g_bookmarks.items[i];
        fprintf(f, "%s|%s|%s\n", s->id, s->title, s->mp3);
    }
    fclose(f);
}

static bool is_bookmarked(const char *id) {
    for (int i = 0; i < g_bookmarks.count; i++)
        if (strcmp(g_bookmarks.items[i].id, id) == 0)
            return true;
    return false;
}

static void toggle_bookmark(int idx) {
    if (idx < 0 || idx >= g_list.count) return;
    Sound *src = &g_list.items[idx];
    for (int i = 0; i < g_bookmarks.count; i++) {
        if (strcmp(g_bookmarks.items[i].id, src->id) == 0) {
            memmove(&g_bookmarks.items[i], &g_bookmarks.items[i + 1],
                    (g_bookmarks.count - i - 1) * sizeof(Sound));
            g_bookmarks.count--;
            g_bm_dirty = true;
            if (g_view == VIEW_BOOKMARKS) {
                g_list = g_bookmarks;
                if (g_sel >= g_list.count)
                    g_sel = g_list.count > 0 ? g_list.count - 1 : 0;
                g_page = g_sel / PAGE_SIZE;
            }
            return;
        }
    }
    if (g_bookmarks.count < API_MAX_RESULTS) {
        Sound *dst = &g_bookmarks.items[g_bookmarks.count++];
        *dst = *src;
        g_bm_dirty = true;
    } else {
        strncpy(g_toast_msg, "Bookmark limit reached", sizeof(g_toast_msg));
        g_toast_timer = 90;
    }
}

/* ---- Nav helpers ---- */

static void do_fetch_trending(void) {
    net_start_trending();
}

static void do_search(void) {
    SwkbdState swk;
    char buf[128];
    swkbdInit(&swk, SWKBD_TYPE_NORMAL, 2, sizeof(buf) - 1);
    swkbdSetHintText(&swk, "Search sounds...");
    swkbdSetValidation(&swk, SWKBD_NOTEMPTY, 0, 0);
    if (swkbdInputText(&swk, buf, sizeof(buf)) != SWKBD_BUTTON_CONFIRM)
        return;
    buf[sizeof(buf) - 1] = '\0';
    net_start_search(buf);
}

static void do_play_selected(void) {
    if (g_list.count == 0 || g_sel >= g_list.count)
        return;
    char err[160] = "";
    player_play_url(g_list.items[g_sel].mp3, err, sizeof(err));
}

static void toggle_info(void) {
    if (g_list.count == 0) return;
    if (g_show_info) {
        g_show_info = false;
        return;
    }
    strncpy(g_info_id, g_list.items[g_sel].id, sizeof(g_info_id) - 1);
    g_info_id[sizeof(g_info_id) - 1] = '\0';
    g_info_state = NET_LOADING;
    g_info_thread = threadCreate(info_thread_func, NULL, 0x10000, 0x30, -1, false);
}

static void select_index(int idx) {
    if (g_list.count == 0) return;
    if (idx < 0) idx = 0;
    if (idx >= g_list.count) idx = g_list.count - 1;
    if (idx != g_sel) {
        g_sel = idx;
        int new_page = g_sel / PAGE_SIZE;
        if (new_page != g_page)
            g_page = new_page;
        g_show_info = false;
    }
}

static void move_grid(int dcol, int drow) {
    if (g_list.count == 0) return;
    int cur_slot = g_sel - g_page * PAGE_SIZE;
    int cur_col = cur_slot % COLS;
    int cur_row = cur_slot / COLS;

    int new_col = cur_col + dcol;
    int new_row = cur_row + drow;
    int new_page = g_page;

    if (new_col >= COLS) { new_col = 0; new_page++; }
    else if (new_col < 0) { new_col = COLS - 1; new_page--; }
    if (new_row >= ROWS) { new_row = 0; new_page++; }
    else if (new_row < 0) { new_row = ROWS - 1; new_page--; }

    int mp = max_page();
    if (new_page < 0) new_page = 0;
    if (new_page > mp) new_page = mp;
    g_page = new_page;

    int on_page = items_on_page();
    int new_slot = new_row * COLS + new_col;
    if (new_slot >= on_page) new_slot = on_page - 1;
    if (new_slot < 0) new_slot = 0;
    select_index(g_page * PAGE_SIZE + new_slot);
}

static void goto_page(int page) {
    if (g_list.count == 0) return;
    int mp = max_page();
    if (page < 0) page = 0;
    if (page > mp) page = mp;
    g_page = page;
    g_sel = page * PAGE_SIZE;
    g_show_info = false;
}

static void switch_view(View v) {
    if (v == g_view) return;
    player_stop();
    if (g_bm_dirty) { bookmark_save(); g_bm_dirty = false; }
    g_view = v;
    g_show_info = false;
    g_have_detail = false;
    g_sel = 0;
    g_page = 0;
    if (v == VIEW_HOME) {
        do_fetch_trending();
    } else {
        g_list = g_bookmarks;
    }
}

/* ---- Drawing helpers ---- */

static void draw_round_btn(float x, float y, float w, float h,
                           const char *label, bool pressed) {
    u32 bg = pressed ? CLR_BTN_HI : CLR_BTN;
    float r = 8.0f;
    /* Center body */
    C2D_DrawRectSolid(x + r, y, 0.8f, w - 2 * r, h, bg);
    /* Left cap */
    C2D_DrawRectSolid(x, y + r, 0.8f, r, h - 2 * r, bg);
    /* Right cap */
    C2D_DrawRectSolid(x + w - r, y + r, 0.8f, r, h - 2 * r, bg);
    /* Four corner circles approximated by squares */
    C2D_DrawRectSolid(x, y, 0.8f, r, r, bg);
    C2D_DrawRectSolid(x + w - r, y, 0.8f, r, r, bg);
    C2D_DrawRectSolid(x, y + h - r, 0.8f, r, r, bg);
    C2D_DrawRectSolid(x + w - r, y + h - r, 0.8f, r, r, bg);
    /* Label */
    C2D_Text t;
    C2D_TextFontParse(&t, NULL, g_tbuf, label);
    float tw, th;
    C2D_TextGetDimensions(&t, 0.35f, 0.35f, &tw, &th);
    C2D_DrawText(&t, C2D_WithColor, x + (w - tw) / 2.0f, y + (h - th) / 2.0f,
                 0.85f, 0.35f, 0.35f, CLR_WHITE);
}

/* ---- Render ---- */

static void render_top(void) {
    if (g_list.count == 0)
        return;

    float cell_text_w = CELL_W - 8.0f;
    int page_start = g_page * PAGE_SIZE;

    for (int slot = 0; slot < PAGE_SIZE; slot++) {
        int idx = page_start + slot;
        if (idx >= g_list.count) break;
        int col = slot % COLS;
        int row = slot / COLS;
        float cx = col * CELL_W;
        float ry = row * ROW_H;
        float bx = cx + (CELL_W - BTN_HALF) / 2.0f;

        if (idx == g_sel) {
            float bx2 = cx + 2.0f, by2 = ry + 2.0f;
            float bw = CELL_W - 4.0f, bh = ROW_H - 4.0f, bt = 2.0f;
            C2D_DrawRectSolid(bx2, by2, 0.1f, bw, bt, CLR_CURSOR);
            C2D_DrawRectSolid(bx2, by2 + bh - bt, 0.1f, bw, bt, CLR_CURSOR);
            C2D_DrawRectSolid(bx2, by2, 0.1f, bt, bh, CLR_CURSOR);
            C2D_DrawRectSolid(bx2 + bw - bt, by2, 0.1f, bt, bh, CLR_CURSOR);
        }

        if (g_btn.tex)
            C2D_DrawImageAt(g_btn, bx, ry + 8.0f, 0.5f, NULL,
                            BTN_HALF / 114.0f, BTN_HALF / 114.0f);

        char title[64];
        snprintf(title, sizeof(title), "%s", g_list.items[idx].title);
        if (!strchr(title, ' '))
            truncate_utf8(title, 9);

        C2D_Text t;
        C2D_TextFontParse(&t, NULL, g_tbuf, title);
        float tw, th;
        C2D_TextGetDimensions(&t, 0.34f, 0.34f, &tw, &th);

        float text_x = cx + (CELL_W - tw) / 2.0f;
        if (text_x < cx + 2.0f) text_x = cx + 2.0f;
        u32 color = (idx == g_sel) ? CLR_WHITE : CLR_DIM;

        if (tw > cell_text_w) {
            int len = (int)strlen(title);
            float avg_cw = (len > 0) ? tw / (float)len : 6.0f;
            int chars_per_line = (int)(cell_text_w / avg_cw);
            if (chars_per_line < 1) chars_per_line = 1;
            float ycur = ry + 78.0f;
            int pos = 0;
            while (pos < len && ycur < ry + ROW_H) {
                int line_end = pos + chars_per_line;
                if (line_end > len) line_end = len;
                if (line_end < len) {
                    int back = line_end;
                    while (back > pos && title[back - 1] != ' ') back--;
                    if (back > pos) line_end = back;
                    if (line_end == pos) line_end = pos + chars_per_line;
                }
                char save = title[line_end];
                title[line_end] = '\0';
                C2D_Text lt;
                C2D_TextFontParse(&lt, NULL, g_tbuf, title + pos);
                float lw2, lh2;
                C2D_TextGetDimensions(&lt, 0.34f, 0.34f, &lw2, &lh2);
                float lx = cx + (CELL_W - lw2) / 2.0f;
                C2D_DrawText(&lt, C2D_WithColor, lx, ycur, 0.5f, 0.34f, 0.34f, color);
                title[line_end] = save;
                ycur += lh2;
                pos = line_end;
                while (pos < len && title[pos] == ' ') pos++;
            }
        } else {
            C2D_DrawText(&t, C2D_WithColor, text_x, ry + 78.0f,
                         0.5f, 0.34f, 0.34f, color);
        }
    }

    /* Page arrows */
    float arrow_h = 50.0f, arrow_w = 18.0f;
    float arrow_y = (240.0f - arrow_h) / 2.0f;
    if (g_page > 0) {
        float ax = 6.0f;
        for (int r = 0; r < (int)arrow_h; r++) {
            float dist = fabsf((float)r + 0.5f - arrow_h / 2.0f);
            float w = arrow_w * (1.0f - dist / (arrow_h / 2.0f));
            if (w < 1.0f) w = 1.0f;
            C2D_DrawRectSolid(ax + (arrow_w - w), arrow_y + (float)r,
                              0.6f, w, 1.0f, CLR_DIM);
        }
    }
    if (g_page < max_page()) {
        float ax = 400.0f - 6.0f - arrow_w;
        for (int r = 0; r < (int)arrow_h; r++) {
            float dist = fabsf((float)r + 0.5f - arrow_h / 2.0f);
            float w = arrow_w * (1.0f - dist / (arrow_h / 2.0f));
            if (w < 1.0f) w = 1.0f;
            C2D_DrawRectSolid(ax, arrow_y + (float)r, 0.6f, w, 1.0f, CLR_DIM);
        }
    }
}

#define BIG_X ((320.0f - 114.0f) / 2.0f)
#define BIG_Y 68.0f

/* Nav button layout at top of bottom screen */
#define NAV_Y      4.0f
#define NAV_H      26.0f
#define NAV_HOME_X  10.0f
#define NAV_HOME_W  80.0f
#define NAV_BM_X   106.0f
#define NAV_BM_W   108.0f
#define NAV_SR_X   230.0f
#define NAV_SR_W   80.0f

/* Star button */
#define STAR_SIZE  62.0f
#define STAR_X     (320.0f - STAR_SIZE)
#define STAR_Y     (240.0f - STAR_SIZE)

static void render_bottom(void) {
    /* Nav buttons (always visible) */
    draw_round_btn(NAV_HOME_X, NAV_Y, NAV_HOME_W, NAV_H, "HOME", false);
    draw_round_btn(NAV_BM_X, NAV_Y, NAV_BM_W, NAV_H, "BOOKMARKS", false);
    draw_round_btn(NAV_SR_X, NAV_Y, NAV_SR_W, NAV_H, "SEARCH", false);

    if (g_list.count > 0 && !g_show_info && g_btn.tex) {
        C2D_Image *img = g_btn_pressed ? &g_btn_pressed_img : &g_btn;
        C2D_DrawImageAt(*img, BIG_X, BIG_Y, 0.5f, NULL, 1.0f, 1.0f);

        char title[64];
        snprintf(title, sizeof(title), "%s", g_list.items[g_sel].title);
        truncate_utf8(title, 40);
        C2D_Text t;
        C2D_TextFontParse(&t, NULL, g_tbuf, title);
        float tw, th;
        C2D_TextGetDimensions(&t, 0.40f, 0.40f, &tw, &th);
        float tx = (320.0f - tw) / 2.0f;
        C2D_DrawText(&t, C2D_WithColor, tx, BIG_Y - th - 14.0f,
                     0.5f, 0.40f, 0.40f, CLR_DIM);
    }

    /* Star bookmark button */
    if (g_list.count > 0 && !g_show_info) {
        bool bm = is_bookmarked(g_list.items[g_sel].id);
        C2D_Image *star_img = bm ? &g_bm_filled : &g_bm_empty;
        if (star_img->tex)
            C2D_DrawImageAt(*star_img, STAR_X, STAR_Y, 0.8f, NULL, 1.0f, 1.0f);
    }

    /* A button hint — centered between button and bottom edge */
    if (g_list.count > 0 && !g_show_info && g_ahint.tex) {
        float hint_sz = 38.0f;
        float btn_bot = BIG_Y + 114.0f;
        float hint_x = (320.0f - hint_sz) / 2.0f;
        float hint_y = (btn_bot + 240.0f) / 2.0f - hint_sz / 2.0f - 6.0f;
        C2D_DrawImageAt(g_ahint, hint_x, hint_y, 0.8f, NULL,
                        hint_sz / 62.0f, hint_sz / 62.0f);
    }

    /* Info panel */
    if (g_show_info && g_have_detail) {
        C2D_DrawRectSolid(10.0f, 36.0f, 0.1f, 300.0f, 168.0f, CLR_PANEL);
        float y = 50.0f;
        C2D_Text t;
        C2D_TextFontParse(&t, NULL, g_tbuf, g_detail.title);
        C2D_DrawText(&t, C2D_WithColor, 20.0f, y, 0.5f, 0.44f, 0.44f, CLR_WHITE);
        y += 28.0f;
        char meta[192];
        snprintf(meta, sizeof(meta), "by %s / %s favs / %s plays",
                 g_detail.uploader_name[0] ? g_detail.uploader_name : "?",
                 g_detail.favorites[0] ? g_detail.favorites : "?",
                 g_detail.views[0] ? g_detail.views : "?");
        C2D_TextFontParse(&t, NULL, g_tbuf, meta);
        C2D_DrawText(&t, C2D_WithColor, 20.0f, y, 0.5f, 0.36f, 0.36f, CLR_DIM);
        y += 18.0f;
        if (g_detail.description[0]) {
            char desc[512];
            snprintf(desc, sizeof(desc), "%.500s", g_detail.description);
            truncate_utf8(desc, 480);
            C2D_TextFontParse(&t, NULL, g_tbuf, desc);
            C2D_DrawText(&t, C2D_WithColor, 20.0f, y, 0.5f, 0.34f, 0.34f, CLR_DIM);
        }
    }

    /* Loading animation */
    if (g_dl_loading || g_net_state == NET_LOADING || g_info_state == NET_LOADING) {
        const float sq = 7.0f, gap = 2.0f;
        const float grid = 3.0f * sq + 2.0f * gap;
        const float ox = 8.0f;
        const float oy = 240.0f - 8.0f - grid;
        const int border_order[8] = {0, 1, 2, 5, 8, 7, 6, 3};
        int off_idx = g_anim / 3 % 8;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                int pos = r * 3 + c;
                int border_pos = -1;
                for (int b = 0; b < 8; b++)
                    if (border_order[b] == pos) { border_pos = b; break; }
                u32 clr = (border_pos == off_idx) ? CLR_CYAN_D : CLR_CYAN;
                C2D_DrawRectSolid(ox + c * (sq + gap), oy + r * (sq + gap),
                                  0.3f, sq, sq, clr);
            }
        }
    }

    if (g_toast_timer > 0) {
        C2D_Text t;
        C2D_TextFontParse(&t, NULL, g_tbuf, g_toast_msg);
        float tw, th;
        C2D_TextGetDimensions(&t, 0.34f, 0.34f, &tw, &th);
        C2D_DrawText(&t, C2D_WithColor, (320.0f - tw) / 2.0f, 240.0f - 24.0f,
                     0.9f, 0.34f, 0.34f, CLR_CYAN);
    }

    if (player_is_playing())
        C2D_DrawRectSolid(308.0f, 228.0f, 0.4f, 6.0f, 6.0f, CLR_WHITE);
}

static void render_frame(void) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TextBufClear(g_tbuf);
    C2D_TargetClear(g_top, CLR_BG);
    C2D_SceneBegin(g_top);
    render_top();
    C2D_TargetClear(g_bot, CLR_BG);
    C2D_SceneBegin(g_bot);
    render_bottom();
    C3D_FrameEnd(0);
}

/* ---- Main ---- */

int main(void) {
    romfsInit();
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    g_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    g_bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    g_tbuf = C2D_TextBufNew(8192);
    g_sheet = C2D_SpriteSheetLoad("romfs:/gfx/button.t3x");
    if (g_sheet) {
        g_btn = C2D_SpriteSheetGetImage(g_sheet, 0);
        if (C2D_SpriteSheetCount(g_sheet) > 1)
            g_btn_pressed_img = C2D_SpriteSheetGetImage(g_sheet, 1);
        else
            g_btn_pressed_img = g_btn;
        if (C2D_SpriteSheetCount(g_sheet) > 3) {
            g_bm_empty = C2D_SpriteSheetGetImage(g_sheet, 2);
            g_bm_filled = C2D_SpriteSheetGetImage(g_sheet, 3);
        }
        if (C2D_SpriteSheetCount(g_sheet) > 4)
            g_ahint = C2D_SpriteSheetGetImage(g_sheet, 4);
    }

    ndspInit();
    api_soc_init();
    bookmark_load();
    do_fetch_trending();

    circlePosition cpos;
    int prev_cpad_x = 0, prev_cpad_y = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();
        hidCircleRead(&cpos);

        player_is_playing();

        if (net_poll()) {
        }
        if (g_info_state == NET_DONE) {
            if (g_info_thread) { threadJoin(g_info_thread, U64_MAX); threadFree(g_info_thread); g_info_thread = NULL; }
            g_have_detail = true;
            g_show_info = true;
            g_info_state = NET_IDLE;
        } else if (g_info_state == NET_ERROR) {
            if (g_info_thread) { threadJoin(g_info_thread, U64_MAX); threadFree(g_info_thread); g_info_thread = NULL; }
            g_info_state = NET_IDLE;
        }

        {
            char perr[160] = "";
            int prc = player_poll_play(perr, sizeof(perr));
            if (prc == -1) {
                g_dl_loading = true;
            } else if (g_dl_loading) {
                g_dl_loading = false;
            }
        }

        if (kDown & KEY_START) break;
        if (kDown & KEY_SELECT) {
            player_stop();
        }

        if (kDown & KEY_Y)
            do_search();
        if (kDown & KEY_L)
            goto_page(g_page - 1);
        if (kDown & KEY_R)
            goto_page(g_page + 1);
        if (kDown & KEY_A)
            do_play_selected();
        if (kDown & KEY_X)
            toggle_info();
        if (g_show_info && (kDown & KEY_B))
            g_show_info = false;

        g_btn_pressed = false;

        if (!g_show_info) {
            if (kDown & KEY_UP) move_grid(0, -1);
            if (kDown & KEY_DOWN) move_grid(0, 1);
            if (kDown & KEY_LEFT) move_grid(-1, 0);
            if (kDown & KEY_RIGHT) move_grid(1, 0);

            float dx = (float)cpos.dx;
            float dy = (float)(-cpos.dy);
            float dist = sqrtf(dx * dx + dy * dy);

            if (dist > (float)CPAD_DEAD && g_list.count > 0) {
                float norm = (dist - (float)CPAD_DEAD) / ((float)(CPAD_MAX - CPAD_DEAD));
                if (norm > 1.0f) norm = 1.0f;
                float ax = dx / dist, ay = dy / dist;
                float speed_x = prev_cpad_x + ax * norm * 0.5f;
                float speed_y = prev_cpad_y + ay * norm * 0.5f;
                int move_x = (int)speed_x;
                int move_y = (int)speed_y;
                if (move_x != 0 || move_y != 0) {
                    if (abs(move_x) >= abs(move_y))
                        move_grid(move_x > 0 ? 1 : -1, 0);
                    else
                        move_grid(0, move_y > 0 ? 1 : -1);
                }
                prev_cpad_x = (int)(speed_x - (float)move_x);
                prev_cpad_y = (int)(speed_y - (float)move_y);
            } else {
                prev_cpad_x = 0;
                prev_cpad_y = 0;
            }

            touchPosition touch;
            hidTouchRead(&touch);

            bool on_btn = touch.px >= (u16)BIG_X && touch.px < (u16)(BIG_X + 114.0f) &&
                          touch.py >= (u16)BIG_Y && touch.py < (u16)(BIG_Y + 114.0f);
            bool on_star = g_list.count > 0 &&
                           touch.px >= (u16)STAR_X && touch.px < (u16)(STAR_X + STAR_SIZE) &&
                           touch.py >= (u16)STAR_Y && touch.py < (u16)(STAR_Y + STAR_SIZE);
            bool on_home = touch.px >= (u16)NAV_HOME_X && touch.px < (u16)(NAV_HOME_X + NAV_HOME_W) &&
                           touch.py >= (u16)NAV_Y && touch.py < (u16)(NAV_Y + NAV_H);
            bool on_bm = touch.px >= (u16)NAV_BM_X && touch.px < (u16)(NAV_BM_X + NAV_BM_W) &&
                         touch.py >= (u16)NAV_Y && touch.py < (u16)(NAV_Y + NAV_H);
            bool on_sr = touch.px >= (u16)NAV_SR_X && touch.px < (u16)(NAV_SR_X + NAV_SR_W) &&
                         touch.py >= (u16)NAV_Y && touch.py < (u16)(NAV_Y + NAV_H);

            g_btn_pressed = (kHeld & KEY_A) || (kHeld & KEY_TOUCH && on_btn);

            if (kDown & KEY_TOUCH) {
                if (on_btn)       do_play_selected();
                else if (on_star) toggle_bookmark(g_sel);
                else if (on_home) switch_view(VIEW_HOME);
                else if (on_bm)   switch_view(VIEW_BOOKMARKS);
                else if (on_sr)   do_search();
            }
        }

        g_anim++;
        if (g_toast_timer > 0) g_toast_timer--;
        render_frame();
        gspWaitForVBlank();
    }

    player_stop();
    if (g_bm_dirty) { bookmark_save(); g_bm_dirty = false; }
    /* Join all network threads before curl cleanup */
    if (g_net_thread) { threadJoin(g_net_thread, U64_MAX); threadFree(g_net_thread); g_net_thread = NULL; }
    if (g_info_thread) { threadJoin(g_info_thread, U64_MAX); threadFree(g_info_thread); g_info_thread = NULL; }
    C2D_TextBufDelete(g_tbuf);
    if (g_sheet) C2D_SpriteSheetFree(g_sheet);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    romfsExit();
    ndspExit();
    api_curl_cleanup();
    api_soc_exit();
    return 0;
}
