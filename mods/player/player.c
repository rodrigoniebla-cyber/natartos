/*
 * player — natartos video player: MPEG-1 video + MP2 audio from the SD card,
 * full-screen on the goggles, sound through the built-in speaker.
 *
 * Files:  *.mpg in natartos/videos/ on the SD card (or /data/local/tmp/videos).
 *         Convert anything with:
 *           ffmpeg -i in.mp4 -c:v mpeg1video -q:v 4 -vf scale=-2:720 \
 *                  -c:a mp2 -b:a 192k -ar 48000 -ac 2 out.mpg
 * UI:     file list → playback. Joystick moves, OK opens / pauses,
 *         left/right seek 10s, BACK stops / leaves.
 * Video:  pl_mpeg (software decode) → BGRA → fitted to 1920×1080 → fb_shared.
 * Audio:  pl_mpeg MP2 → S16 stereo → ring buffer → audio thread → tinyalsa
 *         (static, talks to /dev/snd directly), resampled if the speaker
 *         won't take the file's rate. If the speaker can't be opened (e.g.
 *         dji_glasses holds it) the video still plays, silently.
 *
 * Env (host testing / overrides):
 *   NATARTOS_TMP, NATARTOS_INPUT0, NATARTOS_INPUT3   as in the launcher
 *   NATARTOS_VIDEO_DIRS   colon-separated dirs (replaces the defaults)
 *   NATARTOS_AUDIO        auto (default) | none | card,device | file:/path.raw
 *   NATARTOS_AUDIO_RATE   force the output rate (tests the resampler)
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 -I include -I third_party \
 *          -I third_party/tinyalsa/include -o player mods/player/player.c \
 *          third_party/tinyalsa/pcm.c third_party/tinyalsa/limits.c -lpthread -lm
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <tinyalsa/asoundlib.h>

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg/pl_mpeg.h"

#include "natartos_fb.h"
#include "natartos_ui.h"

/* ── paths / logging ── */

static char g_tmp[128] = "/data/local/tmp";
static char P_SHM[256], P_OVR[256], P_STOP[256], P_LOG[256], P_LOCK[256];
static const char *g_input_paths[2] = { "/dev/input/event0", "/dev/input/event3" };

static void init_paths(void) {
    const char *t = getenv("NATARTOS_TMP");
    if (t && *t) {
        if (strlen(t) >= sizeof g_tmp) { fprintf(stderr, "player: NATARTOS_TMP too long\n"); exit(1); }
        snprintf(g_tmp, sizeof g_tmp, "%s", t);
    }
    snprintf(P_SHM, sizeof P_SHM, "%s/fb_shared", g_tmp);
    snprintf(P_OVR, sizeof P_OVR, "%s/fb_override", g_tmp);
    snprintf(P_STOP, sizeof P_STOP, "%s/fb_stop", g_tmp);
    snprintf(P_LOG, sizeof P_LOG, "%s/natartos_player.log", g_tmp);
    snprintf(P_LOCK, sizeof P_LOCK, "%s/natartos_player.lock", g_tmp);
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

static int exists(const char *p) { return access(p, F_OK) == 0; }

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ── colours ── */

static const nui_rgb BG   = { 0x12, 0x14, 0x18 };
static const nui_rgb ROW  = { 0x24, 0x27, 0x2E };
static const nui_rgb ROWH = { 0x34, 0x39, 0x44 };
static const nui_rgb FG   = { 0xF2, 0xF4, 0xF7 };
static const nui_rgb DIM  = { 0x8A, 0x91, 0x9C };
static const nui_rgb ACC  = { 0x4F, 0x8D, 0xF7 };
static const nui_rgb BLACK = { 0, 0, 0 };

/* ── file list ── */

#define MAX_FILES 200
static char *g_files[MAX_FILES];
static int g_nfiles = 0, g_sel = 0, g_top = 0;

static int cmp_str(const void *a, const void *b) {
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static int is_mpg(const char *n) {
    size_t l = strlen(n);
    return (l > 4 && !strcasecmp(n + l - 4, ".mpg")) || (l > 5 && !strcasecmp(n + l - 5, ".mpeg"));
}

static void scan_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && g_nfiles < MAX_FILES) {
        if (e->d_name[0] == '.' || !is_mpg(e->d_name)) continue;
        char p[512];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) g_files[g_nfiles++] = strdup(p);
    }
    closedir(d);
}

