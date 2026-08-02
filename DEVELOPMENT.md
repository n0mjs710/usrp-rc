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
drained as 20 ms USRP frames — no on-demand synthesis in the hot path, and
no threads.

## Target platform

Developed on a Raspberry Pi 3B+ (similar architecture/OS to the production
target, a NanoPi NEO, which can't run a remote VS Code session). Code is
pushed to GitHub from this dev box and pulled/built on the NanoPi NEO —
there's nothing Pi-specific in the source; it's plain C11 against
`libopus`/`libsystemd`/vendored `tomlc99`.

## Regenerating `vocab_8k/` from source audio

`vocab_8k/` (712 clips) is committed to the repo pre-built, converted from
a 48 kHz reference vocabulary to the 8 kHz mono S16LE the USRP path uses —
a normal clone and build never needs `sox` or the 48 kHz source. Only
regenerate it if the reference vocabulary itself changes:

```bash
sudo apt-get install -y sox
make vocab VOCAB_SRC=/path/to/rc/vocab_pcm   # default: /home/cort/rc/vocab_pcm
git add vocab_8k && git commit
```
