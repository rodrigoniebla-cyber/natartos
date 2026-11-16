/*
 * bootmenu — "Stock or Modified?" chooser shown when the goggles start.
 *
 * inject.sh starts dji_glasses with the loader + enabled mods, then launches
 * this binary once per session. It draws a two-option menu through the
 * overlay mod (fb_shared + fb_override, same path as Doom / PC Stream) and
 * reads the 5D joystick:
 *
 *   MODIFIED → drop fb_override; the overlay hands the screen back to the
 *              already-running modded UI. Nothing restarts.
 *   STOCK    → write "stock" to natartos_mode and restart dji_glasses;
 *              inject.sh sees the mode and respawns it with no LD_PRELOAD.
 *
 * Joystick any direction = move, 5D press = confirm, short BACK = Modified.
 * If nobody touches the stick, Modified starts after COUNTDOWN_SEC.
 * If anything goes wrong (no display, dji_glasses died, overlay gone) the
 * menu exits and leaves the session modified — exactly the old behaviour.
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 -o bootmenu bootmenu.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/input.h>

#define FB_WIDTH   1920
#define FB_HEIGHT  1080
#define FB_SIZE    (FB_WIDTH * FB_HEIGHT * 4)
#define SHM_HEADER 64
/* Two pixel slots, same layout as fb_receiver: we write the back slot and
 * publish it by bumping the counter; the overlay reads slot (counter & 1). */
#define SHM_SIZE   (SHM_HEADER + 2 * FB_SIZE)

#define TMP          "/data/local/tmp/"
#define SHM_PATH     TMP "fb_shared"
#define FLAG_PATH    TMP "fb_override"
#define MODE_PATH    TMP "natartos_mode"
#define MENU_MODE_PATH TMP "natartos_menu_mode"   /* "boot" → launched at power-on */
#define RESTART_PATH TMP "natartos_restart"
#define LOG_PATH     TMP "natartos_bootmenu.log"
#define BOOTCOUNT_PATH "/data/.natartos_bootcount"
#define STATUS_PATH    "/data/.natartos_status"

#define COUNTDOWN_SEC   10
#define READY_TIMEOUT_S 45
#define INPUT_IGNORE_MS 400   /* swallow stale presses from right before the menu appeared */
#define IDLE_TIMEOUT_S  60    /* never hold the joystick forever if left sitting on the menu */

enum { OPT_STOCK = 0, OPT_MODIFIED = 1, OPT_COUNT = 2 };

static FILE *g_log;
static unsigned char *g_map;
static volatile unsigned int *g_counter;
static unsigned char *g_pixels;

static void logmsg(const char *fmt, ...) {
    if (!g_log) return;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    fprintf(g_log, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int file_exists(const char *p) { return access(p, F_OK) == 0; }

static void write_file(const char *path, const char *s) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { logmsg("write %s: %s", path, strerror(errno)); return; }
    if (s) { ssize_t w = write(fd, s, strlen(s)); (void)w; }
    close(fd);
}

/* ── 5x7 bitmap font (row-major, bit 4 = leftmost column) ── */

#define GW 5
#define GH 7

static const unsigned char g_font[128][GH] = {
    ['A'] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['B'] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    ['C'] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    ['D'] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    ['E'] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    ['F'] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    ['G'] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
    ['H'] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['I'] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    ['J'] = {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    ['K'] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    ['L'] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    ['M'] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    ['N'] = {0x11,0x11,0x19,0x15,0x13,0x11,0x11},
    ['O'] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['P'] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    ['Q'] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    ['R'] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    ['S'] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    ['T'] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    ['U'] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['V'] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    ['W'] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
    ['X'] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    ['Y'] = {0x11,0x11,0x11,0x0A,0x04,0x04,0x04},
    ['Z'] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    ['0'] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    ['1'] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    ['2'] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    ['3'] = {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    ['4'] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    ['5'] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    ['6'] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
    ['7'] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    ['8'] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    ['9'] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    ['.'] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},
    [','] = {0x00,0x00,0x00,0x00,0x0C,0x04,0x08},
    [':'] = {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},
    ['-'] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    ['/'] = {0x00,0x01,0x02,0x04,0x08,0x10,0x00},
    ['('] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02},
    [')'] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08},
    ['!'] = {0x04,0x04,0x04,0x04,0x04,0x00,0x04},
    ['?'] = {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},
    ['>'] = {0x08,0x04,0x02,0x01,0x02,0x04,0x08},
    ['<'] = {0x02,0x04,0x08,0x10,0x08,0x04,0x02},
};

