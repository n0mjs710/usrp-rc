# usrp-rc — USRP Repeater Controller

A compiled C daemon that runs on the same SBC as MMDVMHost (FM analog mode)
and acts as both the repeater controller and network bridge. It replaces a
separate AllStar node or remote controller process with a single
single-threaded, no-dependency binary.

`usrp-rc` has two USRP ports:

- **mmdvm** — USRP PCM loopback to/from MMDVMHost (the "repeater" side)
- **link** — USRP PCM or Opus to/from a `rusrp` peer, `usrp-reflector`, or
  any ASL-compatible endpoint (the network side)

Between those two ports sits a full repeater controller — IDs, courtesy
tones, hang timer, timeout, squelch tail elimination, CW/voice/tone
messages — generated entirely locally, with no network dependency for
controller behavior.

## How it works

```
MMDVMHost (loopback)                   usrp-rc                    Network
  USRP PCM  ──► [mmdvm RX] ──┬─────────────────────────────► [link TX]  (STE-gated uplink)
                             │        repeater
                             │        controller
  USRP PCM  ◄── [mmdvm TX] ◄─┴──────┬──────────────────────  [link RX]  (jitter-buffered downlink)
                                    │
                     controller audio (IDs, CT, timeout, hang)
```

- **COR** is derived from the USRP keyup bit — no hardware GPIO, no hidraw.
  MMDVMHost handles all radio hardware and CTCSS decode.
- The repeater controller treats keyup from *either* port as access: a
  network caller opens the repeater exactly like a local radio caller, and
  gets a distinct courtesy tone (`ct_link_message`) so listeners can tell
  local and network-originated traffic apart.

## Build dependencies

```bash
sudo apt-get install -y build-essential pkg-config libsystemd-dev libopus-dev
```

