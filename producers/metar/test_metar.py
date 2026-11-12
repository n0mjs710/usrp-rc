"""Tests for usrp-rc-metar on canned reports. Run: python3 -m unittest test_metar"""

import importlib.machinery
import importlib.util
import os
import tempfile
import types
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
_loader = importlib.machinery.SourceFileLoader("metar", os.path.join(HERE, "usrp-rc-metar"))
_spec = importlib.util.spec_from_loader("metar", _loader)
metar = importlib.util.module_from_spec(_spec)
_loader.exec_module(metar)

ARGS = types.SimpleNamespace(station="KLWC", source_words="", pressure_word="PRESSURE")


def say(words):
    return " ".join(words)


def report(raw, **kw):
    r = {"obsTime": 1794491520, "rawOb": raw, "temp": 5.6, "dewp": 3.9,
         "wdir": 320, "wspd": 7, "cover": "CLR", "clouds": []}
    r.update(kw)
    return r


class Numbers(unittest.TestCase):
    def test_numbers(self):
        self.assertEqual(say(metar.number(0)), "ZERO")
        self.assertEqual(say(metar.number(72)), "SEVENTY TWO")
        self.assertEqual(say(metar.number(-5)), "MINUS FIVE")
        self.assertEqual(say(metar.number(3500)), "THREE THOUSAND FIVE HUNDRED")
        self.assertEqual(say(metar.number(12000)), "TWELVE THOUSAND")


class Wind(unittest.TestCase):
    def test_calm(self):
        self.assertEqual(say(metar.f_wind({"wdir": 0, "wspd": 0})), "WIND CALM")

    def test_variable(self):
        self.assertEqual(say(metar.f_wind({"wdir": "VRB", "wspd": 3})),
                         "WIND VARIABLE AT THREE")

    def test_gust_mph(self):
        # 17 kt = 19.6 mph, 25 kt = 28.8 mph
        self.assertEqual(say(metar.f_wind({"wdir": 350, "wspd": 17, "wgst": 25})),
                         "WIND THREE FIVE ZERO AT TWENTY GUSTING_TO TWENTY NINE")

    def test_direction_digits_padded(self):
        self.assertEqual(say(metar.f_wind({"wdir": 50, "wspd": 5})), "WIND ZERO FIVE ZERO AT SIX")
        self.assertEqual(say(metar.f_wind({"wdir": 330, "wspd": 22})), "WIND THREE THREE ZERO AT TWENTY FIVE")

    def test_missing(self):
        self.assertEqual(say(metar.f_wind({})), "WIND NOT AVAILABLE")


class Source(unittest.TestCase):
    def test_us_drops_k(self):
        self.assertEqual(say(metar.f_source("KLWC", "")), "L W C")

    def test_other_kept(self):
        self.assertEqual(say(metar.f_source("EGLL", "")), "E G L L")
        self.assertEqual(say(metar.f_source("K3AU", "")), "THREE A U")

    def test_override(self):
        self.assertEqual(say(metar.f_source("KLWC", "LAWRENCE")), "LAWRENCE")


class Visibility(unittest.TestCase):
    def v(self, group):
        return say(metar.f_visibility(f"METAR KXXX 121352Z 00000KT {group} CLR 06/04 A3012"))

    def test_whole(self):
        self.assertEqual(self.v("10SM"), "VISIBILITY TEN")
        self.assertEqual(self.v("1SM"), "VISIBILITY ONE")

    def test_fractions(self):
        self.assertEqual(self.v("1/2SM"), "VISIBILITY ONE HALF")
        self.assertEqual(self.v("1 1/2SM"), "VISIBILITY ONE AND_A ONE HALF")
        self.assertEqual(self.v("M1/4SM"), "VISIBILITY LESS THAN ONE QUARTER")
        self.assertEqual(self.v("P6SM"), "VISIBILITY MORE THAN SIX")

    def test_not_confused_by_wind_or_time(self):
        self.assertEqual(self.v("3SM"), "VISIBILITY THREE")

    def test_missing(self):
        self.assertEqual(say(metar.f_visibility("METAR KXXX 121352Z AUTO 00000KT")),
                         "VISIBILITY NOT AVAILABLE")


