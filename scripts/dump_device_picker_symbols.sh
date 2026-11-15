#!/system/bin/sh
# dump_device_picker_symbols.sh — find the classes behind the goggles' device
# picker (where you choose Air Unit / O4 Pro / Avata 2 ...), so natartos can add
# a "Computer" entry there. Read-only.
#
# Run on the goggles:  sh /data/local/tmp/dump_device_picker_symbols.sh
# Output:              /data/local/tmp/device_picker_symbols.txt
#
# Pair it with the menu spy: `touch /data/local/tmp/natartos_menu_spy`, restart
# mods, open the device picker, then `grep spy: /data/local/tmp/natartos.log`.

OUT=/data/local/tmp/device_picker_symbols.txt
PAT='Device|Product|Aircraft|Drone|Uav|Air[_]?Unit|Switch|Select|Pair|Bind|Link|Connect'
echo "=== device picker symbol dump $(date) ===" > "$OUT"

PID=$(pidof dji_glasses)
if [ -z "$PID" ]; then echo "dji_glasses not running" | tee -a "$OUT"; exit 1; fi

LIBS="/system/bin/dji_glasses $(awk '/\.so/ {print $6}' /proc/$PID/maps | sort -u)"

echo "" >> "$OUT"; echo "=== vtables (__vmt_*) matching $PAT ===" >> "$OUT"
for lib in $LIBS; do
    [ -f "$lib" ] || continue
    syms=$(readelf -sW "$lib" 2>/dev/null | awk '{print $NF}' | grep '^__vmt_' | grep -iE "$PAT")
    [ -n "$syms" ] && { echo "--- $lib ---"; echo "$syms"; } >> "$OUT"
done

echo "" >> "$OUT"; echo "=== list/menu classes (any __vmt_ with List|Menu|Picker|Page) ===" >> "$OUT"
for lib in $LIBS; do
    [ -f "$lib" ] || continue
    syms=$(readelf -sW "$lib" 2>/dev/null | awk '{print $NF}' | grep '^__vmt_' | grep -iE 'List|Menu|Picker|Page' | grep -iE "$PAT")
    [ -n "$syms" ] && { echo "--- $lib ---"; echo "$syms"; } >> "$OUT"
done

echo "" >> "$OUT"; echo "=== functions matching $PAT and (Reload|Add|Item|Select|OnSet) ===" >> "$OUT"
for lib in $LIBS; do
    [ -f "$lib" ] || continue
    syms=$(readelf -sW "$lib" 2>/dev/null | awk '$4=="FUNC" {print $NF}' | grep -iE "$PAT" | grep -iE 'Reload|Add|Item|Select|OnSet' | head -300)
    [ -n "$syms" ] && { echo "--- $lib ---"; echo "$syms"; } >> "$OUT"
done

echo "" >> "$OUT"; echo "=== done ===" >> "$OUT"
echo "Saved to $OUT"; wc -l "$OUT"