static void scan_files(void) {
    for (int i = 0; i < g_nfiles; i++) free(g_files[i]);
    g_nfiles = 0;
    const char *env = getenv("NATARTOS_VIDEO_DIRS");
    if (env && *env) {
        char buf[1024];
        snprintf(buf, sizeof buf, "%s", env);
        for (char *s = strtok(buf, ":"); s; s = strtok(NULL, ":")) scan_dir(s);
    } else {
        /* SD card mount points vary; try the usual ones (glob covers media_rw UUIDs). */
        static const char *pats[] = {
            "/storage/sdcard1/natartos/videos", "/storage/sdcard0/natartos/videos",
            "/storage/*/natartos/videos", "/mnt/media_rw/*/natartos/videos",
            "/mnt/extsd/natartos/videos", "/sdcard/natartos/videos",
            "/data/local/tmp/videos",
        };
        char seen[16][256];
        int nseen = 0;
        for (size_t i = 0; i < sizeof pats / sizeof pats[0]; i++) {
            glob_t g;
            if (glob(pats[i], GLOB_NOSORT, NULL, &g) != 0) continue;
            for (size_t j = 0; j < g.gl_pathc; j++) {
                char real[256];
                if (!realpath(g.gl_pathv[j], real)) continue;
                int dup = 0;
                for (int k = 0; k < nseen; k++) if (!strcmp(seen[k], real)) dup = 1;
                if (dup || nseen >= 16) continue;
                snprintf(seen[nseen++], sizeof seen[0], "%s", real);
                scan_dir(real);
            }
            globfree(&g);
        }
    }
    qsort(g_files, g_nfiles, sizeof g_files[0], cmp_str);
    if (g_sel >= g_nfiles) g_sel = g_nfiles ? g_nfiles - 1 : 0;
    logmsg("videos: %d", g_nfiles);
}

/* ── screen ── */

static natartos_fb g_fb;

static void touch(const char *p) { int fd = open(p, O_WRONLY | O_CREAT, 0666); if (fd >= 0) close(fd); }

#define LIST_ROWS 11
#define ROW_H 64

static const char *basename_of(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

static void render_list(const char *status) {
    nui_px = natartos_fb_back(&g_fb);
    nui_rect(0, 0, FB_W, FB_H, BG);
    nui_text("VIDEOS", 120, 90, 7, FG);
    char cnt[32];
    snprintf(cnt, sizeof cnt, "%d", g_nfiles);
    nui_text(cnt, 120 + nui_text_w("VIDEOS", 7) + 40, 90 + 21, 4, DIM);
    if (!g_nfiles) {
        const char *a = "NO VIDEOS YET";
        const char *b = "PUT .MPG FILES IN NATARTOS/VIDEOS ON THE SD CARD";
        nui_text(a, (FB_W - nui_text_w(a, 6)) / 2, 440, 6, FG);
        nui_text(b, (FB_W - nui_text_w(b, 3)) / 2, 520, 3, DIM);
    }
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + LIST_ROWS) g_top = g_sel - LIST_ROWS + 1;
    for (int r = 0; r < LIST_ROWS && g_top + r < g_nfiles; r++) {
        int i = g_top + r, y = 200 + r * (ROW_H + 10);
        int sel = i == g_sel;
        nui_rrect(120, y, FB_W - 240, ROW_H, 16, sel ? ROWH : ROW);
        if (sel) nui_rect(120, y + 12, 8, ROW_H - 24, ACC);
        nui_text_clip(basename_of(g_files[i]), 160, y + (ROW_H - 28) / 2, 4, FB_W - 320, sel ? FG : DIM);
    }
    if (g_top > 0) nui_text("^", FB_W / 2, 175, 3, DIM);
    if (g_top + LIST_ROWS < g_nfiles) nui_text("V", FB_W / 2, 200 + LIST_ROWS * (ROW_H + 10), 3, DIM);
    const char *hint = status && *status ? status : "JOYSTICK: MOVE    PRESS: PLAY    BACK: EXIT";
    nui_text(hint, (FB_W - nui_text_w(hint, 3)) / 2, 1010, 3, status && *status ? ACC : DIM);
    natartos_fb_publish(&g_fb);
}