class Sky(unittest.TestCase):
    def test_clear(self):
        self.assertEqual(say(metar.f_sky({"cover": "CLR", "clouds": []})), "SKY CLEAR")

    def test_scattered_no_ceiling(self):
        self.assertEqual(say(metar.f_sky({"clouds": [{"cover": "FEW", "base": 2000},
                                                     {"cover": "SCT", "base": 5000}]})),
                         "PARTLY CLOUDY")

    def test_ceiling_is_lowest_broken_or_overcast(self):
        clouds = [{"cover": "SCT", "base": 2200}, {"cover": "OVC", "base": 9000},
                  {"cover": "BKN", "base": 3500}]
        self.assertEqual(say(metar.f_sky({"clouds": clouds})),
                         "OVERCAST CEILING THREE THOUSAND FIVE HUNDRED")

    def test_vertical_visibility(self):
        self.assertEqual(say(metar.f_sky({"clouds": [{"cover": "VV", "base": 200}]})),
                         "SKY OBSCURED CEILING TWO HUNDRED")


class Weather(unittest.TestCase):
    def w(self, s):
        return say(metar.f_weather({"wxString": s}))

    def test_none_is_empty(self):
        self.assertEqual(self.w(""), "")
        self.assertEqual(metar.f_weather({}), [])

    def test_groups(self):
        self.assertEqual(self.w("-RA"), "LIGHT RAIN")
        self.assertEqual(self.w("-SHRA"), "LIGHT RAIN SHOWERS")
        self.assertEqual(self.w("+TSRA"), "THUNDERSTORM WITH HEAVY RAIN")
        self.assertEqual(self.w("TS"), "THUNDERSTORM")
        self.assertEqual(self.w("FZRA BR"), "FREEZING RAIN AND_A MIST")
        self.assertEqual(self.w("VCSH"), "SHOWERS NEAR")
        self.assertEqual(self.w("+FC"), "TORNADO")

    def test_unknown_group_skipped(self):
        self.assertEqual(self.w("XX -SN"), "LIGHT SNOW")


class TempPressure(unittest.TestCase):
    def test_temp(self):
        self.assertEqual(say(metar.f_temp(22.2, ["TEMPERATURE"])), "TEMPERATURE SEVENTY TWO")
        self.assertEqual(say(metar.f_temp(-20.6, ["TEMPERATURE"])), "TEMPERATURE MINUS FIVE")
        self.assertEqual(say(metar.f_temp(None, ["D", "P"])), "D P NOT AVAILABLE")

    def test_humidity(self):
        self.assertEqual(say(metar.f_humidity(20, 20)), "RELATIVE HUMIDITY ONE HUNDRED PERCENT")
        self.assertEqual(metar.f_humidity(None, 3), [])

    def test_pressure_from_raw(self):
        r = report("METAR KLWC 121352Z AUTO 32007KT 8SM CLR 06/04 A3002 RMK AO1")
        self.assertEqual(say(metar.f_pressure(r, "PRESSURE")), "PRESSURE THIRTY POINT ZERO TWO")

    def test_pressure_from_hpa(self):
        r = {"rawOb": "METAR EGLL 121350Z 25010KT 9999 FEW030 10/05 Q1013", "altim": 1013.2}
        self.assertEqual(say(metar.f_pressure(r, "ALTIMETER")), "ALTIMETER TWENTY NINE POINT NINE TWO")


class Files(unittest.TestCase):
    def test_write_contract(self):
        r = report("METAR KLWC 121352Z AUTO 32007KT 8SM CLR 06/04 A3012 RMK AO1")
        fields = metar.decode(r, ARGS)
        with tempfile.TemporaryDirectory() as d:
            metar.write_fields(fields, d, r["obsTime"])
            self.assertEqual(sorted(os.listdir(d)), sorted(metar.FIELDS))   # no temp files left
            for name in metar.FIELDS:
                path = os.path.join(d, name)
                self.assertEqual(int(os.stat(path).st_mtime), r["obsTime"])
            with open(os.path.join(d, "weather")) as f:
                self.assertEqual(f.read(), "")                              # empty: nothing to report
            with open(os.path.join(d, "temp")) as f:
                self.assertEqual(f.read(), "TEMPERATURE FORTY TWO\n")


if __name__ == "__main__":
    unittest.main()