/* ── Drawing (BGRA) ── */

typedef struct { unsigned char b, g, r; } rgb_t;
#define RGB(R, G, B) ((rgb_t){ (B), (G), (R) })

static const rgb_t C_BG      = RGB(0x12, 0x14, 0x18);
static const rgb_t C_CARD    = RGB(0x1E, 0x22, 0x28);
static const rgb_t C_EDGE    = RGB(0x3A, 0x3F, 0x47);
static const rgb_t C_SEL     = RGB(0x17, 0x3A, 0x6B);
static const rgb_t C_ACCENT  = RGB(0x4C, 0x9A, 0xFF);
static const rgb_t C_TEXT    = RGB(0xF2, 0xF4, 0xF7);
static const rgb_t C_MUTED   = RGB(0x8B, 0x93, 0x9E);
static const rgb_t C_DIM     = RGB(0x5C, 0x63, 0x6D);

static void fill_rect(unsigned char *fb, int x, int y, int w, int h, rgb_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_WIDTH)  w = FB_WIDTH - x;
    if (y + h > FB_HEIGHT) h = FB_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    unsigned int px = 0xFF000000u | ((unsigned)c.r << 16) | ((unsigned)c.g << 8) | c.b;
    for (int dy = 0; dy < h; dy++) {
        unsigned int *row = (unsigned int *)(fb + ((long)(y + dy) * FB_WIDTH + x) * 4);
        for (int dx = 0; dx < w; dx++) row[dx] = px;
    }
}

static void frame_rect(unsigned char *fb, int x, int y, int w, int h, int t, rgb_t c) {
    fill_rect(fb, x, y, w, t, c);
    fill_rect(fb, x, y + h - t, w, t, c);
    fill_rect(fb, x, y, t, h, c);
    fill_rect(fb, x + w - t, y, t, h, c);
}

static int text_w(const char *s, int sc) {
    int n = (int)strlen(s);
    return n > 0 ? n * (GW + 1) * sc - sc : 0;
}

static void draw_text(unsigned char *fb, const char *s, int x, int y, int sc, rgb_t c) {
    for (; *s; s++, x += (GW + 1) * sc) {
        unsigned char ch = (unsigned char)*s;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch >= 128) continue;
        const unsigned char *gl = g_font[ch];
        for (int r = 0; r < GH; r++)
            for (int col = 0; col < GW; col++)
                if (gl[r] & (0x10 >> col))
                    fill_rect(fb, x + col * sc, y + r * sc, sc, sc, c);
    }
}

static void draw_text_c(unsigned char *fb, const char *s, int cy, int sc, rgb_t c) {
    draw_text(fb, s, (FB_WIDTH - text_w(s, sc)) / 2, cy, sc, c);
}

/* ── Screens ── */

static const char *const OPT_LABEL[OPT_COUNT] = { "STOCK", "MODIFIED" };
static const char *const OPT_SUB[OPT_COUNT] = {
    "ORIGINAL DJI SOFTWARE - NO MODS",
    "NATARTOS - MODS ENABLED",
};

#define CARD_W 960
#define CARD_H 180
#define CARD_X ((FB_WIDTH - CARD_W) / 2)
#define CARD_Y0 380
#define CARD_GAP 40