The TOML parser ([tomlc99](https://github.com/cktan/tomlc99), MIT) is
vendored — no separate install step.

## Building

```bash
make
```

To install system-wide (binary, example config, vocab, systemd unit):

```bash
sudo make install
```

## Configuration

Copy `usrp-rc.toml.sample` to `/etc/usrp-rc/usrp-rc.toml` (or pass a path as
the first argument) and edit for your site. `usrp-rc.toml.sample` is the
full reference — every option, commented — the sections below just explain
what each part is for.

### `[mmdvm]` — the loopback link to MMDVMHost

```toml
[mmdvm]
local_address = "127.0.0.1"
local_port    = 34001
rpt_address   = "127.0.0.1"
rpt_port      = 32001
```

Set MMDVMHost's USRP **Local Port** to usrp-rc's `rpt_port`, and its
**Gateway/RPT Port** to usrp-rc's `local_port`.

### `[link]` — the network peer (rusrp, usrp-reflector, ASL)

```toml
[link]
enabled       = true
remote_host   = "127.0.0.1"
remote_port   = 41001
local_port    = 34002
codec         = "pcm"   # "pcm" or "opus" (narrowband SILK, 8 kHz)
```

Set `enabled = false` to run as a standalone local repeater with no network
bridge at all — no link socket is created, and nothing else in `[link]`
matters.

### `[audio]` — levels and padding

`master_gain` is a final multiplier applied to all generated controller
audio (tones, CW, voice) — set it once for your site's modulator/deviation
headroom (a sustained tone reads much "louder" in FM deviation than speech
at the same digital peak, so start well below 1.0 — MMDVMHost's own USRP
audio gain setting is a factor here too), then balance individual sounds
against each other with `morse_level`, `voice_level`, and per-tone `amp`
within that range. Also covers STE delay and pre/post-message padding — see
`usrp-rc.toml.sample` for the full list with defaults.

### `[timers]` — hang, courtesy-tone delay, timeout, ID interval

Mirrors standard analog repeater controller terminology: `hang`, `ct_delay`,
`kerchunk` (anti-kerchunk COR hold), `timeout` (TOT), `id_interval`,
`id_anxious`.

### `[events]` — which message plays for each occasion

```toml
[events]
initial_ids  = ["default_voice"]
mandatory_ids = ["default_cw"]
ct_message    = "yellow_jacket"
ct_link_message = "bumble_bee"
timeout_message = "timeout_warn"
```

Maps occasions (startup, initial/mandatory/anxious/impolite ID, courtesy
tones, timeout) to message names defined under `[messages.*]`. Several keys
(`initial_ids`, `mandatory_ids`) take a list and rotate through it.

### `[messages.*]` — the actual audio content

Each message is a named sequence of `cw` / `voice` / `tone` elements, mixed
freely, referenced by name from `[events]` above:

```toml
[messages.default_cw]
elements = [{type = "cw", text = "W1XYZ/R"}]

[messages.default_voice]
elements = [{type = "voice", clip = "THIS IS W ONE X Y Z REPEATER"}]

[messages.yellow_jacket]
elements = [
  {type = "tone", freq1 = 330.0, freq2 = 0.0, ms = 50, amp = 0.5},
  {type = "tone", freq1 = 495.0, freq2 = 0.0, ms = 50, amp = 0.5},
  {type = "tone", freq1 = 660.0, freq2 = 0.0, ms = 50, amp = 0.5},
]
```

- A `voice` element's `clip` holds one or more clip names separated by
  spaces, played back to back (clip names never contain spaces, so this is
  unambiguous). `cw` `text` is *not* split this way — spaces there are real
  Morse word gaps.
- A word `"_"` inserts a pause instead of a clip, using `audio.voice_gap_ms`;
  `"_400"` is an explicit 400 ms pause. Only recognized when `_` is followed
  solely by digits or nothing, so a clip literally named `_TEEN` is
  unaffected. For example, `clip = "THIS IS _ W ONE _400 REPEATING"` plays
  "THIS IS", a default-length pause, "W ONE", a 400 ms pause, then
  "REPEATING".
- `tone` elements take `freq1`/`freq2` (a second frequency mixes in a dual
  tone; 0 = unused), `ms` duration, and `amp` (0.0–1.0).
- A message can hold up to 32 elements; a single `voice` element's word list
  up to 32 words/pauses.

`usrp-rc.toml.sample` ships with a full courtesy-tone catalog
(`[messages.honk]`, `[messages.yellow_jacket]`, etc.) you can reference
directly or use as templates.

**Name length**: message names, clip names, and anywhere you reference a
message name (`ct_message`, `initial_ids`, etc.) are capped at 31
characters — they're short identifiers, not content, and longer values are
silently truncated at load. This does not apply to CW `text`.

`access_mode` (top-level key, default `"cor"`) is accepted but `"cor_ctcss"`
has no functional effect in this build: USRP carries a single keyup bit, so
there's no independent CTCSS signal to gate on — CTCSS decode is
MMDVMHost's job. It's present for schema completeness and possible future
use.

## Running

```bash
./build/usrp-rc usrp-rc.toml.sample
# or, once installed:
sudo systemctl enable --now usrp-rc
journalctl -u usrp-rc -f
```

## Vocabulary

`vocab_8k/` ships pre-built with the repo — a normal clone and build never
needs any extra vocabulary tooling.

`user_8k/` ships with the repo (and gets installed to `/etc/usrp-rc/user_8k/`)
ready for your own clips — no need to create it or guess permissions. To add
a clip (or replace a stock one), drop an 8 kHz mono 16-bit WAV file in,
matching `vocab_8k/`'s naming convention (the WAV filename, minus `.wav`, is
the clip name referenced from `voice` elements — see `[messages.*]` above).
Files in `user_8k/` take priority over stock clips of the same name, so you
can override one word without touching the shipped set. See
[user_8k/README.md](user_8k/README.md) for the same detail in one place.

## Not implemented (by design, v1)

No CM119/ALSA/hidraw, no Unix socket monitoring API, no DTMF decode or
remote control, no CTCSS encode or software decode, no IAX2, and exactly
two fixed ports (no multi-port linking).

## Contributing / internals

See [DEVELOPMENT.md](DEVELOPMENT.md) for project origin, target-platform
notes, and how to regenerate the built-in vocabulary from source audio.
