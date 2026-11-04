# Upgrading usrp-rc

What you need to change in `usrp-rc.conf` when moving to a new version, without
reading the whole README. Check your edits with `usrp-rc --check <file>` before
restarting. It reports anything out of date by line number.

## Unreleased (`dtmf-control` branch)

### Required

Rename this setting under `[events]`. usrp-rc will not start until you do:

| Old | New |
| --- | --- |
| `ct_message` | `ct_link_on_message` |

### Optional: new features

All of these are off, or behave as before, until you set them. See the README for
details on each.

| To get | Add |
| --- | --- |
| DTMF commands | `[dtmf]` `enabled = true` |
| A different courtesy tone while the link is off | `[events]` `ct_link_off_message` |
| A tail message | `[events]` `tail_message` |
| Settings that change by time and day | a `[schedule]` section |

With DTMF enabled, these commands work by default. Set a code empty to turn its
command off:

| Enter | Does | Setting |
| --- | --- | --- |
| `5` + digits | reads the digits back | `pad_test_prefix` |
| `39` | speaks the time | `time_code` |
| `72` | turns the link on | `link_on_code` |
| `73` | turns the link off | `link_off_code` |

### Behavior changes

- The courtesy tone after an impolite ID now follows the same rules as any other
  tail. It used to always be the local tone, even after link traffic.