/* Render the chooser. countdown < 0 hides the countdown line. */
static void render_menu(unsigned char *fb, int sel, int countdown) {
    fill_rect(fb, 0, 0, FB_WIDTH, FB_HEIGHT, C_BG);

    draw_text_c(fb, "NATARTOS", 160, 4, C_DIM);
    draw_text_c(fb, "HOW DO YOU WANT TO START?", 230, 7, C_TEXT);

    for (int i = 0; i < OPT_COUNT; i++) {
        int y = CARD_Y0 + i * (CARD_H + CARD_GAP);
        int on = (i == sel);
        fill_rect(fb, CARD_X, y, CARD_W, CARD_H, on ? C_SEL : C_CARD);
        frame_rect(fb, CARD_X, y, CARD_W, CARD_H, on ? 6 : 3, on ? C_ACCENT : C_EDGE);
        if (on) draw_text(fb, ">", CARD_X + 48, y + 40, 7, C_ACCENT);
        draw_text(fb, OPT_LABEL[i], CARD_X + 120, y + 36, 7, on ? C_TEXT : C_MUTED);
        draw_text(fb, OPT_SUB[i], CARD_X + 122, y + 116, 3, on ? C_TEXT : C_DIM);
    }

    draw_text_c(fb, "JOYSTICK: MOVE      PRESS: SELECT", 870, 3, C_MUTED);

    if (countdown >= 0) {
        char line[64];
        snprintf(line, sizeof(line), "STARTING %s IN %d", OPT_LABEL[OPT_MODIFIED], countdown);
        draw_text_c(fb, line, 930, 3, C_ACCENT);
    }
}

static void render_message(unsigned char *fb, const char *title, const char *sub) {
    fill_rect(fb, 0, 0, FB_WIDTH, FB_HEIGHT, C_BG);
    draw_text_c(fb, title, 470, 7, C_TEXT);
    if (sub) draw_text_c(fb, sub, 560, 3, C_MUTED);
}

/* ── fb_shared ── */

static int shm_open_map(void) {
    int fd = open(SHM_PATH, O_RDWR | O_CREAT, 0666);
    if (fd < 0) { logmsg("open fb_shared: %s", strerror(errno)); return -1; }
    if (ftruncate(fd, SHM_SIZE) < 0) { logmsg("ftruncate: %s", strerror(errno)); close(fd); return -1; }
    void *p = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { logmsg("mmap: %s", strerror(errno)); return -1; }
    g_map = p;
    g_counter = (volatile unsigned int *)p;
    g_pixels = g_map + SHM_HEADER;
    memset(g_map, 0, SHM_HEADER);
    return 0;
}

static unsigned char *back_slot(void) {
    unsigned int c = __atomic_load_n(g_counter, __ATOMIC_RELAXED);
    return g_pixels + (unsigned long)((c + 1) & 1) * FB_SIZE;
}

static void publish(void) { __atomic_add_fetch(g_counter, 1, __ATOMIC_RELEASE); }

/* ── dji_glasses discovery ── */

static int find_glasses_pid(void) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        char path[300], comm[32] = {0};
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, comm, sizeof(comm) - 1);
        close(fd);
        if (n > 0 && strncmp(comm, "dji_glasses", 11) == 0) { found = atoi(e->d_name); break; }
    }
    closedir(d);
    return found;
}

static int pid_alive(int pid) { return pid > 0 && kill(pid, 0) == 0; }

/* The overlay is the only way onto the screen — wait until it is mapped. */
static int has_overlay(int pid) {
    char path[64], line[512];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int hit = 0;
    while (fgets(line, sizeof(line), f))
        if (strstr(line, "hook_overlay.so")) { hit = 1; break; }
    fclose(f);
    return hit;
}

/* ── Input ── */

/* Same devices Doom uses: event0 = gpio_keys (5D press + BACK),
 * event3 = omc_keys (joystick directions; uinput created by dji_glasses). */
static const char *const INPUT_DEVS[] = { "/dev/input/event0", "/dev/input/event3" };
#define N_INPUT 2
static int g_in[N_INPUT] = { -1, -1 };

static int open_inputs(void) {
    int n = 0;
    for (int i = 0; i < N_INPUT; i++) {
        if (g_in[i] < 0) {
            g_in[i] = open(INPUT_DEVS[i], O_RDONLY | O_NONBLOCK);
            /* Grab so the GUI underneath doesn't also act on our key presses. */
            if (g_in[i] >= 0) ioctl(g_in[i], EVIOCGRAB, 1);
        }
        if (g_in[i] >= 0) n++;
    }
    return n;
}

static void close_inputs(void) {
    for (int i = 0; i < N_INPUT; i++)
        if (g_in[i] >= 0) { ioctl(g_in[i], EVIOCGRAB, 0); close(g_in[i]); g_in[i] = -1; }
}

