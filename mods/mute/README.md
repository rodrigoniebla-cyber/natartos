# Audio Control

Mutes goggle sounds by intercepting `pcm_write` (tinyalsa).

## How it works

Intercepts `pcm_write` via LD_PRELOAD. When the flag is active, it zeroes the
audio buffer before passing it to the real `pcm_write`.
The real `pcm_write` is resolved via `dlopen("libtinyalsa.so", RTLD_NOLOAD)`.

Requires being in LD_PRELOAD (not just dlopen) — symbol interposition.

## Configuration

| Flag | Effect |
|------|--------|
| `touch /data/local/tmp/natartos_mute` | Mute EVERYTHING |
| `touch /data/local/tmp/natartos_mute_startup` | Mute the first ~5s after launch |
| `touch /data/local/tmp/natartos_mute_buttons` | Mute short sounds (buttons, UI) |

Priority: mute > mute_startup > mute_buttons. Flags are checked every 256 calls.

## Limitations

- The power-on chime is played by the bootloader BEFORE dji_glasses — not intercepted
- `mute_startup` mutes the first ~300 `pcm_write` calls after dji_glasses launches
- `mute_buttons` detects short sounds by buffer size (<=256 frames)

## Speaker recon (toward playing our own audio)

Goal: play our own audio through the goggles' built-in speaker (PC/sim sound
from PC Stream or toolya-display, Doom sound, alerts). Before choosing how, we
need to know how `dji_glasses` uses the speaker.

**1. Audio spy (in this mod, read-only):**

```sh
touch /data/local/tmp/natartos_audio_spy
# restart mods, then trigger sounds: press buttons, open menus, wait idle ~10s
grep "spy:" /data/local/tmp/natartos.log
rm /data/local/tmp/natartos_audio_spy
```

It logs each `pcm_open` (card, device, channels, rate, period size and count,
format) and `pcm_close`, plus one summary per second of `pcm_write` traffic
(calls, bytes, min/max size, largest gap). "burst after idle" lines mean writes
stop between sounds.

**2. ALSA state from the shell (no mod needed):**

```sh
cat /proc/asound/cards /proc/asound/pcm
ls -l /dev/snd
ls -l /proc/$(pidof dji_glasses)/fd | grep snd        # does DJI hold the device open?
cat /proc/asound/card0/pcm*p/sub0/status              # RUNNING while idle?
which tinyplay tinymix tinypcminfo
tinymix 2>/dev/null | head -80                        # speaker route / volume controls
```

**How the answers decide the design:**

| Finding | Playback approach |
|---------|-------------------|
| DJI opens/closes the device per sound (device free when idle) | Standalone player app: open the PCM with tinyalsa, read PCM from stdin/FIFO (like `fb_receiver`). Yield when DJI plays a sound. |
| DJI keeps the device open and writes continuously | Mix in the `pcm_write` hook: ring buffer fed from a FIFO, summed into DJI's buffer (clipped). DJI's own sounds keep working. |
| DJI keeps it open but writes only during sounds | Mixer in the hook plus a feeder thread that writes silence+our audio while DJI is idle. Needs care. |

Format matters too: the spy's `rate` / `channels` / `format` set what the PC
side must send (or what the player must resample to).
