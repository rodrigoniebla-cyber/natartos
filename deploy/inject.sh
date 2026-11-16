#!/system/bin/sh
# natartos inject — restart dji_glasses with LD_PRELOAD + respawn
# Respawn up to MAX_CRASHES times within WINDOW_SEC, then give up.
MAX_CRASHES=5
WINDOW_SEC=120
LOG=/data/local/tmp/natartos_inject.log
PIDFILE=/data/local/tmp/natartos_inject.pid
# Boot menu: on the first start of a session, ask "Stock or Modified?" on the
# goggles' screen. The answer lands in MODE_FILE ("stock" / "modified") and
# holds for every respawn until the next `natartos start`.
# Opt out with: touch /data/.natartos_bootmenu_off
BOOTMENU=/vendor/natartos/apps/bootmenu/bootmenu
BOOTMENU_OFF=/data/.natartos_bootmenu_off
MODE_FILE=/data/local/tmp/natartos_mode

log() { echo "$(date '+%H:%M:%S') $1" >> "$LOG"; }

# ── Single-instance guard ──
# Kill previous inject.sh + its dji_glasses before taking over.
if [ -f "$PIDFILE" ]; then
    OLD_PID=$(cat "$PIDFILE" 2>/dev/null)
    if [ -n "$OLD_PID" ] && kill -0 "$OLD_PID" 2>/dev/null; then
        log "killing old inject pid=$OLD_PID"
        # Kill child dji_glasses first (PPID = old inject)
        for child in $(ps -A -o pid,ppid | while read cpid cppid; do [ "$cppid" = "$OLD_PID" ] && echo "$cpid"; done); do
            kill -9 "$child" 2>/dev/null
        done
        kill -9 "$OLD_PID" 2>/dev/null
        sleep 1
    fi
fi
echo $$ > "$PIDFILE"

sleep 3
rm -f /data/local/tmp/fb_shared "$MODE_FILE"
stop dji_glasses 2>/dev/null
log "stopped init service dji_glasses"
sleep 1
PID=$(pidof dji_glasses)
[ -n "$PID" ] && kill "$PID" && log "killed leftover pid=$PID"
sleep 1

build_preload() {
    # Stock chosen on the boot menu → plain dji_glasses, no loader, no mods.
    [ "$(cat "$MODE_FILE" 2>/dev/null)" = "stock" ] && return
    P=/vendor/natartos/natartos_loader.so
    for so in /vendor/natartos/mods/*.so; do
        s=$(basename "$so")
        [ -f /vendor/natartos/enabled/"$s" ] && P="$P:$so"
    done
    echo "$P"
}

# The menu draws through the overlay mod, so it needs that mod enabled.
bootmenu_wanted() {
    [ -x "$BOOTMENU" ] || return 1
    [ -f "$BOOTMENU_OFF" ] && return 1
    case ":$1:" in *hook_overlay.so:*) return 0 ;; esac
    return 1
}

crash_count=0
last_reset=$(date +%s)
first_start=1

while true; do
    # Each fresh dji_glasses starts clean: drop runtime overlay flags a previous
    # session (or the respawn before this one) left behind. A stale fb_stop
    # otherwise makes the new overlay run cleanup→restart on every boot forever.
    rm -f /data/local/tmp/fb_stop /data/local/tmp/fb_override
    PRELOAD=$(build_preload)
    if [ "$first_start" = 1 ]; then
        first_start=0
        if [ -n "$PRELOAD" ] && bootmenu_wanted "$PRELOAD"; then
            # Starts modded; the menu waits for dji_glasses + overlay, then
            # either hands the screen back (modified) or restarts it stock.
            log "boot menu: launching"
            LD_PRELOAD= "$BOOTMENU" </dev/null >/dev/null 2>&1 &
        else
            echo modified > "$MODE_FILE"
        fi
    fi
    if [ -n "$PRELOAD" ]; then
        export LD_PRELOAD="$PRELOAD"
        log "start dji_glasses LD_PRELOAD=$PRELOAD"
    else
        unset LD_PRELOAD
        log "start dji_glasses STOCK (no mods)"
    fi
    /system/bin/dji_glasses -g -f
    EXIT=$?
    log "exited code=$EXIT"

    # Intentional restart (overlay kicks this on app exit to repaint the UI) —
    # not a crash: respawn at once, don't touch the crash counter.
    if [ -f /data/local/tmp/natartos_restart ]; then
        rm -f /data/local/tmp/natartos_restart
        log "intentional restart (app exit / boot menu)"
        continue
    fi

    now=$(date +%s)
    elapsed=$((now - last_reset))
    if [ "$elapsed" -ge "$WINDOW_SEC" ]; then
        crash_count=0
        last_reset=$now
    fi
    crash_count=$((crash_count + 1))

    if [ "$crash_count" -ge "$MAX_CRASHES" ]; then
        log "GIVE UP: $crash_count crashes in ${elapsed}s"
        break
    fi

    log "respawn #$crash_count in 2s"
    sleep 2
done

rm -f "$PIDFILE"
