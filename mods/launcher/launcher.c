/*
 * launcher — natartos home screen: an icon grid of goggle "apps".
 *
 * Tiles: DJI Goggles (back to the stock UI, mods loaded), PC Display, Doom,
 * audio toggles, plus any user tiles from <tmp>/natartos_apps.conf.
 *
 * Display: draws into fb_shared (two-slot protocol, see natartos_fb.h) with
 * fb_override set, so the overlay shows it instead of the DJI UI.
 * Input:   grabs event0 (gpio_keys: OK/BACK) and event3 (omc_keys: 5D joystick)
 *          with EVIOCGRAB so the DJI UI doesn't also react.
 *
 * Handing the screen to an app: keep fb_override set (so the overlay keeps
 * injecting), release input, run the app in the foreground. When it exits the
 * app/overlay tear down as usual (fb_override removed → overlay cleanup), then
 * we wait out the overlay's re-arm cooldown and take the screen back.
 *
 * Exit to DJI: release input, remove fb_override → overlay cleanup repaints UI.
 *
 * Tile config lines (user file, appended to built-ins):
 *   NAME|SUBTITLE|ICON_PATH|REQUIRES_PATH|ACTION
 *   ACTION: @dji            back to the DJI UI
 *           @toggle:<path>  flag file on/off (shown as ON/OFF)
 *           !<command>      full-screen app: runs in the foreground, then back here
 *           <command>       run in the background, stay here
 *   ICON_PATH: NC01 .rgba (assets/icons/README.md), a built-in icon (@goggles,
 *              @video, @speaker — drawn muted when its toggle is on), or empty
 *              for a letter icon.
 *   REQUIRES_PATH: tile hidden unless this path exists (empty = always shown).
 *
 * Paths are overridable for host testing: NATARTOS_TMP (default /data/local/tmp),
 * NATARTOS_INPUT0 / NATARTOS_INPUT3, NATARTOS_APPS_CONF.
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 -I include -o launcher mods/launcher/launcher.c
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "natartos_fb.h"

/* ── paths ── */

static char g_tmp[128] = "/data/local/tmp";
static char P_SHM[320], P_OVR[320], P_STOP[320], P_LOG[320], P_LOCK[320], P_USERCONF[320];
static const char *g_input_paths[2] = { "/dev/input/event0", "/dev/input/event3" };

static void init_paths(void) {
    const char *t = getenv("NATARTOS_TMP");
    if (t && *t) {
        if (strlen(t) >= sizeof g_tmp) { fprintf(stderr, "launcher: NATARTOS_TMP too long\n"); exit(1); }
        snprintf(g_tmp, sizeof g_tmp, "%s", t);
    }
    snprintf(P_SHM, sizeof P_SHM, "%s/fb_shared", g_tmp);
    snprintf(P_OVR, sizeof P_OVR, "%s/fb_override", g_tmp);
    snprintf(P_STOP, sizeof P_STOP, "%s/fb_stop", g_tmp);
    snprintf(P_LOG, sizeof P_LOG, "%s/natartos_launcher.log", g_tmp);
    snprintf(P_LOCK, sizeof P_LOCK, "%s/natartos_launcher.lock", g_tmp);
    const char *c = getenv("NATARTOS_APPS_CONF");
    snprintf(P_USERCONF, sizeof P_USERCONF, "%s", c && *c ? c : "");
    if (!*P_USERCONF) snprintf(P_USERCONF, sizeof P_USERCONF, "%s/natartos_apps.conf", g_tmp);
    const char *i0 = getenv("NATARTOS_INPUT0"), *i3 = getenv("NATARTOS_INPUT3");
    if (i0 && *i0) g_input_paths[0] = i0;
    if (i3 && *i3) g_input_paths[1] = i3;
}

