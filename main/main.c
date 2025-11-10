// Tanmatsu template app includes ////////////////////////////////////////////////////////
#include <stdio.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "hal/lcd_types.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "portmacro.h"
#include "wifi_connection.h"
#include "wifi_remote.h"
// mini_browser.c includes ///////////////////////////////////////////////////////////////
#include <ctype.h>
#include <strings.h>   /* for strncasecmp */
// Tanmatsu related includes /////////////////////////////////////////////////////////////
#include "http_download.h"
//#include "message_dialog.h"
#include "custom_certificates.h"
#include "esp_http_client.h"
#include "pax_fonts.h"
#include "pax_text.h"
#include "pax_types.h"
#include "console.h"
#include "common/display.h"
#include "common/theme.h"
#include "bsp/tanmatsu.h"
#include "tanmatsu_coprocessor.h"
#include "textedit.h"

// Tanmatsu template app globals and macros //////////////////////////////////////////////

// Constants
static char const TAG[] = "tanmatsu-web-mini";

// Global variables
static size_t                       display_h_res        = 0;
static size_t                       display_v_res        = 0;
static lcd_color_rgb_pixel_format_t display_color_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
static lcd_rgb_data_endian_t        display_data_endian  = LCD_RGB_DATA_ENDIAN_LITTLE;
static pax_buf_t                    fb                   = {0};

#if defined(CONFIG_BSP_TARGET_KAMI)
// Temporary addition for supporting epaper devices (irrelevant for Tanmatsu)
static pax_col_t palette[] = {0xffffffff, 0xff000000, 0xffff0000};  // white, black, red
#endif

// mini_browser.c globals and macros /////////////////////////////////////////////////////

// subsequent globals etc are supplementary to the Tanmatsu template app... //////////////
// these are borrowed from mini_browser.c ////////////////////////////////////////////////
#define MAX_BYTES     (64 * 1024)
#define TIMEOUT_S     10
#define URL_MAX       256
#define PAD_LR        10
#define PAD_TOP       34
#define PAD_BOTTOM    10
#define VIEW_W        716
#define VIEW_H        716
#define URLBAR_H      24
#define LINE_SPACING  2
#define MAX_LINKS     128

/* --- Icon placement inside URL bar --- */
#define ICON_LEFT   2
#define ICON_TOP    2
#define ICON_SIZE   20
#define ICON_GAP    8                 /* gap between icon and URL text */
#define URL_TEXT_X  (PAD_LR + ICON_SIZE + ICON_GAP)  /* start X for URL text */

/* ---------- Home + special-key targets ---------- */
//#define HOME_URL          "https://minibrowser.macip.net"
#define HOME_URL          "https://info.cern.ch"
//#define SPECIAL_URL_124   "https://text.npr.org"
//#define SPECIAL_URL_125   "https://news.ycombinator.com/"
//#define SPECIAL_URL_126   "http://www.textfiles.com/"
//#define SPECIAL_URL_127   "https://ifconfig.co"
//#define SPECIAL_URL_128   "https://ohmeadhbh.github.io/bobcat/"
//#define SPECIAL_URL_129   "https://curl.se/"

/* ---------- Accelerator + special scancodes ---------- */
//#define SC_ACCELERATOR    ((SDL_Scancode)0xE3)  /* Meta key */
//#define SC_SPECIAL_124    ((SDL_Scancode)0x124)
//#define SC_SPECIAL_125    ((SDL_Scancode)0x125)
//#define SC_SPECIAL_126    ((SDL_Scancode)0x126)
//#define SC_SPECIAL_127    ((SDL_Scancode)0x127)
//#define SC_SPECIAL_128    ((SDL_Scancode)0x128)
//#define SC_SPECIAL_129    ((SDL_Scancode)0x129)

#define FONT_W_COLS 5 // XXX adjustments required for Tanmatsu chakrapetch font
#define FONT_H_ROWS 7
#define FONT_COL_GAP 1
#define FONT_SCALE  2
#define CH_W ((FONT_W_COLS + FONT_COL_GAP) * FONT_SCALE)
#define CH_H ((FONT_H_ROWS) * FONT_SCALE)


void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
}

// functions which follow are supplementary to the Tanmatsu template app... /////////////

// borrowed from tanmatsu-launcher http_download.c //////////////////////////////////////

/*const char*                global_callback_text = NULL;
static download_callback_t global_callback      = NULL;

bool download_file(const char* url, const char* path, download_callback_t callback, const char* callback_text) {
    global_callback      = callback;
    global_callback_text = callback_text;
    int retry            = 3;
    while (retry--) {
        if (_download_file(url, path)) return true;
        ESP_LOGI(TAG, "Download waiting to retry ...");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    return false;
}*/
static void download_callback(size_t download_position, size_t file_size, const char* status_text) {
    uint8_t        percentage      = 100 * download_position / file_size;
    static uint8_t last_percentage = 0;
    if (percentage == last_percentage) {
        return;  // No change, no need to update
    }
    last_percentage = percentage;
    char text[64];
    sprintf(text, "%s (%u%%)", status_text, percentage);
    //busy_dialog(get_icon(ICON_DOWNLOADING), "Downloading", text);
};


// these are borrowed from mini_browser.c... ////////////////////////////////////////////

/* ---------- link + page model ---------- */
typedef struct { char href[URL_MAX]; } link_t;
typedef struct {
    char *text;                 /* rendered text with [n] markers */
    link_t links[MAX_LINKS];
    int link_count;
    char base[URL_MAX];         /* base URL for resolution */
} page_t;

