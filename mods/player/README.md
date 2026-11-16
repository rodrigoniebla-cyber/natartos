# Videos (player)

Plays videos from the SD card full-screen on the goggles, with sound through the
built-in speaker. Open it from the **Videos** tile on Home.

## Putting videos on the card

Copy `.mpg` files into **`natartos/videos/`** on the SD card. The player also
looks in `/data/local/tmp/videos`. Convert anything with ffmpeg:

```sh
ffmpeg -i input.mp4 -c:v mpeg1video -q:v 4 -vf scale=-2:720 \
       -c:a mp2 -b:a 192k -ar 48000 -ac 2 output.mpg
```

- **Format:** MPEG-1 video + MP2 audio (`.mpg`). Decoding runs on the goggles'
  CPU (pl_mpeg). H.264/MP4, including DJI's own DVR recordings, needs
  converting first.
- **Resolution:** 720p is the sweet spot. 1080p works if the CPU keeps up.
- **Audio:** 48 kHz stereo is safest. Other rates are resampled.

## Controls

| In the list | During playback |
|---|---|
| Joystick up/down: choose | Press / OK: pause or resume |
| Press / OK: play | Left / right: seek 10 s |
| Left / right: rescan the card | BACK: stop, back to the list |
| BACK: leave | |

## Sound

The player opens the speaker itself through tinyalsa (built in statically), by
trying each playback device in `/proc/asound/pcm`. **Not verified yet:** if
`dji_glasses` holds the speaker open, the player can't get it and plays the
video silently (the reason goes to `natartos_player.log`). The audio spy in
`mods/mute` answers this. If DJI does hold the speaker, the fix is to mix
into DJI's own audio stream from the mute mod's `pcm_write` hook. Force a
device with `NATARTOS_AUDIO=card,device`.

## Status

**Untested on hardware.** Under QEMU with simulated input and audio captured
to a file, all of these pass:
- list, play, pause, seek, BACK and exit
- 720p30 decoded at full rate under emulation, 4:3 video pillarboxed
- 44.1 → 48 kHz resample keeps pitch (440 Hz test tone)

Debug log: `/data/local/tmp/natartos_player.log`. It records the speaker
device it opened (or why it couldn't) and the frame rate actually shown.

Host testing: `NATARTOS_TMP`, `NATARTOS_INPUT0/3`, `NATARTOS_VIDEO_DIRS`,
`NATARTOS_AUDIO=none|card,dev|file:/path.raw`, `NATARTOS_AUDIO_RATE`.
`player --render-only` draws the list once and exits.

Third-party code: `third_party/pl_mpeg` (MIT) and `third_party/tinyalsa` (BSD-3).
