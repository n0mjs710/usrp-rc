# Development notes

Context for anyone working on `usrp-rc` itself, as opposed to running it.
End-user setup and configuration are in [README.md](README.md).

## Design

`usrp-rc` is a single-threaded epoll daemon. `src/port.c` is the repeater state
machine; `src/main.c` owns the sockets and timers and feeds it.

Controller audio (tones, CW, voice clips) is rendered into a buffer when the state
machine decides to play it, then drained as 20 ms / 160-sample frames. Rendering
happens in the main loop and takes a few milliseconds at most; there are no
threads.

Nothing in `port.c` keys or drops PTT directly. `update_ptt()` works it out after
each event: the transmitter is up while a source is repeated, controller audio plays
or has a continuation waiting, an ID plays in the mix, or the tail runs. A timed-out
or downed source isn't repeated, so those rules need nothing of their own. New
controller audio queues through the buffer and gets PTT from the same rule.

The link uplink follows the same pattern in `main.c`: `update_link_tx()` keys the link
while regen plays or the local station is sent to it. Link off, repeater down, an
echo recording and a time-out just leave nothing to send.

## Protocols

Two different protocols, one on each port — see [README.md](README.md) for
the operator-facing view.

- **mmdvm side** — MMDVM-Host's native FM Network protocol
  (`src/fm_protocol.{c,h}`): three-byte ASCII tags, `FMS` (start, carries
  the callsign), `FMD` (audio, S16LE), `FME` (end), `FMP` (5 s keepalive).
  Implemented against **MMDVM-Host's own `FMNetwork.cpp` / `FMControl.cpp`
  as the definitive reference**, not FMGateway's copy — FMGateway's
  receive paths scale S16LE→float by `/65536` where MMDVM-Host uses
  `/32767`, i.e. 6 dB low.

  Two asymmetries worth knowing before touching this code:

  1. **MMDVM-Host accepts only `FMD` from a gateway** — it discards `FMS`
     and `FME` on that direction. There is no key/unkey marker toward the
     modem; PTT is implicit in whether audio is arriving. Stopping the
     `FMD` flow *is* the unkey, which is why a delivery stall and a
     deliberate unkey look identical to the modem. In the other direction
     MMDVM-Host does send `FMS`/`FME`, so receive-side keying is explicit.
  2. **`FMD` payloads are variable-length**, any even count from 2 to 160
     samples — MMDVM-Host forwards whatever the modem handed it that pass.
     Everything downstream here assumes exactly 160, so `src/fm_reframe.h`
     puts the stream back on that grid. It is not a jitter buffer and adds
     no latency of its own.

- **link side** — USRP (`src/usrp_protocol.{c,h}`), PCM or Opus,
  unchanged.

Audio is 8 kHz throughout. Note the ceiling is set by the modem, not by
either protocol: the MMDVM-Host↔modem serial link is 12-bit (packed two
samples per three bytes) and band-limited to 300–2700 Hz, so there is no
audio-quality headroom to be won on the mmdvm side by changing framing.

## DTMF control

`src/dtmf.c` finds tones and `src/command.c` turns them into commands and
mutes. Neither has a clock of its own: both count the 20 ms audio frames they
are fed, so `tests/dtmf_test.c` runs them on synthesized audio with no sockets.
That test also feeds the whole of `vocab_8k/` through the decoder as a
false-detection check on real speech.

The detector taps received audio ahead of the STE delay in `mmdvm_rx_frame()`.
Replies go through `port_queue_reply()` and play from the courtesy tone delay,
the same slot the anxious ID uses.

Every code is a row in `CONFIG_DTMF_CODES` in `src/config.c`: its setting, its kind
(whole code, code + 0/1, capture, or index: code + exactly two digits 0-9) and its
command. `--check` validates from that
list and `command.c` matches against it, so a new command is a new row. Whether a
command may run right now is one rule, `command_allowed()` in `main.c`.

