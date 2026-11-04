# usrp-rc — Repeater Controller for MMDVM-Host

A compiled C daemon that runs on the same SBC as MMDVM-Host (FM analog mode)
and acts as both the repeater controller and network bridge. It replaces a
separate AllStar node or remote controller process with a single
single-threaded, no-dependency binary.

`usrp-rc` has two ports, each speaking the native protocol of what's on the
other end:

- **mmdvm** — MMDVM-Host's **FM Network** protocol, on loopback (the
  "repeater" side). `usrp-rc` takes the place of G4KLX's `fmgateway` here;
  you do not run `fmgateway` as well.
- **link** — **USRP** PCM or Opus to/from a `rusrp` peer, `usrp-reflector`,
  or any ASL-compatible endpoint (the network side)

Between those two ports sits a full repeater controller — IDs, courtesy
tones, hang timer, timeout, squelch tail elimination, CW/voice/tone
messages — generated entirely locally, with no network dependency for
controller behavior.

> **Note on the mmdvm-side protocol.** Earlier development connected to
> MMDVM-Host over USRP. MMDVM-Host's 2026 restructuring removed native USRP
> support and moved FM to its own tagged protocol plus a separate gateway
> program, so `usrp-rc` now speaks that protocol directly. Audio parameters
> are unchanged (8 kHz, 20 ms frames); only the framing on the loopback
> differs. The link side is unaffected and still USRP.

## How it works

```
MMDVM-Host (loopback)                  usrp-rc                    Network
  FM Network ──► [mmdvm RX] ──┬────────────────────────────► [link TX]  (STE-delayed uplink)
                              │        repeater                USRP
                              │        controller
  FM Network ◄── [mmdvm TX] ◄─┴──────┬─────────────────────  [link RX]  (jitter-buffered downlink)
                                     │                         USRP
                      controller audio (IDs, CT, timeout, hang)
```

- **COR** is derived from MMDVM-Host's explicit start/end-of-transmission
  markers — no hardware GPIO, no hidraw. MMDVM-Host handles all radio
  hardware and CTCSS decode.
- **STE** (squelch tail elimination) delays mmdvm-RX audio by `ste_delay_ms`
  before repeating it, on *both* the local mmdvm-TX repeat leg and the
  link-TX uplink, so the squelch-crash tail at the end of a transmission
  is dropped rather than repeated or forwarded. Key-up/unkey signaling is
  not delayed — only the audio is, with silence filling the gap until the
  delay line catches up.
- **Leading-edge trim** mutes the first `lead_trim_ms` of mmdvm-RX audio in
  each transmission, on the same two legs, for the noise MMDVM-Host can
  pass just before the carrier is fully up. The trimmed span goes out as
  silence, so it adds no latency and doesn't shift frame timing.
- The repeater controller treats keyup from *either* port as access: a
  network caller opens the repeater exactly like a local radio caller, and
  gets a distinct courtesy tone (`ct_link_message`) so listeners can tell
  local and network-originated traffic apart.

## Build dependencies

```bash
sudo apt-get install -y build-essential pkg-config libsystemd-dev libopus-dev
```

## Building

```bash
make
```

To install system-wide (binary, example config, vocab, systemd unit):

```bash
sudo make install
```

## Configuration

See [MANUAL.md](MANUAL.md) for the full user manual, including DTMF commands and a
reference to every setting.

Copy `usrp-rc.conf.sample` to `/etc/usrp-rc/usrp-rc.conf` (or pass a path on
the command line) and edit for your site. The format is plain `key = value`
lines under `[section]` headers, with no quotes, and `#` starts a comment. Any
setting you leave out takes its default, so a typical config is short:

```ini
callsign = W1XYZ

[link]
remote_host = 127.0.0.1
remote_port = 41001

[events]
initial_ids     = my_voice
mandatory_ids   = my_cw
ct_link_on_message      = yellow_jacket
ct_link_message = bumble_bee
timeout_message = timeout_warn

[messages]
my_voice = voice: THISIS {call} REPEATER
my_cw    = cw: {call}/R
```

`usrp-rc.conf.sample` is the full reference, with every option and its default.

### Checking a config

```bash
usrp-rc --check /etc/usrp-rc/usrp-rc.conf
```

`usrp-rc` refuses to start if the config has errors, and reports all of them
at once, with line numbers:

```text
config: line 12: timers.hnag: unknown setting (did you mean timers.hang?)
config: line 20: events.ct_link_on_message: no message named 'yelow_jacket' (did you mean 'yellow_jacket'?)
config: line 31: messages.my_voice: no voice clip named 'THSIS'
```

