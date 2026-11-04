# usrp-rc User Manual

`usrp-rc` is a repeater controller for an MMDVM-Host FM repeater with one network
link. It handles IDs, courtesy tones, the time-out timer, messages, a schedule and
DTMF commands.

If you've run an ACC or Arcom controller, most of this will feel familiar, with one
big difference: **you program it by editing a text file, not by sending DTMF
sequences.** Stored speech and sound effects are ordinary WAV clips and text
definitions, so there's no DVR to record or vocabulary table to look up.

This manual covers the `dtmf-control` branch.

- **Part 1, [On the air](#part-1-on-the-air):** what a radio user needs. It can be
  printed and handed out on its own.
- **Part 2, [Setting up](#part-2-setting-up):** how the config file works, and a
  working example.
- **Part 3, [Features](#part-3-features):** messages, events, the schedule and usage
  figures.
- **Part 4, [Settings reference](#part-4-settings-reference):** every setting, with
  its default and allowed range.
- **Part 5, [Recipes and troubleshooting](#part-5-recipes-and-troubleshooting).**
- **Appendices:** [the tone catalog](#appendix-a-tone-catalog) and
  [the voice clips](#appendix-b-voice-clips).

---

# Part 1: On the air

## DTMF commands

DTMF has to be turned on with `enabled = true` under `[dtmf]`. These are the default
codes, and your repeater's owner may have changed or turned off any of them.

| Enter | Command | What you hear |
| --- | --- | --- |
| `39` | Time | "Good morning, the time is seven thirty A M" |
| `38` | Usage | "Count *n*, minutes *n*": key-ups and minutes of traffic over the last 24 hours |
| `72` | Link on | "Link on" |
| `73` | Link off | "Link off" |
| `5` + digits | Pad test | Your digits read back, to check your radio's tones |

Two keys have special jobs:

| Key | Job |
| --- | --- |
| `#` | Cancels what you've entered. You can carry on with a new command. |
| `D` | Runs the command now, without waiting for you to unkey. Useful when a carrier won't drop. |

## Entering a command

1. Key up.
2. Enter the digits.
3. Unkey.

The reply plays in the tail, ahead of the courtesy tone. If anyone keys up during the
reply, it stops.

While you enter digits:

- **You hear a beep on the repeater output** in place of your tones, about one beep a
  second. The start of your first digit may get through as a short blip.
- **Your transmission is cut off from the network** from the first digit until you
  unkey. Neither your digits nor anything you say afterward goes out over the link.
  Anything you said before the first digit has already gone.

A sequence is dropped if:

| What happened | Default limit |
| --- | --- |
| A single tone was held too long | 5 seconds |
| You paused too long between digits | 4 seconds; only the digits before the pause are lost |
| You stayed keyed too long after the last digit | 8 seconds |
| You entered too many digits | 32 |

If a sequence doesn't match any command, nothing happens: there's no error sound.
Commands aren't accepted while the repeater is timed out.

## Pad test

Enter `5` and then any digits, and the repeater reads them back. `*` is read as
"Sierra", and `A`, `B` and `C` as letters. `#` and `D` can't be read back because they
cancel and enter.

## Sounds you may hear

| Sound | Meaning |
| --- | --- |
| Courtesy tone | The last station has finished, and it's clear to transmit. On many repeaters, different tones show local traffic, network traffic and whether the link is on. |
| Beeping | Someone is entering a DTMF command. |
| Short falling tones | The time-out timer has cut off a long transmission. Unkey to reset it. |

---

# Part 2: Setting up

## The config file

The installed config is `/etc/usrp-rc/usrp-rc.conf`. `usrp-rc.conf.sample` in the repo
lists every option.

```ini
# A comment
callsign = W1XYZ

[section]
key = value   # a comment can go after a value
```

- Values don't take quotes.
- Sections can be in any order.
- **Leave a setting out to get its default.** Most sites need only a few lines.
- An empty value (`tail_messages =`) means "none" or "off".
- **Lists of choices use commas:** `mandatory_ids = id1,id2`.
- **Sequences use spaces:** `voice: THISIS {call} REPEATER`.

## Applying a change

Changes take effect when the service restarts, and always check them first:

```bash
sudo nano /etc/usrp-rc/usrp-rc.conf
cd / && /usr/local/bin/usrp-rc --check /etc/usrp-rc/usrp-rc.conf
sudo systemctl restart usrp-rc
journalctl -u usrp-rc -f
```

`usrp-rc` refuses to start if the config has errors. `--check` reports all of them at
once, by line number, and suggests a fix for many typos:

```text
config: line 12: timers.hnag: unknown setting (did you mean timers.hang?)
config: line 20: events.ct_link_on_message: no message named 'yelow_jacket' (did you mean 'yellow_jacket'?)
config: line 31: messages.my_voice: no voice clip named 'THSIS'
```

The `cd /` matters. `usrp-rc` looks for `tones.conf` and its clip folders in the current
directory before the installed copies, so checking from inside a source checkout tests
that checkout's files instead of the ones the service will use.

## A working example

This is a complete config for a linked repeater with DTMF on:

```ini
callsign = W1XYZ

[link]
remote_host = 127.0.0.1
remote_port = 41001

[dtmf]
enabled = true

[events]
initial_ids         = my_voice
mandatory_ids       = my_cw
ct_link_on_message  = yellow_jacket
ct_link_off_message = comet
ct_link_message     = bumble_bee
timeout_message     = timeout_warn

[messages]
my_voice = voice: THISIS {call} REPEATER
my_cw    = cw: {call}/R
```

For MMDVM-Host, set `[FM] LinkMode=1`. In `[FM Network]`, the ports are the mirror image
of `[mmdvm]` here: `LocalPort` matches `rpt_port` and `GatewayPort` matches `local_port`.
`usrp-rc` takes the place of `fmgateway`, so don't run both.

## What happens during a transmission

Most timers and events attach to one point in this cycle:

```text
 Carrier appears
   |  Repeating. Shorter than timers.kerchunk: treated as a kerchunk and ignored.
   |  Longer than timers.timeout: cut off (see below).
 Carrier drops
   |  timers.ct_delay
 Replies and anxious ID play, if any are waiting
   |
 Courtesy tone. The time-out timer resets here.
   |  timers.hang
 Tail message plays, if one is due
   |
 Transmitter drops
```

If someone keys up during the delay or hang, the repeater goes straight back to
repeating, and the time-out timer carries on from where it was.

---

# Part 3: Features

## Messages

A message is a named piece of audio, defined under `[messages]`. It's made of `voice:`,
`cw:` and `tone:` parts, joined with `|`:

```ini
[messages]
my_voice = voice: THISIS {call} REPEATER
my_cw    = cw: {call}/R
my_tone  = tone: 330:50 495:50 660:50
my_mix   = voice: THISIS {call} | tone: 880:100 | cw: {call}/R
```

### Voice

A list of clip names, played one after another. [Appendix B](#appendix-b-voice-clips)
lists the clips that come with `usrp-rc`.

| You write | You get |
| --- | --- |
| `_` | A pause of `audio.voice_gap_ms` (150 ms by default) |
| `_400` | A pause of 400 ms |
| `{call}` | Your callsign, spelled out: `W1XYZ` plays `W ONE X Y Z` |

**Adding your own clips:** put an 8 kHz, mono, 16-bit WAV file in
`/usr/local/share/usrp-rc/user_8k/`. The filename without `.wav` is the clip name. A clip
there replaces a stock clip with the same name.

`usrp-rc` applies no further equalization to your recordings; it only scales them by
`audio.voice_level` and `audio.master_gain`.

Don't normalize clips to full scale. A recording peaking near 0 dBFS will be noticeably
hotter than repeat audio, which sits well below full scale. MMDVM-Host's pre-emphasis also
boosts high frequencies on the way to the transmitter, and a full-scale recording leaves
no room for that. Leave generous headroom, start around -10 dBFS, and adjust by ear or on
a deviation meter.

### CW

The text to send, where spaces are real Morse word gaps. `{call}` sends your callsign,
and you add anything else yourself, as in `{call}/R`. `--check` reports any character
with no Morse code. Speed, pitch and level are under `[audio]`.

### Tones

Notes played left to right:

| You write | You get |
| --- | --- |
| `660:50` | 660 Hz for 50 ms |
| `500+700:100` | 500 Hz and 700 Hz together for 100 ms |
| `_30` | 30 ms of silence |
| `1200:60@0.9` | This note at level 0.9, instead of `audio.tone_level` |

[Appendix A](#appendix-a-tone-catalog) lists ready-made tones you can use by name. To
change one, define a message with the same name.

### Runtime variables

These are worked out each time the message plays. Each one has to be its own word in a
`voice:` part.

| Variable | Speaks |
| --- | --- |
| `{time}` | The time in 12-hour form, such as "seven thirty" |
| `{ampm}` | "A M" or "P M" |
| `{greeting}` | "morning" before 12:00, "afternoon" before 18:00, then "evening" |
| `{day}` | The day of the week |
| `{date}` | The month and day |
| `{keyups_day}` | Key-ups in the last 24 hours |
| `{keyups_week}` | Key-ups in the last 7 days |
| `{minutes_day}` | Minutes of traffic in the last 24 hours |
| `{minutes_week}` | Minutes of traffic in the last 7 days |

`{time}`, `{ampm}` and `{greeting}` use the female `F_` clips, and the others use the
standard voice. `{call}` is the only variable that also works in `cw:`.

### Limits

| Item | Limit |
| --- | --- |
| Messages in total | 64, of which 28 are catalog tones and 4 are built-in replies, leaving 32 unless you replace those |
| Parts in one message | 32, where each note counts as one and each `voice:` or `cw:` part as one |
| Words and pauses in one `voice:` part | 32 |
| Message and clip names | 31 characters |

### The clip check

Every time it starts, and on every `--check`, `usrp-rc` confirms that every clip each
message could need exists, including the built-in replies. Variables are checked against
everything they could ever say, so `{keyups_day}` needs `THOUSAND` even at 12 key-ups. If
a clip is missing, `usrp-rc` names it and won't start.

The check can't hear audio, so listen to your new messages on the air.

## Events

Events connect occasions to messages, under `[events]`. Set one to a message name, or
leave it empty to play nothing.

### IDs

| Setting | When it plays |
| --- | --- |
| `initial_ids` | At the end of the first transmission when no ID timer is running. Rotates through its list. |
| `mandatory_ids` | When the ID timer (`timers.id_interval`) runs out and the repeater isn't busy. Rotates through its list. |
| `anxious_id` | In a tail during the last `timers.id_anxious` seconds before the timer runs out, so the ID doesn't have to key up on its own later. |
| `impolite_id` | When the timer runs out while someone is talking. It's mixed over the conversation, with CW at `audio.impolite_morse_level`. If this is empty, the next `mandatory_ids` message is used. |

If nothing has been transmitted since the last ID, no new ID plays until someone uses the
repeater again.

If someone keys up during a voice ID, the ID stops and the impolite ID plays instead. A
CW ID carries on underneath.

### Courtesy tones

| Setting | Plays after |
| --- | --- |
| `ct_link_on_message` | Local traffic with the link on. Also used with the link off, or no link, if `ct_link_off_message` is empty. |
| `ct_link_off_message` | Local traffic with the link off |
| `ct_link_message` | Traffic from the link. If this is empty, the tones above are used. |

A courtesy tone can be any message, so a voice clip works too. Using a different tone for
link off shows the link's state to anyone listening.

### Time-out and other messages

| Setting | When it plays |
| --- | --- |
| `startup_message` | When `usrp-rc` starts |
| `timeout_message` | When the time-out timer cuts off a transmission |
| `timeout_cancel_message` | When that carrier finally drops and the repeater recovers |
| `tail_messages` | See [tail messages](#tail-messages) |
| `time_message` | Reply to the time command. Default `time_readback`. |
| `stats_message` | Reply to the usage command. Default `stats_readback`. |
| `link_on_message` | Reply to link on. Default `link_on_reply`. |
| `link_off_message` | Reply to link off. Default `link_off_reply`. |

An empty reply setting means the command still works but says nothing. To reword a reply,
define a message with the built-in name:

```ini
[messages]
stats_readback = voice: COUNT {keyups_week} _ MINUTES {minutes_week}
```

### Tail messages

Set one or more messages in `tail_messages`, with commas between them. Each one that
plays is the next in the list. It plays at the end of a hang, just before the transmitter
drops, and never keys the transmitter on its own.

| Setting | Behavior |
| --- | --- |
| `timers.tail_every = 10` | At least 10 tails since it last played. `1` means every tail, and `0` turns tail messages off. |
| `timers.tail_min_interval = 60` | At least 60 seconds since it last started. `0` means no limit. |

Each time a hang ends, the message plays if both are met. Otherwise that tail ends
normally. Nothing queues up, so it plays at most once per tail, however long it's been.
If someone keys up over it, that still counts as played.

For example, `tail_every = 1` with `tail_min_interval = 300` plays it at the end of a
hang whenever at least five minutes have passed since it last started.

`tail_every` is 10 by default, so a tail message plays without further setup.

## The schedule

The schedule changes `[events]` settings during a time window. When the window closes,
the settings go back on their own, so there's no second entry to write.

```ini
[messages]
net_tonight = voice: NET TONIGHT AT SEVEN THIRTY
net_ct      = cw: N

[schedule]
net_reminder = wed 07:00-19:25 tail_messages=net_tonight
net_running  = wed 19:30-20:30 ct_link_on_message=net_ct ct_link_off_message=net_ct
```

Each line is:

```text
name = DAYS HH:MM-HH:MM setting=value [setting=value ...]
```

The name is yours to choose and appears in the log.

| Part | Rule |
| --- | --- |
| Days | `sun` to `sat`, `daily`, `weekdays` or `weekends`. Combine with commas and no spaces: `mon,wed,fri`. |
| Times | 24-hour, on the system clock and timezone. A window includes its start minute but not its end minute. |
| Past midnight | A window that ends before it starts runs overnight and belongs to the day it started: `fri 22:00-02:00` is still open at 01:00 on Saturday. |
| Settings | Any `[events]` setting except `startup_message`. An empty value turns it off: `tail_messages=`. Lists take commas and no spaces: `mandatory_ids=id1,id2`. |
| Overlaps | The later line in the file wins. |
| Limits | 32 windows, with up to 8 settings each. A single window can't run longer than 24 hours. |

Windows take effect at the top of each minute. A restart in the middle of a window picks
it straight back up, because the schedule works from the clock and saves nothing.

**The clock is yours to keep right.** `usrp-rc` trusts the system clock and timezone.

**Testing a window:** add one that opens a couple of minutes from now and watch the log.
At `log.level = info`, you'll see these:

```text
schedule: 'net_reminder' started
tail: 'net_tonight'
schedule: 'net_reminder' ended
```

**Not yet possible:** `[timers]` and `[link]` settings can't be scheduled, so a window
can't turn the link on or off. A change also can't stay in place with no end time.

## The link

| Setting | Effect |
| --- | --- |
| `enabled = false` | This repeater has no link at all. `72` and `73` are ignored. |
| `start_on = false` | The link is off at startup. `72` turns it on. |

While the link is off, nothing is sent to it and anything received from it is ignored.
Link commands aren't saved: every restart goes back to `start_on`.

Anyone can use `72` and `73` for now. A control operator gate is planned.

## DTMF

Commands come from the repeater's receiver only. Tones from the link aren't decoded. See
[Part 1](#part-1-on-the-air) for how they're entered.

```ini
[dtmf]
enabled      = true
time_code    = 39
link_on_code = 1*72
stats_code   =
```

- Codes can use `0-9`, `*`, `A`, `B` and `C`, up to 7 characters. `#` and `D` are reserved
  for cancel and enter.
- An empty code turns that command off, as `stats_code` is above.
- A whole code wins over the pad test prefix, so `51` could be a command even with prefix `5`.
- DTMF isn't secure, since anyone listening can hear and repeat a code. Keep that in mind
  when choosing which commands to leave on.

**Command levels:** these follow the ACC model of user, control operator and programming
levels. Only user commands exist so far. The config file takes the place of programming
mode.

### Tuning

| If | Try |
| --- | --- |
| Voice sets off false digits | Raise `min_tone_ms` |
| Radios with short tones get missed | Lower `min_tone_ms` (the minimum is 30) |
| The first-tone blip bothers you | Set `audio.ste_delay_ms` to about 60 or more, which delays all repeated and link audio by that much |
| The beeping goes on too long | Lower `mute_hang_ms`, keeping it longer than `ste_delay_ms` and shorter than `interdigit_ms` |

At `log.level = info`, the log shows when a sequence starts and how it ended:

```text
dtmf: sequence started with '3'
dtmf: '38': ACCEPTED command = usage
```

The last line starts with one of three words:

| Word | Meaning | Example |
| --- | --- | --- |
| `ACCEPTED` | The command ran | `'5123': ACCEPTED command = DTMF test + 123` |
| `ERROR` | The entry finished but can't run | `'99': ERROR no command matched` |
| `ABANDONED` | The entry was cut short | `'12#': ABANDONED user terminated` |

An abandoned entry also says which timer ran out, if one did. `debug` also logs each digit
and tones that were too short.

## Usage figures

`usrp-rc` counts key-ups and minutes of traffic, local or from the link, over a rolling
week. Kerchunks and time spent timed out don't count. You can hear them with command `38`
or the `{keyups_*}` and `{minutes_*}` variables.

They're stored in `/var/local/usrp-rc/stats`, a small text file with one line for each
hour that had activity:

```text
# usrp-rc usage: hour-since-epoch keyups seconds
498279 3 2.4
```

- **Saving:** every 15 minutes if anything changed, and on a clean shutdown. A power
  failure loses at most that 15 minutes.
- **Size:** it holds one week at most, up to 168 lines, and never grows past that.
- **Damage:** a damaged file keeps the lines it can read.
- **If it can't be written:** counting still works until the next restart, and a warning
  is logged.
- **Other locations:** set `file` under `[stats]`, or leave it empty to keep the figures
  in memory only. The service can only write to `/var/local/usrp-rc`, so another location
  also needs `ReadWritePaths` in the service file.

---

# Part 4: Settings reference

These are the defaults you get by leaving a setting out. Times are in seconds under
`[timers]` and in milliseconds wherever the name ends in `_ms`. An out-of-range value is a
config error.

## Top level

These go above the first section.

| Setting | Default | Allowed | Meaning |
| --- | --- | --- | --- |
| `callsign` | none | Letters and digits | Used by `{call}`. Add `/R` or `REPEATER` in the message itself. |
| `access_mode` | `cor` | `cor`, `cor_ctcss` | Currently no difference: CTCSS decode is set in MMDVM-Host. |

## `[log]`

| Setting | Default | Allowed | Meaning |
| --- | --- | --- | --- |
| `level` | `info` | `error`, `warn`, `info`, `debug` | How much is logged |

## `[mmdvm]`

The connection to MMDVM-Host on the same machine.

| Setting | Default | Meaning |
| --- | --- | --- |
| `local_address` | `127.0.0.1` | Address `usrp-rc` listens on |
| `local_port` | `4810` | Port `usrp-rc` listens on. Equals `GatewayPort` in MMDVM-Host. |
| `rpt_address` | `127.0.0.1` | Address of MMDVM-Host |
| `rpt_port` | `3810` | Port of MMDVM-Host. Equals `LocalPort` in MMDVM-Host. |

## `[link]`

| Setting | Default | Allowed | Meaning |
| --- | --- | --- | --- |
| `enabled` | `true` | `true`, `false` | Whether this repeater has a link at all |
| `start_on` | `true` | `true`, `false` | Whether the link is on at startup |
| `remote_host` | none | | The USRP peer's address. Required when `enabled` is true. |
| `remote_port` | `34001` | | The peer's port |
| `local_port` | `34002` | | Port `usrp-rc` listens on |
| `bind_address` | `0.0.0.0` | | Address `usrp-rc` listens on |
| `codec` | `pcm` | `pcm`, `opus` | Link audio format |
| `opus_bitrate` | `8000` | 4000-64000 | Used when `codec = opus` |
| `opus_frame_ms` | `60` | 20, 40, 60 | Used when `codec = opus` |

## `[timers]`

| Setting | Default | Allowed | Meaning |
| --- | --- | --- | --- |
| `kerchunk` | `0.5` | 0-60 | A carrier shorter than this is ignored |
| `ct_delay` | `0.5` | 0-60 | From carrier loss to the courtesy tone |
| `hang` | `2.5` | 0-600 | How long the transmitter stays up after the courtesy tone |
| `timeout` | `180` | 1-3600 | Time-out timer. Resets at the courtesy tone. |
| `id_interval` | `600` | 1-86400 | ID timer. A warning is logged above the FCC's 10 minutes. |
| `id_anxious` | `60` | 0-3600 | Anxious ID window before the timer runs out. Must be less than `id_interval`. |
| `tail_every` | `10` | 0-1000 | Play `tail_messages` every this many tails; 0 is off |
| `tail_min_interval` | `0` | 0-86400 | Play `tail_messages` no more often than this many seconds; 0 is no limit |

## `[audio]`

| Setting | Default | Allowed | Meaning |
| --- | --- | --- | --- |
| `master_gain` | `1.0` | 0-10 | Final multiplier on all controller audio. Set it once for your modulator's headroom. |
| `morse_wpm` | `20` | 5-60 | CW speed |
| `morse_pitch` | `700` | 100-3400 | CW pitch, in Hz |
| `morse_level` | `0.9` | 0-2 | CW level |
| `impolite_morse_level` | `0.3` | 0-2 | CW level when IDing over someone talking |
| `voice_level` | `0.9` | 0-2 | Voice clip level |
| `tone_level` | `0.5` | 0-2 | Level for tones without their own `@LEVEL`, and for the DTMF beep |
| `voice_gap_ms` | `150` | 0-10000 | Length of a bare `_` pause |
| `ste_delay_ms` | `0` | 0-2000 | Delays repeated and link audio, for squelch tail elimination; 0 is off |
| `lead_trim_ms` | `0` | 0-2000 | Mutes this much at the start of each transmission; 0 is off |
| `pre_message_ms` | `0` | 0-10000 | Dead air after the transmitter keys, before CW or voice |
| `post_message_ms` | `0` | 0-10000 | Dead air after audio ends, before the transmitter drops |

Levels above 1.0 are allowed but can clip.

## `[dtmf]`

| Setting | Default | Allowed | Meaning |
| --- | --- | --- | --- |
| `enabled` | `false` | `true`, `false` | Turns DTMF commands on |
| `pad_test_prefix` | `5` | Up to 7 keys | Pad test prefix; empty is off |
| `time_code` | `39` | Up to 7 keys | Speak the time; empty is off |
| `stats_code` | `38` | Up to 7 keys | Speak usage; empty is off |
| `link_on_code` | `72` | Up to 7 keys | Link on; empty is off |
| `link_off_code` | `73` | Up to 7 keys | Link off; empty is off |
| `min_tone_ms` | `40` | 30-1000 | A tone must hold this long to count as a digit |
| `max_tone_ms` | `5000` | 100-60000 | A tone held longer discards the sequence |
| `interdigit_ms` | `4000` | 100-60000 | A longer gap discards the digits before it |
| `end_of_tx_ms` | `8000` | 100-600000 | A longer gap from the last digit to unkey discards the sequence |
| `mute_hang_ms` | `2000` | 0-60000 | Audio stays muted this long after the last tone. Must be less than `interdigit_ms`. |
| `beep_hz` | `440` | 100-3400 | Beep pitch during the mute |
| `beep_on_ms` | `100` | 0-10000 | Beep length |
| `beep_off_ms` | `900` | 0-10000 | Gap between beeps |

## `[stats]`

| Setting | Default | Meaning |
| --- | --- | --- |
| `file` | `/var/local/usrp-rc/stats` | Where usage figures are kept; empty is memory only |

## `[events]`, `[messages]` and `[schedule]`

See [Events](#events), [Messages](#messages) and [The schedule](#the-schedule). Up to 8
names can go in `initial_ids`, `mandatory_ids` or `tail_messages`.

---

# Part 5: Recipes and troubleshooting

## Recipes

Each of these is an addition to your own config. The names are examples.

**Rotate voice and CW IDs**

```ini
[events]
mandatory_ids = id_cw,id_voice

[messages]
id_cw    = cw: {call}/R
id_voice = voice: THISIS {call} REPEATER
```

**Show the link's state in the courtesy tone**

```ini
[events]
ct_link_on_message  = yellow_jacket
ct_link_off_message = comet
ct_link_message     = bumble_bee
```

**Announce the time in the tail on weekends**

```ini
[messages]
weekend_time = voice: F_GOOD {greeting} _ F_THE F_TIME F_IS {time} {ampm}

[schedule]
weekend_tail = weekends 08:00-20:00 tail_messages=weekend_time
```

**Say the week's usage instead of the day's on command `38`**

```ini
[messages]
stats_readback = voice: COUNT {keyups_week} _ MINUTES {minutes_week}
```

**No tail messages overnight**

```ini
[events]
tail_messages = my_tail

[messages]
my_tail = voice: WELCOME TO {call}

[schedule]
quiet_nights = daily 22:00-06:00 tail_messages=
```

**Change the courtesy tone and ID during a net**

```ini
[messages]
net_ct = cw: N
net_id = voice: THISIS {call} NET

[schedule]
net = wed 19:30-20:30 ct_link_on_message=net_ct ct_link_off_message=net_ct mandatory_ids=net_id
```

**Bring the link up by hand only**

```ini
[link]
remote_host = 127.0.0.1
start_on    = false
```

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| No response to DTMF | `[dtmf] enabled = true` is missing. The default is off. |
| No DTMF in the log | Look for `dtmf: sequence started` with `log.level = info`. If there's nothing, lower `min_tone_ms` and try `debug` to see short tones. |
| A sequence starts, but no reply | Read the `dtmf:` line that ends it, which says what matched or why it was abandoned. If a command matched, check whether its reply is set empty in `[events]`. |
| `72` or `73` does nothing | Look for `link is not configured`, which means `link.enabled = false`. |
| `usrp-rc` won't start | Run `--check` and work from the first error down. |
| `events.ct_message: renamed` | Change it to `ct_link_on_message`. |
| `needs commas between names` | Change `id1 id2` to `id1,id2`. |
| `a variable needs the voice clip` | That clip is missing. Run `sudo make install` to restore stock clips. |
| A tail message never plays | Look for the warning about `tail_every` being 0. Otherwise, a quiet repeater may not have reached the `tail_every` count, or `tail_min_interval` may be holding it back. |
| A window opens at the wrong time | Check the Pi's clock and timezone with `timedatectl`. |
| Usage figures reset on restart | Look for `stats: can't save` in the log. Check that `/var/local/usrp-rc` exists and the service file is current, then run `sudo systemctl daemon-reload`. |
| Audio from the link is missing | Check whether the link was turned off with `73`, or whether `start_on = false` is set. |

## Watching it work

```bash
journalctl -u usrp-rc -f
```

At `log.level = info`, the log shows state changes, IDs, tail messages, schedule windows
and DTMF digits. Every message that plays is named, so it's the quickest way to find out
why something did or didn't happen.

---

# Appendix A: Tone catalog

Any of these can be used by name, as in `ct_link_on_message = yellow_jacket`. They live in
`tones.conf`, which is replaced on upgrade, so don't edit it. To change one, define a
message with the same name in your config.

**One note or chord**

| Name | Notes | Sounds like |
| --- | --- | --- |
| `honk` | `500+700:100` | Short, low two-note horn |
| `beep` | `880:100` | Plain high beep |
| `boop` | `440:100` | Plain low beep |
| `bloop` | `840+500:100` | Short two-note blend |
| `piano_chord` | `660+880:100` | Short, bright chord |
| `rolm` | `525+660:150` | Longer, soft chord |
| `quindar_on` | `2525:250` | Apollo-style high tone, start of a transmission |
| `quindar_off` | `2475:250` | Apollo-style high tone, end of a transmission |

**Rising**

| Name | Notes | Sounds like |
| --- | --- | --- |
| `yellow_jacket` | `330:50 495:50 660:50` | Three quick rising notes |
| `bumble_bee` | `330:100 495:100 660:100` | Same notes as yellow_jacket, twice as slow |
| `ascending` | `500:50 750:50 1000:50` | Three quick rising notes, higher |
| `comet` | `500:100 500:100 750:100` | Two low notes, then a higher one |
| `stardust` | `750:125 880:80 880+1200:80` | Rising, ending on a bright chord |

**Falling**

| Name | Notes | Sounds like |
| --- | --- | --- |
| `descending` | `1000:50 750:50 500:50` | Three quick falling notes |
| `hornet` | `660:50 500:50 385:50` | Three quick falling notes, lower |
| `wasp` | `660:100 500:100 385:100` | Same notes as hornet, twice as slow |
| `tumbleweed` | `1000:20 800:20 600:20` | Very fast falling chirp |
| `shooting_star` | `800:100 800:100 540:100` | Two notes, then a lower one |
| `doorbell` | `800:75 400:50` | Ding-dong |
| `doorbell_chord` | `1450+725:75 725+360:50` | Ding-dong in octave chords |

**Up and down**

| Name | Notes | Sounds like |
| --- | --- | --- |
| `fire_fly` | `1000+1200:120 1200+1400:80 600+800:100` | Chord rises, then drops low |
| `moonbounce` | `1000+800:50 800:50 600:50 1500:50` | Falls, then jumps high |
| `chirp_chomp` | `1500:20 1250:20 1000:20 750:20 500:20 2550:20` | Very fast fall, then a high blip |
| `dunce_cap` | `440+500:200 440+350:200` | Two long, low chords, falling |

**Repeated, with gaps**

| Name | Notes | Sounds like |
| --- | --- | --- |
| `nextel_beep` | `1760:30 _30 1760:30 _30 1760:30` | Three quick high chirps |
| `function_complete` | `480+1200:40 _40 480+1200:40` | Two quick two-note chirps |
| `function_complete_2` | `900:60 _60 900:60` | Two quick beeps |

**Warnings**

| Name | Notes | Sounds like |
| --- | --- | --- |
| `timeout_warn` | `1200:60@0.9 _20 1000:60@0.9 _20 800:60@0.9` | Loud, three falling notes |

For a tone spec that calls for a 640 ms delay from squelch closure to the first tone,
set `timers.ct_delay = 0.64`.

# Appendix B: Voice clips

These 623 clips come with `usrp-rc`, in `/usr/local/share/usrp-rc/vocab_8k/`. Clip
names aren't case-sensitive: `repeater` and `REPEATER` are the same clip.

- **`F_` clips** are the female voice used by the time variables.
- **Clips starting with `_`** are word endings, such as `_S` and `_TEEN`. They're treated as
  clips, not pauses, because a pause is `_` followed only by digits.

**Standard voice**

```text
A ABEAM ABORT ABOUT ABOVE ACCELERATED ACKNOWLEDGE ACTION ADJUST ADVANCED
ADVISE AERIAL AFFIRMATIVE AIR AIRCRAFT AIRPORT AIRSPEED AIR_BRAKES ALERT ALL
ALOFT ALPHA ALTERNATE ALTIMETER ALTITUDE AMATEUR AMPS AND_A AND_B ANSWER
APPROACH APPROACHES APRIL AREA ARRIVAL AS AT ATIS AUGUST AUTO AUTOMATIC
AUTOPILOT AUXILLIARY A_M B BAND BANK BARKER BASE BATTERY BELOW BETWEEN BLOWING
BOARD BOOST BRAKE BRAVO BREAK BREAKING BROKEN BUTTON BY C CABIN CALIBRATE CALL
CALLING CALM CANCEL CAUTION CEILING CELSIUS CENTER CHANGE CHARLIE CHECK
CIRCUIT CLEAR CLEARANCE CLEARANCE_DELIVERY CLIMB CLOCK CLOSED CLUB CODE COME
COMPLETE COMPUTER CONDITION CONGRATULATIONS CONNECT CONTACT CONTROL CONVERGING
COUNT COURSE CRANE CROSSWIND CRYSTALS CURRENT CYCLE CYLINDER D DANGER DAYS
DAYTON DECEMBER DECREASE DECREASING DEGREE DEGREES DELTA DEPARTURE DEVICE DIAL
DINNER DIRECTION DISPLAY DIVIDED DOOR DOORS DOWN DOWNWIND DRIVE DRIZZLE DUST E
EAST ECHO EIGHT EIGHTEEN EIGHTY ELECTRICIAN ELEVATION ELEVEN EMERGENCY ENGINE
ENTER EQUAL EQUALS ERROR ESTIMATED ETA EVACUATE EVACUATION EXIT EXPECT F FAIL
FAILURE FARAD FARENHEIT FAST FEBRUARY FEET FIELD FIFTEEN FIFTY FIF_ FILED
FINAL FIRE FIRST FIVE FLAME_OUT FLAPS FLIGHT FLIGHTWATCH FLOW FOG FOR FORTY
FOUR FOURTEEN FOURTH FOXTROT FREEDOM FREEZING FREQUENCY FRIDAY FROM FRONT FUEL
FULL G GALLEY GALLONS GATE GAUGE GEAR GET GLIDE GO GOLF GRAIN GREAT GREEN
GREENWICH GROUND GUNDISH GUSTING_TO H HAIL HALF HAM HAMFEST HAMVENTION HAVE
HAZARDOUS HAZE HEADING HEAT HEAVY HELP HENRY HERTZ HIGH HOLD HOME HOTEL HOUR
HOURS HUNDRED I ICE ICING IDENTIFY IDLE IFR IGNITE IGNITION ILS IMMEDIATELY IN
INBOUND INCH INCREASE INCREASING INCREASINGTO INDIA INDICATED INFLIGHT
INFORMATION INNER INSPECTOR INSTRUMENTS INTRUDER IS IT J JANUARY JULIET JULY
JUNE K KEY KILO KNOTS L LAND LANDING LANDING_GEAR LATE LAUNCH LEAN LEFT LEG
LESS_THAN LEVEL LEVEL_OFF LIGHT LIGHTS LIMA LINE LINK LIST LOCALIZER LOCK LONG
LOOK LOW LOWER LUNCH M MACHINE MAGNETOS MAINTAIN MANUAL MARCH MARKER MAY
MAYDAY ME MEAN MEASURE MEASURED MEETING MEGA MESSAGES METER MICRO MIDDLE
MIDPOINT MIG MIKE MILES MILL MILLI MILLION MINUS MINUTES MIST MIXTURE MOBILE
MODERATE MONDAY MONTH MORE_THAN MOTOR MOVE MOVING MUCH N NEAR NEGATIVE NET NEW
NEXT NIGHT NINE NINER NINETEEN NINETY NO NORTH NORTHEAST NORTHWEST NOT NOTAM
NOVEMBER NO_TURN NUMBER O OBSCURED OCLOCK OCTOBER OF OFF OHIO OHMS OIL ON ONE
OPEN OPERATION OPERATOR OSCAR OTHER OUT OUTER OVER OVERCAST OVERSPEED P PAPA
PARTIALLY PASS PAST PATCH PATH PAUSE_ PELLETS PER PERCENT PHONE PICO PLAN
PLEASE PLUS POINT POLICE POSITION POWER PRACTICE PRESS PRESSURE PRIVATE PROBE
PROGRAMMING PROPS PULL PUMPS PUSH P_M Q QUEBEC R RADAR RADIAL RADIO RADIOS
RAIN RAISE RANGE RATE READY REAR RECEIVE RED REFUELLING RELEASE REMARK REMOTE
REPAIR REPEAT REPEATER REPEAT_ER RICH RIG RIGHT ROAD ROGER ROLL_OUT ROMEO
ROUTE R_NAV S SAFE SAND SANTACLARA SATURDAY SCATTERED SECOND SECONDS SECURITY
SELCAL SELECT SEPTEMBER SEQUENCE SERVICE SET SEVEN SEVENTEEN SEVENTY SEVERE
SEXY SHORT SHOWERS SHUT SIDE SIERRA SIGHT SIGMET SIX SIXTEEN SIXTY SLEET SLOPE
SLOW SMOKE SNOW SOUTH SOUTHEAST SOUTHWEST SPEED SPOILERS SPRAY SQUAWK
SQUAWKING STABILIZER STALL START STOP STORM STRAY SUNDAY SWITCH SYSTEM T TACAN
TANGO TANK TARGET TAXI TELEPHONE TEMPERATURE TEN TERMINAL TEST THANKYOU THAT
THEE THE_A THE_B THIN THINLY THIRD THIRTEEN THIRTY THIR_ THISIS THOUSAND THREE
THUNDERSTORM THURSDAY TIME TIMER TIMES TO TODAY TOMORROW TONIGHT TOOL TOO_LOW
TORNADO TOUCHDOWN TOWER TRAFFIC TRANSMIT TRIM TRUE TUESDAY TURBULENCE TURN
TWELVE TWENTY TWO U UNDER UNDERCARRIAGE UNICOM UNIFORM UNIT UNLIMITED UNTIL UP
USE_S USE_Z V VACUUM VALLEY VALVE VARIABLE VECTORS VERIFY VFR VHE VICTOR
VISIBILITY VOLTS VOR VORTAC W WAIT WAKE WARNING WATCH WATTS WAY WEATHER
WEDNESDAY WELCOME WEST WHISKEY WHITE WILL WIND WINDOWS WITH WRONG X XRAY Y
YANKEE YELLOW YESTERDAY YOU YOUR Z ZED ZERO ZONE ZULU _ED _ER _S _TEEN _TH _TY
```

**Female voice**

```text
F_AFTERNOON F_A_M F_EIGHT F_EIGHTEEN F_ELEVEN F_EVENING F_FIFTEEN F_FIFTY
F_FIVE F_FORTY F_FOUR F_FOURTEEN F_GOOD F_IS F_MORNING F_NINE F_NINETEEN
F_OCLOCK F_OH F_ONE F_P_M F_SEVEN F_SEVENTEEN F_SIX F_SIXTEEN F_TEN F_THE
F_THIRTEEN F_THIRTY F_THREE F_TIME F_TWELVE F_TWENTY F_TWO
```
