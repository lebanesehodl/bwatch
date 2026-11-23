// Host test for firmware/BitcoinChronograph/weather.h, run by build.sh before
// the sketch compiles:  c++ -I<sketch> -I<ArduinoJson>/src weather_test.cpp
// The answer is Open-Meteo's own (fetched 2026-11-23 04:08 UTC for
// latitude=33.9&longitude=35.5&current=temperature_2m,weather_code&timezone=GMT)
// read through the pinned ArduinoJson. Then the ways an answer must be
// refused, the place as typed, the request, and freshness.
// Exit status: the number of failed checks.
#include <stdio.h>
#include <string.h>
#include "weather.h"

static int fails = 0;
static void check(bool ok, const char *what) {
  if (!ok) { fails++; printf("FAIL %s\n", what); }
}

static const char *REAL =
  "{\"latitude\":33.88798,\"longitude\":35.485966,\"generationtime_ms\":0.051140785217285156,"
  "\"utc_offset_seconds\":0,\"timezone\":\"GMT\",\"timezone_abbreviation\":\"GMT\","
  "\"elevation\":4.0,\"current_units\":{\"time\":\"iso8601\",\"interval\":\"seconds\","
  "\"temperature_2m\":\"\xC2\xB0" "C\",\"weather_code\":\"wmo code\"},"
  "\"current\":{\"time\":\"2026-11-23T04:00\",\"interval\":900,"
  "\"temperature_2m\":19.1,\"weather_code\":0}}";

int main() {
  WxReading r = {0, -1, 0};
  check(wxParseBody(REAL, false, &r), "the real answer parses");
  check(r.temp10 == 191 && r.code == 0, "19.1 C, clear sky");
  check(r.obsUtc == 1795406400u, "timed 2026-11-23 04:00 UTC");
  check(wxIcon(r.code) == WXI_SUN, "code 0 is the sun");

  // refused, and the good reading kept
  const char *bad[] = {
    "",                                                         // nothing
    "<html>rate limited</html>",                                // not JSON
    "{\"error\":true,\"reason\":\"Latitude must be in range\"}", // an error
    "{\"current\":{\"time\":\"2026-11-23T05:00\",\"weather_code\":3}}",          // no temp
    "{\"current\":{\"time\":\"2026-11-23T05:00\",\"temperature_2m\":5}}",        // no code
    "{\"current\":{\"temperature_2m\":5,\"weather_code\":3}}",                   // no time
    "{\"current\":{\"time\":\"yesterday\",\"temperature_2m\":5,\"weather_code\":3}}",
    "{\"current\":{\"time\":\"2026-11-23T05:00\",\"temperature_2m\":75,\"weather_code\":3}}",  // 75 C
    "{\"current\":{\"time\":\"2026-11-23T05:00\",\"temperature_2m\":5,\"weather_code\":42}}",  // no such code
    "{\"current\":{\"time\":\"2026-11-23T05:00\",\"temperature_2m\":\"warm\",\"weather_code\":3}}",
  };
  for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    char what[64]; snprintf(what, sizeof what, "bad answer %u refused, reading kept", i);
    check(!wxParseBody(bad[i], false, &r) && r.temp10 == 191 && r.code == 0, what);
  }
  // the unit decides the range: 75 is a summer day in F, not in C
  { WxReading f = {0, -1, 0};
    check(wxParseBody("{\"current\":{\"time\":\"2026-07-01T12:00\",\"temperature_2m\":75.2,\"weather_code\":2}}", true, &f)
          && f.temp10 == 752 && wxIcon(f.code) == WXI_PART, "75.2 F, partly cloudy"); }
  { WxReading c = {0, -1, 0};
    check(wxParseBody("{\"current\":{\"time\":\"2026-01-10T06:00\",\"temperature_2m\":-12.4,\"weather_code\":73}}", false, &c)
          && c.temp10 == -124 && wxIcon(c.code) == WXI_SNOW, "-12.4 C, snow"); }

  // every icon family
  check(wxIcon(3) == WXI_CLOUD && wxIcon(45) == WXI_FOG && wxIcon(61) == WXI_RAIN &&
        wxIcon(81) == WXI_RAIN && wxIcon(53) == WXI_RAIN && wxIcon(86) == WXI_SNOW &&
        wxIcon(95) == WXI_STORM && wxIcon(-1) == WXI_NONE && wxIcon(100) == WXI_NONE,
        "the WMO codes map to their pictures");

  // freshness
  uint32_t t0 = r.obsUtc;
  check(wxFresh(&r, t0), "fresh when timed now");
  check(wxFresh(&r, t0 + 3 * 3600), "fresh at three hours");
  check(!wxFresh(&r, t0 + 3 * 3600 + 1), "stale past three hours");
  check(wxFresh(&r, t0 - 1800), "a quarter-hour stamp a little ahead is fine");
  check(!wxFresh(&r, t0 - 3601), "a reading from over an hour in the future is not");
  { WxReading none = {0, -1, 0}; check(!wxFresh(&none, t0), "no reading is never fresh"); }

  // the place as typed
  int16_t la = 0, lo = 0;
  check(wxParsePlace("33.8938, 35.5018", &la, &lo) && la == 339 && lo == 355, "33.8938, 35.5018 -> 33.9, 35.5");
  check(wxParsePlace("33.9 35.5", &la, &lo) && la == 339 && lo == 355, "a space between");
  check(wxParsePlace(" -33.87,151.21 ", &la, &lo) && la == -339 && lo == 1512, "Sydney, no space");
  check(wxParsePlace("40.7128; -74.0060", &la, &lo) && la == 407 && lo == -740, "New York, semicolon");
  check(!wxParsePlace("", &la, &lo), "empty is no place");
  check(!wxParsePlace("Beirut", &la, &lo), "a name is not a place (yet)");
  check(!wxParsePlace("91, 10", &la, &lo), "latitude past the pole");
  check(!wxParsePlace("10, 181", &la, &lo), "longitude past 180");
  check(!wxParsePlace("33.9, 35.5, 12", &la, &lo), "three numbers");
  check(!wxParsePlace("33.9", &la, &lo), "one number");
  { char t[24]; wxPlaceText(t, sizeof t, 339, 355); check(!strcmp(t, "33.9, 35.5"), "shown again as typed, rounded"); }
  { char t[24]; wxPlaceText(t, sizeof t, WX_NONE, WX_NONE); check(t[0] == 0, "no place shows blank"); }

  // the request: the place to a tenth, nothing else about the wearer
  { char u[200]; wxUrl(u, sizeof u, 339, 355, false);
    check(!strcmp(u, "https://api.open-meteo.com/v1/forecast?latitude=33.9&longitude=35.5"
                     "&current=temperature_2m,weather_code&timezone=GMT"), "the request, C");
    wxUrl(u, sizeof u, -339, 1512, true);
    check(!strcmp(u, "https://api.open-meteo.com/v1/forecast?latitude=-33.9&longitude=151.2"
                     "&current=temperature_2m,weather_code&timezone=GMT&temperature_unit=fahrenheit"),
          "the request, F"); }

  printf("weather_test: %d failed\n", fails);
  return fails;
}
