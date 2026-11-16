/*
 * doomgeneric platform shim for DJI Goggles 3.
 * Renders to /data/local/tmp/fb_shared (shared memory with hook_popen.so).
 * Reads input from /dev/input/event* (gpio_keys = 5D joystick + back).
 *
 * Display: Doom renders at 320x200, we scale 5x → 1600x1000, centered in 1920x1080.
 */

#include "doomkeys.h"
#include "m_argv.h"
#include "doomgeneric.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <linux/input.h>
#include <sys/ioctl.h>

#define FB_WIDTH  1920
#define FB_HEIGHT 1080
#define FB_STRIDE (FB_WIDTH * 4)
#define FB_SIZE   (FB_STRIDE * FB_HEIGHT)
#define SHM_HEADER 64
/* Two pixel slots (same protocol as fb_receiver / include/natartos_fb.h): draw
 * the back slot, publish via the counter; the overlay reads slot (counter & 1). */
#define SHM_SIZE  (SHM_HEADER + 2 * FB_SIZE)
#define SHM_PATH  "/data/local/tmp/fb_shared"

#define SCALE 5
#define DOOM_W DOOMGENERIC_RESX   /* 320 */
#define DOOM_H DOOMGENERIC_RESY   /* 200 */
#define SCALED_W (DOOM_W * SCALE) /* 1600 */
#define SCALED_H (DOOM_H * SCALE) /* 1000 */
#define OFF_X ((FB_WIDTH - SCALED_W) / 2)   /* 160 */
#define OFF_Y ((FB_HEIGHT - SCALED_H) / 2)  /* 40 */

static volatile unsigned int *shm_counter;
static unsigned char *shm_pixels;

/* Input */
#define MAX_INPUT_FDS 4
static int input_fds[MAX_INPUT_FDS];
static int input_fd_count = 0;

#define KEYQUEUE_SIZE 128
static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueWrite = 0;
static unsigned int s_KeyQueueRead = 0;

static void addKeyToQueue(int pressed, unsigned char key) {
    unsigned short val = (pressed ? 1 : 0) << 8 | key;
    s_KeyQueue[s_KeyQueueWrite % KEYQUEUE_SIZE] = val;
    s_KeyQueueWrite++;
}

/* BACK button: short press = USE, long hold (>500ms) = ESC */
#define BACK_LONG_MS 500
static int back_state;          /* 0=idle, 1=pressed_waiting, 2=esc_sent */
static uint32_t back_press_ms;

/* Auto-release: track held keys, synth release if no key-up within timeout */
#define AUTO_RELEASE_MS 2000
#define MAX_HELD 8
static struct { unsigned char doom_key; uint32_t press_ms; } held_keys[MAX_HELD];
static int held_count = 0;

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void track_press(unsigned char dk) {
    for (int i = 0; i < held_count; i++)
        if (held_keys[i].doom_key == dk) { held_keys[i].press_ms = now_ms(); return; }
    if (held_count < MAX_HELD) {
        held_keys[held_count].doom_key = dk;
        held_keys[held_count].press_ms = now_ms();
        held_count++;
    }
}

static void track_release(unsigned char dk) {
    for (int i = 0; i < held_count; i++) {
        if (held_keys[i].doom_key == dk) {
            held_keys[i] = held_keys[--held_count];
            return;
        }
    }
}

static void check_auto_release(void) {
    uint32_t t = now_ms();
    for (int i = held_count - 1; i >= 0; i--) {
        if ((t - held_keys[i].press_ms) >= AUTO_RELEASE_MS) {
            addKeyToQueue(0, held_keys[i].doom_key);
            held_keys[i] = held_keys[--held_count];
        }
    }
}