/* ---------- URL helpers ---------- */
static void get_scheme_host(const char *url, char *out, size_t cap) {
    const char *p = strstr(url, "://");
    if (!p) { out[0]=0; return; }
    p += 3;
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - url) : strlen(url);
    if (n >= cap) n = cap - 1;
    memcpy(out, url, n); out[n]=0;
}
static void get_dir(const char *url, char *out, size_t cap) {
    const char *q = url;
    const char *p = strrchr(q, '/');
    if (!p) { out[0]=0; return; }
    size_t n = (size_t)(p - q) + 1;
    if (n >= cap) n = cap - 1;
    memcpy(out, q, n); out[n]=0;
}
static void base_no_query_or_hash(const char *u, char *out, size_t cap) {
    size_t n = strlen(u), cut = n;
    for (size_t i=0;i<n;i++){ if (u[i]=='?' || u[i]=='#'){ cut=i; break; } }
    if (cut >= cap) cut = cap-1;
    memcpy(out, u, cut); out[cut]=0;
}
static void base_no_hash(const char *u, char *out, size_t cap) {
    size_t n = strlen(u), cut = n;
    for (size_t i=0;i<n;i++){ if (u[i]=='#'){ cut=i; break; } }
    if (cut >= cap) cut = cap-1;
    memcpy(out, u, cut); out[cut]=0;
}
static void scheme_from_url(const char *base, char *out, size_t cap) {
    if (!base) { strncpy(out, "https", cap); out[cap-1]=0; return; }
    const char *p = strstr(base, "://");
    if (!p) { strncpy(out, "https", cap); out[cap-1]=0; return; }
    size_t n = (size_t)(p - base);
    if (n >= cap) n = cap - 1;
    memcpy(out, base, n); out[n]=0;
}
static void resolve_url(const char *base, const char *href, char *out, size_t cap) {
    if (!href || !*href) { out[0]=0; return; }
    if (strstr(href, "://")) { strncpy(out, href, cap); out[cap-1]=0; return; }
    if (href[0]=='/' && href[1]=='/') {
        char sch[16]; scheme_from_url(base, sch, sizeof sch);
        snprintf(out, cap, "%s:%s", sch, href); return;
    }
    if (href[0]=='/') {
        char origin[URL_MAX]; get_scheme_host(base, origin, sizeof origin);
        snprintf(out, cap, "%s%s", origin, href); return;
    }
    if (href[0]=='?') {
        char base2[URL_MAX]; base_no_query_or_hash(base, base2, sizeof base2);
        snprintf(out, cap, "%s%s", base2, href); return;
    }
    if (href[0]=='#') {
        char base2[URL_MAX]; base_no_hash(base, base2, sizeof base2);
        snprintf(out, cap, "%s%s", base2, href); return;
    }
    if (href[0]=='.' && href[1]=='/') href += 2;
    char dir[URL_MAX]; get_dir(base, dir, sizeof dir);
    snprintf(out, cap, "%s%s", dir, href);
}

/* --- URL sanitation --- */
static void trim_inplace(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    size_t i = 0; while (i < n && isspace((unsigned char)s[i])) i++;
    size_t j = n; while (j > i && isspace((unsigned char)s[j-1])) j--;
    if (i > 0 || j < n) { memmove(s, s + i, j - i); s[j - i] = 0; }
}
static int has_scheme(const char *u) { return u && strstr(u, "://") != NULL; }
static int is_http_scheme(const char *u) {
    return u && (strncmp(u, "http://", 7)==0 || strncmp(u, "https://", 8)==0);
}
static void normalize_typed_url(char *buf) {
    trim_inplace(buf);
    if (!buf[0]) return;
    if (!has_scheme(buf)) {
        char tmp[URL_MAX-8]; strncpy(tmp, buf, URL_MAX-8); tmp[URL_MAX-7]=0;
        snprintf(buf, URL_MAX, "https://%s", tmp);
    }
}

/* ---------- entity decode (minimal) ---------- */
static const char *emit_entity(const char *h, char *out, size_t *o, size_t cap) {
    if      (!strncmp(h,"&amp;",5))  { if(*o<cap) out[(*o)++]='&';  return h+5; }
    else if (!strncmp(h,"&lt;",4))   { if(*o<cap) out[(*o)++]='<';  return h+4; }
    else if (!strncmp(h,"&gt;",4))   { if(*o<cap) out[(*o)++]='>';  return h+4; }
    else if (!strncmp(h,"&quot;",6)) { if(*o<cap) out[(*o)++]='"'; return h+6; }
    else if (!strncmp(h,"&#39;",5))  { if(*o<cap) out[(*o)++]='\''; return h+5; }
    else if (!strncmp(h,"&nbsp;",6)) { if(*o<cap) out[(*o)++]=' ';  return h+6; }
    return NULL;
}

/* --------- href filter --------- */
static int is_supported_href(const char *h) {
    if (!h || !*h) return 0;
    if (h[0] == '#') return 0; /* in-page */
    if (!strncasecmp(h, "javascript:", 11)) return 0;
    if (!strncasecmp(h, "mailto:", 7)) return 0;
    if (!strncasecmp(h, "data:", 5)) return 0;
    return 1; /* allow http(s) and relatives */
}

