#!/system/bin/sh
# natartos boot guard — runs at power-on from /vendor/etc/init/natartos.rc.
#
# Safety model (why this is "safe boot"):
#   * OPT-IN   — does nothing unless /data/.natartos_boot_enabled exists.
#   * KILL-SW  — honours the existing kill-switch /data/.natartos_disabled.
#   * COUNTER  — the loader's boot-counter safe mode still applies: 3 bad boots
#                in a row (no healthy render) → mods auto-skip until re-enabled.
#   * SAFE DEF — launches the menu in BOOT mode, where no input / any failure
#                lands on STOCK (see bootmenu.c). You must pick Modified.
#   * /vendor only — never writes /system; a reboot always clears SELinux back
#                to enforcing and this is re-applied per boot, session-style.
#
# Escape hatch if a modded boot ever misbehaves: power on, do nothing — the
# menu times out to STOCK. Or power-cycle 3× to trip the counter. Or connect
# USB and `toolya natartos safe on` / `uninstall`.

BOOT_FLAG=/data/.natartos_boot_enabled
KILL_SW=/data/.natartos_disabled
INJECT=/vendor/natartos/inject.sh
MENU_MODE=/data/local/tmp/natartos_menu_mode
LOG=/data/local/tmp/natartos_boot.log

log() { echo "$(date '+%H:%M:%S') $1" >> "$LOG"; }

: > "$LOG"
log "boot guard start"

[ -f "$BOOT_FLAG" ] || { log "not enabled (touch $BOOT_FLAG to turn on) — stock"; exit 0; }
[ -f "$KILL_SW" ]   && { log "kill-switch active — stock"; exit 0; }
[ -x "$INJECT" ]    || { log "no inject.sh — stock"; exit 0; }

# SELinux permissive for this session only (resets to enforcing on reboot).
# Same devmem approach toolya uses; addresses are the per-build candidates from
# the toolya contract. If none match, mods just won't load → stock. Not fatal.
for A in 0x17b6648 0x17e6888; do
    V=$(busybox devmem "$A" 32 2>/dev/null) || continue
    case "$V" in
        0x01000100|0x01000000)
            busybox devmem "$(printf 0x%X $((A + 1)))" 8 0x00 2>/dev/null
            break ;;
    esac
done
log "selinux=$(getenforce 2>/dev/null)"

# Tell the boot menu this is a power-on (default STOCK on timeout).
echo boot > "$MENU_MODE"

log "launching inject.sh"
setsid sh "$INJECT" </dev/null >/dev/null 2>&1 &
log "boot guard done"