enum { IN_NONE, IN_MOVE, IN_OK, IN_BACK };

/* Wait up to timeout_ms for one meaningful key press. */
static int read_input(int timeout_ms, long ignore_before) {
    struct pollfd p[N_INPUT];
    int np = 0;
    for (int i = 0; i < N_INPUT; i++)
        if (g_in[i] >= 0) { p[np].fd = g_in[i]; p[np].events = POLLIN; p[np].revents = 0; np++; }
    if (np == 0) { sleep_ms(timeout_ms); return IN_NONE; }
    if (poll(p, np, timeout_ms) <= 0) return IN_NONE;

    int result = IN_NONE;
    for (int i = 0; i < np; i++) {
        if (!(p[i].revents & POLLIN)) continue;
        struct input_event ev;
        while (read(p[i].fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type != EV_KEY || ev.value != 1) continue;   /* presses only */
            if (now_ms() < ignore_before) continue;
            if (result != IN_NONE) continue;                    /* one action per wake */
            switch (ev.code) {
            case KEY_UP: case KEY_DOWN: case KEY_LEFT: case KEY_RIGHT:
                result = IN_MOVE; break;
            case KEY_ENTER:
                result = IN_OK; break;
            case KEY_BACK:
                result = IN_BACK; break;
            default:
                break;
            }
        }
    }
    return result;
}

/* ── Actions ── */

/* This launch bumped the loader's crash counter. Choosing stock is not a
 * crash, so undo that bump — otherwise five stock sessions in a row would
 * look like a crash loop and trip the loader's safe mode. */
static void undo_bootcount(void) {
    unsigned int n = 0;
    int fd = open(BOOTCOUNT_PATH, O_RDWR);
    if (fd < 0) return;
    if (read(fd, &n, 4) == 4 && n > 0) {
        n--;
        if (lseek(fd, 0, SEEK_SET) == 0) { ssize_t w = write(fd, &n, 4); (void)w; }
    }
    close(fd);
}

static void go_modified(const char *why) {
    logmsg("-> MODIFIED (%s)", why);
    write_file(MODE_PATH, "modified\n");
    close_inputs();
    /* Removing the flag makes the overlay stop injecting and repaint the
     * live (modded) UI — same exit path as Doom / PC Stream. */
    unlink(FLAG_PATH);
}

static void go_stock(int pid) {
    logmsg("-> STOCK (pid %d)", pid);
    write_file(MODE_PATH, "stock\n");
    undo_bootcount();

    render_message(back_slot(), "STARTING STOCK", "RESTARTING WITHOUT MODS...");
    publish();
    sleep_ms(700);

    close_inputs();
    /* Tell inject.sh this exit is intentional (not a crash), then restart. */
    write_file(RESTART_PATH, NULL);
    kill(pid, SIGTERM);
    for (int i = 0; i < 50 && pid_alive(pid); i++) sleep_ms(100);
    if (pid_alive(pid)) { logmsg("SIGTERM ignored, SIGKILL"); kill(pid, SIGKILL); }
    unlink(SHM_PATH);
}

/* The exit taken on anything that isn't an explicit pick (timeout, idle, the
 * overlay going away, dji_glasses dying under us). Session mode: fall back to
 * Modified, the running UI. Boot mode: fall back to Stock — an unattended or
 * half-failed power-on must never silently come up modified. */