/* ---------- HTML -> page_t ---------- */
static page_t *html_to_page(const char *html, const char *base_url) {
    if (!html) return NULL;
    size_t L = strlen(html);
    char *buf = (char*)malloc(L + 1 + MAX_LINKS*6);
    if (!buf) return NULL;

    page_t *pg = (page_t*)calloc(1, sizeof(page_t));
    if (!pg) { free(buf); return NULL; }
    strncpy(pg->base, base_url ? base_url : "", URL_MAX);
    pg->base[URL_MAX-1] = 0;

    bool in_head=false, in_script=false, in_style=false;
    size_t o=0;

    for (size_t i=0; i<L; ) {
        char c = html[i];

        if (!in_script && !in_style && !in_head && c=='&') {
            const char *adv = emit_entity(&html[i], buf, &o, L + MAX_LINKS*6);
            if (adv) { i = (size_t)(adv - html); continue; }
        }

        if (c == '<') {
            i++;
            bool closing = false;
            if (i<L && html[i]=='/') { closing=true; i++; }

            char tname[16]; int tn=0;
            while (i<L && tn<(int)sizeof(tname)-1 && isalpha((unsigned char)html[i])) {
                tname[tn++] = (char)tolower((unsigned char)html[i]); i++;
            }
            tname[tn]=0;

            while (i<L && isspace((unsigned char)html[i])) i++;

            if (!closing) {
                if (!strcmp(tname,"head"))   in_head=true;
                else if (!strcmp(tname,"script")) in_script=true;
                else if (!strcmp(tname,"style"))  in_style=true;

                if (!strcmp(tname,"a")) {
                    char href_val[URL_MAX]; href_val[0]=0;

                    while (i<L && html[i] != '>') {
                        char aname[16]; int an=0;
                        while (i<L && isspace((unsigned char)html[i])) i++;
                        while (i<L && an<(int)sizeof(aname)-1 &&
                               (isalnum((unsigned char)html[i]) || html[i]=='-' || html[i]=='_')) {
                            aname[an++] = (char)tolower((unsigned char)html[i]); i++;
                        }
                        aname[an]=0;
                        while (i<L && isspace((unsigned char)html[i])) i++;

                        if (i<L && html[i]=='=') {
                            i++;
                            while (i<L && isspace((unsigned char)html[i])) i++;
                            char quote=0;
                            if (i<L && (html[i]=='"' || html[i]=='\'')) quote = html[i++];

                            char aval[URL_MAX]; int av=0;
                            while (i<L) {
                                if (quote) {
                                    if (html[i]==quote) { i++; break; }
                                } else {
                                    if (isspace((unsigned char)html[i]) || html[i]=='>') break;
                                }
                                if (av < (int)sizeof(aval)-1) aval[av++] = html[i];
                                i++;
                            }
                            aval[av]=0;

                            if (!strcmp(aname,"href") && !href_val[0]) {
                                strncpy(href_val, aval, URL_MAX);
                                href_val[URL_MAX-1]=0;
                            }
                        } else {
                            while (i<L && !isspace((unsigned char)html[i]) && html[i] != '>') i++;
                        }
                    }

                    if (is_supported_href(href_val) && pg->link_count < MAX_LINKS) {
                        char abs[URL_MAX]; resolve_url(pg->base, href_val, abs, sizeof abs);
                        if (is_supported_href(abs)) {
                            int idx = pg->link_count++;
                            strncpy(pg->links[idx].href, abs, URL_MAX);
                            pg->links[idx].href[URL_MAX-1]=0;
                            int wrote = snprintf(buf + o, L + MAX_LINKS*6 - o, "[%d]", idx+1);
                            if (wrote > 0) o += (size_t)wrote;
                        }
                    }

                    while (i<L && html[i] != '>') i++;
                } else if (!strcmp(tname,"br") || !strcmp(tname,"p")) {
                    if (o && buf[o-1] != '\n') buf[o++] = '\n';
                    while (i<L && html[i] != '>') i++;
                } else {
                    while (i<L && html[i] != '>') i++;
                }
            } else {
                if (!strcmp(tname,"head"))   in_head=false;
                else if (!strcmp(tname,"script")) in_script=false;
                else if (!strcmp(tname,"style"))  in_style=false;
                while (i<L && html[i] != '>') i++;
            }

            if (i<L && html[i]=='>') i++;
            continue;
        }

        if (in_script || in_style || in_head) { i++; continue; }

        if (c=='\r') { i++; continue; }
        if (c=='\n') { if (o && buf[o-1] != '\n') buf[o++]='\n'; i++; continue; }
        if (c==' ' && o && buf[o-1]==' ') { i++; continue; }

        if (o < L + MAX_LINKS*6 - 1) buf[o++] = c;
        i++;
    }

    buf[o]=0;
    pg->text = buf;
    return pg;
}

/* ---------- wrap text to columns ---------- */
static char *wrap_text(const char *in, int max_cols) {
    if (!in) return NULL;
    size_t n = strlen(in);
    char *out = (char*)malloc(n + n/(max_cols?max_cols:1) + 8);
    if (!out) return NULL;

    int col = 0;
    size_t o = 0;
    int blank_run = 0;

    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c == '\r') continue;

        if (c == '\n') {
            if (col == 0) {
                if (blank_run) continue;
                blank_run = 1;
            } else {
                blank_run = 0;
            }
            out[o++] = '\n';
            col = 0;
            continue;
        }

        if (c == ' ' && (col == 0 || out[o-1] == ' ')) continue;

        if (max_cols && col >= max_cols && c == ' ') {
            out[o++] = '\n'; col = 0; continue;
        }
        if (max_cols && col >= max_cols) {
            out[o++] = '\n'; col = 0;
        }

        out[o++] = c;
        col++;
        blank_run = 0;
    }
    out[o] = 0;
    return out;
}

/* ---------- tiny text renderer ---------- */
static unsigned utf8_next(const char *s, size_t len, size_t *i) {
    if (*i >= len) return 0;
    unsigned char c = (unsigned char)s[*i];
    if (c < 0x80) { (*i)++; return c; }
    if ((c & 0xE0) == 0xC0 && *i+1 < len) { unsigned cp=((c&0x1F)<<6) | (s[*i+1]&0x3F); *i+=2; return cp; }
    if ((c & 0xF0) == 0xE0 && *i+2 < len) { unsigned cp=((c&0x0F)<<12)|((s[*i+1]&0x3F)<<6)|(s[*i+2]&0x3F); *i+=3; return cp; }
    if ((c & 0xF8) == 0xF0 && *i+3 < len) { unsigned cp=((c&0x07)<<18)|((s[*i+1]&0x3F)<<12)|((s[*i+2]&0x3F)<<6)|(s[*i+3]&0x3F); *i+=4; return cp; }
    (*i)++; return '?';
}
/*static void draw_char(SDL_Renderer *r, int x, int y, char c) {
    if (!r) return;
    if ((unsigned char)c < 32 || (unsigned char)c > 127) c = '?';
    const unsigned char *cols = font5x7[(unsigned char)c - 32]; // XXX
    for (int col = 0; col < FONT_W_COLS; col++) {
        unsigned char bits = cols[col];
        for (int row = 0; row < FONT_H_ROWS; row++) {
            if (bits & (1u << row)) {
                SDL_FRect px = { (float)(x + col*FONT_SCALE), (float)(y + row*FONT_SCALE),
                                 (float)FONT_SCALE, (float)FONT_SCALE };
                SDL_RenderFillRect(r, &px);
            }
        }
    }
}*/
//static void draw_text(SDL_Renderer *r, int x, int y, const char *s, int max_w) {
//static void draw_text(pax_buf_t *r, int x, int y, const char *s, int max_w) {
//    if (!r || !s) return;
//    int cx = x, cy = y;
//    size_t i=0, L=strlen(s);
//    while (i<L) {
//        unsigned cp = utf8_next(s, L, &i);
//        if (cp == 0) break;
//        if (cp == '\n') { cx = x; cy += (CH_H + LINE_SPACING); continue; }
//        if (cp == 0xA0) cp = ' ';                 /* NBSP -> space */
//        if (cp < 32 || cp > 126) continue;        /* draw ASCII only */
//        if (cx + ((FONT_W_COLS + FONT_COL_GAP) * FONT_SCALE) > x + max_w) {
//            cx = x; cy += (CH_H + LINE_SPACING);
//        }
//        //draw_char(r, cx, cy, (char)cp);
//        pax_draw_text(&r, BLACK, pax_font_sky_mono, 16, cx, cy, cp
//        cx += ((FONT_W_COLS + FONT_COL_GAP) * FONT_SCALE);
//    }
//}

