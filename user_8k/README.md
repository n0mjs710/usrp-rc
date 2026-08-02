# user_8k/

Drop your own voice clips here to add new ones or override a stock
`vocab_8k/` clip of the same name — nothing in `vocab_8k/` is touched, so
removing your file here always reverts to the original.

- **Format**: 8000 Hz, mono, 16-bit signed PCM WAV.
- **Clip name**: the filename without `.wav`, case-insensitive — e.g.
  `sorry.wav` becomes clip name `SORRY`.
- **Using it**: reference the clip name from a `voice` element's `clip`
  list in `usrp-rc.toml`, exactly like a stock clip:
  ```toml
  {type = "voice", clip = "SORRY WE ARE DOWN FOR MAINTENANCE"}
  ```

This folder is loaded before `vocab_8k/`, so a same-named file here always
wins. See the main [README.md](../README.md) "Vocabulary" section for more.

On an installed system (via `make install`), this is
`/usr/local/share/usrp-rc/user_8k/` — data, not configuration, so it's not
under `/etc/`.
