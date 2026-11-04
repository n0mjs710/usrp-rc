#!/usr/bin/env python3
"""Convert an old usrp-rc.toml to the line-based usrp-rc.conf format.

    toml2conf.py usrp-rc.toml [usrp-rc.conf]

Writes to stdout if no output path is given. Settings carry over exactly as
written, including any the old parser quietly ignored, so `usrp-rc --check`
on the result shows them by line number. Messages identical to one in
tones.conf are dropped, since the catalog now provides them. Comments are not
carried over. Your callsign can't be picked out of a message reliably, so
switching IDs over to {call} is left to you.

Needs Python 3.11 or newer for tomllib.
"""
import os
import sys
import tomllib

TONE_LEVEL_DEFAULT = 0.5
OLD_TONE_DEFAULTS = {"freq1": 0.0, "freq2": 0.0, "ms": 80, "amp": 0.8}
SECTIONS = ["log", "mmdvm", "link", "audio", "timers", "events"]

# Settings that have since been renamed.
RENAMED = {("events", "ct_message"): "ct_link_on_message"}


def warn(text):
    print(f"toml2conf: {text}", file=sys.stderr)


def num(value):
    return f"{value:.10g}"


def scalar(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, float):
        return num(value)
    if isinstance(value, list):
        return ",".join(scalar(v) for v in value)
    return str(value)


def tone_token(elem, where):
    for key in elem:
        if key != "type" and key not in OLD_TONE_DEFAULTS:
            warn(f"{where}: dropped '{key}', which the old parser ignored")
    tone = {**OLD_TONE_DEFAULTS, **{k: v for k, v in elem.items() if k in OLD_TONE_DEFAULTS}}
    if not tone["freq1"] and not tone["freq2"]:
        return f"_{tone['ms']}"
    token = num(tone["freq1"])
    if tone["freq2"]:
        token += f"+{num(tone['freq2'])}"
    token += f":{tone['ms']}"
    if tone["amp"] != TONE_LEVEL_DEFAULT:
        token += f"@{num(tone['amp'])}"
    return token


def convert_message(name, table):
    parts = []
    tones = []

    def flush_tones():
        if tones:
            parts.append("tone: " + " ".join(tones))
            tones.clear()

    for i, elem in enumerate(table.get("elements", [])):
        where = f"messages.{name}.elements[{i}]"
        kind = elem.get("type")
        if kind in ("tone", "ct"):
            tones.append(tone_token(elem, where))
            continue
        flush_tones()
        if kind == "cw":
            parts.append(f"cw: {elem.get('text', '')}".rstrip())
        elif kind == "voice":
            parts.append(f"voice: {' '.join(elem.get('clip', '').split())}".rstrip())
        else:
            warn(f"{where}: dropped unknown type '{kind}', which the old parser skipped")
    flush_tones()
    return " | ".join(parts)


def read_catalog():
    """Catalog messages as {name: content}, from beside this script or the source tree."""
    here = os.path.dirname(os.path.abspath(__file__))
    for path in (os.path.join(here, "tones.conf"), os.path.join(here, "..", "tones.conf")):
        if os.path.exists(path):
            catalog = {}
            with open(path, encoding="utf-8") as f:
                for line in f:
                    line = line.split(" #")[0].strip()
                    if "=" in line and not line.startswith("#"):
                        name, content = line.split("=", 1)
                        catalog[name.strip()] = content.strip()
            return catalog
    warn("tones.conf not found -- keeping every tone")
    return {}


def convert(config):
    catalog = read_catalog()
    lines = ["# Converted from TOML by toml2conf.py. Run: usrp-rc --check <this file>", ""]

    known = set(SECTIONS) | {"messages"}
    for key, value in config.items():
        if not isinstance(value, dict):
            lines.append(f"{key} = {scalar(value)}")
    lines.append("")

    for section in SECTIONS + [k for k, v in config.items() if isinstance(v, dict) and k not in known]:
        table = config.get(section)
        if table is None:
            continue
        lines.append(f"[{section}]")
        for key, value in table.items():
            key = RENAMED.get((section, key), key)
            lines.append(f"{key} = {scalar(value)}".rstrip())
        lines.append("")

    lines.append("[messages]")
    dropped = 0
    for name, table in config.get("messages", {}).items():
        content = convert_message(name, table)
        if catalog.get(name) == content:
            dropped += 1
            continue
        lines.append(f"{name} = {content}".rstrip())
    if dropped:
        warn(f"{dropped} messages match tones.conf and were left out")
    return "\n".join(lines) + "\n"


def main():
    if len(sys.argv) not in (2, 3):
        print(__doc__, file=sys.stderr)
        return 2
    with open(sys.argv[1], "rb") as f:
        config = tomllib.load(f)
    text = convert(config)
    if len(sys.argv) == 3:
        with open(sys.argv[2], "x", encoding="utf-8") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    warn("to use {call} in your IDs, add callsign = ... at the top and edit the messages")
    return 0


if __name__ == "__main__":
    sys.exit(main())