/* --- draw URL bar text, clipped from the LEFT, starting at URL_TEXT_X --- */
/*static void draw_bar(SDL_Renderer *r, const char *text) {
    int max_cols = (VIEW_W - URL_TEXT_X - PAD_LR) / CH_W;
    if (max_cols < 1) max_cols = 1;

    size_t n = strlen(text);
    char tmp[URL_MAX + 32];
    if ((int)n > max_cols) {
        const char *start = text + (n - (size_t)max_cols + 3);
        snprintf(tmp, sizeof tmp, "...%s", start);
        draw_text(r, URL_TEXT_X, 4, tmp, VIEW_W - URL_TEXT_X - PAD_LR);
    } else {
        draw_text(r, URL_TEXT_X, 4, text, VIEW_W - URL_TEXT_X - PAD_LR);
    }
}*/

/* ---------- UI ---------- */
/*static void draw_ui(SDL_Renderer *r, const char *bar_text) {
    if (!r) return;
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);

    SDL_FRect top = (SDL_FRect){0, 0, VIEW_W, URLBAR_H};
    SDL_SetRenderDrawColor(r, 30, 30, 30, 255);
    SDL_RenderFillRect(r, &top);

    SDL_SetRenderDrawColor(r, 220, 220, 220, 255);
    draw_bar(r, bar_text);

    SDL_FRect mid = (SDL_FRect){0, URLBAR_H + 1, VIEW_W, 2};
    SDL_SetRenderDrawColor(r, 60, 60, 60, 255);
    SDL_RenderFillRect(r, &mid);
}*/

static int is_printable_ascii(const char *text) {
    if (!text || !*text) return 0;
    unsigned char c = (unsigned char)text[0];
    return (c >= 32 && c <= 126);
}

/* ---------- history (for WHY + B) ---------- */
#define HISTORY_MAX 32
static char g_hist[HISTORY_MAX][URL_MAX];
static int  g_hist_len = 0;

static void history_push(const char *u) {
    if (!u || !*u) return;
    if (g_hist_len > 0 && strncmp(g_hist[g_hist_len - 1], u, URL_MAX) == 0) return; /* no dup consec */
    if (g_hist_len < HISTORY_MAX) {
        strncpy(g_hist[g_hist_len], u, URL_MAX);
        g_hist[g_hist_len][URL_MAX-1] = 0;
        g_hist_len++;
    } else {
        memmove(g_hist, g_hist + 1, sizeof(g_hist[0]) * (HISTORY_MAX - 1));
        strncpy(g_hist[HISTORY_MAX - 1], u, URL_MAX);
        g_hist[HISTORY_MAX - 1][URL_MAX - 1] = 0;
    }
}
static int history_back(char *out) {
    if (g_hist_len <= 1) return 0;        /* nowhere to go */
    g_hist_len--;                          /* drop current */
    strncpy(out, g_hist[g_hist_len - 1], URL_MAX);
    out[URL_MAX - 1] = 0;
    return 1;
}

// the main bit of the app ///////////////////////////////////////////////////////////////
// aka hurriedly smooshed Tanmatsu template app and mini_browser.c code //////////////////

void console_write_cb(char* str, size_t len) {
    // NOOP
}

static void keyboard_backlight(void) {
    uint8_t brightness;
    bsp_input_get_backlight_brightness(&brightness);
    if (brightness != 100) {
        brightness = 100;
    } else {
        brightness = 0;
    }
    ESP_LOGI(TAG, "Keyboard brightness: %u%%\r\n", brightness);
    bsp_input_set_backlight_brightness(brightness);
}

static void display_backlight(void) {
    uint8_t brightness;
    bsp_display_get_backlight_brightness(&brightness);
    brightness += 15;
    if (brightness > 100) {
        brightness = 10;
    }
    ESP_LOGI(TAG, "Display brightness: %u%%\r\n", brightness);
    bsp_display_set_backlight_brightness(brightness);
}

//void display_print(pax_buf_t fb, char* str) {
//    // have some concept of current position, say ypos
//    pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, ypos, str);
//    // increment current line by font height + line spacing
//}