/* Linux input key → Doom key (BACK handled separately) */
static unsigned char linuxToDoomKey(unsigned int code) {
    switch (code) {
    case KEY_DOWN:     return KEY_UPARROW;
    case KEY_UP:       return KEY_DOWNARROW;
    case KEY_RIGHT:    return KEY_LEFTARROW;
    case KEY_LEFT:     return KEY_RIGHTARROW;
    case 28:           return 13;           /* 5D press = ENTER (menu select); FIRE added in pollInput */
    case KEY_ESC:      return KEY_ESCAPE;
    case KEY_SPACE:    return KEY_USE;
    case KEY_LEFTCTRL: return KEY_FIRE;
    case KEY_LEFTALT:  return KEY_LALT;     /* strafe modifier */
    case KEY_LEFTSHIFT:return KEY_RSHIFT;   /* run */
    case KEY_TAB:      return KEY_TAB;      /* automap */
    /* Number keys for weapon select */
    case KEY_1:        return '1';
    case KEY_2:        return '2';
    case KEY_3:        return '3';
    case KEY_4:        return '4';
    case KEY_5:        return '5';
    case KEY_6:        return '6';
    case KEY_7:        return '7';
    /* Function keys */
    case KEY_F1:       return 0x80+0x3b;
    case KEY_F2:       return 0x80+0x3c;
    case KEY_F10:      return 0x80+0x44;
    default:
        if (code >= KEY_A && code <= KEY_Z)
            return 'a' + (code - KEY_A);
        return 0;
    }
}

static void pollInput(void) {
    struct input_event ev;
    for (int i = 0; i < input_fd_count; i++) {
        while (read(input_fds[i], &ev, sizeof(ev)) == sizeof(ev)) {
            if (ev.type == EV_KEY) {
                /* value=2 (repeat): refresh auto-release timer */
                if (ev.value == 2) {
                    unsigned char dk = linuxToDoomKey(ev.code);
                    if (dk) track_press(dk);
                    if (ev.code == 28) track_press(0xa3);
                    continue;
                }
                if (ev.value != 0 && ev.value != 1) continue;
                if (ev.code == 158) {
                    if (ev.value == 1) {
                        back_state = 1;
                        back_press_ms = now_ms();
                    } else {
                        if (back_state == 1) {
                            addKeyToQueue(1, KEY_USE);
                            addKeyToQueue(0, KEY_USE);
                        } else if (back_state == 2) {
                            addKeyToQueue(0, 27);
                        }
                        back_state = 0;
                    }
                    continue;
                }
                unsigned char dk = linuxToDoomKey(ev.code);
                if (dk) {
                    addKeyToQueue(ev.value, dk);
                    if (ev.value) track_press(dk); else track_release(dk);
                }
                if (ev.code == 28) {
                    addKeyToQueue(ev.value, 0xa3);
                    /* 5D-centre also answers "yes" to the Quit Game prompt (Doom's
                     * confirm key is 'y', which the goggles have no button for). It
                     * does nothing in gameplay, so this just lets centre close the
                     * exit dialog the user already navigated to. */
                    addKeyToQueue(ev.value, 'y');
                    if (ev.value) track_press(0xa3); else track_release(0xa3);
                }
            }
        }
    }
    /* check BACK long-press timer */
    if (back_state == 1 && (now_ms() - back_press_ms) >= BACK_LONG_MS) {
        addKeyToQueue(1, 27); /* ESC press */
        back_state = 2;
    }
    check_auto_release();
}