/* ── input (same rotated joystick mapping as Doom / the launcher) ── */

enum { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_OK, K_BACK };
static int g_in[2] = { -1, -1 };

static void input_open(void) {
    for (int i = 0; i < 2; i++) {
        if (g_in[i] >= 0) continue;
        g_in[i] = open(g_input_paths[i], O_RDONLY | O_NONBLOCK);
        if (g_in[i] >= 0) ioctl(g_in[i], EVIOCGRAB, 1);
    }
}

static void input_close(void) {
    for (int i = 0; i < 2; i++) if (g_in[i] >= 0) { ioctl(g_in[i], EVIOCGRAB, 0); close(g_in[i]); g_in[i] = -1; }
}

static int map_key(int code) {
    switch (code) {
        case KEY_DOWN:  return K_UP;
        case KEY_UP:    return K_DOWN;
        case KEY_RIGHT: return K_LEFT;
        case KEY_LEFT:  return K_RIGHT;
        case KEY_ENTER: return K_OK;
        case KEY_BACK: case KEY_ESC: return K_BACK;
        default: return K_NONE;
    }
}

static int read_key(int ms) {
    struct pollfd p[2];
    int n = 0;
    for (int i = 0; i < 2; i++) if (g_in[i] >= 0) { p[n].fd = g_in[i]; p[n].events = POLLIN; n++; }
    if (!n) { if (ms > 0) usleep(ms * 1000); return K_NONE; }
    if (poll(p, n, ms) <= 0) return K_NONE;
    for (int i = 0; i < n; i++) {
        if (!(p[i].revents & POLLIN)) continue;
        struct input_event ev;
        while (read(p[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
            if (ev.type != EV_KEY || ev.value == 0) continue;
            int k = map_key(ev.code);
            if ((k == K_OK || k == K_BACK) && ev.value == 2) continue;
            if (k != K_NONE) return k;
        }
    }
    return K_NONE;
}

/* ── audio: ring buffer + output thread ── */

#define RING_FRAMES (48000 * 2)          /* 2 s of stereo frames */
static int16_t g_ring[RING_FRAMES * 2];
static size_t g_rd = 0, g_wr = 0;        /* in frames, monotonically increasing */
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_audio_run = 0, g_audio_paused = 0;
static pthread_t g_audio_thr;

typedef struct {
    int kind;               /* 0 none, 1 alsa, 2 file */
    struct pcm *pcm;
    FILE *file;
    unsigned rate;          /* output rate */
    unsigned period;        /* frames per write */
} audio_out;
static audio_out g_ao;

/* linear resampler state (file rate → output rate) */
static double g_rs_pos = 0, g_rs_step = 1;
static float g_rs_prev[2] = { 0, 0 };

static int16_t f2s(float v) {
    if (v > 1.f) v = 1.f;
    if (v < -1.f) v = -1.f;
    return (int16_t)lrintf(v * 32767.f);
}

static void ring_push(float l, float r) {
    /* called with g_mx held; drops the oldest audio if the ring is full */
    if (g_wr - g_rd >= RING_FRAMES) g_rd++;
    size_t i = (g_wr % RING_FRAMES) * 2;
    g_ring[i] = f2s(l);
    g_ring[i + 1] = f2s(r);
    g_wr++;
}

static void audio_feed(const float *inter, unsigned count) {
    pthread_mutex_lock(&g_mx);
    if (g_rs_step == 1.0) {
        for (unsigned i = 0; i < count; i++) ring_push(inter[2 * i], inter[2 * i + 1]);
    } else {
        /* g_rs_pos: position in input frames, relative to this block; -1..0 = g_rs_prev */
        while (g_rs_pos < count - 1) {
            int i0 = (int)floor(g_rs_pos);
            double fr = g_rs_pos - i0;
            float l0 = i0 < 0 ? g_rs_prev[0] : inter[2 * i0], r0 = i0 < 0 ? g_rs_prev[1] : inter[2 * i0 + 1];
            float l1 = inter[2 * (i0 + 1)], r1 = inter[2 * (i0 + 1) + 1];
            ring_push((float)(l0 + (l1 - l0) * fr), (float)(r0 + (r1 - r0) * fr));
            g_rs_pos += g_rs_step;
        }
        g_rs_pos -= count;
        g_rs_prev[0] = inter[2 * (count - 1)];
        g_rs_prev[1] = inter[2 * (count - 1) + 1];
    }
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mx);
}

static void audio_flush(void) {
    pthread_mutex_lock(&g_mx);
    g_rd = g_wr;
    g_rs_pos = 0;
    pthread_mutex_unlock(&g_mx);
}

static void *audio_thread(void *arg) {
    (void)arg;
    unsigned n = g_ao.period;
    int16_t *buf = malloc((size_t)n * 4);
    while (1) {
        pthread_mutex_lock(&g_mx);
        while (g_audio_run && (g_audio_paused || g_wr - g_rd < n)) pthread_cond_wait(&g_cv, &g_mx);
        if (!g_audio_run) { pthread_mutex_unlock(&g_mx); break; }
        for (unsigned i = 0; i < n; i++) {
            size_t k = ((g_rd + i) % RING_FRAMES) * 2;
            buf[2 * i] = g_ring[k];
            buf[2 * i + 1] = g_ring[k + 1];
        }
        g_rd += n;
        pthread_mutex_unlock(&g_mx);
        if (g_ao.kind == 1) {
            int w = pcm_writei(g_ao.pcm, buf, n);              /* frames; re-prepares after underrun */
            if (w < 0) {
                logmsg("pcm_writei: %s", pcm_get_error(g_ao.pcm));
                pcm_prepare(g_ao.pcm);
            }
        } else if (g_ao.kind == 2) {
            fwrite(buf, 4, n, g_ao.file);
            fflush(g_ao.file);
            usleep((useconds_t)(1e6 * n / g_ao.rate));          /* pace like a device */
        }
    }
    free(buf);
    return NULL;
}

/* Probe playback devices from /proc/asound/pcm: lines like "00-00: ... : playback 1" */
static int alsa_try(unsigned card, unsigned dev, unsigned rate) {
    struct pcm_config cfg = {0};
    cfg.channels = 2;
    cfg.rate = rate;
    cfg.period_size = 1024;
    cfg.period_count = 4;
    cfg.format = PCM_FORMAT_S16_LE;
    struct pcm *p = pcm_open(card, dev, PCM_OUT, &cfg);
    if (!p || !pcm_is_ready(p)) {
        logmsg("audio: card %u dev %u @%u: %s", card, dev, rate, p ? pcm_get_error(p) : "open failed");
        if (p) pcm_close(p);
        return 0;
    }
    g_ao.kind = 1; g_ao.pcm = p; g_ao.rate = rate; g_ao.period = 1024;
    logmsg("audio: card %u dev %u @%u Hz", card, dev, rate);
    return 1;
}

static int alsa_open_any(unsigned card, unsigned dev, unsigned rate) {
    if (alsa_try(card, dev, rate)) return 1;
    if (rate != 48000 && alsa_try(card, dev, 48000)) return 1;
    if (rate != 44100 && alsa_try(card, dev, 44100)) return 1;
    return 0;
}

static void audio_open(unsigned file_rate) {
    memset(&g_ao, 0, sizeof g_ao);
    const char *mode = getenv("NATARTOS_AUDIO");
    const char *force = getenv("NATARTOS_AUDIO_RATE");
    unsigned want = force && *force ? (unsigned)atoi(force) : file_rate;
    if (mode && !strcmp(mode, "none")) { logmsg("audio: disabled"); return; }
    if (mode && !strncmp(mode, "file:", 5)) {
        g_ao.file = fopen(mode + 5, "wb");
        if (g_ao.file) { g_ao.kind = 2; g_ao.rate = want; g_ao.period = 1024; }
    } else if (mode && strchr(mode, ',')) {
        unsigned c = 0, d = 0;
        if (sscanf(mode, "%u,%u", &c, &d) == 2) alsa_open_any(c, d, want);
    } else {
        FILE *f = fopen("/proc/asound/pcm", "r");
        char line[256];
        while (f && fgets(line, sizeof line, f) && !g_ao.kind) {
            unsigned c, d;
            if (sscanf(line, "%u-%u:", &c, &d) == 2 && strstr(line, "playback")) alsa_open_any(c, d, want);
        }
        if (f) fclose(f);
    }
    if (!g_ao.kind) {
        logmsg("audio: no speaker available — playing without sound "
               "(dji_glasses may hold the device; see mods/mute/README.md, speaker recon)");
        return;
    }
    g_rs_step = (double)file_rate / g_ao.rate;
    g_rs_pos = 0;
    g_rd = g_wr = 0;
    g_audio_run = 1;
    g_audio_paused = 0;
    pthread_create(&g_audio_thr, NULL, audio_thread, NULL);
}

static void audio_close(void) {
    if (!g_ao.kind) return;
    pthread_mutex_lock(&g_mx);
    g_audio_run = 0;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mx);
    pthread_join(g_audio_thr, NULL);
    if (g_ao.pcm) pcm_close(g_ao.pcm);
    if (g_ao.file) fclose(g_ao.file);
    memset(&g_ao, 0, sizeof g_ao);
}