void app_main(void) {
    // this section lifted from mini_browser.c ///////////////////////////////////////////
  
    printf("[mini_browser] enter main\n");

    char url_buf[URL_MAX] = HOME_URL;
    char barline[URL_MAX + 64];
    char *content_wrapped = NULL;
    page_t *page = NULL;
    int  scroll_lines = 0;
    int  need_fetch = 1;
    int  need_draw = 0;
    int  sel_link = -1;
    QueueHandle_t input_event_queue = NULL;
    gui_theme_t* theme = get_theme();

#if defined(ESP_PLATFORM)
    //esp_log_level_set("ESP_CURL",        ESP_LOG_ERROR);
    esp_log_level_set("HTTP_CLIENT",     ESP_LOG_ERROR);
    esp_log_level_set("transport_base",  ESP_LOG_ERROR);
#endif

    bool accel_down = false;        /* WHY key held */
    bool inhibit_text_once = false; /* swallow TEXT_INPUT after commands */

    int max_cols = (VIEW_W - 2*PAD_LR) / CH_W;
    int lines_per_page = (VIEW_H - PAD_TOP - PAD_BOTTOM) / (CH_H + LINE_SPACING);
    ESP_LOGI(TAG, "max_cols: %d, lines_per_page: %d", max_cols, lines_per_page);

    // this section from the Tanmatsu template app ///////////////////////////////////////

    // Start the GPIO interrupt service
    gpio_install_isr_service(0);

    // Initialize the Non Volatile Storage service
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        res = nvs_flash_init();
    }
    ESP_ERROR_CHECK(res);

    // Initialize the Board Support Package
    ESP_ERROR_CHECK(bsp_device_initialize());

    uint8_t led_data[] = {
        0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0xFF,
    };
    bsp_led_write(led_data, sizeof(led_data));

    // Get display parameters and rotation
    res = bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    ESP_ERROR_CHECK(res);  // Check that the display parameters have been initialized
    bsp_display_rotation_t display_rotation = bsp_display_get_default_rotation();

    // Convert ESP-IDF color format into PAX buffer type
    pax_buf_type_t format = PAX_BUF_24_888RGB;
    switch (display_color_format) {
        case LCD_COLOR_PIXEL_FORMAT_RGB565:
            format = PAX_BUF_16_565RGB;
            break;
        case LCD_COLOR_PIXEL_FORMAT_RGB888:
            format = PAX_BUF_24_888RGB;
            break;
        default:
            break;
    }

    // Convert BSP display rotation format into PAX orientation type
    pax_orientation_t orientation = PAX_O_UPRIGHT;
    switch (display_rotation) {
        case BSP_DISPLAY_ROTATION_90:
            orientation = PAX_O_ROT_CCW;
            break;
        case BSP_DISPLAY_ROTATION_180:
            orientation = PAX_O_ROT_HALF;
            break;
        case BSP_DISPLAY_ROTATION_270:
            orientation = PAX_O_ROT_CW;
            break;
        case BSP_DISPLAY_ROTATION_0:
        default:
            orientation = PAX_O_UPRIGHT;
            break;
    }

    // Initialize graphics stack
//#if defined(CONFIG_BSP_TARGET_KAMI)
    // Temporary addition for supporting epaper devices (irrelevant for Tanmatsu)
    //format = PAX_BUF_2_PAL;
//#endif
    pax_buf_init(&fb, NULL, display_h_res, display_v_res, format);
    ESP_LOGI(TAG, "display_h_res: %d, display_v_res: %d", display_h_res, display_v_res);
    pax_buf_reversed(&fb, display_data_endian == LCD_RGB_DATA_ENDIAN_BIG);
//#if defined(CONFIG_BSP_TARGET_KAMI)
    // Temporary addition for supporting epaper devices (irrelevant for Tanmatsu)
    //fb.palette      = palette;
    //fb.palette_size = sizeof(palette) / sizeof(pax_col_t);
//#endif
    pax_buf_set_orientation(&fb, orientation);

