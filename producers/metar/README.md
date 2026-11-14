# METAR producer

`usrp-rc-metar` gets the latest METAR for one station from the NOAA Aviation Weather
Center, and writes each field as voice-clip words for usrp-rc.

This is a sample producer. It operates on the K0USY and WA0EDA repeaters, and shows
what a producer can do. It is not part of usrp-rc, and usrp-rc does not need it. For
the producer interface, see MANUAL.md Appendix C.

## Output

The producer writes one file for each field to `<dir>/<name>/`. The default is
`/run/usrp-rc/ext/wx/`. A message uses a field as `{wx.<field>}`.

| Field | Example |
| --- | --- |
| `source` | L W C. A US station is said without the K. |
| `time` | TIME SEVEN FIFTY TWO A_M. Minutes under ten use the letter O: SEVEN O FIVE. |
| `wind` | WIND THREE THREE ZERO AT TWENTY GUSTING_TO TWENTY NINE |
| `visibility` | VISIBILITY EIGHT (statute miles, no unit word) |
| `sky` | BROKEN CEILING TWO THOUSAND SIX HUNDRED (feet, no unit word). CLEAR, SCATTERED, BROKEN or OVERCAST; FEW is said as CLEAR. |
| `weather` | LIGHT RAIN SHOWERS. Empty when there is nothing to report. |
| `temp` | TEMPERATURE FORTY TWO DEGREES (Fahrenheit) |
| `dewpoint` | DO POINT THIRTY NINE DEGREES (DO sounds like DEW) |
| `humidity` | WATER EIGHTY NINE PERCENT (relative humidity, calculated) |
| `pressure` | PRESSURE THIRTY POINT ONE TWO (inches of mercury, no unit word) |

- Each field is complete by itself, with its label, so a message can use one field
  alone, for example `voice: {wx.temp}` as a tail message. When the data is stale, the
  fallback plays in place of the field. With no fallback, the field is silent, and a
  message of only that field does not play.
- Wind direction is three digits, in degrees. Wind speeds are in miles per hour,
  converted from knots, with no unit word.
- Each file's mtime is the observation time, not the fetch time.
- Each file is written to a temp file, then renamed.
- If the fetch fails, the producer writes nothing and exits with status 1. The files
  then become stale.
- usrp-rc drops a variable that has a word with no clip, and plays the fallback. The
  producer looks in the installed `user_8k/` and `vocab_8k/`, and records each field with
  a missing word in the log as `NO CLIP`, with the missing words.

## Options

| Option | Purpose |
| --- | --- |
| `--station` | ICAO station identifier. Required. |
| `--dir`, `--name` | Output directory and producer name |
| `--source-words` | Words for `source`, for example the name of the airport. The default is the station letters, without the K of a US station. |
| `--pressure-word` | Label for `pressure`. The default is `PRESSURE`; `ALTIMETER` is also possible. |
| `--print` | Print the fields. Write no files. |
| `--wav FILE` | Also join the clips of all fields into one WAV file, to listen to |
| `--json FILE` | Read a report from a file, not the network |
| `--vocab DIR` | Clip directory, user clips first. Repeatable. The default is the installed usrp-rc directories, `/usr/local/share/usrp-rc/user_8k` and `vocab_8k`. |

## Installation

The producer runs from a systemd timer. This uses two files with the same base name:

- `usrp-rc-metar.service` tells how to run the producer: the command, the station and
  the security settings. `Type=oneshot` means that it runs one time, writes its files and
  stops. It is not a daemon.
- `usrp-rc-metar.timer` tells when to run it: 30 s after boot, then each 10 minutes.
  When the timer fires, systemd starts the service with the same name.

Enable the timer, not the service. The service has no `[Install]` section, because only
the timer starts it.

How the producer runs is not part of the interface. A timer is one possible method; cron
or a loop in a script also work.

### 1. Install the producer

From this directory. Set `STATION` to your ICAO station identifier first:

```sh
STATION=KXYZ
sudo install -m 755 usrp-rc-metar /usr/local/bin/
sudo install -m 644 usrp-rc-metar.service usrp-rc-metar.timer /etc/systemd/system/
sudo sed -i "s/--station KLWC/--station $STATION/" /etc/systemd/system/usrp-rc-metar.service
sudo systemctl daemon-reload
```

### 2. Run it one time, and examine the output

```sh
sudo systemctl start usrp-rc-metar.service
journalctl -u usrp-rc-metar -n 5 --no-pager
namei -l /run/usrp-rc/ext/wx/temp && cat /run/usrp-rc/ext/wx/temp
```

- The journal shows `metar: KXYZ WRITTEN`. A `DROPPED` line names a field with a word
  that has no clip. That field stays silent.
- `namei` shows `drwxr-xr-x` directories down to the file. Each user must be able to read
  the files, also when `usrp-rc` does not run as root.

### 3. Start the timer

```sh
sudo systemctl enable --now usrp-rc-metar.timer
systemctl list-timers usrp-rc-metar.timer
```

`list-timers` shows the last run and the next run.

### 4. Add the messages to usrp-rc

Add these lines to `/etc/usrp-rc/usrp-rc.conf`. If the file already has a `[dtmf]` or
`[messages]` section, put the lines in it. DTMF commands also need `[dtmf] enabled = true`.

```ini
[external]
wx = 5400

[dtmf]
read_17 = weather

[messages]
weather = voice: WEATHER {wx.source} _ {wx.time} _ {wx.wind} _ {wx.visibility} _ {wx.sky} {wx.weather} _ {wx.temp} _ {wx.dewpoint} _ {wx.humidity} _ {wx.pressure}
```

A fallback is optional: `wx = 5400 wx_unavailable`. It plays once in place of each field
that is not current, so it suits a message of one field better than this one.

Then examine the configuration and restart:

```sh
usrp-rc --check /etc/usrp-rc/usrp-rc.conf && sudo systemctl restart usrp-rc
```

Enter `417` on the air. The log shows `ACCEPTED command = read 17` and a `FRESH` line for
each field. For more information, see MANUAL.md section 5.7 and Appendix C.

### Useful commands

| Command | Function |
| --- | --- |
| `systemctl list-timers usrp-rc-metar.timer` | The last run and the next run |
| `sudo systemctl start usrp-rc-metar.service` | Run the producer now, outside the schedule |
| `journalctl -u usrp-rc-metar` | The output of each run |
| `sudo systemctl disable --now usrp-rc-metar.timer` | Stop the scheduled runs |

## Tests

```sh
python3 -m unittest test_metar
```
