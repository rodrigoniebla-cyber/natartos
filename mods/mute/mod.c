/*
 * mod_mute.so — audio control for DJI Goggles 3 (tinyalsa).
 *
 * Interposes pcm_write (tinyalsa). Zeroes audio buffer based on flags.
 *
 * Flags (touch = active, rm = inactive):
 *   /data/local/tmp/natartos_mute          — mute ALL audio
 *   /data/local/tmp/natartos_mute_startup  — mute startup sound only (first ~5s)
 *   /data/local/tmp/natartos_mute_buttons  — mute button/UI sounds (short buffers)
 *
 *   /data/local/tmp/natartos_audio_spy     — recon only: log how dji_glasses uses
 *                                            the speaker (pcm_open config, write
 *                                            cadence). Changes nothing.
 *
 * Note: tinyalsa's pcm_write() count is in BYTES (pcm_writei takes frames).
 *
 * Checks flags every 256 calls to avoid excessive syscalls.
 * Uses natartos_mod_init protocol to get dlsym from loader.
 */

#define NTLOG_PREFIX "mute"
#include "natartos_common.h"

#define RTLD_NEXT ((void *)-1)

static const char FLAG_MUTE_ALL[]     = "/data/local/tmp/natartos_mute";
static const char FLAG_MUTE_STARTUP[] = "/data/local/tmp/natartos_mute_startup";
static const char FLAG_MUTE_BUTTONS[] = "/data/local/tmp/natartos_mute_buttons";
static const char FLAG_AUDIO_SPY[]    = "/data/local/tmp/natartos_audio_spy";

static void log_str(const char *s) { NTLOG(s); }

/* --- state --- */

#define RTLD_NOLOAD 4

typedef void *(*dlopen_fn)(const char *, int);
typedef int (*pcm_write_fn)(void *pcm, const void *data, unsigned int count);
typedef void *(*pcm_open_fn)(unsigned int card, unsigned int device, unsigned int flags, const void *config);
typedef int (*pcm_close_fn)(void *pcm);
static pcm_write_fn g_real_pcm_write = 0;
static pcm_open_fn  g_real_pcm_open = 0;
static pcm_close_fn g_real_pcm_close = 0;
static void *(*g_loader_dlsym)(void *, const char *) = 0;
static int g_spy = 0;
static int g_mute_all = 0;
static int g_mute_startup = 0;
static int g_mute_buttons = 0;
static unsigned int g_call_count = 0;
#define STARTUP_CALLS 300
#define BUTTON_FRAME_THRESHOLD 256

/* --- audio spy (recon, read-only) --- */

/* Leading fields of tinyalsa's struct pcm_config — stable across versions. */
struct pcm_config_head {
    unsigned int channels;
    unsigned int rate;
    unsigned int period_size;   /* frames */
    unsigned int period_count;
    int format;                 /* 0=S16_LE 1=S32_LE 2=S8 3=S24_LE 4=S24_3LE */
};

