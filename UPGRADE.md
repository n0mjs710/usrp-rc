# Upgrading usrp-rc

This document lists the configuration changes necessary for each new version. After
each change, examine the configuration with `usrp-rc --check <file>`. `--check` shows
each out-of-date setting by line number.

## 1.0 (November 2026)

These changes apply when you update from 1.0b.

### Necessary changes

1. Remove `access_mode` from the top of the configuration file, if it is there. This
   setting had no effect and no longer exists. It causes an error.
2. If `[link]` `remote_host` or `bind_address`, or an address in `[mmdvm]`, is a host
   name, change it to an IPv4 address. `usrp-rc` accepts only IPv4 addresses. `--check`
   now shows this error.
3. If `[link]` `codec = opus`, one frame must be 256 bytes or less:
   `opus_bitrate` × `opus_frame_ms` / 8000. `--check` shows a setting that is too high.

`libsystemd-dev` is no longer necessary to build `usrp-rc`.

### Changes in operation

- The ID timer starts when an ID starts, not when it ends. The time between the
  starts of two IDs is now `id_interval`.
- `pre_message_ms` and `post_message_ms` now apply each time the controller keys the
  transmitter for its own audio and drops it after: also for repeater up and down
  replies and time-out replies. They no longer add a pause before an ID in a tail.
- "Repeater up" when the repeater is already up is an ordinary command reply. It no
  longer restarts the time-out timer.
- DTMF entry no longer takes the link down for the rest of the transmission. The link
  stays keyed and gets the cover tone (the beep) during the mute, the same as the
  repeater. The new `[dtmf]` settings `local_cover_tone` and `link_cover_tone` replace
  the cover tone with silence on either path.
- Digits that cannot be the start of a command are discarded at once. They still
  mute for `mute_hang_ms`, but a false digit in speech no longer holds an entry open
  until the unkey.

## 1.0b (November 2026)

Version 1.0b is the first numbered version. It adds DTMF control, the schedule and new
configuration rules. These changes apply when you update from a `main` before 1.0b. To
see the installed version, enter `usrp-rc --version`.

### Necessary changes

1. Install the service file again, and reload systemd. The usage statistics need a
   state directory with write access.

   ```bash
   sudo make install
   sudo systemctl daemon-reload
   ```

2. In `[events]`, change this setting name. `usrp-rc` does not start with the old name.

   | Old | New |
   | --- | --- |
   | `ct_message` | `ct_link_on_message` |

3. In `[events]`, set `impolite_id`. This setting is now necessary. It is the last ID
   that the repeater uses if `mandatory_ids` is empty.
4. In `[audio]`, remove `voice_filter`, if it is in the file. This setting no longer
   exists. It causes an error.
5. In `initial_ids` and `mandatory_ids`, use commas between names:
   `mandatory_ids = id1,id2`. Spaces between names cause an error.
6. In `[events]`, set each courtesy tone that the repeater must play. An empty
   courtesy tone now plays no tone. It does not use `ct_link_on_message`. A repeater
   with `[link]` `enabled = false` now uses `ct_link_off_message`. To keep one tone
   for all traffic, set the same message in all three:

   ```ini
   ct_link_off_message = yellow_jacket
   ct_link_on_message  = yellow_jacket
   ct_link_message     = yellow_jacket
   ```

7. If the repeater plays tail messages, set `[timers]` `tail_every`. The default is now
   0, which disables tail messages. The old default was 10.
8. If DTMF commands are enabled, examine the codes. Each code must now be 2 to 7 keys.
   Only `pad_test_prefix` can be 1 key. No code can start with `pad_test_prefix` or
   `regen_code`, because these two take each key after them, also `#` and `D`, until
   the unkey. With the default prefix `5`, change each code that starts with `5`.
   `--check` shows each code to change.

### New functions

These functions are off, or operate as before, until they are set. For each function,
see [MANUAL.md](MANUAL.md).

| Function | Setting |
| --- | --- |
| DTMF commands | `[dtmf]` `enabled = true` |
| DTMF regen: send DTMF over the air and over the link | `[dtmf]` `regen_code` |
| Link off at start | `[link]` `start_on = false` |
| Different courtesy tone when the link is off | `[events]` `ct_link_off_message` |
| Reply to link commands when the repeater has no link | `[events]` `link_none_message` |
| Tail messages, which rotate through a list | `[events]` `tail_messages` |
| Minimum time between tail messages | `[timers]` `tail_min_interval` |
| Separate time-out for link traffic | `[timers]` `link_timeout` |
| Repeater down and up by DTMF (no code by default) | `[dtmf]` `repeater_code` |
| Settings that change by time and day | `[schedule]` section |
| No courtesy tone, or no tail at all | `[events]` `ct_*_message` empty; for no tail, also `[timers]` `ct_delay = 0`, `hang = 0` |
| Different location for usage statistics, or memory only | `[stats]` `file` |

When DTMF is enabled, these commands operate by default. An empty code disables its
command.

| Code | Command | Setting |
| --- | --- | --- |
| `5` + digits | Says the digits | `pad_test_prefix` |
| `39` | Says the time | `time_code` |
| `38` | Says the usage for the last 24 hours | `stats_code` |
| `72` | Turns the link on | `link_on_code` |
| `73` | Turns the link off | `link_off_code` |
| `70` | Says the link state, without a change | `link_status_code` |
| Not set | Repeater down (code + 0) and up (code + 1) | `repeater_code` |

### Changes in operation

- A schedule window can now change any setting, except the settings that only take
  effect at startup. Before, a window could change `[events]` settings only.
  `usrp-rc --check` examines each window with the settings that it changes.
- With no courtesy tone set, the local time-out timer still resets at the end of
  `ct_delay`, the same point as a tone. For a short tail with no tone, set `ct_delay` to
  the tail length and `hang = 0`.
- If `initial_ids` is empty, the first key-up plays no ID, as before. But when the ID timer
  expires, the repeater now sends an ID, also if no other transmission occurred.
- The 64-message limit is removed. `usrp-rc` examines all messages. It then removes from
  memory the messages that no event or schedule window uses.
- The maximum length of a message or a clip is 3 minutes. A longer message prevents the
  start. `usrp-rc` does not load a longer clip, and records this in the log. This prevents
  the start only if a message uses the clip.
- `make install` now replaces all stock voice clips in `vocab_8k/`, and removes stock
  clips that are no longer in the repository. Before, it did not replace clips that
  existed. Move any changed stock clips to `user_8k/` before the installation.
- Local and link traffic have separate time-out timers. `timeout` now applies to local
  traffic only. `link_timeout` (default 180) applies to link traffic and resets each time
  the link unkeys. Both accept 0 for off.
- A time-out stops only the source that timed out. A stuck link no longer locks out local
  stations. During a local time-out, link traffic is repeated, and commands that end with
  `D` are accepted.
- The courtesy tone is for the source that was active last. Before, it was for the source
  that keyed up last.
- The courtesy tone after an impolite ID now follows the same rules as other tails.
  Before, it was always the local tone, also after link traffic.
