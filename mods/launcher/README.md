# Home (launcher)

A home screen for the goggles: an icon grid of "apps", drawn full-screen through
the display overlay.

| Tile | Does |
|------|------|
| **DJI Goggles** | Back to the stock DJI UI (mods stay loaded) |
| **PC Display** | Starts PC Stream; shows your Mac/PC (toolya-display or the web PC Stream) |
| **Doom** | Doom |
| **Mute All** / **Mute Buttons** | Audio Control toggles (only if the `mute` mod is enabled) |
| *your tiles* | from `/data/local/tmp/natartos_apps.conf` |

Tiles for apps that aren't installed are hidden.

**Controls:** joystick moves, press (or OK) opens, BACK returns to the DJI UI.
When a full-screen app exits, you land back on Home after about 3 seconds.

**When it shows:** once per boot, ~8 s after mods start, and any time from
**Home** in the More menu. To turn off the automatic one:
`touch /data/local/tmp/natartos_home_noauto`.

## Adding tiles

One line per tile in `/data/local/tmp/natartos_apps.conf`:

```
NAME|SUBTITLE|ICON|REQUIRES|ACTION
```

- `ICON`: an NC01 `.rgba` (see `assets/icons`), `@goggles`, `@speaker`, or empty
  for a letter icon.
- `REQUIRES`: tile is hidden unless this path exists (empty = always shown).
- `ACTION`:
  - `@dji`: back to the DJI UI
  - `@toggle:/path/flag`: switch a flag file on or off (shown as ON/OFF)
  - `!command`: full-screen app, run in the foreground, then back to Home
  - `command`: run in the background and stay on Home

Example:

```
Restart Mods|Reload natartos|||sh /vendor/natartos/inject.sh restart
```

Up to 8 tiles (built-ins first).

## How it works

- Draws into `fb_shared` with the two-slot protocol (`include/natartos_fb.h`)
  and sets `fb_override`, so the overlay shows it instead of the DJI UI.
- Grabs `event0` (OK/BACK) and `event3` (5D joystick) so the DJI UI doesn't
  react while Home is up. The joystick codes are rotated 180°, same mapping as
  Doom.
- Opening a full-screen app hands it the screen with `fb_override` still set.
  When the app exits, the overlay tears down as usual, and Home waits out the
  overlay's re-arm delay before taking the screen back.

## Status

**Untested on hardware.** Verified so far: it builds static for aarch64, and
under QEMU with simulated input the following all pass: drawing,
navigation, toggles, app handoff and return, outside teardown, BACK exit, and
single-instance locking. It isn't in release builds yet (CI only compile-checks
it). Build it with `scripts/build.sh`.

Host testing: `NATARTOS_TMP`, `NATARTOS_INPUT0`/`NATARTOS_INPUT3` and
`NATARTOS_APPS_CONF` override the device paths, and `launcher --render-only [N]`
draws one frame (tile N selected) into `$NATARTOS_TMP/fb_shared` and exits.
