# usrp-rc Operation and Reference Manual

This manual applies to the `dtmf-control` branch of `usrp-rc`. For installation and
MMDVM-Host integration, see [README.md](README.md).

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
| Window | A schedule line that changes events during a time period |

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
| `5` + digits | Pad test | The digits. Use this command to test the DTMF tones of a radio. |

### 2.2 Special keys

| Key | Function |
| --- | --- |
| `#` | Cancels the digits entered. A new command can then be entered. |
| `D` | Executes the command immediately. The repeater does not wait for the unkey. Use this key if a carrier does not drop. |

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

An entry that does not agree with a command has no effect. No error sound plays. The
repeater does not accept commands when it is timed out.

### 2.5 Pad test

Enter `5` and then the digits to test. The repeater says the digits.

| Key | Reply |
| --- | --- |
| `0` to `9` | The digit |
| `*` | "Sierra" |
| `A`, `B`, `C` | The letter |
| `#`, `D` | Not available. These keys cancel and execute. |

### 2.6 Repeater sounds

| Sound | Meaning |
| --- | --- |
| Courtesy tone | The transmission has ended. The next station can transmit. Different tones can show local traffic, link traffic and link off. |
| Beep, one time each second | A station enters a DTMF command. |
| Short falling tones | The time-out timer stopped a long transmission. Unkey to reset the timer. |

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
config: line 20: events.ct_link_on_message: no message named 'yelow_jacket' (did you mean 'yellow_jacket'?)
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
   |  Longer than timers.timeout: time-out (see 6.3).
 Carrier stops
   |  timers.ct_delay
 Replies and the anxious ID play, if they are pending.
   |
 Courtesy tone. The time-out timer resets here.
   |  timers.hang
 Tail message plays, if it is due.
   |
 Transmitter stops
```

A key-up during the delay or the hang starts repetition again. The time-out timer then
continues from its last value.

| Timer | Default | Function |
| --- | --- | --- |
| `kerchunk` | 0.5 s | Minimum carrier time for a full transmission |
| `ct_delay` | 0.5 s | Time from carrier stop to the courtesy tone |
| `hang` | 2.5 s | Time from the end of the courtesy tone to transmitter stop |
| `timeout` | 180 s | Maximum transmission time |

> **Note:** The log value `ptt: OFF ... duration` includes the carrier time, `ct_delay`,
> the courtesy tone and `hang`.

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
| `mandatory_ids` | When the ID timer (`timers.id_interval`) expires and the repeater is not in use | Yes |
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

#### Key-up during an ID

| ID type | Result of a key-up |
| --- | --- |
| Voice | The ID stops. The impolite ID plays in its place. |
| CW | The ID continues under the repeated audio. |

### 6.2 Courtesy tones

| Setting | Plays after |
| --- | --- |
| `ct_link_on_message` | Local traffic with the link on. If `ct_link_off_message` is empty, also local traffic with the link off or with no link. |
| `ct_link_off_message` | Local traffic with the link off |
| `ct_link_message` | Link traffic. If this setting is empty, the two tones above apply. |

A courtesy tone can be any message, also a voice message. A different tone for link off
shows the link state to listeners.

### 6.3 Time-out

| Setting | Plays |
| --- | --- |
| `timeout_message` | When the time-out timer stops a transmission |
| `timeout_cancel_message` | When the carrier stops after a time-out, and the repeater operates again |

### 6.4 Command replies

| Setting | Plays | Default |
| --- | --- | --- |
| `time_message` | Reply to the time command | `time_readback` |
| `stats_message` | Reply to the usage command | `stats_readback` |
| `link_on_message` | Reply to the link-on command | `link_on_reply` |
| `link_off_message` | Reply to the link-off command | `link_off_reply` |
| `link_none_message` | Reply to a link command when the repeater has no link | `link_none_reply` |

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
| `timers.tail_every` | Minimum number of tails since the last tail message. `1` is each tail. `0` disables tail messages. Default: 10. |
| `timers.tail_min_interval` | Minimum number of seconds since the last tail message started. `0` is no limit. Default: 0. |

At the end of each hang, the message plays if both conditions are true. If not, the tail
ends as usual.

- A tail message does not wait in a queue. It plays a maximum of one time in each tail.
- If a key-up interrupts a tail message, the message counts as played.

Example: set `tail_every = 1` and `tail_min_interval = 300`. A tail message then plays
at the end of a hang if 5 minutes have passed since the last tail message started.

---

## 7. Schedule

### 7.1 Function

A schedule window changes `[events]` settings for a time period. When the window closes,
the settings go back to their configured values. No second line is necessary.

```ini
[messages]
net_tonight = voice: NET TONIGHT AT SEVEN THIRTY
net_ct      = cw: N

