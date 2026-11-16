/*
 * natartos_fb.h — the fb_shared protocol, for apps that draw full-screen
 * (launcher, Doom, fb_receiver all follow it).
 *
 * Layout of /data/local/tmp/fb_shared:
 *   [0..63]                    header; u32 at offset 0 = frame counter
 *   [64 .. 64+FB]              pixel slot 0  (1920×1080 BGRA)
 *   [64+FB .. 64+2FB]          pixel slot 1
 *
 * The overlay reads the FRONT slot, (counter & 1), whenever the counter changes.
 * A producer draws into the BACK slot, ((counter + 1) & 1), then increments the
 * counter to publish it — so the overlay never copies a half-drawn frame.
 * Counter 0 means "nothing published yet".
 *
 * Shown only while fb_override exists (see mods/overlay/mod.c).
 * Needs libc (static apps), not for -nostdlib mods.
 */
#ifndef NATARTOS_FB_H
#define NATARTOS_FB_H

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define FB_W 1920
#define FB_H 1080
#define NATARTOS_FB_HEADER 64
#define NATARTOS_FB_FRAME  ((size_t)FB_W * FB_H * 4)
#define NATARTOS_FB_SIZE   (NATARTOS_FB_HEADER + 2 * NATARTOS_FB_FRAME)

typedef struct {
    unsigned char *map;
    volatile unsigned int *counter;
} natartos_fb;

/* Create/open fb_shared at `path`, sized for both slots. 0 on success. */
static inline int natartos_fb_open(natartos_fb *fb, const char *path) {
    fb->map = NULL;
    int fd = open(path, O_RDWR | O_CREAT, 0666);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)NATARTOS_FB_SIZE) != 0) { close(fd); return -1; }
    void *p = mmap(NULL, NATARTOS_FB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return -1;
    fb->map = (unsigned char *)p;
    fb->counter = (volatile unsigned int *)p;
    return 0;
}

static inline void natartos_fb_close(natartos_fb *fb) {
    if (fb->map) munmap(fb->map, NATARTOS_FB_SIZE);
    fb->map = NULL;
}

/* The slot the overlay is NOT reading: draw here. */
static inline unsigned char *natartos_fb_back(natartos_fb *fb) {
    unsigned int c = __atomic_load_n(fb->counter, __ATOMIC_RELAXED);
    return fb->map + NATARTOS_FB_HEADER + (size_t)((c + 1) & 1u) * NATARTOS_FB_FRAME;
}

/* Publish the back slot (it becomes the front slot). */
static inline void natartos_fb_publish(natartos_fb *fb) {
    __atomic_add_fetch(fb->counter, 1, __ATOMIC_RELEASE);
}

#endif /* NATARTOS_FB_H */
