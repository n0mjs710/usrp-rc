# METAR producer

`usrp-rc-metar` gets the latest METAR for one station from the NOAA Aviation Weather
Center, and writes each field as voice-clip words for usrp-rc. It is a test case for
the external producer interface. usrp-rc does not yet read these files.

## Output

The producer writes one file for each field to `<dir>/<name>/`. The default is
`/run/usrp-rc/ext/wx/`. A message uses a field as `{wx.<field>}`.

| Field | Example |
| --- | --- |
| `source` | L W C. A US station is said without the K. |
| `time` | TIME SEVEN FIFTY TWO A_M |
| `wind` | WIND THREE THREE ZERO AT TWENTY GUSTING_TO TWENTY NINE |
| `visibility` | VISIBILITY EIGHT (statute miles, no unit word) |
| `sky` | MOSTLY CLOUDY CEILING TWO THOUSAND SIX HUNDRED (feet, no unit word) |
| `weather` | LIGHT RAIN SHOWERS. Empty when there is nothing to report. |
| `temp` | TEMPERATURE FORTY TWO (Fahrenheit, no unit word) |
| `dewpoint` | D P THIRTY NINE |
| `humidity` | RELATIVE HUMIDITY EIGHTY NINE PERCENT (calculated) |
| `pressure` | PRESSURE THIRTY POINT ONE TWO (inches of mercury, no unit word) |

- Wind direction is three digits, in degrees. Wind speeds are in miles per hour,
  converted from knots, with no unit word.
- Each file's mtime is the observation time, not the fetch time.
- Each file is written to a temp file, then renamed.
- If the fetch fails, the producer writes nothing and exits with status 1. The files
  then become stale.
- usrp-rc does not speak a variable that has a word with no clip. Thus, the producer
  looks in the installed `user_8k/` and `vocab_8k/`, and writes a field with a missing word as an
  empty file. It records each such field in the log as DROPPED, with the missing words.

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

## Installation (sample)

```sh
sudo install -m 755 usrp-rc-metar /usr/local/bin/
sudo install -m 644 usrp-rc-metar.service usrp-rc-metar.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now usrp-rc-metar.timer
```

How the producer runs is not part of the interface. A timer is one possible method.

## Tests

```sh
python3 -m unittest test_metar
```
