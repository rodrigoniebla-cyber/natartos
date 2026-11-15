/*
 * hook_menu — inject custom items into MoreMenu via registration API.
 *
 * Mods register entries via natartos_menu_register() during mod_register phase.
 * Items are built and injected on each EW Reload (menu open), sorted by priority.
 *
 * Approach: PLT interposition of MxBindableAbstractMenu_Reload, filter for
 * MoreMenu VMT, inject via AddItemFromData with cloned templates.
 *
 * Item data offsets (confirmed):
 *   f[5]  (40)  = icon bitmap (ResourcesBitmap EW object)
 *   f[16] (128) = title XString
 *   f[17] (136) = accessibility subtitle
 *   f[18] (144) = type (31)
 *   f[4]  (32)  = MxBindableMenuItemSubTitleActionValue
 *     [4] slot_obj (+0x20)  [5] slot_fn (+0x28)  [6] subtitle (+0x30)
 */

#define NTLOG_PREFIX "menu"
#include "natartos_common.h"

#define MY_RTLD_NEXT    ((void *)(long)-1)
extern void *dlsym(void *handle, const char *symbol);

typedef void (*reload_fn)(void *self);
typedef void (*add_item_fn)(void *self, void *itemdata);
typedef void (*set_noi_fn)(void *, int);
typedef void *(*new_str_fn)(const char *ascii);
typedef int (*system_fn)(const char *cmd);

/* EW bitmap API */
typedef struct { int x, y; } XPoint;
typedef struct { int x1, y1, x2, y2; } XRect;
typedef void *(*create_bmp_fn)(int format, XPoint frameSize, XPoint frameDelay, int frames);
typedef void *(*lock_bmp_fn)(void *bmp, int frameNo, XRect area, int rd, int wr);
typedef void  (*unlock_bmp_fn)(void *lock);
#define EW_FMT_RGBA 0

typedef void *(*new_obj_fn)(void *classVMT, int dummy);

static reload_fn g_orig_reload = 0;
static void *g_more_menu_vmt = 0;
static add_item_fn g_add_item_from_data = 0;
static set_noi_fn g_set_noi = 0;
static new_str_fn g_new_str = 0;
static system_fn g_system = 0;
static create_bmp_fn g_create_bmp = 0;
static lock_bmp_fn   g_lock_bmp   = 0;
static unlock_bmp_fn g_unlock_bmp = 0;
static new_obj_fn g_new_obj = 0;
static void *g_vmt_res_bmp = 0;

/* ── Menu spy (recon, read-only) ──
 * When the flag file exists, log the class (VMT symbol) of every menu/list
 * that reloads — once per class. Open a screen on the goggles, then read
 * natartos.log to learn which class draws it (e.g. the device picker).
 * Nothing is modified. */
#define SPY_FLAG "/data/local/tmp/natartos_menu_spy"
#define SPY_MAX_SEEN 64
typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } dl_info_t;
typedef int (*dladdr_fn)(const void *addr, dl_info_t *info);
static dladdr_fn g_dladdr = 0;
static void *g_spy_seen[SPY_MAX_SEEN];
static int g_spy_n = 0;

/* ── Menu slot registry ── */

#define MAX_STR  64
#define MAX_CMD  512
#define MAX_ICON 128

typedef struct {
    char id[MAX_STR];
    char name[MAX_STR];
    char subtitle[MAX_STR];
    char icon_path[MAX_ICON];
    char command[MAX_CMD];
    void (*on_action)(void);
    int priority;
    /* Runtime (rebuilt each Reload) */
    int marker;
    unsigned char item_data[256] __attribute__((aligned(8)));
    unsigned char act_data[64] __attribute__((aligned(8)));
    void *icon;
    void *str_name;
    void *str_sub;
} menu_slot;

static menu_slot g_slots[NATARTOS_MAX_MENU_ITEMS];
static int g_slot_count = 0;

/* Scratch for runtime RGBA override files (<=256x256 RGBA) */
static unsigned char g_icon_buf[256 * 256 * 4] __attribute__((aligned(8)));

/* ── Helpers ── */

static inline reload_fn resolve_original(void) {
    if (g_orig_reload) return g_orig_reload;
    g_orig_reload = (reload_fn)dlsym(MY_RTLD_NEXT,
        "MxBindableAbstractMenu_Reload");
    return g_orig_reload;
}

static void my_memcpy(void *dst, const void *src, int n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}