static void audio_pause(int p) {
    pthread_mutex_lock(&g_mx);
    g_audio_paused = p;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mx);
}

/* ── video: frame → fitted BGRA in the back slot ── */

static uint8_t *g_frame;          /* native-size BGRA of the last decoded frame */
static int g_fw = 0, g_fh = 0;
static int *g_xmap = NULL;        /* dst x → src byte offset */
static int g_dx, g_dy, g_dw, g_dh;
static double g_osd_until = 0;
static int g_paused = 0;
static plm_t *g_plm;
static const char *g_title = "";
static long g_frames_shown = 0;

static void fit_setup(int w, int h) {
    g_fw = w; g_fh = h;
    free(g_frame);
    g_frame = malloc((size_t)w * h * 4);
    memset(g_frame, 0xFF, (size_t)w * h * 4);          /* alpha = 255; pl_mpeg leaves A untouched */
    /* fit inside 1920×1080, keep aspect */
    if ((long)w * FB_H > (long)h * FB_W) { g_dw = FB_W; g_dh = (int)((long)h * FB_W / w); }
    else                                  { g_dh = FB_H; g_dw = (int)((long)w * FB_H / h); }
    g_dx = (FB_W - g_dw) / 2; g_dy = (FB_H - g_dh) / 2;
    free(g_xmap);
    g_xmap = malloc(sizeof(int) * g_dw);
    for (int x = 0; x < g_dw; x++) g_xmap[x] = (int)((long)x * w / g_dw) * 4;
}