[schedule]
net_reminder = wed 07:00-19:25 tail_messages=net_tonight
net_running  = wed 19:30-20:30 ct_link_on_message=net_ct ct_link_off_message=net_ct
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
| Settings | Any `[events]` setting, but not `startup_message`. An empty value disables the setting: `tail_messages=`. Lists use commas with no spaces: `mandatory_ids=id1,id2`. |
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

### 7.5 Timing

- Windows start and stop at the start of each minute.
- The schedule uses the system clock and time zone. Keep the clock and time zone of the
  computer correct.
- The schedule stores no data. After a restart during a window, the window applies again
  immediately.

### 7.6 Limits

| Item | Limit |
| --- | --- |
| Windows | 32 |
| Settings in one window | 8 |
| Calendar days in one window | 8 |
| Names in a list | 8. `--check` shows a warning if `+=` can add more. Names after the eighth are not added. |
| Length of one window | 24 hours |

### 7.7 Test a window

1. Add a window that opens 2 minutes after the current time.
2. Apply the change. See [3.3](#33-apply-a-change).
3. Monitor the log. At `log.level = info`, the log shows these lines:

```text
schedule: 'net_reminder' started
tail: 'net_tonight'
schedule: 'net_reminder' ended
```

### 7.8 Functions not available

- A window cannot change `[timers]` or `[link]` settings. Thus, a window cannot turn the
  link on or off.
- A change without an end time is not available.
- "Last Friday" and one-time dates are not available.

---

## 8. Link

| Setting | Effect |
| --- | --- |
| `enabled = false` | The repeater has no link. `70`, `72` and `73` reply with `link_none_message`: "No link service". |
| `start_on = false` | The link is off at start. `72` turns it on. |

- When the link is off, `usrp-rc` sends nothing to the link. It ignores all audio from
  the link.
- `usrp-rc` does not keep the link state after a restart. At each start, the link state
  is the value of `start_on`.
- To hear the link state without a change, use the link-status command, `70`.
- To show the link state with each courtesy tone, set a different `ct_link_off_message`.
  See [6.2](#62-courtesy-tones).
- At this time, each station can use `72` and `73`. A control operator level is planned.
- At this time, `codec = opus` operates only with other tools from the same author.

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

- A code can contain `0` to `9`, `*`, `A`, `B` and `C`. The maximum length is 7
  characters.
- `#` and `D` are not available in codes. They cancel and execute.
- An empty code disables its command. In the example above, `stats_code` is empty.
- A full code has priority over the pad test prefix. Thus, `51` can be a command, also
  with prefix `5`.

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

### 9.6 Log

At `log.level = info`, the log shows the start and the result of each entry:

```text
dtmf: sequence started with '3'
dtmf: '38': ACCEPTED command = usage
```

| Result | Meaning | Example |
| --- | --- | --- |
| `ACCEPTED` | The command executed. | `'5123': ACCEPTED command = DTMF test + 123` |
| `ERROR` | The entry is complete, but no command can execute. | `'99': ERROR no command matched` |
| | A link command on a repeater with no link. The reply is "No link service". | `'70': ERROR command = link status, no link service` |
| `ABANDONED` | The entry stopped before it was complete. | `'12#': ABANDONED user terminated` |

An `ABANDONED` line also names the timer that expired, if a timer expired. At
`log.level = debug`, the log also shows each digit and each tone that was too short.

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
configuration error.

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
| `timeout` | `180` | 1-3600 | Time-out timer. It resets at the courtesy tone. |
| `id_interval` | `600` | 1-86400 | ID timer. A value above 600 (the FCC limit of 10 minutes) causes a warning. |
| `id_anxious` | `60` | 0-3600 | Anxious ID period before the ID timer expires. Must be less than `id_interval`. |
| `tail_every` | `10` | 0-1000 | Tails between tail messages. 0 disables tail messages. |
| `tail_min_interval` | `0` | 0-86400 | Minimum seconds between tail messages. 0 is no limit. |

### 11.6 `[audio]`

| Setting | Default | Range | Function |
| --- | --- | --- | --- |
| `master_gain` | `1.0` | 0-10 | Last multiplier on all controller audio. Set it for the transmitter deviation. |
| `morse_wpm` | `20` | 5-60 | CW speed, in words per minute |
| `morse_pitch` | `700` | 100-3400 | CW frequency, in Hz |
| `morse_level` | `0.9` | 0-2 | CW level |
| `impolite_morse_level` | `0.3` | 0-2 | CW level for an impolite ID |
| `voice_level` | `0.9` | 0-2 | Voice clip level |
| `tone_level` | `0.5` | 0-2 | Level of tones without `@LEVEL`, and of the DTMF beep |
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
| `pad_test_prefix` | `5` | 7 keys maximum | Pad test prefix. Empty is off. |
| `time_code` | `39` | 7 keys maximum | Time command. Empty is off. |
| `stats_code` | `38` | 7 keys maximum | Usage command. Empty is off. |
| `link_on_code` | `72` | 7 keys maximum | Link-on command. Empty is off. |
| `link_off_code` | `73` | 7 keys maximum | Link-off command. Empty is off. |
| `link_status_code` | `70` | 7 keys maximum | Link-status command. Empty is off. |
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

### 11.9 `[events]`

All `[events]` settings are message names. Empty plays nothing.

| Setting | Default | Lists | Section |
| --- | --- | --- | --- |
| `startup_message` | Empty | No | [6.5](#65-startup-message) |
| `initial_ids` | Empty | Yes | [6.1](#61-identification) |
| `mandatory_ids` | Empty | Yes | [6.1](#61-identification) |
| `anxious_id` | Empty | No | [6.1](#61-identification) |
| `impolite_id` | None. Necessary. | No | [6.1](#61-identification) |
| `ct_link_on_message` | Empty | No | [6.2](#62-courtesy-tones) |
| `ct_link_off_message` | Empty | No | [6.2](#62-courtesy-tones) |
| `ct_link_message` | Empty | No | [6.2](#62-courtesy-tones) |
| `timeout_message` | Empty | No | [6.3](#63-time-out) |
| `timeout_cancel_message` | Empty | No | [6.3](#63-time-out) |
| `tail_messages` | Empty | Yes | [6.6](#66-tail-messages) |
| `time_message` | `time_readback` | No | [6.4](#64-command-replies) |
| `stats_message` | `stats_readback` | No | [6.4](#64-command-replies) |
| `link_on_message` | `link_on_reply` | No | [6.4](#64-command-replies) |
| `link_off_message` | `link_off_reply` | No | [6.4](#64-command-replies) |
| `link_none_message` | `link_none_reply` | No | [6.4](#64-command-replies) |

A list holds a maximum of 8 names.

### 11.10 `[messages]` and `[schedule]`

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
ct_link_on_message  = yellow_jacket
ct_link_off_message = comet
ct_link_message     = bumble_bee
```

### 12.3 Announce the time in the tail on weekends

```ini
[messages]
weekend_time = voice: F_GOOD {greeting} _ F_THE F_TIME F_IS {time} {ampm}

[schedule]
weekend_tail = weekends 08:00-20:00 tail_messages=weekend_time
```

### 12.4 Speak the usage for 7 days with command `38`

```ini
[messages]
stats_readback = voice: {keyups_week} KEY UP _S _ {minutes_week} MINUTES
```

### 12.5 Disable tail messages at night

```ini
[events]
tail_messages = my_tail

[messages]
my_tail = voice: WELCOME TO {call}

[schedule]
quiet_nights = daily 22:00-06:00 tail_messages=
```

### 12.6 Change the courtesy tone and the ID during a net

```ini
[messages]
net_ct = cw: N
net_id = voice: THISIS {call} NET

[schedule]
net = wed 19:30-20:30 ct_link_on_message=net_ct ct_link_off_message=net_ct mandatory_ids=net_id
```

### 12.7 Turn on the link only by command

```ini
[link]
remote_host = 127.0.0.1
start_on    = false
```

### 12.8 Delay the courtesy tone by 640 ms

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
| `a variable needs the voice clip` | A stock clip is missing. | Do `sudo make install` to install the missing stock clips. |
| A tail message does not play | `tail_every` is 0, or the conditions are not true. | Look for the `tail_every` warning. On a quiet repeater, the `tail_every` count can be not yet reached. `tail_min_interval` can also prevent the message. |
| A window opens at the wrong time | The clock or the time zone is wrong. | Examine the clock and time zone with `timedatectl`. |
| Usage statistics reset at restart | `usrp-rc` cannot write the file. | Look for `stats: can't save` in the log. Make sure that `/var/local/usrp-rc` exists and the service file is current. Then do `sudo systemctl daemon-reload`. |
| No audio from the link | The link is off. | Look for `73` in the log, or for `start_on = false`. |

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
case-sensitive: `repeater` and `REPEATER` are the same clip.

| Clip type | Function |
| --- | --- |
| `F_` clips | Female voice. The time variables use these clips. |
| Clips that start with `_` | Word endings, for example `_S` and `_TEEN`. They are clips, not pauses. A pause is `_` with only digits after it. |

### Standard voice

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

### Female voice

```text
F_AFTERNOON F_A_M F_EIGHT F_EIGHTEEN F_ELEVEN F_EVENING F_FIFTEEN F_FIFTY
F_FIVE F_FORTY F_FOUR F_FOURTEEN F_GOOD F_IS F_MORNING F_NINE F_NINETEEN
F_OCLOCK F_OH F_ONE F_P_M F_SEVEN F_SEVENTEEN F_SIX F_SIXTEEN F_TEN F_THE
F_THIRTEEN F_THIRTY F_THREE F_TIME F_TWELVE F_TWENTY F_TWO
```
