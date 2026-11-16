/*
 * mod_launcher.so — natartos Home (launcher) entry for DJI Goggles 3.
 *
 * - Registers "Home" in the natartos menu (DJI More menu): opens the launcher.
 * - Shows the launcher automatically once per boot when mods start, ~8s after
 *   dji_glasses comes up. Opt out: touch /data/local/tmp/natartos_home_noauto
 *
 * "Once per boot" is keyed on the kernel boot_id, so dji_glasses respawns
 * (app exits, crash guard) don't pop the launcher up again.
 */

#define NTLOG_PREFIX "launcher"
#include "natartos_common.h"

#define RTLD_DEFAULT ((void *)0)
typedef int (*system_fn)(const char *);

#define LAUNCHER "/vendor/natartos/apps/launcher/launcher"

int natartos_mod_init(void *(*loader_dlsym)(void *, const char *)) {
    (void)loader_dlsym;
    return 0;
}

int natartos_mod_register(void *(*loader_dlsym)(void *, const char *)) {
    natartos_menu_register_fn reg =
        (natartos_menu_register_fn)loader_dlsym(RTLD_DEFAULT, "natartos_menu_register");
    if (reg) {
        natartos_menu_entry home = {
            .id        = "home",
            .name      = "Home",
            .subtitle  = "natartos apps",
            .icon_path = "/vendor/natartos/icons/natartos_icon_home.rgba",
            .command   = "nohup " LAUNCHER " >/dev/null 2>&1 &",
            .priority  = 10,   /* first */
        };
        reg(&home);
    } else {
        NTLOG("menu_register not found");
    }

    system_fn sys = (system_fn)loader_dlsym(RTLD_DEFAULT, "system");
    if (sys) {
        sys("sh -c '"
            "T=/data/local/tmp; "
            "[ -f $T/natartos_home_noauto ] && exit 0; "
            "[ -x " LAUNCHER " ] || exit 0; "
            "b=$(cat /proc/sys/kernel/random/boot_id); "
            "[ \"$(cat $T/natartos_home_boot 2>/dev/null)\" = \"$b\" ] && exit 0; "
            "echo $b > $T/natartos_home_boot; "
            "sleep 8; exec " LAUNCHER
            "' >/dev/null 2>&1 &");
        NTLOG("autostart scheduled (once per boot)");
    }
    return 1;
}