Unknown or misplaced settings, wrong types, out-of-range values, and references
to messages or voice clips that don't exist are all errors. Run `--check` from
the same directory the service uses, since `tones.conf`, `user_8k/` and
`vocab_8k/` are looked for there before the installed copies. An empty
`events.mandatory_ids` is only a warning, in case the site IDs some other way.

### Converting an old `usrp-rc.toml`

```bash
python3 tools/toml2conf.py usrp-rc.toml usrp-rc.conf   # needs Python 3.11+
usrp-rc --check usrp-rc.conf
```

Settings carry over as written, including any the old parser quietly ignored,
so `--check` shows those by line number. Messages identical to a catalog tone
are left out. Comments don't carry over, and it won't overwrite an existing
file. `sudo make install` runs this for you if it finds a
`/etc/usrp-rc/usrp-rc.toml` and no `usrp-rc.conf`.

### `[mmdvm]` — the loopback link to MMDVM-Host

```ini
[mmdvm]
local_address = 127.0.0.1
local_port    = 4810
rpt_address   = 127.0.0.1
rpt_port      = 3810
```

These are the defaults, so most sites can leave this section out. `usrp-rc`
binds `local_port` to receive from MMDVM-Host and sends to
`rpt_address:rpt_port`. In `MMDVM-Host.ini` the mapping is reversed —
`LocalPort` is what MMDVM-Host *binds*, `GatewayPort` is what it *sends to*:

```ini
[FM Network]
Enable=1
LocalPort=3810       ; must equal usrp-rc's rpt_port
GatewayPort=4810     ; must equal usrp-rc's local_port
```

Two more requirements on the MMDVM-Host side:

- **`[FM] LinkMode=1`.** This strips MMDVM-Host and the modem of their own
  kerchunk / hang / courtesy-tone / ID logic so `usrp-rc` owns the repeater.
  Without it you get two controllers fighting each other.
- **Do not run `fmgateway`.** `usrp-rc` occupies that role. Running both
  means two programs bound to the same gateway port.

### `[link]` — the network peer (rusrp, usrp-reflector, ASL)

```ini
[link]
enabled     = true
remote_host = 127.0.0.1
remote_port = 41001
local_port  = 34002
codec       = pcm     # pcm or opus (narrowband SILK, 8 kHz)
```

Set `enabled = false` to run as a standalone local repeater with no network
bridge at all — no link socket is created, and nothing else in `[link]`
matters.

### `[audio]` — levels and padding

`master_gain` is a final multiplier applied to all generated controller
audio (tones, CW, voice) — set it once for your site's modulator/deviation
headroom (a sustained tone reads much "louder" in FM deviation than speech
at the same digital peak, so start well below 1.0 — MMDVM-Host's own
`[FM Network] TXAudioGain` is a factor here too), then balance individual sounds
against each other with `morse_level`, `voice_level`, and `tone_level`
within that range. Also covers STE delay and pre/post-message padding — see
`usrp-rc.conf.sample` for the full list with defaults.

### `[timers]` — hang, courtesy-tone delay, timeout, ID interval

Mirrors standard analog repeater controller terminology: `hang`, `ct_delay`,
`kerchunk` (anti-kerchunk COR hold), `timeout` (TOT), `id_interval`,
`id_anxious`.

`tail_every` and `tail_min_interval` set when `events.tail_messages` plays, always at
the end of a hang. It never keys the transmitter on its own:

- `tail_every = 10` plays it on every 10th tail, just before the carrier drops.
  A keyup cuts it off, and that still counts as played.
- `tail_min_interval = 60` plays it no more often than every 60 seconds, however
  often the count comes up.

Each time a hang ends, the message plays if both are met, counted from when it last
started. Otherwise that tail ends normally. Nothing queues up.

### `[dtmf]` — over-the-air commands

Off unless you set `enabled = true`. DTMF is decoded from the repeater
receiver only; tones from the link are not decoded.

```ini
[dtmf]
enabled         = true
pad_test_prefix = 5
```

Commands follow the three-tier structure of ACC controllers: user commands,
control operator commands behind a prefix, and programming behind an unlock.
Only the user tier has commands so far. Programming is done in this file.

| Command | Enter | Result |
| --- | --- | --- |
| Pad test | `5`, then digits | reads the digits back (`*` is spoken "Sierra") |
| Time | `39` | speaks the time |
| Usage | `38` | speaks key-ups and minutes of traffic over the last 24 hours |
| Link on | `72` | connects the link; says "link on" |
| Link off | `73` | disconnects the link; says "link off" |