static void str_copy(char *dst, const char *src, int maxlen) {
    if (!src) { dst[0] = 0; return; }
    int i = 0;
    while (i < maxlen - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Sort slots by priority (insertion sort, stable) */
static void sort_slots(void) {
    for (int i = 1; i < g_slot_count; i++) {
        menu_slot tmp;
        my_memcpy(&tmp, &g_slots[i], sizeof(menu_slot));
        int j = i;
        while (j > 0 && g_slots[j-1].priority > tmp.priority) {
            my_memcpy(&g_slots[j], &g_slots[j-1], sizeof(menu_slot));
            j--;
        }
        if (j != i) my_memcpy(&g_slots[j], &tmp, sizeof(menu_slot));
    }
}

/* ── Registration API (exported) ── */

int natartos_menu_register(const natartos_menu_entry *entry) {
    if (!entry || !entry->id || !entry->name) return -1;
    if (g_slot_count >= NATARTOS_MAX_MENU_ITEMS) {
        NTLOG("menu_register: full");
        return -2;
    }

    /* Update existing entry with same id */
    for (int i = 0; i < g_slot_count; i++) {
        if (natartos_strncmp(g_slots[i].id, entry->id, MAX_STR) == 0) {
            str_copy(g_slots[i].name, entry->name, MAX_STR);
            str_copy(g_slots[i].subtitle, entry->subtitle, MAX_STR);
            str_copy(g_slots[i].icon_path, entry->icon_path, MAX_ICON);
            str_copy(g_slots[i].command, entry->command, MAX_CMD);
            g_slots[i].on_action = entry->on_action;
            g_slots[i].priority = entry->priority;
            sort_slots();
            NTLOG("register: updated");
            NTLOG(entry->id);
            return 0;
        }
    }

    menu_slot *s = &g_slots[g_slot_count];
    natartos_memset(s, 0, sizeof(menu_slot));
    str_copy(s->id, entry->id, MAX_STR);
    str_copy(s->name, entry->name, MAX_STR);
    str_copy(s->subtitle, entry->subtitle, MAX_STR);
    str_copy(s->icon_path, entry->icon_path, MAX_ICON);
    str_copy(s->command, entry->command, MAX_CMD);
    s->on_action = entry->on_action;
    s->priority = entry->priority;
    s->marker = g_slot_count + 1;
    g_slot_count++;
    sort_slots();

    NTLOG("register: added");
    NTLOG(entry->id);
    NTLOG_INT("count=", g_slot_count);
    return 0;
}

/* ── Bitmap helpers ── */

static void bmp_frame_size(void *bmp, int *w, int *h) {
    *w = 0; *h = 0;
    if (!bmp) return;
    int bw = *(int *)((unsigned char *)bmp + 56);
    int bh = *(int *)((unsigned char *)bmp + 60);
    if (bw > 0 && bw <= 512 && bh > 0 && bh <= 512) { *w = bw; *h = bh; }
}

static long load_rgba_file(const char *path, int *w, int *h) {
    int fd = (int)sys_openat(AT_FDCWD, path, O_RDONLY, 0);
    if (fd < 0) return 0;
    unsigned char hdr[12];
    long got = 0;
    while (got < 12) {
        long r = sys_read(fd, hdr + got, 12 - got);
        if (r <= 0) { sys_close(fd); return 0; }
        got += r;
    }
    if (hdr[0] != 'N' || hdr[1] != 'C' || hdr[2] != '0' || hdr[3] != '1') { sys_close(fd); return 0; }
    int fw = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (hdr[7] << 24);
    int fh = hdr[8] | (hdr[9] << 8) | (hdr[10] << 16) | (hdr[11] << 24);
    if (fw < 1 || fw > 256 || fh < 1 || fh > 256) { sys_close(fd); return 0; }
    long need = (long)fw * fh * 4;
    long off = 0;
    while (off < need) {
        long r = sys_read(fd, g_icon_buf + off, need - off);
        if (r <= 0) { sys_close(fd); return 0; }
        off += r;
    }
    sys_close(fd);
    *w = fw; *h = fh;
    return need;
}

static void *make_icon(int w, int h, const unsigned char *src,
                       unsigned char pr, unsigned char pg, unsigned char pb) {
    if (!g_create_bmp || !g_lock_bmp || !g_unlock_bmp) return 0;
    if (w < 1 || w > 256 || h < 1 || h > 256) { w = 64; h = 48; }

    XPoint sz; sz.x = w; sz.y = h;
    XPoint dl; dl.x = 0; dl.y = 0;
    void *raw = g_create_bmp(EW_FMT_RGBA, sz, dl, 1);
    if (!raw) return 0;

    XRect area; area.x1 = 0; area.y1 = 0; area.x2 = w; area.y2 = h;
    unsigned char *lk = (unsigned char *)g_lock_bmp(raw, 0, area, 0, 1);
    if (lk) {
        unsigned char *px = *(unsigned char **)(lk + 0);
        int pitchX = *(int *)(lk + 8);
        int pitchY = *(int *)(lk + 12);
        if (px && pitchX > 0 && pitchY > 0) {
            for (int y = 0; y < h; y++) {
                unsigned char *row = px + (long)y * pitchY;
                for (int x = 0; x < w; x++) {
                    unsigned char *p = row + (long)x * pitchX;
                    if (src) {
                        const unsigned char *s = src + ((long)y * w + x) * 4;
                        p[0] = s[2]; p[1] = s[1]; p[2] = s[0]; p[3] = s[3];
                    } else {
                        int border = (x < 2 || y < 2 || x >= w - 2 || y >= h - 2);
                        p[0] = border ? 255 : pb;
                        p[1] = border ? 255 : pg;
                        p[2] = border ? 255 : pr;
                        p[3] = 255;
                    }
                }
            }
        }
        g_unlock_bmp(lk);
    }

    /* Wrap raw XBitmap in ResourcesBitmap EW object */
    if (g_new_obj && g_vmt_res_bmp) {
        void *obj = g_new_obj(g_vmt_res_bmp, 0);
        if (obj) {
            unsigned char *o = (unsigned char *)obj;
            unsigned char *r = (unsigned char *)raw;
            *(void **)(o + 40) = raw;
            *(int *)(o + 48) = *(int *)(r + 12);
            *(int *)(o + 52) = *(int *)(r + 20);
            unsigned long xb4;
            my_memcpy(&xb4, r + 4, 8);
            *(unsigned long *)(o + 56) = xb4;
            *(o + 65) = 1;
            return obj;
        }
    }
    return raw;
}

static void *build_icon(const char *file, int w, int h,
                        unsigned char pr, unsigned char pg, unsigned char pb) {
    if (file && file[0]) {
        int fw = 0, fh = 0;
        if (load_rgba_file(file, &fw, &fh) > 0)
            return make_icon(fw, fh, g_icon_buf, 0, 0, 0);
    }
    return make_icon(w, h, 0, pr, pg, pb);
}

/* ── Action dispatch ── */

static void menu_action_handler(void *obj) {
    menu_slot *s = (menu_slot *)obj;
    if (!s || !s->command[0]) return;
    NTLOG("action:");
    NTLOG(s->id);
    if (g_system) g_system(s->command);
}

/* ── Inject registered items into menu ── */

static void inject_items(void *self, void *menudata) {
    NTLOG("inject: enter");
    if (g_slot_count == 0) { NTLOG("inject: no items"); return; }

    void **md_vmt = *(void ***)menudata;
    typedef int (*count_fn)(void *);
    typedef void *(*get_fn)(void *, int);
    count_fn get_count = (count_fn)md_vmt[11];
    get_fn get_item = (get_fn)md_vmt[12];

    int cnt = get_count(menudata);
    NTLOG_INT("inject: cnt=", cnt);
    if (cnt < 1) return;

    void *tmpl = get_item(menudata, cnt - 1);
    if (!tmpl) return;

    /* Get stock bitmap reference for fallback icon size */
    void *stock_bmp = 0;
    {
        void *item0 = get_item(menudata, 0);
        if (item0) stock_bmp = (void *)((unsigned long *)item0)[5];
    }
    int icon_w = 0, icon_h = 0;
    bmp_frame_size(stock_bmp, &icon_w, &icon_h);
    if (icon_w == 0) { icon_w = 104; icon_h = 104; }

    unsigned long *tp = (unsigned long *)tmpl;

    for (int i = 0; i < g_slot_count; i++) {
        menu_slot *s = &g_slots[i];

        /* Clone template action, then override slot_obj → our slot,
         * slot_fn → menu_action_handler so clicks run s->command */
        if (tp[4]) {
            my_memcpy(s->act_data, (void *)tp[4], 64);
        }
        ((unsigned long *)s->act_data)[4] = (unsigned long)s;
        ((unsigned long *)s->act_data)[5] = (unsigned long)menu_action_handler;

        /* Build EW strings (recreated every Reload — EW GC frees them) */
        s->str_name = g_new_str(s->name);
        s->str_sub = s->subtitle[0] ? g_new_str(s->subtitle) : 0;

        if (s->str_sub)
            ((unsigned long *)s->act_data)[6] = (unsigned long)s->str_sub;

        /* Build icon */
        s->icon = build_icon(s->icon_path, icon_w, icon_h, 128, 128, 128);

        /* Build item data from template */
        my_memcpy(s->item_data, tmpl, 256);
        unsigned long *d = (unsigned long *)s->item_data;
        d[4]  = (unsigned long)s->act_data;
        d[16] = (unsigned long)s->str_name;
        if (s->str_sub) d[17] = (unsigned long)s->str_sub;
        if (s->icon)        d[5] = (unsigned long)s->icon;
        else if (stock_bmp) d[5] = (unsigned long)stock_bmp;

        g_add_item_from_data(self, s->item_data);
        NTLOG("inject: added");
        NTLOG(s->id);
    }

    if (g_set_noi) g_set_noi(self, 0);
    void **vmt = (void **)g_more_menu_vmt;
    ((void (*)(void *))vmt[58])(self);
    NTLOG_INT("inject: done, items=", g_slot_count);
}

static void spy_log(void *self) {
    if (sys_faccessat(AT_FDCWD, SPY_FLAG, F_OK) != 0) return;
    void *vmt = *(void **)self;
    for (int i = 0; i < g_spy_n; i++) if (g_spy_seen[i] == vmt) return;
    if (g_spy_n < SPY_MAX_SEEN) g_spy_seen[g_spy_n++] = vmt;
    char msg[200];
    const char *pre = "spy: Reload class=";
    int i = 0;
    while (*pre) msg[i++] = *pre++;
    dl_info_t info;
    natartos_memset(&info, 0, sizeof info);
    const char *name = 0;
    if (g_dladdr && g_dladdr(vmt, &info) && info.dli_sname && info.dli_saddr == vmt)
        name = info.dli_sname;
    if (name) {
        while (*name && i < 190) msg[i++] = *name++;
        msg[i] = 0;
        NTLOG(msg);
    } else {
        msg[i] = 0;
        NTLOG_HEX("spy: Reload class=? vmt=", (unsigned long)vmt);
    }
}

/* PLT interposition — intercepts ALL Reload calls, filters for MoreMenu */
void MxBindableAbstractMenu_Reload(void *self) {
    static int s_log_once = 0;
    reload_fn orig = resolve_original();
    if (!s_log_once) {
        s_log_once = 1;
        NTLOG_HEX("Reload: orig=", (unsigned long)orig);
        NTLOG_HEX("Reload: vmt=", (unsigned long)g_more_menu_vmt);
    }
    if (!orig) return;
    orig(self);
    spy_log(self);

    if (!g_more_menu_vmt) return;
    void **obj_vmt = *(void ***)self;
    if ((void *)obj_vmt != g_more_menu_vmt) return;

    unsigned long *p = (unsigned long *)self;
    void *menudata = (void *)p[73];
    if (menudata) inject_items(self, menudata);
}

/* ── mod_init: resolve EW symbols ── */

int natartos_mod_init(void *(*loader_dlsym)(void *, const char *)) {
    NTLOG("=== init ===");
    g_more_menu_vmt = loader_dlsym(RTLD_DEFAULT,
        "__vmt_DuThemedControlsMoreMenu");
    NTLOG_HEX("vmt_more=", (unsigned long)g_more_menu_vmt);
    resolve_original();
    g_add_item_from_data = (add_item_fn)loader_dlsym(RTLD_DEFAULT,
        "MxBindableAbstractMenu_AddItemFromData");
    g_set_noi = (set_noi_fn)loader_dlsym(RTLD_DEFAULT,
        "CoreVerticalList_OnSetNoOfItems");
    g_new_str = (new_str_fn)loader_dlsym(RTLD_DEFAULT,
        "EwNewStringAnsi");
    g_system = (system_fn)loader_dlsym(RTLD_DEFAULT, "system");
    g_create_bmp = (create_bmp_fn)loader_dlsym(RTLD_DEFAULT, "EwCreateBitmap");
    g_lock_bmp   = (lock_bmp_fn)loader_dlsym(RTLD_DEFAULT, "EwLockBitmap");
    g_unlock_bmp = (unlock_bmp_fn)loader_dlsym(RTLD_DEFAULT, "EwUnlockBitmap");
    g_new_obj = (new_obj_fn)loader_dlsym(RTLD_DEFAULT, "EwNewObjectIndirect");
    g_vmt_res_bmp = loader_dlsym(RTLD_DEFAULT, "__vmt_ResourcesBitmap");
    g_dladdr = (dladdr_fn)loader_dlsym(RTLD_DEFAULT, "dladdr");
    NTLOG("=== init done ===");
    return 1;
}

/* ── mod_register: menu framework only, apps register themselves ── */

int natartos_mod_register(void *(*loader_dlsym)(void *, const char *)) {
    (void)loader_dlsym;
    NTLOG_INT("=== register ready, slots=", g_slot_count);
    return 1;
}
