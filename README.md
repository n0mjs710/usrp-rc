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

### `[mmdvm]` — the loopback link to MMDVM-Host

```toml
[mmdvm]
local_address = "127.0.0.1"
local_port    = 4810
rpt_address   = "127.0.0.1"
rpt_port      = 3810
```

`usrp-rc` binds `local_port` to receive from MMDVM-Host and sends to
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
at the same digital peak, so start well below 1.0 — MMDVM-Host's own
`[FM Network] TXAudioGain` is a factor here too), then balance individual sounds
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
has no functional effect in this build: the mmdvm link carries only
start/end-of-transmission, so there's no independent CTCSS signal to gate
on — CTCSS decode is MMDVM-Host's job (`[FM] CTCSSFrequency` / `AccessMode`).
It's present for schema completeness and possible future use.

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

`user_8k/` ships with the repo (and gets installed to
`/usr/local/share/usrp-rc/user_8k/` — not `/etc/`, since these are data
files, not configuration) ready for your own clips — no need to create it or
guess permissions. To add a clip (or replace a stock one), drop an 8 kHz
mono 16-bit WAV file in, matching `vocab_8k/`'s naming convention (the WAV
filename, minus `.wav`, is the clip name referenced from `voice` elements —
see `[messages.*]` above). Files in `user_8k/` take priority over stock
clips of the same name, so you can override one word without touching the
shipped set. See
[user_8k/README.md](user_8k/README.md) for the same detail in one place.

## Not implemented (by design, v1)

No CM119/ALSA/hidraw, no Unix socket monitoring API, no DTMF decode or
remote control, no CTCSS encode or software decode, no IAX2, and exactly
two fixed ports (no multi-port linking).

## Contributing / internals

See [DEVELOPMENT.md](DEVELOPMENT.md) for project origin, target-platform
notes, and how to regenerate the built-in vocabulary from source audio.
