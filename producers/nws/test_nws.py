"""Tests for usrp-rc-nws on canned alerts. Run: python3 -m unittest test_nws"""

import importlib.machinery
import importlib.util
import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
_loader = importlib.machinery.SourceFileLoader("nws", os.path.join(HERE, "usrp-rc-nws"))
_spec = importlib.util.spec_from_loader("nws", _loader)
nws = importlib.util.module_from_spec(_spec)
_loader.exec_module(nws)

EVENTS = dict(nws.pairs(nws.EVENTS, "--event"))
NOW = 1794500000
LATER = "2026-11-15T23:00:00-06:00"


def alert(event, *ugc, ends=LATER):
    return {"properties": {"event": event, "geocode": {"UGC": list(ugc)},
                           "ends": ends, "expires": ends}}


def say(found, labels):
    return " _ ".join(" ".join(w) for w in nws.phrases(found, EVENTS, labels, NOW))


class Parse(unittest.TestCase):
    def test_area(self):
        self.assertEqual(nws.areas(["d g = ksc045, KSZ040"]), [(["D", "G"], {"KSC045", "KSZ040"})])

    def test_bad(self):
        for bad in (["D G"], ["=KSC045"], ["D G="]):
            with self.assertRaises(ValueError):
                nws.areas(bad)
        with self.assertRaises(ValueError):
            nws.pairs(["Tornado Warning"], "--event")


class Phrases(unittest.TestCase):
    LABELS = nws.areas(["D G=KSC045,KSZ040", "S N=KSC177"])

    def found(self, *features):
        return nws.matches(features, EVENTS, self.LABELS)

    def test_none(self):
        self.assertEqual(say(self.found(), self.LABELS), "")

    def test_watch_and_warning_in_one_county(self):
        f = self.found(alert("Tornado Watch", "KSC045", "KSC177"), alert("Tornado Warning", "KSC045"))
        self.assertEqual(say(f, self.LABELS), "TORNADO WARNING D G _ TORNADO WATCH D G _ TORNADO WATCH S N")

    def test_event_list_sets_the_sequence(self):
        f = self.found(alert("Severe Thunderstorm Warning", "KSC177"), alert("Tornado Warning", "KSC045"))
        self.assertEqual(say(f, self.LABELS), "TORNADO WARNING D G _ SEVERE THUNDERSTORM WARNING S N")

    def test_not_on_the_list_or_not_in_the_area(self):
        f = self.found(alert("Dense Fog Advisory", "KSZ040"), alert("Tornado Warning", "KSC087"))
        self.assertEqual(say(f, self.LABELS), "")

    def test_shared_label_is_said_once(self):
        labels = nws.areas(["AREA=KSC045,KSC177"])
        f = nws.matches([alert("Tornado Watch", "KSC045"), alert("Tornado Watch", "KSC177"),
                         alert("Tornado Warning", "KSC177")], EVENTS, labels)
        self.assertEqual(say(f, labels), "TORNADO WARNING AREA _ TORNADO WATCH AREA")

    def test_ended_alert_is_dropped(self):
        f = self.found(alert("Tornado Warning", "KSC045", ends="2026-01-01T00:00:00-06:00"))
        self.assertEqual(say(f, self.LABELS), "")

    def test_fit_keeps_whole_phrases_from_the_top(self):
        p = [["A"] * 5] * 6                       # 5 words + a pause each: 35 in all
        self.assertEqual(len(nws.fit(p, 32)), 5)  # 5*5 + 4 pauses = 29
        self.assertEqual(len(nws.fit(p[:2], 32)), 2)

    def test_expires_when_no_ends(self):
        a = alert("Tornado Watch", "KSC045")
        a["properties"]["ends"] = None
        self.assertEqual(say(self.found(a), self.LABELS), "TORNADO WATCH D G")


if __name__ == "__main__":
    unittest.main()