The capture codes (pad test, regen) switch `command.c` into capturing: every key after
them, `#` and `D` included, goes into the entry until the unkey, which is why
`check_consistency()` won't let another code start with one. The read prefix is an
index code, held to the same rule: its two digits pick a `read_NN` message, and an
empty slot is an `ERROR` with no reply. Otherwise an entry
that no code starts with is dropped at the digit that kills it. The mute doesn't
care: any valid tone mutes both paths, so no DTMF leaks. Regen's link copy
is the only controller audio that goes to the link. `main.c` keys the link and paces
it on its own 20 ms deadline (`regen_tick()`), independent of the port's tail, and a
local transmission that starts meanwhile isn't sent to the link.

The echo test is split the same way. `port.c` owns the arming, because only the port
knows when the tail ends (`on_hang()` plays the cancel). `main.c` claims the next local
keyup with `port_take_echo()`, records what it repeats, keeps that transmission off
the link, drops any command in it, and queues the recording as the reply at the unkey.

### Decoder limits

The energy share (0.65) and group dominance (10 dB) limits were set from on-air
measurements on lecompton in November 2026, where voice at `min_tone_ms` 80 still
made false digits. Each tone is the strongest of its nominal bin and bins 1% either
side, so the standard's +-1.5% passes; +-3.5% doesn't. The instrumented decoder is
kept at tag `dtmf-metrics-2026-11`: it logs one `dtmf-metrics:` line per run of a key
that passes loose limits (0.40, 6 dB), with how many windows also pass the real ones.
To measure again, build that tag, or merge main into a branch from it.

| | Energy share | Dominance | Twist (low over high) | Low 2nd harmonic |
| --- | --- | --- | --- | --- |
| Real keys, narrow bins | 0.84 and up | 14.5 dB and up | +2 to +7.5 dB | |
| Real keys, side bins | 0.77 and up | 14 dB and up | +3.3 to +7.2 dB | -23.8 dB or lower |
| Voice (about 110 runs) | mostly 0.40-0.60, at most 0.74 | | | up to +0.2 dB |
| Voice windows passing both limits, side bins | none of about 60 runs | | | |

With narrow bins, one 3-window voice run passed both limits. The others that came
close were probably overdeviated users. Margins to watch if keys stop decoding:
twist reached +7.5 dB against the 8 dB limit, and half balance 0.53 against 0.5. A
2nd-harmonic limit would reject more voice, but keys 6 and B read about -13.6 dB
there, as 1477 Hz leaks into the 2 x 770 Hz bin.

The reference controllers are the ACC RC-96 (command structure and behavior) and
the Arcom RC-210 (mute timer, tail message triggers). Their macro sets, patches,
and multi-port routing are deliberately not the model here.

## State and timers

Every timer is an absolute deadline (0 = off). `main.c` fires its own from the
`TIMERS` table in a fixed order; the port's five (`ct_delay`, hang, the two time-outs,
ID) are one row of it. A new timer is a new row.

The stateful pieces, and where their state lives:

| What | State | Driven by |
| --- | --- | --- |
| Port | `state`: IDLE, ACTIVE, TAIL, TIMEOUT | Keyup edges, port timers, drain continuations |
| Controller audio | `ctrl` buffer, its one continuation `on_drain`, the ID in it, `last_audio` for the gaps | The pacing tick |
| ID under repeat audio | `mix_*` | Each frame sent |
| PTT | Derived: `update_ptt()` | Every port event |
| ID owed | `id_owed`, `id_deadline`, the two rotations | PTT rising, repeated keyups, IDs starting |
| Time-outs | Per source: the flag, the deadline; local `tot_used`/`start` | Keyups, the courtesy tone |
| Tail messages | `tails_since_message`, `tail_last_ms`, `tail_rot` | Each hang's end |
| Reply | `reply`, one deep | Commands; played at `ct_delay` |
| Echo test | Port `echo_armed`, then `main.c` `echo_recording` | Command, next local keyup, unkey or hang's end |
| Repeater down | `down`, and the state file | Repeater command |
| Link on/off | `link_up` (the port's copy picks the courtesy tone) | Link commands |
| Link uplink | Derived: `update_link_tx()`; `local_to_link` per transmission | Keyups, regen, link and down commands |
| Receive, both sides | `prev_*_keyup`, the watchdogs; the reframer, the jitter buffer | Packets, watchdog timers |
| Regen | `regen` buffer, position, deadline | Its own 20 ms row in `TIMERS` |
| DTMF | Decoder debounce; entry digits, capture, mute | Received frames |
| STE | Delay line, lead trim | Received frames |
| Schedule | `schedule_active[]`; `cfg` rebuilt from `base` | Minute timer |
| Producer variables | None in memory: the files are the state. `s_ext_seen[]` only remembers each variable's last state, to log a change once | Each render |
| Usage | Hour buckets | Usage callback, save timer |

## Schedule

`src/schedule.c` is a pure function of the config and a time: it holds no state, so
windows have no start or end events to miss. `main.c` keeps the config as loaded in
`app_t.base` and re-evaluates the schedule every minute into `app_t.cfg`, which every
module points at. So anything read from the config at the time it's used follows the
schedule. Anything cached at startup (sockets, the codec, the DTMF decoder, the STE
buffer, the DTMF cover tone) must be listed in `not_schedulable()` in `src/config.c`.

## External producers

A producer is a separate program that writes clip words to files; `{producer.field}`
in a voice part speaks the file `<external.dir>/<producer>/<field>`. The contract is in
MANUAL section 5.6, and `producers/metar/` is the first one.

`usrp-rc` reads the file when the message renders, in the main loop. That is safe
because the read is bounded (1024 bytes, 32 words) and the default directory is on
tmpfs: nothing here waits on the network or on the producer. A producer that's slow,
dead or never installed just leaves its files stale.

- Freshness is the file's mtime against the producer's lifetime, judged per variable.
  The producer sets the mtime to the time of the data.
- A file with any word that isn't a clip is dropped whole (`ext_read()`): skipping a
  word could change what the report means. Producers check the installed clip
  directories themselves.
- `message_render()` is automatic and never plays a fallback. `message_render_readback()`
  is for DTMF reads. A fallback renders as automatic, so it can't nest.
- The fallback is a message name, not words: only `[messages]` hold clips, so it gets
  the same validation, and `config_prune_messages()` keeps it, like a `read_NN` message.
- Validation at load checks that each variable names a declared producer and that
  each fallback exists. File contents can't be checked at load.

## Usage statistics

`src/stats.c` keeps a rolling week in hourly buckets, keyed by hours since the epoch so
daylight saving can't disturb them. It's the only thing that writes to disk during
normal running. It saves on a timer rather than per key-up to go easy on SD cards, and
uses a temp file and rename so a power failure can't corrupt it.

## Target platform

Developed on a Raspberry Pi 3B+ (similar architecture/OS to the production
target, a NanoPi NEO, which can't run a remote VS Code session). Code is
pushed to GitHub from this dev box and pulled/built on the NanoPi NEO —
there's nothing Pi-specific in the source; it's plain C11 against
`libopus`.

## Regenerating `vocab_8k/` from source audio

`vocab_8k/` (623 clips) is committed to the repo pre-built, converted from
a render of `Vocab_Master.cpp` in
[TI LPC Speech Tools](https://github.com/n0mjs710/lpc) to the 8 kHz mono S16LE both audio paths use —
a normal clone and build never needs `sox` or the 48 kHz source. Only
regenerate it if the reference vocabulary itself changes:

```bash
sudo apt-get install -y sox
make vocab VOCAB_SRC=/path/to/rc/vocab_pcm   # default: /home/cort/rc/vocab_pcm
git add vocab_8k && git commit
```