#if defined(CONFIG_BSP_TARGET_KAMI)
#define BLACK 0
#define WHITE 1
#define RED   2
#else
#define BLACK 0xFF000000
#define WHITE 0xFFFFFFFF
#define RED   0xFFFF0000
#endif

    struct cons_insts_s console_instance;

    struct cons_config_s con_conf = {
        .font = pax_font_sky_mono, 
	.font_size_mult = 1.5, 
	.paxbuf = &fb,
	.output_cb = console_write_cb
    };

    // Get input event queue from BSP
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    ESP_LOGI(TAG, "console_init()");
    //console_init(&console_instance, &con_conf);
    //console_set_colors(&console_instance, CONS_COL_VGA_GREEN, CONS_COL_VGA_BLACK);
    //console_instance.fg = 0xff00ff00;
    //console_instance.bg = 0xff000000;
    //keyboard_backlight();
    max_cols = 80; //console_instance.chars_x;
    lines_per_page = 36; //console_instance.chars_y;

    // Start WiFi stack (if your app does not require WiFi or BLE you can remove this section)
    pax_background(&fb, WHITE);
 
    pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 10, 20, "Connecting to radio...");
    //pax_buf_t        *gfx,
    //pax_col_t         color,
    //pax_font_t const *font,
    //float             font_size,
    //bool              shrink_to_fit,
    ////char const       *text,
    //size_t            text_len,
    //ptrdiff_t         cursorpos,
    //pax_recti         bounds,
    //pax_align_t       halign,
    //pax_align_t       valign

    ESP_LOGI(TAG, "console_printf()");
    //console_printf(&console_instance, "Connecting to WiFi...\n");
    blit();

    if (wifi_remote_initialize() == ESP_OK) {
        pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 10, 40, "Starting WiFi stack...");
        //console_printf(&console_instance, "Starting WiFi stack...\n");
        blit();
        wifi_connection_init_stack();  // Start the Espressif WiFi stack

        pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 10, 60, "Connecting to WiFi network...");
        //console_printf(&console_instance, "Connecting to WiFi network...\n");
        blit();

        if (wifi_connect_try_all() == ESP_OK) {
            pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 10, 80, "Succesfully connected to WiFi network");
            //console_printf(&console_instance, "Successfully connected to WiFi network\n");
            blit();
        } else {
            pax_draw_text(&fb, RED, pax_font_sky_mono, 16, 10, 80, "Failed to connect to WiFi network");
            //console_printf(&console_instance, "Failed to connect to WiFi network\n");
            blit();
        }
    } else {
        bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
        ESP_LOGE(TAG, "WiFi radio not responding, WiFi not available");
        pax_background(&fb, RED);
        pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 10, 40, "WiFi unavailable");
        //console_printf(&console_instance, "WiFi unavailable\n");
        blit();
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_ERROR_CHECK(initialize_custom_ca_store());

    // Main section of the app //////////////////////////////////////////////////////////

    //console_clear(&console_instance);
    //console_set_cursor(&console_instance, 0, 0);

    while (1) {
	// now back to Tanmatsu template app /////////////////////////////////////////////
        bsp_input_event_t event;
        
        if (xQueueReceive(input_event_queue, &event, pdMS_TO_TICKS(10)) == pdTRUE) {
            bsp_led_write(led_data, sizeof(led_data));
            switch (event.type) {
                case INPUT_EVENT_TYPE_NAVIGATION:
                    //ESP_LOGI(TAG, "Navigation event %0" PRIX32 ": %s", (uint32_t)event.args_navigation.key, event.args_navigation.state ? "pressed" : "released");
		    if (event.args_navigation.state) {
		        switch (event.args_navigation.key) {
			    case BSP_INPUT_NAVIGATION_KEY_F1:
			        ESP_LOGI(TAG, "close key pressed - returning to app launcher");
                                bsp_device_restart_to_launcher();
			        break;
                            case BSP_INPUT_NAVIGATION_KEY_F2:
			        ESP_LOGI(TAG, "keyboard backlight toggle");
                                keyboard_backlight();
			        break;
                            case BSP_INPUT_NAVIGATION_KEY_F3:
			        ESP_LOGI(TAG, "display backlight toggle");
                                display_backlight();
			        break;
                            case BSP_INPUT_NAVIGATION_KEY_F4:
                            case BSP_INPUT_NAVIGATION_KEY_START:
			        ESP_LOGI(TAG, "f4 (circle) key pressed - prompting for url");
                                char tmp_u[URL_MAX+17];
                                pax_background(&fb, WHITE);
				memset(url_buf, 0, sizeof(url_buf));
				bool accepted = false;
                                menu_textedit(&fb, theme, "URL to open", tmp_u, sizeof(tmp_u), true, &accepted);
				if (accepted) {
				    ESP_LOGI(TAG, "user input URL: %s", tmp_u);
                                    strncpy(url_buf, tmp_u, URL_MAX);
				    url_buf[URL_MAX-1]=0;
                            	    need_fetch = 1;
				}
			        break;
                            case BSP_INPUT_NAVIGATION_KEY_DOWN:
			        ESP_LOGI(TAG, "down key pressed");
                                int lpp = (VIEW_H - PAD_TOP - PAD_BOTTOM) / (CH_H + LINE_SPACING);
                                //scroll_lines += lpp > 2 ? lpp - 2 : 1;
                                scroll_lines += lines_per_page;
				need_draw = 1;
                                break;
                            case BSP_INPUT_NAVIGATION_KEY_UP:
			        ESP_LOGI(TAG, "up key pressed");
                                //if (scroll_lines >= 5) scroll_lines -= 5; else scroll_lines = 0;
                                if (scroll_lines >= lines_per_page) scroll_lines -= lines_per_page; else scroll_lines = 0;
				need_draw = 1;
                                break;
                            case BSP_INPUT_NAVIGATION_KEY_TAB:
				ESP_LOGI(TAG, "tab key pressed");
    				// bool shift = (ev.key.mod & SDL_KMOD_SHIFT) != 0;
				bool shift = false;
				if (event.args_navigation.modifiers) {
				    ESP_LOGI(TAG, "+ shift held down");
				    shift = true;
				}
    				if (page && page->link_count > 0) {
				    ESP_LOGI(TAG, "sel_link (before): %d", sel_link);
        			    if (sel_link < 0) {
            				/* First time we tab (Tab or Shift+Tab): start at the first link */
            			        sel_link = 0;
        			    } else if (shift) {
            				/* Move backward, wrapping to last if we're at the start */
            				sel_link = (sel_link == 0) ? (page->link_count - 1) : (sel_link - 1);
        			    } else {
            				/* Move forward with wraparound */
            				sel_link = (sel_link + 1) % page->link_count;
        			    }
				    ESP_LOGI(TAG, "sel_link (after): %d", sel_link);
    				}
    			        break;
                            case BSP_INPUT_NAVIGATION_KEY_RETURN:
                        	if (page && sel_link >= 0 && sel_link < page->link_count) {
                            	    strncpy(url_buf, page->links[sel_link].href, URL_MAX);
                            	    url_buf[URL_MAX-1]=0;
                            	    need_fetch = 1;
                        	} else {
                            	    need_fetch = 1;
                        	}
				ESP_LOGI(TAG, "return key pressed, requesting url: %s", url_buf);
                        	break;

                            case BSP_INPUT_NAVIGATION_KEY_NONE:
                            case BSP_INPUT_NAVIGATION_KEY_ESC:
                            case BSP_INPUT_NAVIGATION_KEY_LEFT:
                            case BSP_INPUT_NAVIGATION_KEY_RIGHT:
                            case BSP_INPUT_NAVIGATION_KEY_HOME:
                            case BSP_INPUT_NAVIGATION_KEY_END:
                            case BSP_INPUT_NAVIGATION_KEY_PGUP:
                            case BSP_INPUT_NAVIGATION_KEY_PGDN:
                            case BSP_INPUT_NAVIGATION_KEY_MENU:
                            case BSP_INPUT_NAVIGATION_KEY_SELECT:
                            case BSP_INPUT_NAVIGATION_KEY_SUPER:
                            case BSP_INPUT_NAVIGATION_KEY_BACKSPACE:
                            case BSP_INPUT_NAVIGATION_KEY_SPACE_L:
                            case BSP_INPUT_NAVIGATION_KEY_SPACE_M:
                            case BSP_INPUT_NAVIGATION_KEY_SPACE_R:
                            case BSP_INPUT_NAVIGATION_KEY_F5:
                            case BSP_INPUT_NAVIGATION_KEY_F6:
                            case BSP_INPUT_NAVIGATION_KEY_F7:
                            case BSP_INPUT_NAVIGATION_KEY_F8:
                            case BSP_INPUT_NAVIGATION_KEY_F9:
                            case BSP_INPUT_NAVIGATION_KEY_F10:
                            case BSP_INPUT_NAVIGATION_KEY_F11:
                            case BSP_INPUT_NAVIGATION_KEY_F12:
                            case BSP_INPUT_NAVIGATION_KEY_GAMEPAD_A:
                            case BSP_INPUT_NAVIGATION_KEY_GAMEPAD_B:
                            case BSP_INPUT_NAVIGATION_KEY_GAMEPAD_X:
                            case BSP_INPUT_NAVIGATION_KEY_GAMEPAD_Y:
                            case BSP_INPUT_NAVIGATION_KEY_JOYSTICK_PRESS:
                            case BSP_INPUT_NAVIGATION_KEY_VOLUME_UP:
                            case BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN:
			    default:
				// XXX it's a NOOP for now
				break;
			}
		    }
                    break;
                case INPUT_EVENT_TYPE_KEYBOARD:
		    break;
                    //if (event.args_keyboard.ascii != '\b' ||
                    //    event.args_keyboard.ascii != '\t') {  // Ignore backspace & tab keyboard events
                    //    ESP_LOGI(TAG, "Keyboard event %c (%02x) %s", event.args_keyboard.ascii,
                    //             (uint8_t)event.args_keyboard.ascii, event.args_keyboard.utf8);
                    //    pax_simple_rect(&fb, WHITE, 0, 0, pax_buf_get_width(&fb), 72);
                    //    pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 0, "Keyboard event");
                    //    char text[64];
                    //    snprintf(text, sizeof(text), "ASCII:     %c (0x%02x)", event.args_keyboard.ascii,
                    //             (uint8_t)event.args_keyboard.ascii);
                    //    pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 18, text);
                    //    snprintf(text, sizeof(text), "UTF-8:     %s", event.args_keyboard.utf8);
                    //    pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 36, text);
                    //    snprintf(text, sizeof(text), "Modifiers: 0x%0" PRIX32, event.args_keyboard.modifiers);
                    //    pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 54, text);
                    //    blit();
                    //}
                    //break;
                case INPUT_EVENT_TYPE_ACTION:
                    ESP_LOGI(TAG, "Action event 0x%0" PRIX32 ": %s", (uint32_t)event.args_action.type, event.args_action.state ? "yes" : "no");
		    // XXX there will be some actions like press return to fetch URL
                    break;
                case INPUT_EVENT_TYPE_SCANCODE:
                    ESP_LOGI(TAG, "Scancode event 0x%0" PRIX32, (uint32_t)event.args_scancode.scancode);
                    /*pax_simple_rect(&fb, WHITE, 0, 300 + 0, pax_buf_get_width(&fb), 72);
                    pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 300 + 0, "Scancode event");
                    char text[64];
                    snprintf(text, sizeof(text), "Scancode:  0x%0" PRIX32, (uint32_t)event.args_scancode.scancode);
                    pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 300 + 36, text);
                    blit();*/
                    break;
                case INPUT_EVENT_TYPE_NONE:
                case INPUT_EVENT_TYPE_LAST:
                default:
		    // XXX these are all a NOOP, at least for now
                    break;
	    }
	}

	// these bits lifted from mini_browser.c ////////////////////////////////////////

        snprintf(barline, sizeof(barline), "%s", url_buf);
        //draw_ui(ren, barline);

        int running = 1;
        if (need_fetch) {
            trim_inplace(url_buf);
            if (!url_buf[0]) { need_fetch = 0; }
            else {
                if (!has_scheme(url_buf) && !(url_buf[0]=='/' && url_buf[1]=='/')) {
                    normalize_typed_url(url_buf);
                }
                if (url_buf[0]=='/' && url_buf[1]=='/') {
                    char sch[16]; scheme_from_url(page ? page->base : "https://info.cern.ch/", sch, sizeof sch);
                    char tmp[URL_MAX+17]; snprintf(tmp, sizeof tmp, "%s:%s", sch, url_buf);
                    strncpy(url_buf, tmp, URL_MAX);
		    url_buf[URL_MAX-1]=0;
                }
                if (is_http_scheme(url_buf)) {
                    /* record in history just before fetching */
                    history_push(url_buf);

                    snprintf(barline, sizeof(barline), "%s", url_buf);
                    //draw_ui(ren, barline);
                    //SDL_RenderPresent(ren);

                    uint8_t* mem_buf = NULL;
                    size_t mem_len = 0;
		    char status_text[64] = {0};
                    //int rc = fetch_url(url_buf, &m);
		    ESP_LOGI(TAG, "about to download_ram(): %s", url_buf);
		    int rc = download_ram(url_buf, &mem_buf, &mem_len, download_callback, status_text);
		    if (rc != true) {
			// XXX TODO: display status code info to user
			// XXX maybe need_fetch = 0;
			// XXX maybe free(mem_buf);
			// XXX maybe free(status_text);
                        ESP_LOGI(TAG, "[mini_browser] fetch error %d URL='%s'\n", rc, url_buf);
                        // printf("[mini_browser] fetch error %d URL='%s'\n", rc, url_buf);
                        if (page) { free(page->text); free(page); page=NULL; }
                        free(content_wrapped); content_wrapped=NULL;
                        sel_link=-1;
                    } else {
                        page_t *pg;
			if (mem_buf) {
			    pg = html_to_page((char *)mem_buf, url_buf);
			} else {
			    pg = html_to_page("", url_buf);
		        }
                        if (!pg) {
                            if (page) { free(page->text); free(page); page=NULL; }
                            free(content_wrapped); content_wrapped=NULL;
                            sel_link=-1;
                        } else {
                            char *wrapped = wrap_text(pg->text, max_cols);
                            free(content_wrapped); content_wrapped = wrapped;
                            if (page) { free(page->text); free(page); }
                            page = pg;
                            scroll_lines = 0;
                            sel_link = -1;
                            printf("[mini_browser] parsed %d links from %s\n", page->link_count, url_buf);

                            if (wrapped) {
                                printf("\n--- CONTENT START ---\n%s\n--- CONTENT END ---\n", wrapped);
                            }
			    need_draw = 1;
                        }
                    }
                    free(mem_buf);
                }
            }
            need_fetch = 0;
        }

        /* Compose bar text */
        if (page && sel_link >= 0 && sel_link < page->link_count) {
            snprintf(barline, sizeof(barline), "[%d/%d]  %s",
                     sel_link+1, page->link_count, page->links[sel_link].href);
        } else {
            snprintf(barline, sizeof(barline), "%s", url_buf);
        }

        /* Draw UI + visible text slice */
        //draw_ui(ren, barline);
        if (content_wrapped && need_draw) {
            need_draw = 0;
            pax_background(&fb, WHITE);
            int y = PAD_TOP;
            const char *p = content_wrapped;
            for (int s = 0; s < scroll_lines && p && *p; ) { if (*p++ == '\n') s++; }
            //SDL_SetRenderDrawColor(ren, 220, 220, 220, 255);
            int drawn = 0;
            while (p && *p && drawn < lines_per_page) {
                const char *nl = strchr(p, '\n');
                int len = nl ? (int)(nl - p) : (int)strlen(p);
                char tmp[1024];
                if (len > (int)sizeof(tmp)-1) len = (int)sizeof(tmp)-1;
                memcpy(tmp, p, len); tmp[len] = 0;
		//console_printf(&console_instance, tmp);
		//console_puts(&console_instance, tmp);
		//console_puts(&console_instance, "\n");
		// mini_browser.c original code: draw_text(ren, PAD_LR, y, tmp, VIEW_W - 2*PAD_LR);
                pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, y, tmp);
		//ESP_LOGI(TAG, "pax_draw_text() on y: %d", y);
                blit();
                y += (CH_H + LINE_SPACING);
                drawn++;
                p = nl ? nl + 1 : NULL;
            }
        }
    }
}


