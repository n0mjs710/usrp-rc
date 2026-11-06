# usrp-rc Operation and Reference Manual

This manual applies to `usrp-rc` version 1.0b. To see the installed version, enter
`usrp-rc --version`. For installation and MMDVM-Host integration, see
[README.md](README.md).

> [!WARNING]
> **MMDVM-Host FM audio quality.** At the date of this document (November 2026),
> MMDVM-Host has defects that decrease the audio quality of FM mode. The symptoms are
> crackles, distortion and an incorrect frequency response (EQ). The defects are worst
> on the network and with `[FM] LinkMode=1`, because these two functions use the same
> code path. For the best performance, use the patched versions of MMDVM and MMDVM-Host
> at [github.com/n0mjs710](https://github.com/n0mjs710).

## Contents

1. [Introduction](#1-introduction)
2. [Over-the-air operation](#2-over-the-air-operation)
3. [Configuration file](#3-configuration-file)
4. [Transmission cycle](#4-transmission-cycle)
5. [Messages](#5-messages)
6. [Events](#6-events)
7. [Schedule](#7-schedule)
8. [Link](#8-link)
9. [DTMF control](#9-dtmf-control)
10. [Usage statistics](#10-usage-statistics)
11. [Settings reference](#11-settings-reference)
12. [Configuration examples](#12-configuration-examples)
13. [Log and troubleshooting](#13-log-and-troubleshooting)

- [Appendix A: Tone catalog](#appendix-a-tone-catalog)
- [Appendix B: Voice clips](#appendix-b-voice-clips)

---

## 1. Introduction

### 1.1 Scope

`usrp-rc` is a repeater controller for an MMDVM-Host FM repeater with one network link.
It controls IDs, courtesy tones, the time-out timer, messages, a schedule and DTMF
commands.

The operator programs `usrp-rc` with a text configuration file. DTMF programming is not
used. Speech and sound effects are WAV clips and text definitions.

### 1.2 Structure of this manual

| Section | Contents | Reader |
| --- | --- | --- |
| 2 | DTMF commands and repeater sounds | Radio users. This section can be printed separately. |
| 3 to 10 | Functions and their configuration | Repeater operators |
| 11 | All settings, with defaults and ranges | Repeater operators |
| 12 and 13 | Examples, log messages and fault isolation | Repeater operators |

### 1.3 Terms

| Term | Meaning |
| --- | --- |
| COR | Carrier operated relay. The signal that a station transmits. `usrp-rc` gets it from MMDVM-Host and from the link. |
| Key-up | The start of a transmission into the repeater |
| Kerchunk | A key-up shorter than `timers.kerchunk` |
| Courtesy tone | The sound after a transmission. It shows that the next station can transmit. |
| Hang | The time that the transmitter stays on after the courtesy tone |
| Tail | The period from the end of a transmission to the end of the hang |
| Message | A named sequence of voice, CW and tone audio |
| Event | A setting that names the message for one occasion |
| Window | A schedule line that changes settings during a time period |

### 1.4 Units

- Settings in `[timers]` are in seconds.
- Settings with names that end in `_ms` are in milliseconds.
- Levels are multipliers. 1.0 is full scale.

---

## 2. Over-the-air operation

### 2.1 DTMF commands

These are the default codes. The repeater operator can change or disable each code. DTMF
commands operate only if the operator sets `[dtmf] enabled = true`.

| Code | Command | Reply |
| --- | --- | --- |
| `39` | Time | "Good morning, the time is seven thirty A M" |
| `38` | Usage | "*n* key ups, *n* minutes": key-ups and minutes of traffic in the last 24 hours |
| `72` | Link on | "Link on" |
| `73` | Link off | "Link off" |
| `70` | Link status | "Link on" or "Link off". The link state does not change. |
| `5` + keys | Pad test | The keys. Use this command to test the DTMF tones of a radio. |
| Code + keys | DTMF regen | The keys, as new DTMF tones. No code by default. See [2.6](#26-dtmf-regen). |

### 2.2 Special keys

| Key | Function |
| --- | --- |
| `#` | Cancels the digits entered. A new command can then be entered. |
| `D` | Executes the command immediately. The repeater does not wait for the unkey. Use this key if a carrier does not drop. |

After the pad test prefix or the regen code, `#` and `D` are keys like the others. They
do not cancel or execute. Only the unkey ends the entry. To stop the entry, pause for
more than 4 seconds. The repeater discards the keys before the pause, and a new command
can then be entered. Or stay keyed for more than 8 seconds after the last key, then
unkey.

### 2.3 Command entry

Enter one command in each transmission.

1. Key the radio.
2. Enter the digits.
3. Unkey the radio.

The reply plays after the unkey, before the courtesy tone. A key-up stops the reply.

During digit entry:

- The repeater transmits a beep in place of the DTMF tones. The beep occurs
  approximately one time each second.
- The start of the first digit can go out as a short blip.
- The repeater does not send the transmission to the link. This starts at the first digit
  and continues until the unkey. Audio before the first digit has already gone to the link.

### 2.4 Discarded entries

The repeater discards an entry for these conditions:

| Condition | Default limit |
| --- | --- |
| One tone is too long | 5 seconds |
| The pause between two digits is too long | 4 seconds. Only the digits before the pause are discarded. |
| The transmission continues too long after the last digit | 8 seconds |
| The entry has too many digits | 32 |

If the repeater has no link, `70`, `72` and `73` reply "No link service".

An entry that does not agree with a command has no effect. No error sound plays.

When the repeater has timed out a local transmission, it accepts a command only if the
command ends with `D`. An unkey does not enter the command. See [6.3](#63-time-out).
Thus, the pad test and DTMF regen are not available then.

### 2.5 Pad test

Enter `5` and then the keys to test, then unkey. The repeater says the keys.

| Key | Reply |
| --- | --- |
| `0` to `9` | The digit |
| `*` | "Sierra" |
| `#` | "P" |
| `A`, `B`, `C`, `D` | The letter |

### 2.6 DTMF regen

Enter the regen code and then the keys to send, then unkey. The repeater transmits the
keys as new, clean DTMF tones. All 16 keys are available, also `#` and `D`.

| Link | The tones go to |
| --- | --- |
| On | The repeater transmitter and the link |
| Off, or no link | The repeater transmitter only |

Use this command to send DTMF commands to the system at the other end of the link, for
example an AllStarLink node. The repeater does not send the tones that the station
enters. It sends only the new tones.

- The repeater sends only the sequence of keys. Each tone has the same length and the
  same gap, as set by the operator. The length of the entered tones has no effect.
- On the repeater, the tones play after the unkey, before the courtesy tone, as a
  command reply.
- A local transmission that starts while the tones go to the link is not sent to the
  link.

### 2.7 Repeater sounds

| Sound | Meaning |
| --- | --- |
| Courtesy tone | The transmission has ended. The next station can transmit. Different tones can show local traffic, link traffic and link off. |
| Beep, one time each second | A station enters a DTMF command. |
| Short falling tones | The time-out timer stopped a long transmission. Unkey to reset the timer. |
| "Repeater" replies with no repeated audio | The repeater is timed out. A command that ends with `D` gets a reply. |

---

## 3. Configuration file

### 3.1 Location

| File | Function |
| --- | --- |
| `/etc/usrp-rc/usrp-rc.conf` | Configuration |
| `/etc/usrp-rc/usrp-rc.conf.sample` | All settings, with defaults and comments |

### 3.2 Format

```ini
# A comment
callsign = W1XYZ

[section]
key = value   # a comment after a value
```

| Rule | Example |
| --- | --- |
| Values have no quotation marks. | `callsign = W1XYZ` |
| Sections can be in any sequence. | |
| A setting that is not in the file has its default value. | |
| An empty value means "none" or "off". | `tail_messages =` |
| Commas separate a list of choices. | `mandatory_ids = id1,id2` |
| Spaces separate the items of a sequence. | `voice: THISIS {call} REPEATER` |
| Settings before the first section are top-level settings. | `callsign`, `access_mode` |

### 3.3 Apply a change

1. Edit the configuration:

   ```bash
   sudo nano /etc/usrp-rc/usrp-rc.conf
   ```

2. Examine the configuration for errors:

   ```bash
   cd / && /usr/local/bin/usrp-rc --check /etc/usrp-rc/usrp-rc.conf
   ```

3. Correct all errors. Then do step 2 again.
4. Restart the service:

   ```bash
   sudo systemctl restart usrp-rc
   ```

5. Monitor the log:

   ```bash
   journalctl -u usrp-rc -f
   ```

> **Note:** `usrp-rc` looks for `tones.conf`, `user_8k/` and `vocab_8k/` in the current
> directory first. The service operates from `/`. Thus, do the check from `/`. A check from
> a source directory examines the files in that directory, not the installed files.

> **Note:** A change has effect only after a restart.

### 3.4 Configuration errors

`usrp-rc` does not start if the configuration has errors. `--check` shows all errors
together, with line numbers. For many typing errors, it also shows the probable correct
value:

```text
config: line 12: timers.hnag: unknown setting (did you mean timers.hang?)
config: line 20: events.impolite_id: no message named 'idd' (did you mean 'id'?)
config: line 31: messages.my_voice: no voice clip named 'THSIS'
```

### 3.5 Example

This configuration is complete for a linked repeater with DTMF commands:

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
impolite_id         = my_cw
ct_link_on_message  = yellow_jacket
ct_link_off_message = comet
ct_link_message     = bumble_bee
timeout_message     = timeout_warn

[messages]
my_voice = voice: THISIS {call} REPEATER
my_cw    = cw: {call}/R
```

---

## 4. Transmission cycle

Most timers and events start at one point in this cycle:

```text
 Carrier starts
   |  The repeater repeats the audio.
   |  Shorter than timers.kerchunk: kerchunk. No courtesy tone, no tail.
   |  Longer than timers.timeout (local) or timers.link_timeout (link):
   |  time-out (see 6.3).
 Carrier stops
   |  timers.ct_delay
 Replies and the anxious ID play, if they are pending.
   |
 Courtesy tone, if one is set. The local time-out timer resets here,
 also with no tone.
   |  timers.hang
 Tail message plays, if it is due.
   |
 Transmitter stops
```

A local key-up during the delay or the hang starts repetition again. The local time-out
timer then continues from its last value. A link key-up always starts the link time-out
timer from zero.

With no courtesy tone, the local time-out timer still resets at the end of `ct_delay`.
For a short tail with no tone, use `ct_delay` as the hang: set `ct_delay` to the tail
length and `hang` to 0. The transmitter stays on for `ct_delay`, then stops. See
[12.8](#128-no-courtesy-tone-short-tail).

| Timer | Default | Function |
| --- | --- | --- |
| `kerchunk` | 0.5 s | Minimum carrier time for a full transmission |
| `ct_delay` | 0.5 s | Time from carrier stop to the courtesy tone. 0 is no delay. |
| `hang` | 2.5 s | Time from the end of the courtesy tone to transmitter stop. 0 is no hang. |
| `timeout` | 180 s | Maximum local transmission time |
| `link_timeout` | 180 s | Maximum link transmission time |

> **Note:** The log value `ptt: OFF ... duration` includes the carrier time, `ct_delay`,
> the courtesy tone and `hang`.

### 4.1 No tail

For no tail ("true zero hang time"), set all three:

```ini
[timers]
ct_delay = 0
hang     = 0

[events]
ct_link_off_message =
ct_link_on_message  =
ct_link_message     =
```

The transmitter stops in the next 20 ms audio frame after the carrier stops. An ID or a
command reply that is due still plays before the transmitter stops.

---

## 5. Messages

### 5.1 Definition

A message is a named sequence of audio in `[messages]`. It contains `voice:`, `cw:` and
`tone:` parts. `|` joins the parts:

```ini
[messages]
my_voice = voice: THISIS {call} REPEATER
my_cw    = cw: {call}/R
my_tone  = tone: 330:50 495:50 660:50
my_mix   = voice: THISIS {call} | tone: 880:100 | cw: {call}/R
```

### 5.2 Voice parts

A voice part is a list of clip names. The clips play in sequence. For the stock clips, see
[Appendix B](#appendix-b-voice-clips).

| Item | Result |
| --- | --- |
| `REPEATER` | The clip `REPEATER` |
| `_` | A pause of `audio.voice_gap_ms` (150 ms by default) |
| `_400` | A pause of 400 ms |
| `{call}` | The callsign, spelled: `W1XYZ` plays `W ONE X Y Z` |
| `{time}` and other variables | See [5.5](#55-variables) |

### 5.3 CW parts

A CW part is the text to send. A space is a Morse word gap. `{call}` sends the callsign.
Add other text in the message, for example `{call}/R`.

`--check` shows each character that has no Morse code. The speed, pitch and level are in
`[audio]`.

### 5.4 Tone parts

The notes play from left to right:

| Item | Result |
| --- | --- |
| `660:50` | 660 Hz for 50 ms |
| `500+700:100` | 500 Hz and 700 Hz together for 100 ms |
| `_30` | 30 ms of silence |
| `1200:60@0.9` | This note at level 0.9, in place of `audio.tone_level` |

| Value | Range |
| --- | --- |
| Frequency | 0 to 4000 Hz |
| Duration of one note | 1 to 60000 ms |
| Level | 0 to 2 |

For the tone catalog, see [Appendix A](#appendix-a-tone-catalog).

### 5.5 Variables

`usrp-rc` calculates each variable when the message plays. Each variable must be a
separate word in a `voice:` part.

| Variable | Speaks |
| --- | --- |
| `{time}` | The time, 12-hour format, for example "seven thirty" |
| `{ampm}` | "A M" or "P M" |
| `{greeting}` | "morning" before 12:00, "afternoon" before 18:00, then "evening" |
| `{day}` | The day of the week |
| `{date}` | The month and day |
| `{year}` | The year, as a whole number: "two thousand twenty six" |
| `{keyups_day}` | Key-ups in the last 24 hours |
| `{keyups_week}` | Key-ups in the last 7 days |
| `{minutes_day}` | Minutes of traffic in the last 24 hours |
| `{minutes_week}` | Minutes of traffic in the last 7 days |

- `{time}`, `{ampm}` and `{greeting}` use the female `F_` clips. The other variables use
  the standard voice.
- `{call}` is the only variable that also operates in `cw:` parts.
- The time and date come from the system clock and time zone of the computer. If the
  computer uses UTC, the repeater also uses UTC.

### 5.6 Site voice clips

To add a clip, or to replace a stock clip:

1. Prepare an 8 kHz, mono, 16-bit WAV file. Its maximum length is 3 minutes.
2. Put the file in `/usr/local/share/usrp-rc/user_8k/`. The file name without `.wav` is
   the clip name.
3. Restart the service.

A clip in `user_8k/` replaces a stock clip with the same name. Clip names are not
case-sensitive.

`usrp-rc` applies no equalization to clips. It changes only their level, with
`audio.voice_level` and `audio.master_gain`.

> **Note:** Do not normalize clips to full scale. A clip with peaks near 0 dBFS is
> louder than repeat audio. MMDVM-Host pre-emphasis also increases high frequencies to the
> transmitter. Start with peaks near -10 dBFS. Then adjust by ear or with a deviation meter.

### 5.7 Limits

| Item | Limit |
| --- | --- |
| Length of one message or one clip | 3 minutes |
| Parts in one message | 32. Each note is one part. Each `voice:` or `cw:` part is one part. |
| Words and pauses in one `voice:` part | 32 |
| Message names and clip names | 31 characters |
| Number of messages | No limit |

### 5.8 Validation

At each start and at each `--check`, `usrp-rc` makes sure that each clip for each message
exists. This includes the built-in replies.

- For a variable, `usrp-rc` examines all values that the variable can speak. Thus,
  `{keyups_day}` needs the clip `THOUSAND`, also at 12 key-ups.
- A missing clip is an error. `usrp-rc` shows the clip name and does not start.
- A message longer than 3 minutes is an error.
- A clip longer than 3 minutes is not loaded. `usrp-rc` records this in the log. This is
  an error only if a message uses the clip.
- Messages that no event uses get the same validation. An error in such a message also
  stops the start.

> **Note:** The validation cannot hear the audio. Listen to each new message on the air.

### 5.9 Unused messages

After validation, `usrp-rc` keeps only the messages that an `[events]` setting or a
schedule window names. Unused messages and unused catalog tones use no memory. At start
and at `--check`, `usrp-rc` shows the two quantities:

```text
usrp-rc: 34 messages defined, 9 in use
```

---

## 6. Events

Each `[events]` setting names the message for one occasion. An empty setting plays
nothing.

### 6.1 Identification

| Setting | Plays | Lists |
| --- | --- | --- |
| `initial_ids` | At the end of the first transmission when no ID timer operates | Yes |
| `mandatory_ids` | When the ID timer (`timers.id_interval`) expires and no station transmits. In a tail, the ID plays in full. The courtesy tone and the end of the hang wait for it. | Yes |
| `anxious_id` | In a tail during the last `timers.id_anxious` seconds before the ID timer expires. Thus, the ID does not have to key the transmitter later. | No |
| `impolite_id` | When the ID timer expires during a transmission. The ID mixes with the conversation. CW plays at `audio.impolite_morse_level`. **Necessary.** | No |

A setting with a list rotates through the list. Each ID is the next name in the list.

#### ID rules

1. A transmission makes an ID necessary.
2. An ID of any type satisfies this requirement.
3. When the ID timer expires, the repeater sends an ID only if one is necessary. If no ID
   is necessary, the timer expires and nothing plays.
4. If no transmission occurs after an ID, no new ID plays until the next transmission.

Thus, a single key-up with an initial ID needs no other ID.

If `initial_ids` is empty, the ID remains necessary after the first transmission. When
the ID timer expires, the repeater sends one of these IDs, in this sequence of priority:

1. The anxious ID, if a tail occurs in the anxious period
2. The mandatory ID
3. The impolite ID

#### ID protection

`impolite_id` is necessary. `usrp-rc` does not start without it.

If `mandatory_ids` is empty when an ID is necessary, `impolite_id` plays in its place. The
repeater keys the transmitter for this ID if necessary. `--check` shows a warning for an
empty `mandatory_ids`.

This protects ID integrity against configuration mistakes.

> **CAUTION:** Do not use this protection as the ID configuration. Always set
> `mandatory_ids`.

`anxious_id` has no protection. If it is empty, the repeater waits for the mandatory ID.

#### US rules

In the US, FCC §97.119 applies to the ID. `--check` shows a warning for each of these
items. It does not stop the start, because the rules of other countries are different.

| Rule | Warning |
| --- | --- |
| An ID at least each 10 minutes (§97.119(a)) | `id_interval` is more than 600 |
| An automatic CW ID at 20 words per minute or less (§97.119(b)(1)) | `morse_wpm` is more than 20 and an ID uses CW |
| The ID must be heard | An ID level is 0: `master_gain`, `morse_level`, `voice_level` or `impolite_morse_level` |

- Each ID must contain the call sign. Use `{call}`, or a site voice clip that speaks the
  call sign. `--check` does not examine this.
- The ID timer starts when an ID ends. Thus, the time from the start of one ID to the
  start of the next is `id_interval` plus the length of the ID, plus `pre_message_ms`
  if the ID keys the transmitter. For a margin below 10 minutes, set `id_interval` to
  less than 600, for example 540.
- The rule asks for an ID at the end of each communication. Without an initial ID, the
  ID after a short communication comes when the ID timer expires. This is the usual
  practice of repeater controllers. For an ID nearer to the end of each communication,
  set `initial_ids` and `anxious_id`.

#### Key-up during an ID

| ID type | Result of a key-up |
| --- | --- |
| Voice | The ID stops. The impolite ID plays in its place. |
| CW | The ID continues under the repeated audio. |

This applies to each ID type. In both cases, the ID counts as sent, and the ID timer
starts again when it ends.

### 6.2 Courtesy tones

| Setting | Plays after |
| --- | --- |
| `ct_link_off_message` | Local traffic with the link off, or with no link (`[link]` `enabled = false`) |
| `ct_link_on_message` | Local traffic with the link on |
| `ct_link_message` | Link traffic |

- Each setting is independent. An empty setting plays no tone. It does not use one of
  the other tones.
- To play one tone for all traffic, set the same message in all three settings.
- A courtesy tone can be any message, also a voice message.
- A different tone for link off shows the link state to listeners.
- The local time-out timer resets at the end of `ct_delay`, also when no tone is set.
  See [6.3](#63-time-out).

### 6.3 Time-out

Local traffic and link traffic have separate time-out timers. A value of 0 disables a
timer.

| Timer | Setting | Resets |
| --- | --- | --- |
| Local | `timers.timeout` | At the courtesy tone, which is the end of `ct_delay`, also with no tone set. Also when a local station keys up during link traffic, or immediately after it. |
| Link | `timers.link_timeout` | Each time the link unkeys. Link stations cannot hear the courtesy tone. |

A local station that keys up before the courtesy tone does not reset the local timer.
Thus, local stations must wait for the courtesy tone. With no courtesy tone, they must
wait for the end of `ct_delay`.

When a timer expires, the repeater stops the audio from that source until the source
unkeys:

| Source timed out | Other source |
| --- | --- |
| Local | Link traffic is repeated. Local audio does not go to the link. |
| Link | Local traffic is repeated as usual. |

If neither source is repeated, the repeater plays `timeout_message` and the transmitter
stops. When the timed-out source unkeys, the repeater plays `timeout_cancel_message` and
operates again.

During a local time-out, the repeater continues to decode DTMF from the receiver:

- A command must end with `D`. The carrier of a timed-out station does not drop, thus
  an unkey cannot enter the command. An entry without `D` is discarded at the unkey.
- All commands are available.
- The repeater keys the transmitter for the reply. It does not repeat the receiver audio.

> **CAUTION:** With `link_timeout = 0`, a link that stays keyed holds the transmitter on
> with no limit. Use this value only for a link with a continuous stream, for example a
> remote base. `--check` shows a warning for each timer set to 0.

| Setting | Plays |
| --- | --- |
| `timeout_message` | When a time-out stops the transmission |
| `timeout_cancel_message` | When the timed-out source unkeys, and the repeater operates again |

### 6.4 Command replies

| Setting | Plays | Default |
| --- | --- | --- |
| `time_message` | Reply to the time command | `time_readback` |
| `stats_message` | Reply to the usage command | `stats_readback` |
| `link_on_message` | Reply to the link-on command | `link_on_reply` |
| `link_off_message` | Reply to the link-off command | `link_off_reply` |
| `link_none_message` | Reply to a link command when the repeater has no link | `link_none_reply` |
| `repeater_up_message` | Reply to repeater up | `repeater_up_reply` |
| `repeater_down_message` | Reply to repeater down. An ID follows it. | `repeater_down_reply` |

The link-status command replies with `link_on_message` or `link_off_message`.

If a reply setting is empty, the command operates without a reply.

To change the words of a reply, define a message with the built-in name:

```ini
[messages]
stats_readback = voice: {keyups_week} KEY UP _S _ {minutes_week} MINUTES
```

### 6.5 Startup message

`startup_message` plays one time when `usrp-rc` starts.

### 6.6 Tail messages

`tail_messages` is a list of messages. Commas separate the names. Each tail message is the
next name in the list.

A tail message plays at the end of a hang, immediately before the transmitter stops. A
tail message never keys the transmitter.

| Setting | Condition |
| --- | --- |
| `timers.tail_every` | Minimum number of tails since the last tail message. `1` is each tail. `0` disables tail messages. Default: 0. |
| `timers.tail_min_interval` | Minimum number of seconds since the last tail message started. `0` is no limit. Default: 0. |

At the end of each hang, the message plays if both conditions are true. If not, the tail
ends as usual. Tail messages are off by default: to use them, set `tail_messages` and
`tail_every`. If `tail_messages` is set and `tail_every` is 0, `--check` shows a warning.

- A tail message does not wait in a queue. It plays a maximum of one time in each tail.
- If a key-up interrupts a tail message, the message counts as played.

Example: set `tail_every = 1` and `tail_min_interval = 300`. A tail message then plays
at the end of a hang if 5 minutes have passed since the last tail message started.

---

## 7. Schedule

### 7.1 Function

A schedule window changes settings for a time period. When the window closes, the
settings go back to their configured values. No second line is necessary.

A window can change timers, audio, DTMF and event settings. Examples: a longer time-out
timer and a shorter hang during a net, or a different CW speed and pitch.

```ini
[messages]
net_tonight = voice: NET TONIGHT AT SEVEN THIRTY
net_ct      = cw: N

[schedule]
net_reminder = wed 07:00-19:25 tail_messages=net_tonight tail_every=5
net_running  = wed 19:30-20:30 timeout=600 hang=1 ct_link_on_message=net_ct
```

### 7.2 Window format

```text
name = DAYS HH:MM-HH:MM setting=value [setting=value ...]
```

| Part | Rule |
| --- | --- |
| Name | Any name. The log uses this name. |
| Days | See [7.3](#73-days). Commas separate days, with no spaces: `mon,wed,fri` or `1st-tue,3rd-tue`. |
| Times | 24-hour format. The window includes its start minute. It does not include its end minute. |
| Midnight | If the end time is before the start time, the window continues past midnight. The window belongs to its start day: `fri 22:00-02:00` is open at 01:00 on Saturday. |
| Settings | See [7.5](#75-settings). An empty value disables an event or a code: `tail_messages=`. Lists use commas with no spaces: `mandatory_ids=id1,id2`. |
| Overlaps | Windows apply in file sequence. The last line has priority. |

### 7.3 Days

| Item | Matches |
| --- | --- |
| `sun` to `sat` | That day each week |
| `daily` | Each day |
| `weekdays` | Monday to Friday |
| `weekends` | Saturday and Sunday |
| `2nd-tue` | The 2nd Tuesday of each month. `1st` to `5th` operate with each day of the week. |
| `15th` | The 15th day of each month |
| `last-day` | The last day of each month |
| `dec-8` | December 8 of each year |

- A day that does not occur in a month does not match. `5th-tue` does not match in a
  month with four Tuesdays. `31st` does not match in a shorter month. `feb-29` matches in
  leap years.
- `--check` rejects a day that cannot exist, for example `6th-tue` or `feb-30`.
- A date such as `dec-8` occurs on a different day of the week each year. For a meeting
  on a weekday, update the date each year, or use a rule such as `2nd-tue`.

### 7.4 List operators

These operators apply to `initial_ids`, `mandatory_ids` and `tail_messages`:

| Item | Result |
| --- | --- |
| `tail_messages=a,b` | Replaces the list |
| `tail_messages+=a` | Adds the name to the list, if the list does not contain it |
| `tail_messages-=a` | Removes the name from the list, if the list contains it |

The open windows apply in file sequence. They start from the list in `[events]`.

Use `+=` and `-=` for windows that can overlap. Then the windows do not cancel each other:

```ini
[messages]
net_tonight  = voice: NET TONIGHT AT SEVEN THIRTY
club_meeting = voice: CLUB MEETING TONIGHT
ident_cw     = cw: {call}/R
ident_voice  = voice: THISIS {call} REPEATER

[timers]
tail_every = 5

[events]
mandatory_ids = ident_cw,ident_voice

[schedule]
weekly   = wed 07:00-19:25 tail_messages+=net_tonight
meeting  = 2nd-wed 07:00-19:25 tail_messages+=club_meeting
voice_id = weekends 00:00-23:59 mandatory_ids-=ident_cw
```

On the 2nd Wednesday, the tail messages rotate through both messages. On other Wednesdays,
only `net_tonight` plays.

#### Rules for mandatory IDs

- `-=` does not remove the last mandatory ID. One ID stays in the list. `--check` shows a
  warning.
- `mandatory_ids=` with an empty value leaves only the [ID protection](#id-protection).
  `--check` shows a warning.
- A window can change `impolite_id`. A window cannot set `impolite_id` to empty.

#### Line sequence with `=`

A general line after a specific line replaces the specific line. In this sequence, `weekly`
replaces `meeting` each time. Thus, `meeting` has no effect:

```ini
[messages]
meeting_ct = cw: M
weekly_ct  = cw: W

[schedule]
meeting = 2nd-wed 19:00-21:00 ct_link_on_message=meeting_ct
weekly  = wed 19:00-21:00 ct_link_on_message=weekly_ct
```

Put the most specific line last.

### 7.5 Settings

A window can change each setting in these sections:

- `[timers]`
- `[audio]`, but not `ste_delay_ms` or `lead_trim_ms`
- `[dtmf]`, but not `enabled`, `min_tone_ms`, the `beep_` settings or the `regen_`
  settings
- `[events]`, but not `startup_message`

The other settings take effect only at startup. `--check` rejects them in a window.

- Write a setting by its name, for example `hang=1`. The name `section.name` is also
  correct, for example `timers.hang=1`.
- Each value has the same range as in its own section.
- `--check` examines each window together with the settings in the file. It applies the
  same rules as to the file. For example, a window cannot set `time_code` to a code
  that another command uses. A window that sets `timeout=0` causes a warning.
- `--check` also examines the length of each message during each window that changes
  `[audio]` settings. A message must be 3 minutes or shorter in each window.
- `--check` does not examine two windows that overlap together.

A new value has effect the next time that it is used. A timer that runs when the window
opens or closes keeps its value. The new value applies when the timer starts again.

### 7.6 Timing

- Windows start and stop at the start of each minute.
- The schedule uses the system clock and time zone. Keep the clock and time zone of the
  computer correct.
- The schedule stores no data. After a restart during a window, the window applies again
  immediately.

### 7.7 Limits

| Item | Limit |
| --- | --- |
| Windows | 32 |
| Settings in one window | 16 |
| Calendar days in one window | 8 |
| Names in a list | 8. `--check` shows a warning if `+=` can add more. Names after the eighth are not added. |
| Length of one window | 24 hours |

### 7.8 Test a window

1. Add a window that opens 2 minutes after the current time.
2. Apply the change. See [3.3](#33-apply-a-change).
3. Monitor the log. At `log.level = info`, the log shows these lines:

```text
schedule: 'net_reminder' started
tail: 'net_tonight'
schedule: 'net_reminder' ended
```

### 7.9 Functions not available

- A window cannot change `[link]` settings. Thus, a window cannot turn the link on or
  off.
- A change without an end time is not available.
- "Last Friday" and one-time dates are not available.

---

## 8. Link

### 8.1 Link state

| Setting | Effect |
| --- | --- |
| `enabled = false` | The repeater has no link. `70`, `72` and `73` reply with `link_none_message`: "No link service". |
| `start_on = false` | The link is off at start. `72` turns it on. |

- When the link is off, `usrp-rc` sends nothing to the link. It ignores all audio from
  the link.
- `usrp-rc` does not keep the link state after a restart. At each start, the link state
  is the value of `start_on`.
- To hear the link state without a change, use the link-status command, `70`.
- To show the link state with each courtesy tone, set different messages in
  `ct_link_on_message` and `ct_link_off_message`. See [6.2](#62-courtesy-tones).
- At this time, each station can use `72` and `73`. A control operator level is planned.
- At this time, `codec = opus` operates only with other tools from the same author.

### 8.2 Traffic priority

Local traffic has priority over link traffic.

| Condition | Result |
| --- | --- |
| A local station transmits during link traffic | The repeater repeats the local station. The link audio stops. |
| Link traffic arrives during a local transmission | The repeater does not transmit the link audio. |
| The local station unkeys and link traffic continues | The repeater transmits the link audio again. |

- During a local transmission, `usrp-rc` sends the local audio to the link, also when link
  traffic arrives at the same time.
- The repeater stays in one transmission while either source is active. The tail starts
  when both sources have stopped.
- The courtesy tone is for the source that was active last.
- Each source has its own time-out timer. A local station that keys up during link
  traffic starts with a full timer. See [6.3](#63-time-out).
- `usrp-rc` operates the link as full duplex. It sends local audio to the link and
  receives link audio at the same time. The system at the other end of the link is the
  arbiter of simultaneous traffic. Its behavior is not controlled by `usrp-rc`.

---

## 9. DTMF control

### 9.1 Function

`usrp-rc` decodes DTMF from the repeater receiver only. It does not decode tones from the
link. For command entry, see [section 2](#2-over-the-air-operation).

```ini
[dtmf]
enabled      = true
time_code    = 39
link_on_code = 1*72
stats_code   =
```

### 9.2 Codes

- A code can contain `0` to `9`, `*`, `A`, `B` and `C`.
- The length of a code is 2 to 7 keys. The pad test prefix can also be 1 key, for the
  `5` that most controllers use.
- `#` and `D` are not available in codes. They cancel and execute.
- An empty code disables its command. In the example above, `stats_code` is empty.
- The pad test prefix and the regen code take each key after them, to the unkey. Thus,
  no other code can start with one of them. With the default prefix `5`, no code can
  start with `5`. `--check` shows each code that does.

> **Note:** DTMF gives no security. A station that monitors the repeater can hear a code
> and use it. Select the enabled commands with this risk in mind.

### 9.3 Command levels

The commands follow the ACC model: user commands, control operator commands and
programming commands. At this time, only user commands are available. The configuration
file replaces DTMF programming. The programming level stays in the design for possible
future use.

### 9.4 Muting

At the first valid tone, the repeater mutes the repeated audio. A beep replaces the
audio: 100 ms on, 900 ms off, by default. The mute stops `mute_hang_ms` after the last
tone. A new tone starts the mute again.

If `audio.ste_delay_ms` is 0, the start of the first tone goes out before the mute
starts. To prevent this blip, set `audio.ste_delay_ms` longer than `min_tone_ms`.

### 9.5 Adjustment

| Condition | Action |
| --- | --- |
| Voice causes false digits | Increase `min_tone_ms`. |
| The repeater does not detect short tones from some radios | Decrease `min_tone_ms`. The minimum is 30. |
| The first tone goes out as a blip | Set `audio.ste_delay_ms` longer than `min_tone_ms`, for example 60. This delays all repeated audio and link audio by the same time. |
| The beep continues too long | Decrease `mute_hang_ms`. Keep it longer than `ste_delay_ms` and shorter than `interdigit_ms`. |
| The system at the other end of the link misses the first regen tone | Increase `regen_lead_ms`. |
| The system at the other end of the link misses regen tones | Increase `regen_tone_ms` and `regen_gap_ms`, or change `regen_level`. |

### 9.6 Log

At `log.level = info`, the log shows the start and the result of each entry:

```text
dtmf: sequence started with '3'
dtmf: '38': ACCEPTED command = usage
```

| Result | Meaning | Example |
| --- | --- | --- |
| `ACCEPTED` | The command executed. | `'5123': ACCEPTED command = DTMF test + 123` |
| | | `'69*3': ACCEPTED command = regen *3` |
| `ERROR` | The entry is complete, but no command can execute. | `'99': ERROR no command matched` |
| | A link command on a repeater with no link. The reply is "No link service". | `'70': ERROR command = link status, no link service` |
| `ABANDONED` | The entry stopped before it was complete. | `'12#': ABANDONED user terminated` |

An `ABANDONED` line also names the timer that expired, if a timer expired. At
`log.level = debug`, the log also shows each digit and each tone that was too short.

### 9.7 Repeater down and up

The repeater command takes the repeater out of service and puts it back. Enter the code,
then `0` or `1`:

| Entry | Result | Reply |
| --- | --- | --- |
| Code + `0` | Repeater down | "Repeater down", then an ID |
| Code + `1` | Repeater up | "Repeater up" |

`repeater_code` is empty by default. Thus, the command is off until the operator sets a
code.

> **CAUTION:** At this time, each station can use each command. A station that hears the
> code can take the repeater out of service. Set a code only if this risk is acceptable.
> A control operator level is planned.

When the repeater is down:

- No audio goes through the repeater: no local repeat, no local audio to the link, and
  no link audio to the transmitter.
- The repeater continues to decode DTMF from the receiver. Commands operate as usual,
  but only the repeater command transmits a reply.
- No courtesy tones, tail messages, IDs or time-outs occur.
- The down state stays after a restart. It is kept in `[state] file`. The startup message
  does not play.

The reply "Repeater down" is a transmission. Thus, the repeater sends a mandatory ID
immediately after it. No other ID is then necessary while the repeater is down.

If a station enters code + `1` with `D` and stays keyed, the repeater repeats that
station immediately. The reply plays in its tail.

### 9.8 DTMF regen

`regen_code` is empty by default. Thus, the command is off until the operator sets a
code. For use, see [2.6](#26-dtmf-regen).

```ini
[dtmf]
enabled    = true
regen_code = 69
```

> **CAUTION:** Each station that hears the code can send DTMF to the system at the other
> end of the link. Set a code only if this risk is acceptable.

| Setting | Default | Function |
| --- | --- | --- |
| `regen_tone_ms` | `100` | Length of each tone |
| `regen_gap_ms` | `100` | Silence between tones. On the link, silence of 2 × this time also follows the last tone, before the unkey. |
| `regen_lead_ms` | `200` | Silence on the link before the first tone. Some decoders do not detect a tone that starts at the key-up. |
| `regen_level` | `0.5` | Level of the tones. The two frequencies of each tone have the same level. |

- These settings take effect only at startup. A schedule window cannot change them.
- `audio.master_gain` applies to the tones on the repeater transmitter. It does not
  apply to the tones on the link.
- The tones on the link do not wait for the tones on the transmitter. They start
  immediately after the unkey.
- The log shows `link: KEY -> peer (DTMF)` at the start and `link: DTMF sent` at the
  end. Link off, repeater down or a stop of `usrp-rc` stops the tones. The log then shows
  `link: DTMF cut short`.

---

## 10. Usage statistics

### 10.1 Function

`usrp-rc` counts key-ups and minutes of traffic. Local traffic and link traffic both
count. The count covers a rolling period of 7 days. Kerchunks and time-outs do not count.

The usage command (`38`) and the `{keyups_*}` and `{minutes_*}` variables speak these
values.

### 10.2 Statistics file

The default file is `/var/local/usrp-rc/stats`. It contains one line for each hour with
traffic:

```text
# usrp-rc usage: hour-since-epoch keyups seconds
498279 3 2.4
```

| Item | Behavior |
| --- | --- |
| Save | Each 15 minutes if the values changed, and at each normal stop. A power failure loses a maximum of 15 minutes. |
| Size | A maximum of 7 days, 168 lines. The file does not grow past this size. |
| Damage | `usrp-rc` keeps the lines that it can read. |
| Write failure | The count continues until the next restart. The log shows a warning. |
| Other location | Set `[stats] file`. An empty value keeps the values in memory only. |

> **Note:** The service can write only to `/var/local/usrp-rc`. A file in a different
> location also needs a `ReadWritePaths` line in the service file.

---

## 11. Settings reference

The default applies when a setting is not in the file. A value out of range is a
configuration error. A schedule window can change the settings in `[timers]`, `[audio]`,
`[dtmf]` and `[events]`, with some exceptions. See [7.5](#75-settings).

### 11.1 Top level

These settings go before the first section.

| Setting | Default | Range | Function |
| --- | --- | --- | --- |
| `callsign` | None | Letters and digits | Used by `{call}`. Add `/R` or `REPEATER` in the message. |
| `access_mode` | `cor` | `cor`, `cor_ctcss` | No difference at this time. MMDVM-Host decodes CTCSS. |

### 11.2 `[log]`

| Setting | Default | Range | Function |
| --- | --- | --- | --- |
| `level` | `info` | `error`, `warn`, `info`, `debug` | Quantity of log data |

### 11.3 `[mmdvm]`

The connection to MMDVM-Host on the same computer.

| Setting | Default | Function |
| --- | --- | --- |
| `local_address` | `127.0.0.1` | Address on which `usrp-rc` receives. Equal to `GatewayAddress` in MMDVM-Host. |
| `local_port` | `4810` | Port on which `usrp-rc` receives. Equal to `GatewayPort` in MMDVM-Host. |
| `rpt_address` | `127.0.0.1` | Address of MMDVM-Host. Equal to `LocalAddress` in MMDVM-Host. |
| `rpt_port` | `3810` | Port of MMDVM-Host. Equal to `LocalPort` in MMDVM-Host. |

These settings have the same names and functions as `[Network]` in `FMGateway.ini`. Set
the ports to agree with `MMDVM-Host.ini`. For the procedure, see
[README.md](README.md#mmdvm-host-integration).

### 11.4 `[link]`

| Setting | Default | Range | Function |
| --- | --- | --- | --- |
| `enabled` | `true` | `true`, `false` | The repeater has a link |
| `start_on` | `true` | `true`, `false` | The link is on at start |
| `remote_host` | None | | Address of the USRP peer. Necessary when `enabled` is true. |
| `remote_port` | `34001` | | Port of the peer |
| `local_port` | `34002` | | Port on which `usrp-rc` receives |
| `bind_address` | `0.0.0.0` | | Address on which `usrp-rc` receives |
| `codec` | `pcm` | `pcm`, `opus` | Audio format of the link |
| `opus_bitrate` | `8000` | 4000-64000 | Applies when `codec = opus` |
| `opus_frame_ms` | `60` | 20, 40, 60 | Applies when `codec = opus` |

### 11.5 `[timers]`

| Setting | Default | Range | Function |
| --- | --- | --- | --- |
| `kerchunk` | `0.5` | 0-60 | The repeater ignores a carrier shorter than this time. |
| `ct_delay` | `0.5` | 0-60 | Time from carrier stop to the courtesy tone |
| `hang` | `2.5` | 0-600 | Time that the transmitter stays on after the courtesy tone |
| `timeout` | `180` | 0-3600 | Local time-out timer. It resets at the courtesy tone (the end of `ct_delay`), also with no tone set. 0 is off. |
| `link_timeout` | `180` | 0-3600 | Link time-out timer. It resets each time the link unkeys. 0 is off. |
| `id_interval` | `600` | 1-86400 | ID timer, from the end of an ID. A value above 600 (the US limit of 10 minutes) causes a warning. See [6.1](#61-identification). |
| `id_anxious` | `60` | 0-3600 | Anxious ID period before the ID timer expires. Must be less than `id_interval`. |
| `tail_every` | `0` | 0-1000 | Tails between tail messages. 0 disables tail messages. |
| `tail_min_interval` | `0` | 0-86400 | Minimum seconds between tail messages. 0 is no limit. |

### 11.6 `[audio]`

| Setting | Default | Range | Function |
| --- | --- | --- | --- |
| `master_gain` | `1.0` | 0-10 | Last multiplier on all controller audio. Set it for the transmitter deviation. |
| `morse_wpm` | `20` | 5-60 | CW speed, in words per minute. Above 20 with a CW ID causes a warning. See [6.1](#61-identification). |
| `morse_pitch` | `700` | 100-3400 | CW frequency, in Hz |
| `morse_level` | `0.9` | 0-2 | CW level |
| `impolite_morse_level` | `0.3` | 0-2 | CW level for an impolite ID |
| `voice_level` | `0.9` | 0-2 | Voice clip level |
| `tone_level` | `0.5` | 0-2 | Level of tones without `@LEVEL`, and of the DTMF beep. The beep uses the value at startup. |
| `voice_gap_ms` | `150` | 0-10000 | Length of a `_` pause |
| `ste_delay_ms` | `0` | 0-2000 | Delay of repeated audio and link audio, for squelch tail elimination. 0 is off. |
| `lead_trim_ms` | `0` | 0-2000 | Mute at the start of each transmission. 0 is off. |
| `pre_message_ms` | `0` | 0-10000 | Silence after the transmitter starts, before CW or voice |
| `post_message_ms` | `0` | 0-10000 | Silence after the audio, before the transmitter stops |

Levels above 1.0 are permitted, but they can cause clipping.

### 11.7 `[dtmf]`

| Setting | Default | Range | Function |
| --- | --- | --- | --- |
| `enabled` | `false` | `true`, `false` | Enables DTMF commands |
| `pad_test_prefix` | `5` | 1-7 keys | Pad test prefix. Empty is off. |
| `time_code` | `39` | 2-7 keys | Time command. Empty is off. |
| `stats_code` | `38` | 2-7 keys | Usage command. Empty is off. |
| `link_on_code` | `72` | 2-7 keys | Link-on command. Empty is off. |
| `link_off_code` | `73` | 2-7 keys | Link-off command. Empty is off. |
| `link_status_code` | `70` | 2-7 keys | Link-status command. Empty is off. |
| `repeater_code` | Empty | 2-7 keys | Repeater command: code + `0` is down, code + `1` is up. Empty is off. See [9.7](#97-repeater-down-and-up). |
| `regen_code` | Empty | 2-7 keys | DTMF regen: code + keys to send. Empty is off. See [9.8](#98-dtmf-regen). |
| `regen_tone_ms` | `100` | 40-1000 | Regen tone length |
| `regen_gap_ms` | `100` | 40-1000 | Silence between regen tones |
| `regen_lead_ms` | `200` | 0-2000 | Silence on the link before the first regen tone |
| `regen_level` | `0.5` | 0-1 | Regen tone level |
| `min_tone_ms` | `40` | 30-1000 | Minimum tone length for a digit |
| `max_tone_ms` | `5000` | 100-60000 | A longer tone discards the entry. |
| `interdigit_ms` | `4000` | 100-60000 | A longer pause discards the digits before it. |
| `end_of_tx_ms` | `8000` | 100-600000 | A longer time from the last digit to the unkey discards the entry. |
| `mute_hang_ms` | `2000` | 0-60000 | Mute time after the last tone. Must be less than `interdigit_ms`. |
| `beep_hz` | `440` | 100-3400 | Beep frequency during the mute |
| `beep_on_ms` | `100` | 0-10000 | Beep length |
| `beep_off_ms` | `900` | 0-10000 | Time between beeps |

### 11.8 `[stats]`

| Setting | Default | Function |
| --- | --- | --- |
| `file` | `/var/local/usrp-rc/stats` | Statistics file. Empty keeps the values in memory only. |

### 11.9 `[state]`

| Setting | Default | Function |
| --- | --- | --- |
| `file` | `/var/local/usrp-rc/state` | Keeps the repeater down state after a restart. Empty does not keep it. |

### 11.10 `[events]`

All `[events]` settings are message names. Empty plays nothing. No event uses another
event's message in its place.

| Setting | Default | Lists | Section |
| --- | --- | --- | --- |
| `startup_message` | Empty | No | [6.5](#65-startup-message) |
| `initial_ids` | Empty | Yes | [6.1](#61-identification) |
| `mandatory_ids` | Empty | Yes | [6.1](#61-identification) |
| `anxious_id` | Empty | No | [6.1](#61-identification) |
| `impolite_id` | None. Necessary. | No | [6.1](#61-identification) |
| `ct_link_off_message` | Empty | No | [6.2](#62-courtesy-tones) |
| `ct_link_on_message` | Empty | No | [6.2](#62-courtesy-tones) |
| `ct_link_message` | Empty | No | [6.2](#62-courtesy-tones) |
| `timeout_message` | Empty | No | [6.3](#63-time-out) |
| `timeout_cancel_message` | Empty | No | [6.3](#63-time-out) |
| `tail_messages` | Empty | Yes | [6.6](#66-tail-messages) |
| `time_message` | `time_readback` | No | [6.4](#64-command-replies) |
| `stats_message` | `stats_readback` | No | [6.4](#64-command-replies) |
| `link_on_message` | `link_on_reply` | No | [6.4](#64-command-replies) |
| `link_off_message` | `link_off_reply` | No | [6.4](#64-command-replies) |
| `link_none_message` | `link_none_reply` | No | [6.4](#64-command-replies) |
| `repeater_up_message` | `repeater_up_reply` | No | [6.4](#64-command-replies) |
| `repeater_down_message` | `repeater_down_reply` | No | [6.4](#64-command-replies) |

A list holds a maximum of 8 names.

### 11.11 `[messages]` and `[schedule]`

See [section 5](#5-messages) and [section 7](#7-schedule).

---

## 12. Configuration examples

Add each example to the configuration. The message names are examples.

### 12.1 Rotate voice and CW IDs

```ini
[events]
mandatory_ids = id_cw,id_voice

[messages]
id_cw    = cw: {call}/R
id_voice = voice: THISIS {call} REPEATER
```

### 12.2 Show the link state with the courtesy tone

```ini
[events]
ct_link_off_message = comet
ct_link_on_message  = yellow_jacket
ct_link_message     = bumble_bee
```

### 12.3 Announce the time in the tail on weekends

```ini
[messages]
weekend_time = voice: F_GOOD {greeting} _ F_THE F_TIME F_IS {time} {ampm}

[schedule]
weekend_tail = weekends 08:00-20:00 tail_messages=weekend_time tail_every=5
```

### 12.4 Speak the usage for 7 days with command `38`

```ini
[messages]
stats_readback = voice: {keyups_week} KEY UP _S _ {minutes_week} MINUTES
```

### 12.5 Disable tail messages at night

```ini
[timers]
tail_every = 10

[events]
tail_messages = my_tail

[messages]
my_tail = voice: WELCOME TO {call}

[schedule]
quiet_nights = daily 22:00-06:00 tail_messages=
```

### 12.6 Change the timers, the courtesy tone and the ID during a net

```ini
[messages]
net_ct = cw: N
net_id = voice: THISIS {call} NET

[schedule]
timers = wed 19:30-20:30 timeout=600 hang=1
tones  = wed 19:30-20:30 ct_link_on_message=net_ct ct_link_off_message=net_ct
ids    = wed 19:30-20:30 mandatory_ids=net_id
```

One window can hold all of these settings. Separate windows keep each line short.

### 12.7 Turn on the link only by command

```ini
[link]
remote_host = 127.0.0.1
start_on    = false
```

### 12.8 No courtesy tone, short tail

Some repeaters use no courtesy tone and a total tail of 1 to 2 seconds. Use `ct_delay`
as the tail, and set `hang` to 0. The transmitter stays on for `ct_delay`, then stops.
The local time-out timer resets at the end of `ct_delay`:

```ini
[timers]
ct_delay = 1.5
hang     = 0

[events]
ct_link_off_message =
ct_link_on_message  =
ct_link_message     =
```

A long hang with no tone is also correct. The time-out timer then resets at the end of
`ct_delay`, and the transmitter stays on for `hang` after that.

For no tail at all, see [4.1](#41-no-tail).

### 12.9 Delay the courtesy tone by 640 ms

Some courtesy tone specifications have a delay of 640 ms from squelch closure to the
first tone:

```ini
[timers]
ct_delay = 0.64
```

---

## 13. Log and troubleshooting

### 13.1 Log

```bash
journalctl -u usrp-rc -f
```

At `log.level = info`, the log shows state changes, IDs, tail messages, schedule windows
and DTMF entries. The log names each message that plays. Use the log first to find the
cause of a problem.

### 13.2 Troubleshooting

| Symptom | Probable cause | Action |
| --- | --- | --- |
| No response to DTMF | `[dtmf] enabled = true` is not set. The default is off. | Set `enabled = true`. |
| No DTMF lines in the log | The decoder does not detect the tones. | At `log.level = info`, look for `dtmf: sequence started`. If it is not there, decrease `min_tone_ms`. At `log.level = debug`, look for short tones. |
| An entry starts, but no reply plays | The command did not execute, or its reply is empty. | Read the `dtmf:` line at the end of the entry. If a command executed, make sure that its reply setting in `[events]` is not empty. |
| `70`, `72` or `73` replies "No link service" | `link.enabled = false` | If the repeater has a link, set `[link] enabled = true` and `remote_host`. |
| `usrp-rc` does not start | Configuration error | Do `--check`. Correct the errors from the first error down. |
| `events.impolite_id is required` | `impolite_id` is not set. | Set `impolite_id` in `[events]`. |
| `events.ct_message: renamed` | Old setting name | Change it to `ct_link_on_message`. |
| `needs commas between names` | Spaces separate list names. | Change `id1 id2` to `id1,id2`. |
| `a variable needs the voice clip` | A stock clip is missing. | Do `sudo make install`. It replaces all stock clips. |
| No courtesy tone in one link state | The `ct_` setting for that state is empty. An empty setting plays no tone. | Set all three `ct_` settings. See [6.2](#62-courtesy-tones). |
| A tail message does not play | `tail_every` is 0, or the conditions are not true. | Look for the `tail_every` warning. On a quiet repeater, the `tail_every` count can be not yet reached. `tail_min_interval` can also prevent the message. |
| A window opens at the wrong time | The clock or the time zone is wrong. | Examine the clock and time zone with `timedatectl`. |
| Usage statistics reset at restart | `usrp-rc` cannot write the file. | Look for `stats: can't save` in the log. Make sure that `/var/local/usrp-rc` exists and the service file is current. Then do `sudo systemctl daemon-reload`. |
| No audio from the link | The link is off. | Look for `73` in the log, or for `start_on = false`. |
| The repeater does not repeat anything | The repeater is down. | Look for `repeater DOWN` in the log. Enter the repeater code + `1`. To clear it without DTMF, stop the service, change `repeater = down` to `up` in `/var/local/usrp-rc/state`, and start the service. |

---

## Appendix A: Tone catalog

Use these tones by name, for example `ct_link_on_message = yellow_jacket`. The tones are in
`tones.conf`. An upgrade replaces this file.

> **Note:** Do not edit `tones.conf`. To change a tone, define a message with the same
> name in the configuration.

### One note or chord

| Name | Notes | Sound |
| --- | --- | --- |
| `honk` | `500+700:100` | Short, low two-note horn |
| `beep` | `880:100` | Plain high beep |
| `boop` | `440:100` | Plain low beep |
| `bloop` | `840+500:100` | Short two-note blend |
| `piano_chord` | `660+880:100` | Short, bright chord |
| `rolm` | `525+660:150` | Longer, soft chord |
| `quindar_on` | `2525:250` | Apollo-style high tone, start of a transmission |
| `quindar_off` | `2475:250` | Apollo-style high tone, end of a transmission |

### Rising

| Name | Notes | Sound |
| --- | --- | --- |
| `yellow_jacket` | `330:50 495:50 660:50` | Three fast rising notes |
| `bumble_bee` | `330:100 495:100 660:100` | The notes of `yellow_jacket`, at half speed |
| `ascending` | `500:50 750:50 1000:50` | Three fast rising notes, higher |
| `comet` | `500:100 500:100 750:100` | Two low notes, then a higher note |
| `stardust` | `750:125 880:80 880+1200:80` | Rising, with a bright chord at the end |

### Falling

| Name | Notes | Sound |
| --- | --- | --- |
| `descending` | `1000:50 750:50 500:50` | Three fast falling notes |
| `hornet` | `660:50 500:50 385:50` | Three fast falling notes, lower |
| `wasp` | `660:100 500:100 385:100` | The notes of `hornet`, at half speed |
| `tumbleweed` | `1000:20 800:20 600:20` | Very fast falling chirp |
| `shooting_star` | `800:100 800:100 540:100` | Two notes, then a lower note |
| `doorbell` | `800:75 400:50` | Ding-dong |
| `doorbell_chord` | `1450+725:75 725+360:50` | Ding-dong in octave chords |

### Up and down

| Name | Notes | Sound |
| --- | --- | --- |
| `fire_fly` | `1000+1200:120 1200+1400:80 600+800:100` | A chord rises, then drops low |
| `moonbounce` | `1000+800:50 800:50 600:50 1500:50` | Falls, then jumps high |
| `chirp_chomp` | `1500:20 1250:20 1000:20 750:20 500:20 2550:20` | Very fast fall, then a high blip |
| `dunce_cap` | `440+500:200 440+350:200` | Two long, low chords, falling |

### Repeated, with gaps

| Name | Notes | Sound |
| --- | --- | --- |
| `nextel_beep` | `1760:30 _30 1760:30 _30 1760:30` | Three fast high chirps |
| `function_complete` | `480+1200:40 _40 480+1200:40` | Two fast two-note chirps |
| `function_complete_2` | `900:60 _60 900:60` | Two fast beeps |

### Warnings

| Name | Notes | Sound |
| --- | --- | --- |
| `timeout_warn` | `1200:60@0.9 _20 1000:60@0.9 _20 800:60@0.9` | Loud, three falling notes |

---

## Appendix B: Voice clips

`usrp-rc` includes 623 clips in `/usr/local/share/usrp-rc/vocab_8k/`. Clip names are not
case-sensitive: `repeater`, `Repeater` and `REPEATER` are the same clip. The lists below
use lowercase. A bold letter is the clip for that letter, for example `C` in
`voice: W ONE C`.

| Clip type | Function |
| --- | --- |
| `F_` clips | Female voice. The time variables use these clips. |
| Clips that start with `_` | Word endings, for example `_S` and `_TEEN`. They are clips, not pauses. A pause is `_` with only digits after it. |

### Standard voice

<table>
<tr><td><b>A</b></td><td><b>D</b></td><td><b>H</b></td><td><b>M</b></td><td><b>Q</b></td><td><b>T</b></td></tr>
<tr><td>a_m</td><td>danger</td><td>hail</td><td>machine</td><td>quebec</td><td>tacan</td></tr>
<tr><td>abeam</td><td>days</td><td>half</td><td>magnetos</td><td></td><td>tango</td></tr>
<tr><td>abort</td><td>dayton</td><td>ham</td><td>maintain</td><td><b>R</b></td><td>tank</td></tr>
<tr><td>about</td><td>december</td><td>hamfest</td><td>manual</td><td>r_nav</td><td>target</td></tr>
<tr><td>above</td><td>decrease</td><td>hamvention</td><td>march</td><td>radar</td><td>taxi</td></tr>
<tr><td>accelerated</td><td>decreasing</td><td>have</td><td>marker</td><td>radial</td><td>telephone</td></tr>
<tr><td>acknowledge</td><td>degree</td><td>hazardous</td><td>may</td><td>radio</td><td>temperature</td></tr>
<tr><td>action</td><td>degrees</td><td>haze</td><td>mayday</td><td>radios</td><td>ten</td></tr>
<tr><td>adjust</td><td>delta</td><td>heading</td><td>me</td><td>rain</td><td>terminal</td></tr>
<tr><td>advanced</td><td>departure</td><td>heat</td><td>mean</td><td>raise</td><td>test</td></tr>
<tr><td>advise</td><td>device</td><td>heavy</td><td>measure</td><td>range</td><td>thankyou</td></tr>
<tr><td>aerial</td><td>dial</td><td>help</td><td>measured</td><td>rate</td><td>that</td></tr>
<tr><td>affirmative</td><td>dinner</td><td>henry</td><td>meeting</td><td>ready</td><td>the_a</td></tr>
<tr><td>air</td><td>direction</td><td>hertz</td><td>mega</td><td>rear</td><td>the_b</td></tr>
<tr><td>air_brakes</td><td>display</td><td>high</td><td>messages</td><td>receive</td><td>thee</td></tr>
<tr><td>aircraft</td><td>divided</td><td>hold</td><td>meter</td><td>red</td><td>thin</td></tr>
<tr><td>airport</td><td>door</td><td>home</td><td>micro</td><td>refuelling</td><td>thinly</td></tr>
<tr><td>airspeed</td><td>doors</td><td>hotel</td><td>middle</td><td>release</td><td>thir_</td></tr>
<tr><td>alert</td><td>down</td><td>hour</td><td>midpoint</td><td>remark</td><td>third</td></tr>
<tr><td>all</td><td>downwind</td><td>hours</td><td>mig</td><td>remote</td><td>thirteen</td></tr>
<tr><td>aloft</td><td>drive</td><td>hundred</td><td>mike</td><td>repair</td><td>thirty</td></tr>
<tr><td>alpha</td><td>drizzle</td><td></td><td>miles</td><td>repeat</td><td>thisis</td></tr>
<tr><td>alternate</td><td>dust</td><td><b>I</b></td><td>mill</td><td>repeat_er</td><td>thousand</td></tr>
<tr><td>altimeter</td><td></td><td>ice</td><td>milli</td><td>repeater</td><td>three</td></tr>
<tr><td>altitude</td><td><b>E</b></td><td>icing</td><td>million</td><td>rich</td><td>thunderstorm</td></tr>
<tr><td>amateur</td><td>east</td><td>identify</td><td>minus</td><td>rig</td><td>thursday</td></tr>
<tr><td>amps</td><td>echo</td><td>idle</td><td>minutes</td><td>right</td><td>time</td></tr>
<tr><td>and_a</td><td>eight</td><td>ifr</td><td>mist</td><td>road</td><td>timer</td></tr>
<tr><td>and_b</td><td>eighteen</td><td>ignite</td><td>mixture</td><td>roger</td><td>times</td></tr>
<tr><td>answer</td><td>eighty</td><td>ignition</td><td>mobile</td><td>roll_out</td><td>to</td></tr>
<tr><td>approach</td><td>electrician</td><td>ils</td><td>moderate</td><td>romeo</td><td>today</td></tr>
<tr><td>approaches</td><td>elevation</td><td>immediately</td><td>monday</td><td>route</td><td>tomorrow</td></tr>
<tr><td>april</td><td>eleven</td><td>in</td><td>month</td><td></td><td>tonight</td></tr>
<tr><td>area</td><td>emergency</td><td>inbound</td><td>more_than</td><td><b>S</b></td><td>too_low</td></tr>
<tr><td>arrival</td><td>engine</td><td>inch</td><td>motor</td><td>safe</td><td>tool</td></tr>
<tr><td>as</td><td>enter</td><td>increase</td><td>move</td><td>sand</td><td>tornado</td></tr>
<tr><td>at</td><td>equal</td><td>increasing</td><td>moving</td><td>santaclara</td><td>touchdown</td></tr>
<tr><td>atis</td><td>equals</td><td>increasingto</td><td>much</td><td>saturday</td><td>tower</td></tr>
<tr><td>august</td><td>error</td><td>india</td><td></td><td>scattered</td><td>traffic</td></tr>
<tr><td>auto</td><td>estimated</td><td>indicated</td><td><b>N</b></td><td>second</td><td>transmit</td></tr>
<tr><td>automatic</td><td>eta</td><td>inflight</td><td>near</td><td>seconds</td><td>trim</td></tr>
<tr><td>autopilot</td><td>evacuate</td><td>information</td><td>negative</td><td>security</td><td>true</td></tr>
<tr><td>auxilliary</td><td>evacuation</td><td>inner</td><td>net</td><td>selcal</td><td>tuesday</td></tr>
<tr><td></td><td>exit</td><td>inspector</td><td>new</td><td>select</td><td>turbulence</td></tr>
<tr><td><b>B</b></td><td>expect</td><td>instruments</td><td>next</td><td>september</td><td>turn</td></tr>
<tr><td>band</td><td></td><td>intruder</td><td>night</td><td>sequence</td><td>twelve</td></tr>
<tr><td>bank</td><td><b>F</b></td><td>is</td><td>nine</td><td>service</td><td>twenty</td></tr>
<tr><td>barker</td><td>fail</td><td>it</td><td>niner</td><td>set</td><td>two</td></tr>
<tr><td>base</td><td>failure</td><td></td><td>nineteen</td><td>seven</td><td></td></tr>
<tr><td>battery</td><td>farad</td><td><b>J</b></td><td>ninety</td><td>seventeen</td><td><b>U</b></td></tr>
<tr><td>below</td><td>farenheit</td><td>january</td><td>no</td><td>seventy</td><td>under</td></tr>
<tr><td>between</td><td>fast</td><td>juliet</td><td>no_turn</td><td>severe</td><td>undercarriage</td></tr>
<tr><td>blowing</td><td>february</td><td>july</td><td>north</td><td>sexy</td><td>unicom</td></tr>
<tr><td>board</td><td>feet</td><td>june</td><td>northeast</td><td>short</td><td>uniform</td></tr>
<tr><td>boost</td><td>field</td><td></td><td>northwest</td><td>showers</td><td>unit</td></tr>
<tr><td>brake</td><td>fif_</td><td><b>K</b></td><td>not</td><td>shut</td><td>unlimited</td></tr>
<tr><td>bravo</td><td>fifteen</td><td>key</td><td>notam</td><td>side</td><td>until</td></tr>
<tr><td>break</td><td>fifty</td><td>kilo</td><td>november</td><td>sierra</td><td>up</td></tr>
<tr><td>breaking</td><td>filed</td><td>knots</td><td>number</td><td>sight</td><td>use_s</td></tr>
<tr><td>broken</td><td>final</td><td></td><td></td><td>sigmet</td><td>use_z</td></tr>
<tr><td>button</td><td>fire</td><td><b>L</b></td><td><b>O</b></td><td>six</td><td></td></tr>
<tr><td>by</td><td>first</td><td>land</td><td>obscured</td><td>sixteen</td><td><b>V</b></td></tr>
<tr><td></td><td>five</td><td>landing</td><td>oclock</td><td>sixty</td><td>vacuum</td></tr>
<tr><td><b>C</b></td><td>flame_out</td><td>landing_gear</td><td>october</td><td>sleet</td><td>valley</td></tr>
<tr><td>cabin</td><td>flaps</td><td>late</td><td>of</td><td>slope</td><td>valve</td></tr>
<tr><td>calibrate</td><td>flight</td><td>launch</td><td>off</td><td>slow</td><td>variable</td></tr>
<tr><td>call</td><td>flightwatch</td><td>lean</td><td>ohio</td><td>smoke</td><td>vectors</td></tr>
<tr><td>calling</td><td>flow</td><td>left</td><td>ohms</td><td>snow</td><td>verify</td></tr>
<tr><td>calm</td><td>fog</td><td>leg</td><td>oil</td><td>south</td><td>vfr</td></tr>
<tr><td>cancel</td><td>for</td><td>less_than</td><td>on</td><td>southeast</td><td>vhe</td></tr>
<tr><td>caution</td><td>forty</td><td>level</td><td>one</td><td>southwest</td><td>victor</td></tr>
<tr><td>ceiling</td><td>four</td><td>level_off</td><td>open</td><td>speed</td><td>visibility</td></tr>
<tr><td>celsius</td><td>fourteen</td><td>light</td><td>operation</td><td>spoilers</td><td>volts</td></tr>
<tr><td>center</td><td>fourth</td><td>lights</td><td>operator</td><td>spray</td><td>vor</td></tr>
<tr><td>change</td><td>foxtrot</td><td>lima</td><td>oscar</td><td>squawk</td><td>vortac</td></tr>
<tr><td>charlie</td><td>freedom</td><td>line</td><td>other</td><td>squawking</td><td></td></tr>
<tr><td>check</td><td>freezing</td><td>link</td><td>out</td><td>stabilizer</td><td><b>W</b></td></tr>
<tr><td>circuit</td><td>frequency</td><td>list</td><td>outer</td><td>stall</td><td>wait</td></tr>
<tr><td>clear</td><td>friday</td><td>localizer</td><td>over</td><td>start</td><td>wake</td></tr>
<tr><td>clearance</td><td>from</td><td>lock</td><td>overcast</td><td>stop</td><td>warning</td></tr>
<tr><td>clearance_delivery</td><td>front</td><td>long</td><td>overspeed</td><td>storm</td><td>watch</td></tr>
<tr><td>climb</td><td>fuel</td><td>look</td><td></td><td>stray</td><td>watts</td></tr>
<tr><td>clock</td><td>full</td><td>low</td><td><b>P</b></td><td>sunday</td><td>way</td></tr>
<tr><td>closed</td><td></td><td>lower</td><td>p_m</td><td>switch</td><td>weather</td></tr>
<tr><td>club</td><td><b>G</b></td><td>lunch</td><td>papa</td><td>system</td><td>wednesday</td></tr>
<tr><td>code</td><td>galley</td><td></td><td>partially</td><td></td><td>welcome</td></tr>
<tr><td>come</td><td>gallons</td><td></td><td>pass</td><td></td><td>west</td></tr>
<tr><td>complete</td><td>gate</td><td></td><td>past</td><td></td><td>whiskey</td></tr>
<tr><td>computer</td><td>gauge</td><td></td><td>patch</td><td></td><td>white</td></tr>
<tr><td>condition</td><td>gear</td><td></td><td>path</td><td></td><td>will</td></tr>
<tr><td>congratulations</td><td>get</td><td></td><td>pause_</td><td></td><td>wind</td></tr>
<tr><td>connect</td><td>glide</td><td></td><td>pellets</td><td></td><td>windows</td></tr>
<tr><td>contact</td><td>go</td><td></td><td>per</td><td></td><td>with</td></tr>
<tr><td>control</td><td>golf</td><td></td><td>percent</td><td></td><td>wrong</td></tr>
<tr><td>converging</td><td>grain</td><td></td><td>phone</td><td></td><td></td></tr>
<tr><td>count</td><td>great</td><td></td><td>pico</td><td></td><td><b>X</b></td></tr>
<tr><td>course</td><td>green</td><td></td><td>plan</td><td></td><td>xray</td></tr>
<tr><td>crane</td><td>greenwich</td><td></td><td>please</td><td></td><td></td></tr>
<tr><td>crosswind</td><td>ground</td><td></td><td>plus</td><td></td><td><b>Y</b></td></tr>
<tr><td>crystals</td><td>gundish</td><td></td><td>point</td><td></td><td>yankee</td></tr>
<tr><td>current</td><td>gusting_to</td><td></td><td>police</td><td></td><td>yellow</td></tr>
<tr><td>cycle</td><td></td><td></td><td>position</td><td></td><td>yesterday</td></tr>
<tr><td>cylinder</td><td></td><td></td><td>power</td><td></td><td>you</td></tr>
<tr><td></td><td></td><td></td><td>practice</td><td></td><td>your</td></tr>
<tr><td></td><td></td><td></td><td>press</td><td></td><td></td></tr>
<tr><td></td><td></td><td></td><td>pressure</td><td></td><td><b>Z</b></td></tr>
<tr><td></td><td></td><td></td><td>private</td><td></td><td>zed</td></tr>
<tr><td></td><td></td><td></td><td>probe</td><td></td><td>zero</td></tr>
<tr><td></td><td></td><td></td><td>programming</td><td></td><td>zone</td></tr>
<tr><td></td><td></td><td></td><td>props</td><td></td><td>zulu</td></tr>
<tr><td></td><td></td><td></td><td>pull</td><td></td><td></td></tr>
<tr><td></td><td></td><td></td><td>pumps</td><td></td><td></td></tr>
<tr><td></td><td></td><td></td><td>push</td><td></td><td></td></tr>
</table>

### Female voice

<table>
<tr><td>f_a_m</td><td>f_fifteen</td><td>f_good</td><td>f_oh</td><td>f_sixteen</td><td>f_time</td></tr>
<tr><td>f_afternoon</td><td>f_fifty</td><td>f_is</td><td>f_one</td><td>f_ten</td><td>f_twelve</td></tr>
<tr><td>f_eight</td><td>f_five</td><td>f_morning</td><td>f_p_m</td><td>f_the</td><td>f_twenty</td></tr>
<tr><td>f_eighteen</td><td>f_forty</td><td>f_nine</td><td>f_seven</td><td>f_thirteen</td><td>f_two</td></tr>
<tr><td>f_eleven</td><td>f_four</td><td>f_nineteen</td><td>f_seventeen</td><td>f_thirty</td><td></td></tr>
<tr><td>f_evening</td><td>f_fourteen</td><td>f_oclock</td><td>f_six</td><td>f_three</td><td></td></tr>
</table>

### Word endings

Add an ending after a word: `voice: KEY UP _S`.

<table>
<tr><td>_ed</td><td>_er</td><td>_s</td><td>_teen</td><td>_th</td><td>_ty</td></tr>
</table>