All of these codes can be changed with `pad_test_prefix`, `time_code`, `stats_code`,
`link_on_code` and `link_off_code`. An empty value turns that command off.

While the link is off, nothing is sent to it and anything received from it is
ignored. It returns to `[link] start_on` (true by default) when `usrp-rc` restarts.
To hear the difference on the air, set `ct_link_off_message` under `[events]`: local
traffic gets that courtesy tone while the link is off, and `ct_link_on_message` while it's on.

The time comes from the system clock and timezone, which are yours to keep right.

Enter a command in one transmission, then unkey. Over the air:

- `D` evaluates what you've entered without waiting for the unkey, for when a
  carrier won't drop.
- `#` cancels what you've entered so far, and you can carry on with a new command.

While you enter digits:

- The first valid tone mutes repeat audio, replacing it with a beep (100 ms on,
  900 ms off by default). The mute releases `mute_hang_ms` after the last tone,
  and a later tone starts it again. With `audio.ste_delay_ms` at 0, the start of
  the first tone is repeated before it can be muted. A delay of about 60 ms or
  more hides it, as long as `mute_hang_ms` is longer than the delay.
- The first valid tone also ends the transmission on the link, and the link stays
  down until you unkey, so neither the digits nor anything you say afterward goes
  out over the network. Audio from before the first tone has already gone.

A reply plays in the tail, ahead of the courtesy tone, and keying up interrupts
it. Commands aren't accepted during a timeout lockout.

A sequence is discarded if a tone is held past `max_tone_ms`, if digits are more
than `interdigit_ms` apart (only what came before the gap is lost), or if the
transmission continues more than `end_of_tx_ms` after the last digit. If voice
ever sets off false digits, `min_tone_ms` is the first setting to raise. At
`log.level = info` the log shows when a sequence starts and how it ended; `debug` also
logs each digit and tones that were too
short.

### `[events]` — which message plays for each occasion

```ini
[events]
initial_ids     = my_voice
mandatory_ids   = my_cw
ct_link_on_message      = yellow_jacket
ct_link_message = bumble_bee
timeout_message = timeout_warn
```

Maps occasions (startup, initial/mandatory/anxious/impolite ID, courtesy
tones, timeout, DTMF command replies) to message names. `initial_ids`, `mandatory_ids` and `tail_messages` take a
comma-separated list and rotate through it. An empty value plays nothing.

### `[stats]` — usage figures

`usrp-rc` counts key-ups and the time spent repeating traffic, local or from the link,
in hourly buckets over a rolling week. Kerchunks and timed-out transmissions don't
count. The `{keyups_*}` and `{minutes_*}` variables speak these figures, for a message
or the usage command. Rewording the reply is a matter of redefining `stats_readback`.

The figures survive restarts in `file` (`/var/local/usrp-rc/stats` by default), which is
saved every 15 minutes if anything changed and at shutdown. A power failure loses at
most that much. The file holds one week at most and never grows past that. Set `file`
empty to keep them in memory only. If the file can't be written, usage still works until
the next restart and a warning is logged. The service file only allows writes to
`/var/local/usrp-rc`, so a `file` anywhere else also needs its own `ReadWritePaths`.

### `[schedule]` — changing events by time and day

```ini
[messages]
net_tonight = voice: NET TONIGHT AT SEVEN THIRTY
net_ct      = cw: N

[schedule]
net_reminder = wed 07:00-19:25 tail_messages=net_tonight
net_running  = wed 19:30-20:30 ct_link_on_message=net_ct ct_link_off_message=net_ct
```

Each line is `name = DAYS HH:MM-HH:MM setting=value ...`, and any `[events]` setting
except `startup_message` can be changed. While the window is open its settings are
in force; when it closes they go back by themselves, so there is no second entry to
write. A restart in the middle of a window picks it straight back up.

- `DAYS` is `sun` to `sat`, `daily`, `weekdays` or `weekends`, comma-separated.
- Times are 24-hour, from the system clock and timezone. The window includes its
  start minute but not its end minute.
- A window that ends before it starts runs past midnight, and belongs to the day it
  started on: `fri 22:00-02:00` is still open at 01:00 on Saturday.
- If windows overlap, the later line wins.
- Lists like `mandatory_ids` take no spaces here (`mandatory_ids=id1,id2`), and
  an empty value turns a setting off (`tail_messages=`).

