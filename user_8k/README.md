# user_8k/

This directory contains site voice clips. A clip in this directory adds a new clip, or
replaces a stock clip in `vocab_8k/` with the same name. `usrp-rc` does not change
`vocab_8k/`. To go back to a stock clip, remove the site clip with the same name.

| Item | Requirement |
| --- | --- |
| Format | WAV, 8000 Hz, mono, 16-bit signed PCM |
| Length | 3 minutes maximum. `usrp-rc` does not load a longer clip, and records this in the log. |
| Clip name | The file name without `.wav`. Names are not case-sensitive: `sorry.wav` is the clip `SORRY`. |
| Installed location | `/usr/local/share/usrp-rc/user_8k/`. This directory contains data, not configuration. Thus, it is not in `/etc/`. |

Use a site clip in a `voice:` message the same as a stock clip:

```ini
[messages]
maintenance = voice: SORRY WE ARE DOWN FOR MAINTENANCE
```

For more data, see [README.md](../README.md), "Voice clips", and
[MANUAL.md](../MANUAL.md), section 5.6.

## Preparation of recordings

`usrp-rc` applies no equalization to clips. It changes only their level, with
`audio.voice_level` and `audio.master_gain`.

- Do not normalize clips to full scale. A clip with peaks near 0 dBFS is louder than
  repeat audio.
- MMDVM-Host pre-emphasis increases high frequencies to the transmitter. A full-scale
  clip has no headroom for this increase.
- Start with peaks near -10 dBFS. Then adjust by ear or with a deviation meter.