static void logmsg(const char *fmt, ...) {
    FILE *f = fopen(P_LOG, "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    fprintf(f, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static int exists(const char *p) { return p && *p && access(p, F_OK) == 0; }

static void touch(const char *p) {
    int fd = open(p, O_WRONLY | O_CREAT, 0666);
    if (fd >= 0) close(fd);
}

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ── tiles ── */

#define MAX_TILES 8
#define MAX_TXT   40

typedef struct {
    char name[MAX_TXT];
    char sub[MAX_TXT];
    char icon[160];
    char action[512];
    unsigned char *icon_px;  /* premultiplied RGBA */
    int icon_w, icon_h;
} tile;

static tile g_tiles[MAX_TILES];
static int g_ntiles = 0;
static int g_sel = 0;

static void upcase(char *s) { for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32; }

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
}

static void load_icon(tile *t) {
    t->icon_px = NULL;
    if (!*t->icon || t->icon[0] == '@') return;
    FILE *f = fopen(t->icon, "rb");
    if (!f) return;
    unsigned char h[12];
    if (fread(h, 1, 12, f) == 12 && !memcmp(h, "NC01", 4)) {
        unsigned w = h[4] | h[5] << 8 | h[6] << 16 | (unsigned)h[7] << 24;
        unsigned hh = h[8] | h[9] << 8 | h[10] << 16 | (unsigned)h[11] << 24;
        if (w >= 1 && w <= 256 && hh >= 1 && hh <= 256) {
            unsigned char *px = malloc((size_t)w * hh * 4);
            if (px && fread(px, 1, (size_t)w * hh * 4, f) == (size_t)w * hh * 4) {
                t->icon_px = px; t->icon_w = (int)w; t->icon_h = (int)hh;
            } else free(px);
        }
    }
    fclose(f);
}

/* line: NAME|SUB|ICON|REQUIRES|ACTION */
static void add_tile_line(const char *line_in) {
    if (g_ntiles >= MAX_TILES) return;
    char line[1024];
    snprintf(line, sizeof line, "%s", line_in);
    trim(line);
    if (!*line || *line == '#') return;
    char *f[5] = {0};
    char *p = line;
    for (int i = 0; i < 5; i++) {
        f[i] = p;
        if (i < 4) {
            char *bar = strchr(p, '|');
            if (!bar) { logmsg("bad tile line (need 5 fields): %s", line_in); return; }
            *bar = 0;
            p = bar + 1;
        }
    }
    if (*f[3] && !exists(f[3])) return;   /* requirement missing → hide */
    tile *t = &g_tiles[g_ntiles];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", f[0]);
    snprintf(t->sub, sizeof t->sub, "%s", f[1]);
    snprintf(t->icon, sizeof t->icon, "%s", f[2]);
    snprintf(t->action, sizeof t->action, "%s", f[4]);
    upcase(t->name);
    upcase(t->sub);
    if (!*t->action) return;
    load_icon(t);
    g_ntiles++;
}

static void load_tiles(void) {
    for (int i = 0; i < g_ntiles; i++) free(g_tiles[i].icon_px);
    g_ntiles = 0;
    char l[1024];
    const char *T = g_tmp;
    add_tile_line("DJI Goggles|Back to DJI UI|@goggles||@dji");
    snprintf(l, sizeof l,
        "PC Display|Mac or PC over USB|/vendor/natartos/icons/natartos_icon_stream.rgba|"
        "/vendor/natartos/apps/stream/fb_receiver|"
        "!rm -f %s/fb_pipe; mkfifo %s/fb_pipe; /vendor/natartos/apps/stream/fb_receiver %s/fb_pipe; rm -f %s/fb_pipe",
        T, T, T, T);
    add_tile_line(l);
    snprintf(l, sizeof l,
        "Doom|Rip and tear|/vendor/natartos/icons/natartos_icon_doom.rgba|"
        "/vendor/natartos/apps/doom/doom_goggles|"
        "!touch %s/fb_override; /vendor/natartos/apps/doom/doom_goggles -iwad /vendor/natartos/apps/doom/freedoom1.wad; rm -f %s/fb_override",
        T, T);
    add_tile_line(l);
    add_tile_line("Videos|From the SD card|@video|/vendor/natartos/apps/player/player|"
                  "!/vendor/natartos/apps/player/player");
    snprintf(l, sizeof l, "Mute All|Goggle sounds|@speaker|/vendor/natartos/enabled/mod_mute.so|@toggle:%s/natartos_mute", T);
    add_tile_line(l);
    snprintf(l, sizeof l, "Mute Buttons|Button clicks|@speaker|/vendor/natartos/enabled/mod_mute.so|@toggle:%s/natartos_mute_buttons", T);
    add_tile_line(l);

    FILE *f = fopen(P_USERCONF, "r");
    if (f) {
        while (fgets(l, sizeof l, f)) add_tile_line(l);
        fclose(f);
    }
    if (g_sel >= g_ntiles) g_sel = g_ntiles - 1;
    if (g_sel < 0) g_sel = 0;
    logmsg("tiles: %d", g_ntiles);
}

/* ── 5×7 font: ASCII 32..95 (uppercase), row-major, bit4 = leftmost ── */

static const unsigned char FONT[64][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, /* ! */
    {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x0A,0x1F,0x0A,0x0A,0x0A,0x1F,0x0A}, /* # */
    {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, /* $ */
    {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, /* % */
    {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}, /* & */
    {0x04,0x04,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, /* ( */
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, /* ) */
    {0x00,0x04,0x15,0x0E,0x15,0x04,0x00}, /* * */
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x04,0x04,0x08}, /* , */
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x04}, /* . */
    {0x00,0x01,0x02,0x04,0x08,0x10,0x00}, /* / */
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, /* 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 5 */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 9 */
    {0x00,0x00,0x04,0x00,0x00,0x04,0x00}, /* : */
    {0x00,0x00,0x04,0x00,0x04,0x04,0x08}, /* ; */
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, /* < */
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, /* = */
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, /* > */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, /* ? */
    {0x0E,0x11,0x01,0x0D,0x15,0x15,0x0E}, /* @ */
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, /* D */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /* G */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /* J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* M */
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, /* N */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* Q */
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* S */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, /* W */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
    {0x11,0x11,0x11,0x0A,0x04,0x04,0x04}, /* Y */
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* Z */
    {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, /* [ */
    {0x00,0x10,0x08,0x04,0x02,0x01,0x00}, /* \ */
    {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, /* ] */
    {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, /* _ */
};

/* ── drawing (BGRA, FB_W × FB_H) ── */

typedef struct { unsigned char r, g, b; } rgb;

static unsigned char *g_px;  /* current back slot */

static inline void put(int x, int y, rgb c) {
    if ((unsigned)x >= FB_W || (unsigned)y >= FB_H) return;
    unsigned char *p = g_px + ((size_t)y * FB_W + x) * 4;
    p[0] = c.b; p[1] = c.g; p[2] = c.r; p[3] = 255;
}

static void rect(int x, int y, int w, int h, rgb c) {
    for (int yy = y; yy < y + h; yy++) for (int xx = x; xx < x + w; xx++) put(xx, yy, c);
}

/* filled rounded rect */
static void rrect(int x, int y, int w, int h, int r, rgb c) {
    for (int yy = 0; yy < h; yy++) {
        int inset = 0;
        int dy = yy < r ? r - yy : (yy >= h - r ? yy - (h - r - 1) : 0);
        if (dy) {
            while (inset < r) {
                int dx = r - inset;
                if (dx * dx + dy * dy <= r * r) break;
                inset++;
            }
        }
        for (int xx = x + inset; xx < x + w - inset; xx++) put(xx, y + yy, c);
    }
}

static void disc(int cx, int cy, int r, rgb c) {
    for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++)
        if (x * x + y * y <= r * r) put(cx + x, cy + y, c);
}

static int text_w(const char *s, int sc) {
    int n = (int)strlen(s);
    return n ? n * 6 * sc - sc : 0;
}

static void text(const char *s, int x, int y, int sc, rgb c) {
    for (; *s; s++, x += 6 * sc) {
        int ch = (unsigned char)*s;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < 32 || ch > 95) ch = '?';
        const unsigned char *g = FONT[ch - 32];
        for (int r = 0; r < 7; r++)
            for (int col = 0; col < 5; col++)
                if (g[r] & (0x10 >> col))
                    rect(x + col * sc, y + r * sc, sc, sc, c);
    }
}

/* text clipped to max_w by dropping trailing chars */
static void text_fit(const char *s, int cx, int y, int sc, int max_w, rgb c) {
    char buf[MAX_TXT];
    snprintf(buf, sizeof buf, "%s", s);
    while (*buf && text_w(buf, sc) > max_w) buf[strlen(buf) - 1] = 0;
    text(buf, cx - text_w(buf, sc) / 2, y, sc, c);
}

/* premultiplied RGBA icon, nearest-neighbour scaled by `sc` */
static void blit_icon(const tile *t, int x, int y, int sc) {
    for (int iy = 0; iy < t->icon_h * sc; iy++) {
        for (int ix = 0; ix < t->icon_w * sc; ix++) {
            const unsigned char *s = t->icon_px + ((size_t)(iy / sc) * t->icon_w + ix / sc) * 4;
            int a = s[3];
            if (!a) continue;
            int px = x + ix, py = y + iy;
            if ((unsigned)px >= FB_W || (unsigned)py >= FB_H) continue;
            unsigned char *d = g_px + ((size_t)py * FB_W + px) * 4;
            d[0] = (unsigned char)(s[2] + d[0] * (255 - a) / 255);
            d[1] = (unsigned char)(s[1] + d[1] * (255 - a) / 255);
            d[2] = (unsigned char)(s[0] + d[2] * (255 - a) / 255);
        }
    }
}

/* filled triangle (for the speaker cone) */
static void tri(int x0, int y0, int x1, int y1, int x2, int y2, rgb c) {
    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++) {
        int w0 = (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0);
        int w1 = (x2 - x1) * (y - y1) - (y2 - y1) * (x - x1);
        int w2 = (x0 - x2) * (y - y2) - (y0 - y2) * (x - x2);
        if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) put(x, y, c);
    }
}

/* ring segment on the right side (sound waves) */
static void arc_right(int cx, int cy, int r, int th, rgb c) {
    for (int y = -r; y <= r; y++) for (int x = 0; x <= r; x++) {
        int d = x * x + y * y;
        if (d <= r * r && d >= (r - th) * (r - th) && x * 10 >= (y < 0 ? -y : y) * 7) put(cx + x, cy + y, c);
    }
}

static void thick_line(int x0, int y0, int x1, int y1, int th, rgb c) {
    int steps = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
    for (int i = 0; i <= steps; i++) {
        int x = x0 + (x1 - x0) * i / (steps ? steps : 1);
        int y = y0 + (y1 - y0) * i / (steps ? steps : 1);
        rect(x - th / 2, y - th / 2, th, th, c);
    }
}

static const rgb BG      = { 0x12, 0x14, 0x18 };
static const rgb TILE    = { 0x24, 0x27, 0x2E };
static const rgb TILE_HI = { 0x34, 0x39, 0x44 };
static const rgb RING    = { 0xF2, 0xF4, 0xF7 };
static const rgb FG      = { 0xF2, 0xF4, 0xF7 };
static const rgb DIM     = { 0x8A, 0x91, 0x9C };
static const rgb ON      = { 0x3D, 0xD6, 0x8C };

static rgb accent_for(const char *s) {
    static const rgb pal[] = {
        {0x4F,0x8D,0xF7}, {0xF2,0x8C,0x38}, {0x9B,0x6B,0xF2},
        {0x2F,0xB8,0xA6}, {0xE8,0x5D,0x75}, {0xD9,0xB3,0x2E},
    };
    unsigned h = 5381;
    for (; *s; s++) h = h * 33 + (unsigned char)*s;
    return pal[h % (sizeof pal / sizeof pal[0])];
}

static int toggle_on(const tile *t) {
    return !strncmp(t->action, "@toggle:", 8) && exists(t->action + 8);
}

/* layout: up to 4 columns × 2 rows, centred */
#define COLS 4
#define TW 360
#define TH 330
#define GAP 40
#define GRID_TOP 230

static void tile_pos(int i, int n, int *x, int *y) {
    int row = i / COLS, col = i % COLS;
    int in_row = n - row * COLS;
    if (in_row > COLS) in_row = COLS;
    int row_w = in_row * TW + (in_row - 1) * GAP;
    *x = (FB_W - row_w) / 2 + col * (TW + GAP);
    *y = GRID_TOP + row * (TH + GAP);
}

static void draw_tile(int i) {
    const tile *t = &g_tiles[i];
    int x, y;
    tile_pos(i, g_ntiles, &x, &y);
    int sel = i == g_sel;
    if (sel) rrect(x - 8, y - 8, TW + 16, TH + 16, 34, RING);
    rrect(x, y, TW, TH, 28, sel ? TILE_HI : TILE);

    int icx = x + TW / 2, icy = y + 40 + 104;   /* icon centre */
    if (!strcmp(t->icon, "@goggles")) {
        disc(icx, icy, 96, accent_for(t->name));
        rrect(icx - 70, icy - 30, 64, 52, 18, FG);          /* lenses */
        rrect(icx + 6, icy - 30, 64, 52, 18, FG);
        rect(icx - 8, icy - 14, 16, 10, FG);                 /* bridge */
        rect(icx - 82, icy - 40, 164, 10, FG);               /* strap line */
    } else if (!strcmp(t->icon, "@video")) {
        disc(icx, icy, 96, accent_for(t->name));
        rrect(icx - 62, icy - 44, 124, 88, 16, FG);           /* screen */
        tri(icx - 16, icy - 26, icx - 16, icy + 26, icx + 28, icy, accent_for(t->name));
    } else if (!strcmp(t->icon, "@speaker")) {
        int muted = toggle_on(t);
        disc(icx, icy, 96, muted ? (rgb){0x5A,0x60,0x6B} : accent_for(t->name));
        rect(icx - 56, icy - 18, 26, 36, FG);                /* box */
        tri(icx - 32, icy - 18, icx + 2, icy - 48, icx + 2, icy + 48, FG);
        tri(icx - 32, icy + 18, icx - 32, icy - 18, icx + 2, icy + 48, FG);
        if (muted) {
            thick_line(icx + 18, icy - 22, icx + 58, icy + 22, 10, FG);
            thick_line(icx + 18, icy + 22, icx + 58, icy - 22, 10, FG);
        } else {
            arc_right(icx + 4, icy, 34, 9, FG);
            arc_right(icx + 4, icy, 58, 9, FG);
        }
    } else if (t->icon_px) {
        int sc = 2;
        blit_icon(t, icx - t->icon_w * sc / 2, icy - t->icon_h * sc / 2, sc);
    } else {
        rgb a = accent_for(t->name);
        disc(icx, icy, 96, a);
        char letter[2] = { t->name[0] ? t->name[0] : '?', 0 };
        text(letter, icx - text_w(letter, 14) / 2, icy - 7 * 14 / 2, 14, FG);
    }

    text_fit(t->name, x + TW / 2, y + 256, 4, TW - 28, FG);
    if (!strncmp(t->action, "@toggle:", 8)) {
        int on = toggle_on(t);
        text_fit(on ? "ON" : "OFF", x + TW / 2, y + 296, 3, TW - 28, on ? ON : DIM);
    } else {
        text_fit(t->sub, x + TW / 2, y + 296, 3, TW - 28, DIM);
    }
}

static natartos_fb g_fb;

static void render(void) {
    g_px = natartos_fb_back(&g_fb);
    rect(0, 0, FB_W, FB_H, BG);
    text("NATARTOS", 120, 90, 7, FG);
    text("HOME", 120 + text_w("NATARTOS", 7) + 40, 90 + 7 * 7 - 7 * 4, 4, DIM);
    if (!g_ntiles) {
        const char *m = "NO APPS INSTALLED";
        text(m, (FB_W - text_w(m, 5)) / 2, 500, 5, DIM);
    }
    for (int i = 0; i < g_ntiles; i++) draw_tile(i);
    const char *hint = "JOYSTICK: MOVE    PRESS: OPEN    BACK: GOGGLES";
    text(hint, (FB_W - text_w(hint, 3)) / 2, 1000, 3, DIM);
    natartos_fb_publish(&g_fb);
}

/* ── input ── */

static int g_in[2] = { -1, -1 };

static void input_open(void) {
    for (int i = 0; i < 2; i++) {
        if (g_in[i] >= 0) continue;
        g_in[i] = open(g_input_paths[i], O_RDONLY | O_NONBLOCK);
        if (g_in[i] >= 0) ioctl(g_in[i], EVIOCGRAB, 1);   /* fails harmlessly on non-evdev */
    }
}

static void input_close(void) {
    for (int i = 0; i < 2; i++) {
        if (g_in[i] < 0) continue;
        ioctl(g_in[i], EVIOCGRAB, 0);
        close(g_in[i]);
        g_in[i] = -1;
    }
}

enum { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_OK, K_BACK };

/* The G3's joystick codes come out rotated 180° relative to how it is held
 * (same mapping as doomgeneric_goggles.c, verified on hardware). */
static int map_key(int code) {
    switch (code) {
        case KEY_DOWN:  return K_UP;
        case KEY_UP:    return K_DOWN;
        case KEY_RIGHT: return K_LEFT;
        case KEY_LEFT:  return K_RIGHT;
        case KEY_ENTER: return K_OK;      /* 28: OK button and 5D press */
        case KEY_BACK:  return K_BACK;    /* 158 */
        case KEY_ESC:   return K_BACK;
        default:        return K_NONE;
    }
}

/* wait up to `ms` for a key press; returns K_* (K_NONE on timeout) */
static int read_key(int ms) {
    struct pollfd p[2];
    int n = 0;
    for (int i = 0; i < 2; i++) if (g_in[i] >= 0) { p[n].fd = g_in[i]; p[n].events = POLLIN; n++; }
    if (!n) { sleep_ms(ms); input_open(); return K_NONE; }
    if (poll(p, n, ms) <= 0) return K_NONE;
    for (int i = 0; i < n; i++) {
        if (!(p[i].revents & POLLIN)) continue;
        struct input_event ev;
        while (read(p[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
            if (ev.type != EV_KEY || ev.value == 0) continue;   /* press (1) or repeat (2) */
            int k = map_key(ev.code);
            if (k == K_OK && ev.value == 2) continue;          /* don't auto-repeat OK */
            if (k != K_NONE) return k;
        }
    }
    return K_NONE;
}

/* ── screen ownership ── */

static int take_screen(void) {
    if (natartos_fb_open(&g_fb, P_SHM) != 0) { logmsg("fb_shared: %s", strerror(errno)); return -1; }
    render();
    touch(P_OVR);
    input_open();
    return 0;
}

static void release_screen(int remove_override) {
    input_close();
    natartos_fb_close(&g_fb);
    if (remove_override) unlink(P_OVR);
}

static volatile sig_atomic_t g_quit = 0;
static void on_signal(int s) { (void)s; g_quit = 1; }

/* Run a full-screen app, then take the screen back. */
static void run_app(const char *cmd) {
    logmsg("app: %s", cmd);
    release_screen(0);          /* keep fb_override: the overlay keeps injecting */
    int rc = system(cmd);
    logmsg("app exited rc=%d", rc);

    /* The app's exit normally tears the overlay down (fb_override removed →
     * cleanup kills apps, deletes fb_shared, repaints the DJI UI). Wait for that
     * to finish, then past the overlay's ~2s re-arm cooldown, before retaking. */
    if (!exists(P_OVR)) {
        for (int i = 0; i < 40 && exists(P_SHM); i++) sleep_ms(50);
        sleep_ms(2500);
    }
    if (take_screen() != 0) g_quit = 1;
}

static void activate(const tile *t) {
    const char *a = t->action;
    if (!strcmp(a, "@dji")) { g_quit = 1; return; }
    if (!strncmp(a, "@toggle:", 8)) {
        const char *f = a + 8;
        if (exists(f)) unlink(f); else touch(f);
        logmsg("toggle %s -> %s", f, exists(f) ? "on" : "off");
        render();
        return;
    }
    if (a[0] == '!') { run_app(a + 1); return; }
    char bg[600];
    snprintf(bg, sizeof bg, "(%s) >/dev/null 2>&1 &", a);
    logmsg("run: %s", a);
    if (system(bg) != 0) logmsg("run failed");
}

static void move(int k) {
    int n = g_ntiles;
    if (!n) return;
    int row = g_sel / COLS, col = g_sel % COLS, rows = (n + COLS - 1) / COLS;
    switch (k) {
        case K_LEFT:  g_sel = (g_sel + n - 1) % n; return;
        case K_RIGHT: g_sel = (g_sel + 1) % n; return;
        case K_UP:    row = (row + rows - 1) % rows; break;
        case K_DOWN:  row = (row + 1) % rows; break;
    }
    int idx = row * COLS + col;
    if (idx >= n) idx = n - 1;
    g_sel = idx;
}

int main(int argc, char **argv) {
    init_paths();
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* one instance */
    int lock = open(P_LOCK, O_WRONLY | O_CREAT, 0666);
    if (lock < 0) { fprintf(stderr, "launcher: %s: %s\n", P_LOCK, strerror(errno)); return 1; }
    if (flock(lock, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "launcher: already running\n");
        return 0;
    }

    load_tiles();

    /* --render-only: draw once and exit (host testing / screenshots) */
    if (argc > 1 && !strcmp(argv[1], "--render-only")) {
        if (argc > 2) g_sel = atoi(argv[2]);
        if (natartos_fb_open(&g_fb, P_SHM) != 0) return 1;
        render();
        natartos_fb_close(&g_fb);
        return 0;
    }

    unlink(P_STOP);             /* a stale stop flag would make the overlay tear us down */
    if (take_screen() != 0) return 1;
    logmsg("launcher up");

    while (!g_quit) {
        int k = read_key(250);
        if (!exists(P_OVR)) { logmsg("fb_override gone — leaving"); release_screen(0); return 0; }
        if (k == K_NONE) continue;
        if (k == K_BACK) break;
        if (k == K_OK) { if (g_ntiles) activate(&g_tiles[g_sel]); continue; }
        move(k);
        render();
    }

    logmsg("back to DJI");
    release_screen(1);          /* overlay cleanup repaints the DJI UI */
    return 0;
}
