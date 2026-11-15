# NWS alert producer

`usrp-rc-nws` gets the active National Weather Service watches and warnings for a list
of counties and zones from `api.weather.gov`, and writes them as voice-clip words for
usrp-rc.

This is a sample producer. It shows how a producer can open a schedule window. It is
not part of usrp-rc, and usrp-rc does not need it. For the producer interface, see
MANUAL.md Appendix C.

## Output

The producer writes one file, `<dir>/<name>/alerts`. The default is
`/run/usrp-rc/ext/nws/alerts`. A message uses it as `{nws.alerts}`.

The file has one phrase for each active alert: the event words, then the area label.
A pause (`_`) separates the phrases. Example:

```
TORNADO WARNING D G _ TORNADO WATCH D G _ TORNADO WATCH S N
```

- With no active alert on the event list, the file is empty. An empty file closes a
  window that uses `{nws.alerts}`.
- The phrases are in the sequence of the event list, then of the area list.
- A watch and a warning in the same county are both said. Each is true: the watch is
  for the whole county, the warning is for part of it.
- An event that is active more than one time for a label is said one time. Thus a
  label such as `AREA` for several counties gives `TORNADO WATCH AREA` one time.
- usrp-rc reads a maximum of 32 words from a field. If the phrases have more words, the
  producer writes only the phrases that fit, from the start of the list, and records
  `LEFT OUT` in the log.
- The file's mtime is the time of the run.
- A word with no clip is recorded in the log as `NO CLIP`. usrp-rc does not play a field
  that has a word with no clip. Add the clip to `user_8k/`, or change the words.

### When the fetch fails

The producer keeps each alert that it gets in `<dir>/<name>/.cache.json`, until the time
that the alert ends. If a fetch fails, the producer writes the alerts from the cache
that have not ended. If there are none, it writes nothing, and the file becomes stale.

Thus a short loss of the network does not stop a warning. The window closes when the
last known alert ends, or when the file becomes stale.

## Counties and zones

The NWS uses two types of code for an area:

| Code | Example | Use |
| --- | --- | --- |
| County | `KSC045` | Tornado and severe thunderstorm warnings and watches |
| Forecast zone | `KSZ040` | Advisories, for example fog, wind, heat and winter weather |

An alert uses only one type. To get all alerts for a county, give both codes. To find
the codes for a location, use its latitude and longitude:

```sh
curl -s -A usrp-rc-nws https://api.weather.gov/points/38.97,-95.24 | grep -E '"(county|forecastZone)"'
```

The last part of each URL is the code.

## Options

| Option | Purpose |
| --- | --- |
| `--area 'WORDS=CODES'` | A label, and the codes it covers, for example `'D G=KSC045,KSZ040'`. Required. Repeatable. To say one label for all counties, give each code to that label: `'AREA=KSC045,KSZ040,KSC177,KSZ039'`. |
| `--event 'EVENT=WORDS'` | An NWS event name and its words, for example `'Tornado Warning=TORNADO WARNING'`. Repeatable. The sequence sets the sequence of the phrases. |
| `--dir`, `--name` | Output directory and producer name |
| `--print` | Print the phrases. Write no files. |
| `--wav FILE` | Also join the clips into one WAV file, to listen to |
| `--json FILE` | Read an API response from a file, not the network |
| `--vocab DIR` | Clip directory, user clips first. Repeatable. The default is the installed usrp-rc directories, `/usr/local/share/usrp-rc/user_8k` and `vocab_8k`. |

The default event list is:

```
Tornado Warning=TORNADO WARNING
Severe Thunderstorm Warning=SEVERE THUNDERSTORM WARNING
Tornado Watch=TORNADO WATCH
Severe Thunderstorm Watch=SEVERE THUNDERSTORM WATCH
```

An `--event` option replaces the full default list. The names must agree with the NWS
event names. For the list, see `https://api.weather.gov/alerts/types`.

## Installation

The producer runs from a systemd timer, one time each minute. usrp-rc examines schedule
windows one time each minute, so a faster timer gives no advantage. For the function of
the `.service` and `.timer` files, see the METAR producer README.

### 1. Install the producer

From this directory:

```sh
sudo install -m 755 usrp-rc-nws /usr/local/bin/
sudo install -m 644 usrp-rc-nws.service usrp-rc-nws.timer /etc/systemd/system/
sudo systemctl edit --full usrp-rc-nws.service     # set the --area options on ExecStart
sudo systemctl daemon-reload
```

### 2. Run it one time, and examine the output

```sh
sudo systemctl start usrp-rc-nws.service
journalctl -u usrp-rc-nws -n 5 --no-pager
cat /run/usrp-rc/ext/nws/alerts
```

The journal shows `nws: WRITTEN, 0 active: none` when there is no alert. To examine the
words for an alert type that is active now somewhere else, give its zone code and event:

```sh
usrp-rc-nws --print --area 'AREA=KSZ013' --event 'Dense Fog Advisory=FOG'
```

### 3. Start the timer

```sh
sudo systemctl enable --now usrp-rc-nws.timer
```

### 4. Add the window and messages to usrp-rc

```ini
[external]
nws = 300

[messages]
weather_alert = voice: ALERT _ {nws.alerts}

[schedule]
alert = {nws.alerts} announce=weather_alert tail_messages+=weather_alert tail_every=1 tail_min_interval=60
```

- The window opens when the file has words, and closes when it is empty or stale.
- `announce=` keys the transmitter and plays the message one time when the window opens.
  If a station is talking, the message waits for the end of the transmission.
- While the window is open, the message plays on each tail, but not more than one time
  each 60 seconds. When the window closes, the tail settings go back to their usual values.
- If the alerts change while the window is open, for example a warning after a watch,
  the next tail message says the new alerts. The window does not open again, thus
  `announce=` does not play again.
- Put this window after the other windows in the file. The last window has priority for a
  setting that two windows change.

Then examine the configuration and restart:

```sh
usrp-rc --check /etc/usrp-rc/usrp-rc.conf && sudo systemctl restart usrp-rc
```

For more information, see MANUAL.md section 7.9 and Appendix C.

## Tests

```sh
python3 -m unittest test_nws
```