static void go_default(int boot_mode, int pid, const char *why) {
    if (boot_mode) go_stock(pid);   /* writes stock, flags restart, kills pid */
    else           go_modified(why);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    g_log = fopen(LOG_PATH, "w");

    /* Boot mode: launched at power-on (no USB attached) rather than from
     * `natartos start`. The safe default then flips — an unattended or failed
     * boot must land on STOCK, never silently come up modified. The user has
     * to actively pick Modified. From `natartos start` the default stays
     * Modified (you're right there at the keyboard either way). */
    char mm[16] = {0};
    int mfd = open(MENU_MODE_PATH, O_RDONLY);
    if (mfd >= 0) { ssize_t n = read(mfd, mm, sizeof(mm) - 1); (void)n; close(mfd); }
    unlink(MENU_MODE_PATH);   /* consume once — a stale flag must not bias a later USB start */
    int boot_mode = (strncmp(mm, "boot", 4) == 0);
    const char *default_mode = boot_mode ? "stock\n" : "modified\n";
    logmsg("bootmenu: start (%s mode)", boot_mode ? "boot" : "session");

    /* 1. Wait for the modded dji_glasses to be up with the overlay mapped. */
    int pid = 0;
    long deadline = now_ms() + READY_TIMEOUT_S * 1000L;
    while (now_ms() < deadline) {
        pid = find_glasses_pid();
        if (pid && has_overlay(pid) && file_exists(INPUT_DEVS[1])) break;
        pid = 0;
        sleep_ms(250);
    }
    if (!pid) {
        /* Can't show anything — keep the old behaviour (mods on). */
        write_file(MODE_PATH, default_mode);
        logmsg("dji_glasses/overlay not ready after %ds, default=%s", READY_TIMEOUT_S, default_mode);
        return 0;
    }
    logmsg("dji_glasses pid=%d, overlay mapped", pid);
    sleep_ms(1500);   /* let the GUI finish bringing up its display buffers */

    /* The loader writes "active" only when it actually initialised the mods.
     * In safe mode (kill-switch / crash counter) the overlay never starts
     * injecting, so the menu would be invisible while holding the joystick. */
    char status[16] = {0};
    int sfd = open(STATUS_PATH, O_RDONLY);
    if (sfd >= 0) { ssize_t n = read(sfd, status, sizeof(status) - 1); (void)n; close(sfd); }
    if (strncmp(status, "active", 6) != 0) {
        write_file(MODE_PATH, default_mode);
        logmsg("loader status '%s' (not active), skipping menu, default=%s", status, default_mode);
        return 0;
    }

    /* 2. Take over the screen. */
    if (shm_open_map() < 0) { write_file(MODE_PATH, default_mode); return 0; }
    int sel = boot_mode ? OPT_STOCK : OPT_MODIFIED;
    int countdown = COUNTDOWN_SEC;
    render_menu(back_slot(), sel, countdown);
    publish();
    write_file(FLAG_PATH, NULL);   /* overlay watcher starts injecting fb_shared */

    int nin = 0;
    for (int i = 0; i < 20 && (nin = open_inputs()) < N_INPUT; i++) sleep_ms(250);
    logmsg("menu shown, inputs=%d", nin);

    long ignore_before = now_ms() + INPUT_IGNORE_MS;
    long tick = now_ms() + 1000;
    long last_input = now_ms();

    /* 3. Menu loop. */
    for (;;) {
        int in = read_input(100, ignore_before);

        if (!pid_alive(pid)) {
            /* dji_glasses died under us; inject.sh respawns it modded. */
            logmsg("dji_glasses gone");
            write_file(MODE_PATH, default_mode);
            break;
        }
        if (!file_exists(FLAG_PATH)) {
            /* Overlay stopped (long BACK, or a restart). */
            go_default(boot_mode, pid, "overlay stopped");
            break;
        }

        if (in != IN_NONE) last_input = now_ms();
        else if (now_ms() - last_input > IDLE_TIMEOUT_S * 1000L) {
            go_default(boot_mode, pid, "idle");
            break;
        }

        int redraw = 0;
        if (in == IN_MOVE) {
            sel = (sel + 1) % OPT_COUNT;
            countdown = -1;
            redraw = 1;
        } else if (in == IN_OK) {
            if (sel == OPT_STOCK) go_stock(pid); else go_modified("selected");
            break;
        } else if (in == IN_BACK) {
            go_modified("back");
            break;
        }

        if (countdown >= 0 && now_ms() >= tick) {
            tick += 1000;
            if (--countdown <= 0) { go_default(boot_mode, pid, "timeout"); break; }
            redraw = 1;
        }

        if (redraw) {
            render_menu(back_slot(), sel, countdown);
            publish();
        }
    }

    close_inputs();
    if (g_map) munmap(g_map, SHM_SIZE);
    logmsg("bootmenu: exit");
    if (g_log) fclose(g_log);
    return 0;
}
