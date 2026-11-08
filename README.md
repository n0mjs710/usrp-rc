# usrp-rc: Repeater Controller for MMDVM-Host

`usrp-rc` is a repeater controller and network bridge for an MMDVM-Host FM repeater. It
operates on the same computer as MMDVM-Host. It replaces FMGateway and the controller
functions of MMDVM-Host.

For operation and the settings reference, see [MANUAL.md](MANUAL.md).

Version: 1.0b (beta). To update from an earlier `main`, see [UPGRADE.md](UPGRADE.md).

> [!WARNING]
> **MMDVM-Host FM audio quality.** At the date of this document (November 2026),
> MMDVM-Host has defects that decrease the audio quality of FM mode. The symptoms are
> crackles, distortion and an incorrect frequency response (EQ). The defects are worst
> on the network and with `[FM] LinkMode=1`, because these two functions use the same
> code path. For the best performance, use the patched versions of MMDVM and MMDVM-Host
> at [github.com/n0mjs710](https://github.com/n0mjs710).

## Contents

- [Description](#description)
- [Functional overview](#functional-overview)
- [Requirements](#requirements)
- [Installation](#installation)
- [MMDVM-Host integration](#mmdvm-host-integration)
- [Configuration](#configuration)
- [Operation](#operation)
- [Voice clips](#voice-clips)
- [Limits and exclusions](#limits-and-exclusions)
- [Related documents](#related-documents)

## Description

`usrp-rc` is one compiled C program. It operates in one thread. It has two ports:

| Port | Protocol | Connects to |
| --- | --- | --- |
| mmdvm | MMDVM-Host FM Network protocol, on the loopback interface | MMDVM-Host on the same computer |
| link | USRP, with PCM or Opus audio | A USRP reflector or hub, or an AllStarLink (ASL) node in USRP mode |

> **Note:** At this time, Opus audio operates only with other tools from the same author.

The controller functions are:

- Station identification: initial, mandatory, anxious and impolite IDs
- Courtesy tones, with different tones for local traffic, link traffic and link off
- Hang timer, kerchunk filter and time-out timer
- Squelch tail elimination
- CW, voice and tone messages, and tail messages
- Clock and calendar schedule
- DTMF commands, DTMF regen to send commands over the link, and an echo test
- Usage statistics

All controller functions operate locally. They do not need a network connection or a
remote ASL server.

> **Note:** Earlier versions connected to MMDVM-Host with USRP. In 2026, MMDVM-Host
> removed its USRP support. FM moved to its own protocol and a separate gateway program.
> `usrp-rc` uses that protocol directly. The audio format did not change: 8 kHz, 20 ms
> frames. The link port continues to use USRP.

## Functional overview

```text
MMDVM-Host (loopback)          usrp-rc                         Network

 FM Network ─► [mmdvm RX] ─┬─────────────────────────► [link TX] USRP
                           │                           STE-delayed uplink
                           │   repeater controller
                           │
 FM Network ◄─ [mmdvm TX] ◄┴─────┬───────────────────── [link RX] USRP
                                 │                     jitter-buffered
             controller audio (IDs, CT, time-out, hang)
```

| Audio | Source | Destination |
| --- | --- | --- |
| Local receive audio | mmdvm RX | mmdvm TX (repeat) and link TX (uplink) |
| Link receive audio | link RX, through a jitter buffer | mmdvm TX |
| Controller audio | `usrp-rc` | mmdvm TX |

### Carrier detection

MMDVM-Host sends a start marker and an end marker for each received transmission.
`usrp-rc` uses these markers as COR. MMDVM-Host controls all radio hardware. MMDVM-Host
also does CTCSS encode and decode.

### Access

A key-up on either port gives access to the repeater. A link station opens the repeater
the same as a local station. Link traffic gets its own courtesy tone, `ct_link_message`.
Thus, listeners can hear the source of the traffic.

Local traffic has priority over link traffic. During a local transmission, the repeater
does not transmit link audio. For details, see [MANUAL.md](MANUAL.md#82-traffic-priority),
section 8.2.

### Squelch tail elimination

`audio.ste_delay_ms` delays local receive audio. The delay applies to the repeat path and
to the link uplink. At the end of a transmission, `usrp-rc` discards the delayed audio.
Thus, the squelch tail and DTMF blips do not go out.

The delay applies to audio only. Key-up and unkey are not delayed. At the start of a
transmission, silence fills the gap until the delayed audio arrives.

### Leading-edge trim

`audio.lead_trim_ms` mutes the start of local receive audio in each transmission. The
trim applies to the repeat path and to the link uplink. It removes noise that MMDVM-Host
can send before the carrier audio is stable.

The muted part goes out as silence. Thus, the trim adds no delay and does not change the
frame timing.

## Requirements

| Item | Requirement |
| --- | --- |
| Computer | The Linux computer that operates MMDVM-Host |
| Operating system | Linux with systemd |
| MMDVM-Host | A version with the FM Network protocol (2026 or later) |
| Packages | `build-essential`, `pkg-config`, `libopus-dev` |

## Installation

### Install the program

1. Install the packages:

   ```bash
   sudo apt-get install -y build-essential pkg-config libopus-dev
   ```

2. Build the program:

   ```bash
   make
   ```

3. Install the program, the data files and the service:

   ```bash
   sudo make install
   sudo systemctl daemon-reload
   ```

### Installed files

| Path | Contents |
| --- | --- |
| `/usr/local/bin/usrp-rc` | Program |
| `/etc/usrp-rc/usrp-rc.conf` | Configuration |
| `/etc/usrp-rc/usrp-rc.conf.sample` | Sample configuration, with all settings and their defaults |
| `/usr/local/share/usrp-rc/tones.conf` | Tone catalog |
| `/usr/local/share/usrp-rc/vocab_8k/` | Stock voice clips |
| `/usr/local/share/usrp-rc/user_8k/` | Site voice clips |
| `/usr/local/lib/systemd/system/usrp-rc.service` | systemd service |
| `/var/local/usrp-rc/` | Usage statistics and the repeater down state |

> **Note:** `make install` does not replace a configuration file that exists. If no
> configuration exists, it copies the sample to `usrp-rc.conf`.

> **Note:** `make install` replaces all stock voice clips in `vocab_8k/`. It removes
> stock clips that are no longer in the repository. Put site clips in `user_8k/` only.
> `make install` does not change `user_8k/`.

### Remove the program

```bash
sudo make uninstall
```

This command removes the program, the service, the stock voice clips and the tone
catalog. It keeps the configuration, the usage statistics and the site voice
clips.

## MMDVM-Host integration

### Port settings

`usrp-rc` takes the place of FMGateway. The `[mmdvm]` settings in `usrp-rc` have the same
names and functions as the `[Network]` settings in `FMGateway.ini`. The names in
`MMDVM-Host.ini` are opposite.

| `usrp-rc` `[mmdvm]` | `FMGateway.ini` `[Network]` | `MMDVM-Host.ini` `[FM Network]` | Function |
| --- | --- | --- | --- |
| `local_address` | `LocalAddress` | `GatewayAddress` | Address on which `usrp-rc` receives |
| `local_port` | `LocalPort` | `GatewayPort` | Port on which `usrp-rc` receives |
| `rpt_address` | `RptAddress` | `LocalAddress` | Address of MMDVM-Host |
| `rpt_port` | `RptPort` | `LocalPort` | Port of MMDVM-Host |

The `usrp-rc` defaults are `127.0.0.1`, `local_port = 4810` and `rpt_port = 3810`. If
`MMDVM-Host.ini` uses the same values, the `[mmdvm]` section is not necessary. If not, set
the ports to agree with `MMDVM-Host.ini`.

### Procedure

1. In `MMDVM-Host.ini`, set `LinkMode=1` in the `[FM]` section.

   This setting removes almost all controller functions from MMDVM-Host and the modem:
   kerchunk, hang, courtesy tone, ID and other functions. `usrp-rc` then controls the
   repeater.

   > **CAUTION:** If `LinkMode` is not 1, two controllers operate the same repeater.

2. In `MMDVM-Host.ini`, make sure that `[FM Network]` has `Enable=1`. Record the address
   and port values. Example, from a site that uses other ports:

   ```ini
   [FM Network]
   Enable=1
   LocalAddress=127.0.0.1
   LocalPort=20012
   GatewayAddress=127.0.0.1
   GatewayPort=20013
   ```

3. In `/etc/usrp-rc/usrp-rc.conf`, set `[mmdvm]` to agree. For the example in step 2:

   ```ini
   [mmdvm]
   local_port = 20013
   rpt_port   = 20012
   ```

   If FMGateway operated before, copy `LocalPort` and `RptPort` from `[Network]` in
   `FMGateway.ini`.

4. Stop FMGateway, and make sure that it does not start again at boot. `usrp-rc` uses its
   port.

   > **CAUTION:** Do not operate FMGateway and `usrp-rc` at the same time.

5. Restart MMDVM-Host.

### Audio levels

1. Set `audio.master_gain` for the deviation of the transmitter. This setting changes the
   level of all controller audio: tones, CW and voice.
2. Start with `master_gain` well below 1.0. A steady tone gives more deviation than
   speech at the same digital peak level.
3. Set `morse_level`, `voice_level` and `tone_level` to balance the sounds against each
   other.
4. Measure the deviation with a deviation meter, or adjust by ear.

> **Note:** In `MMDVM-Host.ini`, `[FM Network]` `TXAudioGain` also changes the level of
> controller audio. `PreEmphasis=1` increases high frequencies to the transmitter.

## Configuration

The configuration file is `/etc/usrp-rc/usrp-rc.conf`. It contains `key = value` lines
under `[section]` headers. `#` starts a comment. A setting that is not in the file has its
default value.

This configuration is complete for a linked repeater:

```ini
callsign = W1XYZ

[link]
remote_host = 127.0.0.1
remote_port = 41001

[events]
initial_ids          = my_voice
mandatory_ids        = my_cw,my_voice
impolite_id          = my_cw
ct_link_off_message  = yellow_jacket
ct_link_on_message   = yellow_jacket
ct_link_message      = bumble_bee
timeout_message      = timeout_warn

[messages]
my_voice = voice: THISIS {call} REPEATER
my_cw    = cw: {call}/R
```

For the file format, all settings, DTMF commands, messages, events and the schedule, see
[MANUAL.md](MANUAL.md).

### Apply a change

To apply a change and to read configuration errors, see
[MANUAL.md](MANUAL.md#33-apply-a-change), section 3.3. `usrp-rc` does not start if the
configuration has errors.

## Operation

| Task | Command |
| --- | --- |
| Start the service, and start it at each boot | `sudo systemctl enable --now usrp-rc` |
| Stop the service | `sudo systemctl stop usrp-rc` |
| Restart the service | `sudo systemctl restart usrp-rc` |
| Monitor the log | `journalctl -u usrp-rc -f` |

At startup, the log shows the number of messages defined and the number in use.

### Test from the source directory

```bash
./build/usrp-rc usrp-rc.conf.sample
```

The program operates in the foreground. It uses `tones.conf` and `vocab_8k/` from the
source directory. To stop it, push Ctrl+C.

> **CAUTION:** Stop the service before this test. The service and the test use the same
> ports.

## Voice clips

| Directory | Contents |
| --- | --- |
| `vocab_8k/` | Stock voice clips. They are in the repository. No other tools are necessary. |
| `user_8k/` | Site voice clips. A clip in this directory replaces a stock clip with the same name. |

Clip requirements:

- WAV file, 8 kHz, mono, 16-bit
- 3 minutes maximum
- The file name without `.wav` is the clip name.

For the preparation of recordings, see [user_8k/README.md](user_8k/README.md). For the use
of clips in messages, see [MANUAL.md](MANUAL.md).

### Origin of the stock vocabulary

The stock voice is the Texas Instruments LPC speech vocabulary. For decades, this voice
has been familiar on amateur repeaters through ACC controllers and others that used TI
speech chips.

The clips in `vocab_8k/` are not recordings of a controller. Each word was rendered
directly from its LPC data by an emulation of the TMS5220 speech synthesizer, from the
[TI LPC Speech Tools](https://github.com/n0mjs710/lpc) project:

- The synthesis core is a port of the MAME `tms5220` core. Its ROM tables were verified
  against decapped TMS5220 and TMS5220C dies. It reproduces the behavior of the chip at
  the sample level, to the 8-bit DAC output.
- The vocabulary, `Vocab_Master.cpp`, has 623 words. It was assembled from the LPCSpeech
  AVR, FF-800 and Talkie vocabularies. Where more than one recording of a word existed,
  the best one was selected by listening. Misnamed words were corrected.

In a side-by-side comparison with an ACC controller, the two voices cannot be told apart.

## Limits and exclusions

This version does not include:

- CM119, ALSA or hidraw radio interfaces
- A Unix socket interface for monitoring
- CTCSS encode, or CTCSS decode in software
- IAX2
- More than two ports, or linking of more than one network port

## Related documents

| Document | Contents |
| --- | --- |
| [MANUAL.md](MANUAL.md) | Operation and settings reference: DTMF commands, messages, events, schedule and all settings |
| [UPGRADE.md](UPGRADE.md) | Changes between versions |
| [DEVELOPMENT.md](DEVELOPMENT.md) | Program internals, project origin and the rebuild of the stock vocabulary |
| [user_8k/README.md](user_8k/README.md) | Preparation of voice clips |
| [usrp-rc.conf.sample](usrp-rc.conf.sample) | All settings, with their defaults |
