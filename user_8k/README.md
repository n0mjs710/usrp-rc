# user_8k/

Drop your own voice clips here to add new ones or override a stock
`vocab_8k/` clip of the same name — nothing in `vocab_8k/` is touched, so
removing your file here always reverts to the original.

- **Format**: 8000 Hz, mono, 16-bit signed PCM WAV.
- **Clip name**: the filename without `.wav`, case-insensitive — e.g.
  `sorry.wav` becomes clip name `SORRY`.
- **Using it**: use the clip name in a `voice` message in `usrp-rc.conf`,
  exactly like a stock clip:
  ```ini
  [messages]
  maintenance = voice: SORRY WE ARE DOWN FOR MAINTENANCE
  ```

This folder is loaded before `vocab_8k/`, so a same-named file here always
wins. See the main [README.md](../README.md) "Vocabulary" section for more.

On an installed system (via `make install`), this is
`/usr/local/share/usrp-rc/user_8k/` — data, not configuration, so it's not
under `/etc/`.

A clip can be at most 3 minutes long. A longer one is logged and skipped.

## Preparing recordings

`usrp-rc` applies no further equalization to your recordings; it only scales them by
`audio.voice_level` and `audio.master_gain`.

Don't normalize clips to full scale. A recording peaking near 0 dBFS will be noticeably
hotter than repeat audio, which sits well below full scale. MMDVM-Host's pre-emphasis also
boosts high frequencies on the way to the transmitter, and a full-scale recording leaves
no room for that. Leave generous headroom, start around -10 dBFS, and adjust by ear or on
a deviation meter.