Changes take effect at the top of each minute, and `log.level = info` logs each window
starting and ending.

### `[messages]` — the actual audio content

Each line names a message, made of `cw:`, `voice:`, and `tone:` parts:

```ini
[messages]
my_voice = voice: THISIS {call} REPEATER
my_cw    = cw: {call}/R
my_tone  = tone: 330:50 495:50 660:50
my_mix   = voice: THISIS {call} | tone: 880:100 | cw: {call}/R
```

- **Variables** are spoken as of when the message plays, each as its own word in a
  `voice:` part: `{time}`, `{ampm}`, `{greeting}` (morning, afternoon or evening),
  `{day}`, `{date}` (month and day), `{keyups_day}`, `{keyups_week}`, `{minutes_day}`
  and `{minutes_week}`. Time and greeting use the female `F_` clips; the rest use the
  standard ones. `usrp-rc --check` confirms every clip they need exists.
- **`{call}`** is replaced by the top-level `callsign` setting, which holds only
  the callsign (letters and digits). Add `/R`, `REPEATER`, or anything else in
  the message itself. CW sends it as written; voice spells it out, so `W1XYZ`
  plays the clips `W ONE X Y Z`.
- **`voice:`** takes clip names separated by spaces, played back to back. `_`
  inserts a pause of `audio.voice_gap_ms` and `_400` a 400 ms pause. Only `_`
  followed solely by digits or nothing counts, so a clip named `_TEEN` is
  unaffected.
- **`cw:`** takes the text to send. Spaces are real Morse word gaps.
- **`tone:`** takes notes played in order:

  | You write | You get |
  |---|---|
  | `660:50` | 660 Hz for 50 ms |
  | `500+700:100` | 500 Hz and 700 Hz together for 100 ms |
  | `_30` | 30 ms of silence |
  | `1200:60@0.9` | this note at level 0.9 instead of `audio.tone_level` |

- **`|`** joins parts into one message.
- A message can hold up to 32 elements, where each note counts as one and each
  `cw:` or `voice:` part as one. A `voice:` part holds up to 32 words and pauses.

**Tone catalog**: [`tones.conf`](tones.conf) is installed with `usrp-rc` and its
tones are always available by name, e.g. `ct_link_on_message = yellow_jacket`. It's a
readable text file listing each tone's notes and what it sounds like. To make
your own tone, add it under `[messages]` in your config; using a catalog name
replaces that tone. Up to 64 messages are available, counting the catalog's 28.

**Name length**: message names, clip names, and anywhere you reference a
message name (`ct_link_on_message`, `initial_ids`, etc.) are capped at 31 characters,
and a longer value is a config error. This does not apply to `cw:` text.

`access_mode` (top-level setting, default `cor`) is accepted but `cor_ctcss`
has no functional effect in this build: the mmdvm link carries only
start/end-of-transmission, so there's no independent CTCSS signal to gate
on — CTCSS decode is MMDVM-Host's job (`[FM] CTCSSFrequency` / `AccessMode`).
It's present for schema completeness and possible future use.

## Running

```bash
./build/usrp-rc usrp-rc.conf.sample
# or, once installed:
sudo systemctl enable --now usrp-rc
journalctl -u usrp-rc -f
```

## Vocabulary

`vocab_8k/` ships pre-built with the repo — a normal clone and build never
needs any extra vocabulary tooling.

`user_8k/` ships with the repo (and gets installed to
`/usr/local/share/usrp-rc/user_8k/` — not `/etc/`, since these are data
files, not configuration) ready for your own clips — no need to create it or
guess permissions. To add a clip (or replace a stock one), drop an 8 kHz
mono 16-bit WAV file in, matching `vocab_8k/`'s naming convention (the WAV
filename, minus `.wav`, is the clip name used in `voice:` messages —
see `[messages]` above). Files in `user_8k/` take priority over stock
clips of the same name, so you can override one word without touching the
shipped set. See
[user_8k/README.md](user_8k/README.md) for the same detail in one place.

## Not implemented (by design, v1)

No CM119/ALSA/hidraw, no Unix socket monitoring API, no CTCSS encode or
software decode, no IAX2, and exactly two fixed ports (no multi-port linking).

## Upgrading

See [UPGRADE.md](UPGRADE.md) for what changes between versions.

## Contributing / internals

See [DEVELOPMENT.md](DEVELOPMENT.md) for project origin, target-platform
notes, and how to regenerate the built-in vocabulary from source audio.