#define __NR_clock_gettime 113
#define CLOCK_MONOTONIC 1
static long now_ms(void) {
    struct timespec_t ts;
    register long x0 __asm__("x0") = CLOCK_MONOTONIC;
    register long x1 __asm__("x1") = (long)&ts;
    register long x8 __asm__("x8") = __NR_clock_gettime;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Resolve real tinyalsa functions lazily (the library is a dependency of
 * dji_glasses, so it is already mapped when mods init). */
static void *tinyalsa_sym(const char *name) {
    if (!g_loader_dlsym) return 0;
    dlopen_fn my_dlopen = (dlopen_fn)g_loader_dlsym(RTLD_DEFAULT, "dlopen");
    void *h = my_dlopen ? my_dlopen("libtinyalsa.so", RTLD_NOLOAD) : 0;
    return h ? g_loader_dlsym(h, name) : 0;
}

/* write-cadence window: tells continuous streaming apart from per-sound bursts */
static long g_win_start = 0, g_last_write = 0;
static unsigned int g_win_calls = 0, g_win_bytes = 0, g_win_min = 0, g_win_max = 0;
static long g_win_gap_max = 0;

static void spy_write(void *pcm, unsigned int count) {
    long t = now_ms();
    if (g_last_write && t - g_last_write > 500) {
        NTLOG_INT("spy: write burst after idle ms=", (int)(t - g_last_write));
    }
    if (!g_win_start) g_win_start = t;
    if (g_last_write && t - g_last_write > g_win_gap_max) g_win_gap_max = t - g_last_write;
    g_last_write = t;
    g_win_calls++;
    g_win_bytes += count;
    if (!g_win_min || count < g_win_min) g_win_min = count;
    if (count > g_win_max) g_win_max = count;
    if (t - g_win_start >= 1000) {
        NTLOG_HEX("spy: write pcm=", (unsigned long)pcm);
        NTLOG_INT("spy:   calls/s=", (int)g_win_calls);
        NTLOG_INT("spy:   bytes/s=", (int)g_win_bytes);
        NTLOG_INT("spy:   count min=", (int)g_win_min);
        NTLOG_INT("spy:   count max=", (int)g_win_max);
        NTLOG_INT("spy:   gap max ms=", (int)g_win_gap_max);
        g_win_start = t; g_win_calls = g_win_bytes = g_win_min = g_win_max = 0; g_win_gap_max = 0;
    }
}

void *pcm_open(unsigned int card, unsigned int device, unsigned int flags, const void *config) {
    if (!g_real_pcm_open) g_real_pcm_open = (pcm_open_fn)tinyalsa_sym("pcm_open");
    if (!g_real_pcm_open) return 0;
    void *r = g_real_pcm_open(card, device, flags, config);
    if (natartos_flag_exists(FLAG_AUDIO_SPY)) {
        g_spy = 1;
        NTLOG_INT("spy: pcm_open card=", (int)card);
        NTLOG_INT("spy:   device=", (int)device);
        NTLOG_HEX("spy:   flags=", flags);   /* bit0 = PCM_IN (capture) */
        if (config) {
            const struct pcm_config_head *c = (const struct pcm_config_head *)config;
            NTLOG_INT("spy:   channels=", (int)c->channels);
            NTLOG_INT("spy:   rate=", (int)c->rate);
            NTLOG_INT("spy:   period_size=", (int)c->period_size);
            NTLOG_INT("spy:   period_count=", (int)c->period_count);
            NTLOG_INT("spy:   format=", c->format);
        }
        NTLOG_HEX("spy:   -> pcm=", (unsigned long)r);
    }
    return r;
}

int pcm_close(void *pcm) {
    if (!g_real_pcm_close) g_real_pcm_close = (pcm_close_fn)tinyalsa_sym("pcm_close");
    if (g_spy) NTLOG_HEX("spy: pcm_close pcm=", (unsigned long)pcm);
    return g_real_pcm_close ? g_real_pcm_close(pcm) : 0;
}

/* --- interpose pcm_write --- */

int pcm_write(void *pcm, const void *data, unsigned int count) {
    g_call_count++;
    if (g_call_count == 1)
        log_str("pcm_write: FIRST CALL (interposition OK)");
    if (g_call_count <= 1 || (g_call_count & 0xFF) == 0) {
        g_mute_all     = natartos_flag_exists(FLAG_MUTE_ALL);
        g_mute_startup = natartos_flag_exists(FLAG_MUTE_STARTUP);
        g_mute_buttons = natartos_flag_exists(FLAG_MUTE_BUTTONS);
        g_spy          = natartos_flag_exists(FLAG_AUDIO_SPY);
    }
    if (g_spy) spy_write(pcm, count);

    if (!g_real_pcm_write)
        return 0;

    int should_mute = 0;
    if (g_mute_all)
        should_mute = 1;
    else if (g_mute_startup && g_call_count <= STARTUP_CALLS)
        should_mute = 1;
    else if (g_mute_buttons && count <= BUTTON_FRAME_THRESHOLD)
        should_mute = 1;

    if (should_mute) {
        /* count is BYTES in tinyalsa's pcm_write — zero exactly the buffer.
         * (Was count * 4, which wrote 3x past the end of the caller's buffer.) */
        natartos_memset((void *)data, 0, (long)count);
    }

    return g_real_pcm_write(pcm, data, count);
}

/* --- natartos_mod_init: called by loader with its dlsym --- */

int natartos_mod_init(void *(*loader_dlsym)(void *, const char *)) {
    if (!loader_dlsym) {
        log_str("mod_mute: no dlsym");
        return -1;
    }

    g_loader_dlsym = loader_dlsym;
    dlopen_fn my_dlopen = (dlopen_fn)loader_dlsym(RTLD_DEFAULT, "dlopen");
    void *tinyalsa = 0;
    if (my_dlopen)
        tinyalsa = my_dlopen("libtinyalsa.so", RTLD_NOLOAD);
    if (tinyalsa)
        g_real_pcm_write = (pcm_write_fn)loader_dlsym(tinyalsa, "pcm_write");
    else
        g_real_pcm_write = 0;
    g_mute_all     = natartos_flag_exists(FLAG_MUTE_ALL);
    g_mute_startup = natartos_flag_exists(FLAG_MUTE_STARTUP);
    g_mute_buttons = natartos_flag_exists(FLAG_MUTE_BUTTONS);
    g_spy          = natartos_flag_exists(FLAG_AUDIO_SPY);
    g_real_pcm_open  = (pcm_open_fn)tinyalsa_sym("pcm_open");
    g_real_pcm_close = (pcm_close_fn)tinyalsa_sym("pcm_close");
    if (g_spy) log_str("mod_mute: audio spy ON");

    log_str(g_real_pcm_write ? "mod_mute: pcm_write resolved OK" :
                                "mod_mute: pcm_write NOT found");
    log_str(g_mute_all     ? "mod_mute: ALL muted" :
            g_mute_startup ? "mod_mute: startup muted" :
            g_mute_buttons ? "mod_mute: buttons muted" :
                             "mod_mute: state=normal");
    return g_real_pcm_write ? 0 : -2;
}

int natartos_mod_register(void *(*dlsym_fn)(void *, const char *)) {
    natartos_menu_register_fn reg =
        (natartos_menu_register_fn)dlsym_fn(RTLD_DEFAULT, "natartos_menu_register");
    if (reg) {
        natartos_menu_entry e = {
            .id       = "mute",
            .name     = "Audio Control",
            .subtitle = "Toggle mute",
            .command  = "sh -c '"
                        "f=/data/local/tmp/natartos_mute; "
                        "if [ -f $f ]; then rm -f $f; else touch $f; fi"
                        "'",
            .priority = 400,
        };
        reg(&e);
        log_str("mod_mute: menu registered");
    }
    return 0;
}