/* ---------- main ---------- */
/* int main(void) {
    SDL_StartTextInput(win);

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) { running = 0; break; }

            if (ev.type == SDL_EVENT_TEXT_INPUT) {
                if (inhibit_text_once) {
                    inhibit_text_once = false;
                    continue;
                }
                const char *t = ev.text.text;
                if (is_printable_ascii(t)) {
                    size_t curlen = strlen(url_buf);
                    if (curlen < URL_MAX - 1) {
                        url_buf[curlen] = t[0];
                        url_buf[curlen + 1] = 0;
                        sel_link = -1;
                    }
                }
            }

            if (ev.type == SDL_EVENT_KEY_DOWN) {
                SDL_Scancode sc = ev.key.scancode;

		*/
                /* Track accelerator press/release */
                /* if (sc == SC_ACCELERATOR) {
                    accel_down = true;
                    inhibit_text_once = true;
                    continue;
                }

                *//* Special one-shot keys -> direct navigate */
                /*if (sc == SC_SPECIAL_124) { strncpy(url_buf, SPECIAL_URL_124, URL_MAX); url_buf[URL_MAX-1]=0; need_fetch=1; sel_link=-1; inhibit_text_once=true; continue; }
                if (sc == SC_SPECIAL_125) { strncpy(url_buf, SPECIAL_URL_125, URL_MAX); url_buf[URL_MAX-1]=0; need_fetch=1; sel_link=-1; inhibit_text_once=true; continue; }
                if (sc == SC_SPECIAL_126) { strncpy(url_buf, SPECIAL_URL_126, URL_MAX); url_buf[URL_MAX-1]=0; need_fetch=1; sel_link=-1; inhibit_text_once=true; continue; }
                if (sc == SC_SPECIAL_127) { strncpy(url_buf, SPECIAL_URL_127, URL_MAX); url_buf[URL_MAX-1]=0; need_fetch=1; sel_link=-1; inhibit_text_once=true; continue; }
                if (sc == SC_SPECIAL_128) { strncpy(url_buf, SPECIAL_URL_128, URL_MAX); url_buf[URL_MAX-1]=0; need_fetch=1; sel_link=-1; inhibit_text_once=true; continue; }
                if (sc == SC_SPECIAL_129) { strncpy(url_buf, SPECIAL_URL_129, URL_MAX); url_buf[URL_MAX-1]=0; need_fetch=1; sel_link=-1; inhibit_text_once=true; continue; }

                *//* Accelerator combos (E,H,R,Q,B) */
                /*if (accel_down) {
                    switch (sc) {
                        case SDL_SCANCODE_E:
                            strncpy(url_buf, "https://", URL_MAX);
                            url_buf[URL_MAX-1] = 0;
                            sel_link = -1;
                            inhibit_text_once = true;
                            break;
                        case SDL_SCANCODE_H:
                            strncpy(url_buf, HOME_URL, URL_MAX);
                            url_buf[URL_MAX-1] = 0;
                            need_fetch = 1;
                            sel_link = -1;
                            inhibit_text_once = true;
                            break;
                        case SDL_SCANCODE_R:
                            need_fetch = 1;
                            inhibit_text_once = true;
                            break;
                        case SDL_SCANCODE_B: { *//* BACK */
                            /*char prev[URL_MAX];
                            if (history_back(prev)) {
                                strncpy(url_buf, prev, URL_MAX);
                                url_buf[URL_MAX-1] = 0;
                                need_fetch = 1;
                                sel_link = -1;
                            }
                            inhibit_text_once = true;
                            break;
                        }
                        case SDL_SCANCODE_Q:
                            running = 0;
                            inhibit_text_once = true;
                            break;
                        default:
                            break;
                    }
                    continue;
                }

                *//* Normal keys (no accelerator) */
                /*switch (sc) {
                    *//* URL actions */
                    /*case SDL_SCANCODE_BACKSPACE: {
                        size_t curlen = strlen(url_buf);
                        if (curlen) url_buf[curlen - 1] = 0;
                        sel_link = -1;
                        break;
                    }

                    *//* Scrolling */
                    /*case SDL_SCANCODE_DOWN:
                    case SDL_SCANCODE_J: scroll_lines++; break;
                    case SDL_SCANCODE_UP:
                    case SDL_SCANCODE_K: if (scroll_lines>0) scroll_lines--; break;
                    case SDL_SCANCODE_HOME: scroll_lines = 0; break;

                    *//* Link navigation */

    //SDL_StopTextInput(win);
    //if (page) { free(page->text); free(page); }
    //free(content_wrapped);
    //SDL_DestroyRenderer(ren);
    //SDL_DestroyWindow(win);
    //SDL_QuitSubSystem(SDL_INIT_VIDEO);
    //SDL_Quit();
    //curl_global_cleanup();
    //printf("[mini_browser] exit main\n");
    //return 0;

