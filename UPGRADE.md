# Upgrading usrp-rc

What you need to change in `usrp-rc.conf` when moving to a new version, without
reading the whole README. Check your edits with `usrp-rc --check <file>` before
restarting. It reports anything out of date by line number.

## Unreleased (`dtmf-control` branch)

### Required

Reinstall the service file and reload systemd, as usage statistics need a writable
state directory:

```bash
sudo make install
sudo systemctl daemon-reload
```

Rename this setting under `[events]`. usrp-rc will not start until you do:

| Old | New |
| --- | --- |
| `ct_message` | `ct_link_on_message` |

Set `impolite_id` under `[events]`. It's now required, because it's the ID of last resort if
`mandatory_ids` is empty.

An empty `initial_ids` still plays nothing on the first key-up, but the repeater now
identifies when the ID timer runs out, even if nothing else was transmitted.

Delete `voice_filter` from `[audio]` if you have it. The setting is gone, and it's an
error if left in.

If `initial_ids` or `mandatory_ids` lists more than one message, separate the names with
commas (`mandatory_ids = id1,id2`). Spaces are now an error.

### Optional: new features

All of these are off, or behave as before, until you set them. See the README for
details on each.

| To get | Add |
| --- | --- |
| DTMF commands | `[dtmf]` `enabled = true` |
| The link off at startup | `[link]` `start_on = false` |
| A different courtesy tone while the link is off | `[events]` `ct_link_off_message` |
| Tail messages, rotating through a list | `[events]` `tail_messages` |
| A limit on how often it plays | `[timers]` `tail_min_interval` |
| Settings that change by time and day | a `[schedule]` section |
| Usage figures in a different place, or not on disk | `[stats]` `file` |

With DTMF enabled, these commands work by default. Set a code empty to turn its
command off:

| Enter | Does | Setting |
| --- | --- | --- |
| `5` + digits | reads the digits back | `pad_test_prefix` |
| `39` | speaks the time | `time_code` |
| `38` | speaks usage over the last 24 hours | `stats_code` |
| `72` | turns the link on | `link_on_code` |
| `73` | turns the link off | `link_off_code` |

### Behavior changes

- The courtesy tone after an impolite ID now follows the same rules as any other
  tail. It used to always be the local tone, even after link traffic.
