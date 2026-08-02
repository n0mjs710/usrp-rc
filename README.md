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
- All controller audio (tones, CW, pre-rendered voice clips) is rendered
  into a buffer up front the instant the state machine decides to play it,
  then drained as 20 ms USRP frames — no on-demand synthesis in the hot
  path, and no threads.

See `CLAUDE.md`-equivalent design notes in the project history for the full
state-machine specification; it is a from-scratch C translation of the
Python reference controller at `../rc` (see especially `port.py`).

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
the first argument) and edit for your site. Config sections:

- `[mmdvm]` — local UDP endpoint and MMDVMHost's endpoint for the loopback
  USRP session. Set MMDVMHost's USRP **Local Port** to usrp-rc's `rpt_port`
  and its **Gateway/RPT Port** to usrp-rc's `local_port`.
- `[link]` — the network peer (rusrp, usrp-reflector, ASL). `codec = "pcm"`
  or `"opus"` (narrowband SILK, 8 kHz). Set `enabled = false` to run as a
  standalone local repeater with no network bridge at all — no link socket
  is created.
- `[audio]` — `master_gain` is a final multiplier applied to all generated
  controller audio (tones, CW, voice) — set it once for your site's
  modulator/deviation headroom (a sustained tone reads much "louder" in FM
  deviation than speech at the same digital peak, so start well below
  1.0 — MMDVMHost's own USRP audio gain setting is a factor here too), then
  balance individual sounds against each other with `morse_level`,
  `voice_level`, and per-tone `amp` within that range. Also STE delay,
  pre/post-message padding.
- `[timers]` — hang, ct_delay, kerchunk, timeout (TOT), id_interval,
  id_anxious. Mirrors standard analog repeater controller terminology.
- `[events]` — which named message plays for each occasion (startup,
  initial/mandatory/anxious/impolite ID, courtesy tones, timeout).
- `[messages.*]` — named sequences of `cw` / `voice` / `tone` elements,
  shared by all events. A `voice` element's `clip` can hold several clip
  names separated by spaces (e.g. `"THIS IS W ONE X Y Z REPEATER"`), played
  back to back — clip names never contain spaces, so this is unambiguous.
  `cw` text is *not* split this way; spaces there are real Morse word gaps.
  A word `"_"` inserts a pause instead of a clip: bare `_` uses
  `audio.voice_gap_ms`, `"_400"` is an explicit 400 ms pause. Only
  recognized when `_` is followed solely by digits or nothing, so clip
  names like `_TEEN` are unaffected. A message can hold up to 32 elements,
  and a single `voice` element's word list up to 32 words/pauses.

**Name length**: message names, clip names, and anywhere you reference a
message name (`ct_message`, `initial_ids`, etc.) are capped at 31
characters — they're short identifiers, not content, and longer values are
silently truncated at load. This does not apply to CW `text`.

`access_mode` (top-level key, default `"cor"`) is accepted but `"cor_ctcss"`
has no functional effect in this build: USRP carries a single keyup bit, so
there's no independent CTCSS signal to gate on — CTCSS decode is MMDVMHost's
job. It's present for schema completeness and possible future use.

## Running

```bash
./build/usrp-rc usrp-rc.toml.sample
# or, once installed:
sudo systemctl enable --now usrp-rc
journalctl -u usrp-rc -f
```

## Vocabulary

`vocab_8k/` (712 clips) is pre-built and committed to this repo, converted
from the 48 kHz reference vocabulary to the 8 kHz mono S16LE the USRP path
uses — a normal clone and build never needs `sox` or the 48 kHz source.

To add your own clips, drop 8 kHz mono 16-bit WAV files into `user_8k/` (or
`/etc/usrp-rc/user_8k/` once installed), matching `vocab_8k/`'s naming
convention — they take priority over the stock clips.

To regenerate `vocab_8k/` itself from an updated 48 kHz reference set
(maintainers only — not part of the normal build/install flow):

```bash
sudo apt-get install -y sox
make vocab VOCAB_SRC=/path/to/rc/vocab_pcm   # default: /home/cort/rc/vocab_pcm
git add vocab_8k && git commit
```

## Target platform note

Developed on a Raspberry Pi 3B+ (similar architecture/OS to the production
target, a NanoPi NEO, which can't run a remote VS Code session). Code is
pushed to GitHub from this dev box and pulled/built on the NanoPi NEO —
there's nothing Pi-specific in the source; it's plain C11 against
`libopus`/`libsystemd`/vendored `tomlc99`.

## Not implemented (by design, v1)

No CM119/ALSA/hidraw, no Unix socket monitoring API, no DTMF decode or
remote control, no CTCSS encode or software decode, no IAX2, and exactly
two fixed ports (no multi-port linking). See the project history for
rationale — these mirror the Python reference controller's own roadmap.