static void fmt_time(double t, char *out, size_t n) {
    int s = (int)t;
    snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

static void draw_osd(void) {
    double t = plm_get_time(g_plm), d = plm_get_duration(g_plm);
    int y = FB_H - 130;
    nui_rrect(80, y - 20, FB_W - 160, 110, 24, (nui_rgb){ 0x12, 0x14, 0x18 });
    nui_text_clip(g_title, 120, y, 3, FB_W - 640, FG);
    char a[16], b[16], tm[64];
    fmt_time(t, a, sizeof a); fmt_time(d, b, sizeof b);
    snprintf(tm, sizeof tm, "%s%s / %s", g_paused ? "PAUSED   " : "", a, b);
    nui_text(tm, FB_W - 120 - nui_text_w(tm, 3), y, 3, g_paused ? ACC : DIM);
    int bw = FB_W - 240;
    nui_rect(120, y + 46, bw, 10, (nui_rgb){ 0x3A, 0x3F, 0x4A });
    if (d > 0) nui_rect(120, y + 46, (int)(bw * (t / d > 1 ? 1 : t / d)), 10, ACC);
}

static void show_frame(void) {
    uint8_t *dst = natartos_fb_back(&g_fb);
    nui_px = dst;
    /* letterbox bars (cheap: only the margins) */
    if (g_dy) { nui_rect(0, 0, FB_W, g_dy, BLACK); nui_rect(0, g_dy + g_dh, FB_W, FB_H - g_dy - g_dh, BLACK); }
    if (g_dx) { nui_rect(0, g_dy, g_dx, g_dh, BLACK); nui_rect(g_dx + g_dw, g_dy, FB_W - g_dx - g_dw, g_dh, BLACK); }
    for (int y = 0; y < g_dh; y++) {
        const uint8_t *srow = g_frame + (size_t)((long)y * g_fh / g_dh) * g_fw * 4;
        uint32_t *drow = (uint32_t *)(dst + ((size_t)(g_dy + y) * FB_W + g_dx) * 4);
        for (int x = 0; x < g_dw; x++) drow[x] = *(const uint32_t *)(srow + g_xmap[x]);
    }
    if (g_paused || now_s() < g_osd_until) draw_osd();
    natartos_fb_publish(&g_fb);
    g_frames_shown++;
}

static void on_video(plm_t *plm, plm_frame_t *frame, void *user) {
    (void)plm; (void)user;
    if ((int)frame->width != g_fw || (int)frame->height != g_fh) fit_setup((int)frame->width, (int)frame->height);
    plm_frame_to_bgra(frame, g_frame, g_fw * 4);
    show_frame();
}

static void on_audio(plm_t *plm, plm_samples_t *s, void *user) {
    (void)plm; (void)user;
    if (g_ao.kind) audio_feed(s->interleaved, s->count);
}

static volatile sig_atomic_t g_quit = 0;
static void on_signal(int s) { (void)s; g_quit = 1; }

/* returns 1 if the user asked to leave the player entirely */
static int play(const char *path) {
    g_plm = plm_create_with_filename(path);
    if (!g_plm || !plm_probe(g_plm, 5000 * 1024)) {
        logmsg("can't open %s", path);
        if (g_plm) plm_destroy(g_plm);
        g_plm = NULL;
        render_list("CAN'T PLAY THIS FILE (NEEDS MPEG-1 .MPG)");
        return 0;
    }
    g_title = basename_of(path);
    int has_audio = plm_get_num_audio_streams(g_plm) > 0;
    logmsg("play %s: %dx%d %.2ffps %.1fs audio=%d@%d", path, plm_get_width(g_plm), plm_get_height(g_plm),
           plm_get_framerate(g_plm), plm_get_duration(g_plm), has_audio, has_audio ? plm_get_samplerate(g_plm) : 0);
    plm_set_video_decode_callback(g_plm, on_video, NULL);
    if (has_audio) {
        audio_open((unsigned)plm_get_samplerate(g_plm));
        plm_set_audio_enabled(g_plm, g_ao.kind != 0);
        plm_set_audio_stream(g_plm, 0);
        plm_set_audio_decode_callback(g_plm, on_audio, NULL);
        /* decode audio ahead by about the device buffer so playback never starves */
        if (g_ao.kind) plm_set_audio_lead_time(g_plm, (double)(g_ao.period * 4) / g_ao.rate);
    } else {
        plm_set_audio_enabled(g_plm, 0);
    }
    g_paused = 0;
    g_osd_until = now_s() + 3;
    g_frames_shown = 0;

    int leave = 0;
    double last = now_s(), started = last;
    double frame_dt = 1.0 / (plm_get_framerate(g_plm) > 0 ? plm_get_framerate(g_plm) : 30);
    while (!g_quit && !plm_has_ended(g_plm)) {
        int k = read_key((int)(frame_dt * 500));
        if (!exists(P_OVR)) { leave = 1; break; }
        if (k == K_BACK) break;
        if (k == K_OK) {
            g_paused = !g_paused;
            audio_pause(g_paused);
            if (g_fw) show_frame();                 /* repaint with / without the OSD */
        } else if (k == K_LEFT || k == K_RIGHT) {
            double t = plm_get_time(g_plm) + (k == K_RIGHT ? 10 : -10);
            if (t < 0) t = 0;
            audio_flush();
            g_osd_until = now_s() + 2.5;
            plm_seek(g_plm, t, 0);                    /* nearest keyframe: fast */
        }
        double now = now_s();
        double dt = now - last;
        last = now;
        if (g_paused) continue;
        if (dt > 0.25) dt = 0.25;                     /* don't fast-forward after a stall */
        plm_decode(g_plm, dt);
        if (g_osd_until && now > g_osd_until && g_osd_until > 0) {   /* hide the OSD */
            g_osd_until = 0;
            if (g_fw) show_frame();
        }
    }
    double secs = now_s() - started;
    logmsg("stopped: %ld frames in %.1fs (%.1f fps shown)%s", g_frames_shown, secs,
           secs > 0 ? g_frames_shown / secs : 0, plm_has_ended(g_plm) ? " [end]" : "");
    audio_close();
    plm_destroy(g_plm);
    g_plm = NULL;
    return leave;
}

int main(int argc, char **argv) {
    init_paths();
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int lock = open(P_LOCK, O_WRONLY | O_CREAT, 0666);
    if (lock < 0) { fprintf(stderr, "player: %s: %s\n", P_LOCK, strerror(errno)); return 1; }
    if (flock(lock, LOCK_EX | LOCK_NB) != 0) { fprintf(stderr, "player: already running\n"); return 0; }

    scan_files();
    if (natartos_fb_open(&g_fb, P_SHM) != 0) { logmsg("fb_shared: %s", strerror(errno)); return 1; }

    if (argc > 1 && !strcmp(argv[1], "--render-only")) {   /* host screenshot of the list */
        render_list(NULL);
        natartos_fb_close(&g_fb);
        return 0;
    }

    unlink(P_STOP);
    render_list(NULL);
    touch(P_OVR);
    input_open();
    logmsg("player up");

    while (!g_quit) {
        int k = read_key(300);
        if (!exists(P_OVR)) { logmsg("fb_override gone — leaving"); input_close(); natartos_fb_close(&g_fb); return 0; }
        if (k == K_NONE) continue;
        if (k == K_BACK) break;
        if (k == K_OK && g_nfiles) {
            if (play(g_files[g_sel])) { input_close(); natartos_fb_close(&g_fb); return 0; }
            render_list(NULL);
            continue;
        }
        if (k == K_UP && g_nfiles) g_sel = (g_sel + g_nfiles - 1) % g_nfiles;
        if (k == K_DOWN && g_nfiles) g_sel = (g_sel + 1) % g_nfiles;
        if (k == K_LEFT || k == K_RIGHT) { scan_files(); }   /* rescan (e.g. after inserting a card) */
        render_list(NULL);
    }

    logmsg("player exit");
    input_close();
    natartos_fb_close(&g_fb);
    unlink(P_OVR);              /* overlay cleanup → back to the DJI UI (or the launcher) */
    return 0;
}
