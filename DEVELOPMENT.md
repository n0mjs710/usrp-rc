# Development notes

Context for anyone working on `usrp-rc` itself, as opposed to running it.
End-user setup and configuration are in [README.md](README.md).

## Origin

`usrp-rc` is a from-scratch C translation of the Python reference
repeater controller at `../rc` (see especially `port.py`) — the state
machine, timer semantics, and event names all mirror that implementation
deliberately. See the project history for the full state-machine
specification.

Controller audio (tones, CW, pre-rendered voice clips) is rendered into a
buffer up front the instant the state machine decides to play it, then
drained as 20 ms / 160-sample frames — no on-demand synthesis in the hot
path, and no threads.

## Protocols

Two different protocols, one on each port — see [README.md](README.md) for
the operator-facing view.

- **mmdvm side** — MMDVM-Host's native FM Network protocol
  (`src/fm_protocol.{c,h}`): three-byte ASCII tags, `FMS` (start, carries
  the callsign), `FMD` (audio, S16LE), `FME` (end), `FMP` (5 s keepalive).
  Implemented against **MMDVM-Host's own `FMNetwork.cpp` / `FMControl.cpp`
  as the definitive reference**, not `fmgateway`'s copy — fmgateway's
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

## Target platform

Developed on a Raspberry Pi 3B+ (similar architecture/OS to the production
target, a NanoPi NEO, which can't run a remote VS Code session). Code is
pushed to GitHub from this dev box and pulled/built on the NanoPi NEO —
there's nothing Pi-specific in the source; it's plain C11 against
`libopus`/`libsystemd`/vendored `tomlc99`.

## Regenerating `vocab_8k/` from source audio

`vocab_8k/` (712 clips) is committed to the repo pre-built, converted from
a 48 kHz reference vocabulary to the 8 kHz mono S16LE both audio paths use —
a normal clone and build never needs `sox` or the 48 kHz source. Only
regenerate it if the reference vocabulary itself changes:

```bash
sudo apt-get install -y sox
make vocab VOCAB_SRC=/path/to/rc/vocab_pcm   # default: /home/cort/rc/vocab_pcm
git add vocab_8k && git commit
```