void DG_Init(void) {
    /* Open shared memory */
    int fd = open(SHM_PATH, O_RDWR | O_CREAT, 0666);
    if (fd < 0) { perror("open fb_shared"); exit(1); }
    ftruncate(fd, SHM_SIZE);
    void *p = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    memset(p, 0, SHM_SIZE);
    shm_counter = (volatile unsigned int *)p;
    shm_pixels = (unsigned char *)p + SHM_HEADER;

    /* Open only relevant input devices, retry for uinput (event3) */
    const char *devs[] = {
        "/dev/input/event0",  /* gpio_keys: ENTER + BACK */
        "/dev/input/event3",  /* omc_keys: 5D joystick directions (uinput from dji_glasses) */
    };
    int want = 2;
    for (int attempt = 0; attempt < 20 && input_fd_count < want; attempt++) {
        for (int i = 0; i < want; i++) {
            int already = 0;
            for (int j = 0; j < input_fd_count; j++) {
                char link[64], target[128];
                snprintf(link, sizeof(link), "/proc/self/fd/%d", input_fds[j]);
                int n = readlink(link, target, sizeof(target)-1);
                if (n > 0) { target[n] = 0; if (strcmp(target, devs[i]) == 0) already = 1; }
            }
            if (already) continue;
            int ifd = open(devs[i], O_RDONLY | O_NONBLOCK);
            if (ifd >= 0) {
                ioctl(ifd, EVIOCGRAB, 1);
                input_fds[input_fd_count++] = ifd;
            }
        }
        if (input_fd_count >= want) break;
        struct timespec wait = { 0, 500000000L };
        nanosleep(&wait, NULL);
    }
    fprintf(stderr, "DG_Init: shm=%p inputs=%d\n", p, input_fd_count);
}

void DG_DrawFrame(void) {
    /* DG_ScreenBuffer is DOOM_W x DOOM_H, XRGB (uint32_t).
     * We scale 5x and center in 1920x1080 BGRA. */
    uint32_t *src = DG_ScreenBuffer;
    unsigned int c = __atomic_load_n(shm_counter, __ATOMIC_RELAXED);
    uint32_t *dst = (uint32_t *)(shm_pixels + (size_t)((c + 1) & 1u) * FB_SIZE);  /* back slot */

    /* Top black bars */
    memset(dst, 0, OFF_Y * FB_WIDTH * 4);

    for (int sy = 0; sy < DOOM_H; sy++) {
        int dy_base = OFF_Y + sy * SCALE;
        /* Build one scaled row */
        uint32_t *row = dst + dy_base * FB_WIDTH;
        /* Left bar */
        memset(row, 0, OFF_X * 4);
        /* Scaled pixels: XRGB → BGRA (same memory layout on little-endian) */
        uint32_t *rp = row + OFF_X;
        for (int sx = 0; sx < DOOM_W; sx++) {
            uint32_t px = src[sy * DOOM_W + sx] | 0xFF000000; /* ensure alpha */
            for (int k = 0; k < SCALE; k++)
                *rp++ = px;
        }
        /* Right bar */
        memset(rp, 0, (FB_WIDTH - OFF_X - SCALED_W) * 4);
        /* Duplicate row SCALE times */
        for (int dup = 1; dup < SCALE; dup++)
            memcpy(dst + (dy_base + dup) * FB_WIDTH, row, FB_WIDTH * 4);
    }

    /* Bottom black bars */
    int bot_start = OFF_Y + SCALED_H;
    memset(dst + bot_start * FB_WIDTH, 0, (FB_HEIGHT - bot_start) * FB_WIDTH * 4);

    /* Signal new frame */
    __atomic_add_fetch(shm_counter, 1, __ATOMIC_RELEASE);
}

void DG_SleepMs(uint32_t ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

uint32_t DG_GetTicksMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int DG_GetKey(int *pressed, unsigned char *doomKey) {
    pollInput();
    if (s_KeyQueueRead == s_KeyQueueWrite) return 0;
    unsigned short val = s_KeyQueue[s_KeyQueueRead % KEYQUEUE_SIZE];
    s_KeyQueueRead++;
    *pressed = (val >> 8) & 1;
    *doomKey = val & 0xFF;
    return 1;
}

void DG_SetWindowTitle(const char *title) {
    (void)title;
}

int main(int argc, char **argv) {
    doomgeneric_Create(argc, argv);
    for (;;)
        doomgeneric_Tick();
    return 0;
}
