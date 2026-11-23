// JOE's weather: the current conditions from Open-Meteo for one place the
// wearer typed on the setup page. No account, no key, no lookup by address.
// Plain C++ over the pinned ArduinoJson, with no Arduino in it, so
// firmware/ci/build.sh can test it on the build machine
// (firmware/ci/weather_test.cpp) before the sketch compiles.
//
// What leaves the watch: the place, rounded to a tenth of a degree (about
// 11 km), in one request an hour at most, and only while a place is set.
// Nothing is sent before the wearer enters one, and clearing the field stops it.
// What the face may show: a reading the provider timed within the last three
// hours, in range for the unit, and not from the future. Anything else is
// "--", never an old number passed off as now.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ArduinoJson.h>

#define WX_NONE        INT16_MIN       // no place: no weather, nothing sent
#define WX_EVERY_MIN   60              // at most one fetch an hour
#define WX_STALE_S     (3L * 3600)     // older than this: "--"
#define WX_HOST        "https://api.open-meteo.com/v1/forecast"

// What the wearer typed, "33.89, 35.50" (a space or a comma between them),
// as tenths of a degree. False for anything else, or out of range.
static inline bool wxParsePlace(const char *s, int16_t *lat10, int16_t *lon10) {
  if (!s) return false;
  double la, lo;
  char tail = 0;
  if (sscanf(s, " %lf%*[ ,;]%lf %c", &la, &lo, &tail) != 2) return false;
  if (!(la >= -90 && la <= 90 && lo >= -180 && lo <= 180)) return false;
  *lat10 = (int16_t)lround(la * 10);
  *lon10 = (int16_t)lround(lo * 10);
  return true;
}

// the place as the setup page shows it again: "33.9, 35.5", or "" for none
static inline void wxPlaceText(char *out, int n, int16_t lat10, int16_t lon10) {
  if (lat10 == WX_NONE || lon10 == WX_NONE) { if (n) out[0] = 0; return; }
  snprintf(out, n, "%.1f, %.1f", lat10 / 10.0, lon10 / 10.0);
}

// The request: the place to a tenth of a degree, the two things the face
// shows, times in GMT.
static inline int wxUrl(char *out, int n, int16_t lat10, int16_t lon10, bool fahrenheit) {
  return snprintf(out, n, WX_HOST "?latitude=%.1f&longitude=%.1f"
                  "&current=temperature_2m,weather_code&timezone=GMT%s",
                  lat10 / 10.0, lon10 / 10.0,
                  fahrenheit ? "&temperature_unit=fahrenheit" : "");
}

// "2026-11-23T04:00" (GMT) as unix seconds; 0 if it is not that shape.
static inline int64_t wxDaysFromCivil(int y, int m, int d) {   // Hinnant
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}
static inline uint32_t wxIsoUtc(const char *s) {
  int y, mo, d, h, mi;
  if (!s || strlen(s) < 16 ||
      sscanf(s, "%4d-%2d-%2dT%2d:%2d", &y, &mo, &d, &h, &mi) != 5) return 0;
  if (y < 2020 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
      h < 0 || h > 23 || mi < 0 || mi > 59) return 0;
  return (uint32_t)(wxDaysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60);
}

// The WMO codes Open-Meteo answers with, and the picture for each.
enum { WXI_NONE = 0, WXI_SUN, WXI_PART, WXI_CLOUD, WXI_FOG, WXI_RAIN, WXI_SNOW, WXI_STORM };
static inline int wxIcon(int code) {
  switch (code) {
    case 0:                                         return WXI_SUN;
    case 1: case 2:                                 return WXI_PART;
    case 3:                                         return WXI_CLOUD;
    case 45: case 48:                               return WXI_FOG;
    case 51: case 53: case 55: case 56: case 57:    // drizzle
    case 61: case 63: case 65: case 66: case 67:    // rain
    case 80: case 81: case 82:                      return WXI_RAIN;   // showers
    case 71: case 73: case 75: case 77:
    case 85: case 86:                               return WXI_SNOW;
    case 95: case 96: case 99:                      return WXI_STORM;
    default:                                        return WXI_NONE;
  }
}

// One reading, as kept in RTC memory by the sketch.
struct WxReading {
  int16_t  temp10;    // tenths of a degree, in the unit asked for
  int8_t   code;      // WMO code; -1: no reading
  uint32_t obsUtc;    // when the provider timed it
};

// Open-Meteo's answer, read and checked. False leaves *r as it was: a bad
// answer never replaces a good reading. fahrenheit says which unit was asked
// for, so the range check is the right one.
static inline bool wxParseBody(const char *body, bool fahrenheit, WxReading *r) {
  JsonDocument filter;
  filter["current"]["time"] = true;
  filter["current"]["temperature_2m"] = true;
  filter["current"]["weather_code"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;
  JsonVariant cur = doc["current"];
  if (!cur["temperature_2m"].is<float>() || !cur["weather_code"].is<int>() ||
      !cur["time"].is<const char *>()) return false;
  float t = cur["temperature_2m"].as<float>();
  int code = cur["weather_code"].as<int>();
  uint32_t obs = wxIsoUtc(cur["time"].as<const char *>());
  float lo = fahrenheit ? -130 : -90, hi = fahrenheit ? 140 : 60;   // Earth's records, about
  if (!(t >= lo && t <= hi) || wxIcon(code) == WXI_NONE || obs == 0) return false;
  r->temp10 = (int16_t)lroundf(t * 10);
  r->code = (int8_t)code;
  r->obsUtc = obs;
  return true;
}

// Can the face show it at unix time now? Timed within WX_STALE_S, and not
// more than an hour ahead (the provider's quarter-hours, a clock a little off).
static inline bool wxFresh(const WxReading *r, uint32_t now) {
  if (r->code < 0 || r->obsUtc == 0) return false;
  if (r->obsUtc > now + 3600) return false;
  return now - (r->obsUtc > now ? now : r->obsUtc) <= (uint32_t)WX_STALE_S;
}
