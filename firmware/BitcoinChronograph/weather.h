// JOE's weather: the current conditions from Open-Meteo for one place the
// wearer typed on the setup page. No account, no key, no lookup by address.
// Plain C++ over the pinned ArduinoJson, with no Arduino in it, so
// firmware/ci/build.sh can test it on the build machine
// (firmware/ci/weather_test.cpp) before the sketch compiles.
//
// What leaves the watch: the place, rounded to a tenth of a degree (about
// 11 km north-south, less east-west), the unit, and the two fields the face
// shows, in one request an hour at most, only while a place is set, and only
// to api.open-meteo.com over TLS checked against the roots in
// weather_roots.h. Open-Meteo also sees the watch's internet address, and
// its privacy policy says its API logs may keep coordinates for 90 days.
// Nothing is sent before the wearer enters a place, and clearing it stops it.
//
// What the face may show: a reading the provider timed within the last three
// hours, in the unit asked for, in GMT, in range for the unit, and not from
// the future. Anything else is "--", never an old number passed off as now.
// A refused answer leaves the last good reading exactly as it was.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ArduinoJson.h>

#define WX_NONE        INT16_MIN       // no place: no weather, nothing sent
#define WX_DEFAULT_F   true            // Fahrenheit unless the wearer picks C (OP)
#define WX_EVERY_MIN   60              // at most one attempt an hour, any path
#define WX_STALE_S     (3L * 3600)     // older than this: "--"
#define WX_AHEAD_S     3600L           // timed further ahead than this: refused
#define WX_HOSTNAME    "api.open-meteo.com"
#define WX_PATH        "/v1/forecast"

// The transport's limits (the sketch's fetchWeather()):
#define WX_CONNECT_MS  4000            // TCP connect, and each send
#define WX_HANDSHAKE_S 10              // TLS handshake (the core's default: 120 s)
#define WX_READ_MS     8000            // request sent to the answer's last byte
#define WX_RESP_MAX    4096            // the whole answer, headers and all

// ---------------- the place ----------------

// What the wearer typed, "33.9, 35.5" (a space, comma or semicolon between
// them), as tenths of a degree. False for anything else, or out of range.
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

// The setup page's field, after the portal. `typed` is what came back,
// `shown` what the page was given; *lat10/*lon10 hold the current place and
// are changed only by WXE_SET and WXE_CLEARED.
enum { WXE_SAME = 0, WXE_SET, WXE_CLEARED, WXE_REFUSED };
static inline int wxPlaceEdit(const char *typed, const char *shown,
                              int16_t *lat10, int16_t *lon10) {
  if (!typed || !strcmp(typed, shown ? shown : "")) return WXE_SAME;
  if (strspn(typed, " ") == strlen(typed)) {
    *lat10 = *lon10 = WX_NONE;
    return WXE_CLEARED;
  }
  int16_t la, lo;
  if (!wxParsePlace(typed, &la, &lo)) return WXE_REFUSED;
  *lat10 = la; *lon10 = lo;
  return WXE_SET;
}
// What the serial log may say about it: fixed words, never what was typed.
// A refused field can be a precise place with a typo in it.
static inline const char *wxPlaceEditLog(int e) {
  switch (e) {
    case WXE_SET:     return "place set (rounded)";
    case WXE_CLEARED: return "place cleared";
    case WXE_REFUSED: return "place not understood, the old one kept";
    default:          return "place unchanged";
  }
}

// The setup page's unit field: Fahrenheit unless it says C, so a blank or
// unreadable field keeps the default.
static inline bool wxUnitIsF(const char *u) {
  while (u && *u == ' ') u++;
  if (!u || !*u) return WX_DEFAULT_F;
  if (*u == 'C' || *u == 'c') return false;
  return true;
}

// ---------------- the request ----------------

// The path and query: the place to a tenth of a degree, the two things the
// face shows, times in GMT, the unit.
static inline int wxPath(char *out, int n, int16_t lat10, int16_t lon10, bool fahrenheit) {
  return snprintf(out, n, WX_PATH "?latitude=%.1f&longitude=%.1f"
                  "&current=temperature_2m,weather_code&timezone=GMT%s",
                  lat10 / 10.0, lon10 / 10.0,
                  fahrenheit ? "&temperature_unit=fahrenheit" : "");
}
// The whole request. HTTP/1.0, so the answer is never chunked and the
// server's close ends it; no user agent, no cookies, nothing about the watch.
static inline int wxRequest(char *out, int n, int16_t lat10, int16_t lon10, bool fahrenheit) {
  char path[160];
  int p = wxPath(path, sizeof path, lat10, lon10, fahrenheit);
  if (p < 0 || p >= (int)sizeof path) return -1;
  return snprintf(out, n, "GET %s HTTP/1.0\r\nHost: " WX_HOSTNAME "\r\n"
                  "Accept: application/json\r\nAccept-Encoding: identity\r\n"
                  "Connection: close\r\n\r\n", path);
}

// Reading the answer, bounded. C is the connection (the sketch's
// WiFiClientSecure, the test's fake): available(), read(buf, n), connected().
// clock() is milliseconds, idle() waits a little. The answer is complete
// only when the peer closes: more than `cap` bytes is WXR_TOO_BIG, and
// anything still open after `ms` is WXR_TIMEOUT, however slowly it drips.
enum { WXR_OK = 0, WXR_TOO_BIG, WXR_TIMEOUT };
template <class C, class Clock, class Idle>
static inline int wxReadAll(C &c, char *buf, size_t cap, size_t *len,
                            Clock clock, Idle idle, uint32_t ms) {
  uint32_t t0 = clock();
  size_t n = 0;
  *len = 0;
  for (;;) {
    if ((uint32_t)(clock() - t0) > ms) return WXR_TIMEOUT;
    int a = c.available();
    if (a > 0) {
      if (n >= cap) return WXR_TOO_BIG;
      size_t want = (size_t)a < cap - n ? (size_t)a : cap - n;
      int got = c.read((uint8_t *)buf + n, want);
      if (got > 0) n += (size_t)got;
      continue;
    }
    if (!c.connected()) { *len = n; return WXR_OK; }
    idle();
  }
}

// ---------------- the answer ----------------

static inline bool wxHeaderIs(const char *line, size_t n, const char *name) {
  size_t k = strlen(name);
  if (n <= k || line[k] != ':') return false;
  for (size_t i = 0; i < k; i++) {
    char a = line[i], b = name[i];
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (a != b) return false;
  }
  return true;
}
static inline const char *wxHeaderValue(const char *line, size_t n, const char *name, size_t *vn) {
  size_t k = strlen(name) + 1;
  while (k < n && (line[k] == ' ' || line[k] == '\t')) k++;
  size_t e = n;
  while (e > k && (line[e - 1] == ' ' || line[e - 1] == '\t')) e--;
  *vn = e - k;
  return line + k;
}
static inline bool wxValueIs(const char *v, size_t vn, const char *want) {
  size_t k = strlen(want);
  if (vn != k) return false;
  for (size_t i = 0; i < k; i++) {
    char a = v[i];
    if (a >= 'A' && a <= 'Z') a += 32;
    if (a != want[i]) return false;
  }
  return true;
}

// The HTTP answer in buf[0..n) (buf has room for one more byte): a 200,
// headers we can read, and the body, de-chunked if a server chunks anyway,
// whole by its Content-Length if it gives one. True puts a NUL-terminated
// body at *body. Anything else is refused, a raw NUL anywhere included, so
// the body's C-string length is its whole length.
static inline bool wxHttpBody(char *buf, size_t n, const char **body) {
  buf[n] = 0;
  if (memchr(buf, 0, n)) return false;
  if (n < 12 || (strncmp(buf, "HTTP/1.0 200", 12) && strncmp(buf, "HTTP/1.1 200", 12))) return false;
  if (n > 12 && buf[12] != ' ' && buf[12] != '\r') return false;
  char *end = strstr(buf, "\r\n\r\n");
  if (!end || (size_t)(end - buf) > n) return false;
  char *b = end + 4;
  size_t bn = n - (size_t)(b - buf);
  bool chunked = false, haveLen = false;
  unsigned long clen = 0;
  for (char *line = strstr(buf, "\r\n") + 2; line < end + 2; ) {
    char *eol = strstr(line, "\r\n");
    size_t ln = (size_t)(eol - line);
    size_t vn;
    if (wxHeaderIs(line, ln, "Transfer-Encoding")) {
      const char *v = wxHeaderValue(line, ln, "Transfer-Encoding", &vn);
      if (!wxValueIs(v, vn, "chunked")) return false;
      chunked = true;
    } else if (wxHeaderIs(line, ln, "Content-Encoding")) {
      const char *v = wxHeaderValue(line, ln, "Content-Encoding", &vn);
      if (!wxValueIs(v, vn, "identity")) return false;            // asked for identity
    } else if (wxHeaderIs(line, ln, "Content-Length")) {
      const char *v = wxHeaderValue(line, ln, "Content-Length", &vn);
      if (haveLen || vn == 0 || vn > 6) return false;
      clen = 0;
      for (size_t i = 0; i < vn; i++) {
        if (v[i] < '0' || v[i] > '9') return false;
        clen = clen * 10 + (unsigned long)(v[i] - '0');
      }
      haveLen = true;
    }
    line = eol + 2;
  }
  if (chunked) {                       // de-chunk in place
    if (haveLen) return false;          // both: refuse, as RFC 9112 advises
    char *in = b, *out = b, *stop = b + bn;
    for (;;) {
      unsigned long size = 0;
      int digits = 0;
      while (in < stop) {
        char c = *in;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
              : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) break;
        if (++digits > 6) return false;
        size = size * 16 + (unsigned long)d;
        in++;
      }
      if (!digits) return false;
      // The size, then CRLF at once. A chunk extension (";...") is refused,
      // well-formed or not: the request is HTTP/1.0, so no server may chunk
      // this answer at all (RFC 9112 6.1), and this narrow subset is all a
      // server that does so anyway gets.
      if (stop - in < 2 || in[0] != '\r' || in[1] != '\n') return false;
      in += 2;
      if (size == 0) {                 // the last chunk: no trailers, one CRLF, the end
        if (stop - in != 2 || in[0] != '\r' || in[1] != '\n') return false;
        break;
      }
      if ((unsigned long)(stop - in) < size + 2) return false;   // cut short
      memmove(out, in, size);
      out += size; in += size;
      if (in[0] != '\r' || in[1] != '\n') return false;
      in += 2;
    }
    *out = 0;
  } else if (haveLen) {
    if (bn != clen) return false;      // cut short, or more than it said
  }
  *body = b;
  return true;
}

// "2026-11-23T04:00", exactly: GMT to the minute, a real date. Unix seconds,
// or 0 for anything else (a suffix, an offset, seconds, 31 February).
static inline int64_t wxDaysFromCivil(int y, int m, int d) {   // Hinnant
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}
static inline uint32_t wxIsoUtc(const char *s) {
  static const char shape[] = "dddd-dd-ddTdd:dd";
  if (!s || strlen(s) != sizeof shape - 1) return 0;
  for (unsigned i = 0; i < sizeof shape - 1; i++) {
    if (shape[i] == 'd' ? (s[i] < '0' || s[i] > '9') : s[i] != shape[i]) return 0;
  }
  int y  = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
  int mo = (s[5] - '0') * 10 + (s[6] - '0');
  int d  = (s[8] - '0') * 10 + (s[9] - '0');
  int h  = (s[11] - '0') * 10 + (s[12] - '0');
  int mi = (s[14] - '0') * 10 + (s[15] - '0');
  static const int8_t mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (y < 2020 || y > 2100 || mo < 1 || mo > 12 || h > 23 || mi > 59) return 0;
  bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  if (d < 1 || d > mdays[mo - 1] + (mo == 2 && leap)) return 0;
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

// Can the face show it at unix time now? Timed within WX_STALE_S, and not
// more than WX_AHEAD_S ahead (the provider's quarter-hours, a clock a little
// off).
static inline bool wxFresh(const WxReading *r, uint32_t now) {
  if (r->code < 0 || r->obsUtc == 0) return false;
  if (r->obsUtc > now + (uint32_t)WX_AHEAD_S) return false;
  return now - (r->obsUtc > now ? now : r->obsUtc) <= (uint32_t)WX_STALE_S;
}

// ArduinoJson reads the answer through this, so the bytes it took are
// counted. It reads none past the object's closing brace; weather_test's
// "one byte after the object" holds the pinned version to that.
struct WxCountedReader {
  const char *p;
  size_t n, at;
  int read() { return at < n ? (unsigned char)p[at++] : -1; }
  size_t readBytes(char *out, size_t k) {
    size_t m = n - at < k ? n - at : k;
    memcpy(out, p + at, m);
    at += m;
    return m;
  }
};

// A JSON string with no NUL inside it, or nullptr. A "\u0000" would hide
// whatever follows it from every C-string check below.
static inline const char *wxStr(JsonVariantConst v) {
  if (!v.is<const char *>()) return nullptr;
  JsonString s = v.as<JsonString>();
  if (s.isNull() || strlen(s.c_str()) != s.size()) return nullptr;
  return s.c_str();
}

// Open-Meteo's answer, checked whole before anything is kept: one JSON
// object and nothing after it but whitespace, GMT with no offset, the unit
// that was asked for, a temperature in range for it, a code we have a
// picture for, a timestamp of the exact shape, fresh at `now` and no older
// than the reading already held. False leaves *r exactly as it was: a bad
// answer never replaces a good reading.
static inline bool wxParseBody(const char *body, bool fahrenheit, uint32_t now, WxReading *r) {
  JsonDocument filter;
  filter["utc_offset_seconds"] = true;
  filter["timezone"] = true;
  filter["current_units"]["temperature_2m"] = true;
  filter["current"]["time"] = true;
  filter["current"]["temperature_2m"] = true;
  filter["current"]["weather_code"] = true;
  JsonDocument doc;
  if (!body) return false;
  WxCountedReader in = {body, strlen(body), 0};
  if (deserializeJson(doc, in, DeserializationOption::Filter(filter))) return false;
  for (size_t i = in.at; i < in.n; i++)                         // one document, nothing after
    if (body[i] != ' ' && body[i] != '\t' && body[i] != '\r' && body[i] != '\n') return false;
  if (!doc["utc_offset_seconds"].is<int>() || doc["utc_offset_seconds"].as<int>() != 0) return false;
  const char *zone = wxStr(doc["timezone"]);
  if (!zone || strcmp(zone, "GMT")) return false;
  const char *unit = wxStr(doc["current_units"]["temperature_2m"]);
  if (!unit || strcmp(unit, fahrenheit ? "\xC2\xB0" "F" : "\xC2\xB0" "C")) return false;
  JsonVariant cur = doc["current"];
  const char *when = wxStr(cur["time"]);
  if (!cur["temperature_2m"].is<float>() || !cur["weather_code"].is<int>() || !when) return false;
  float t = cur["temperature_2m"].as<float>();
  int code = cur["weather_code"].as<int>();
  float lo = fahrenheit ? -130 : -90, hi = fahrenheit ? 140 : 60;   // Earth's records, about
  if (!(t >= lo && t <= hi) || wxIcon(code) == WXI_NONE) return false;
  WxReading cand;
  cand.temp10 = (int16_t)lroundf(t * 10);
  cand.code = (int8_t)code;
  cand.obsUtc = wxIsoUtc(when);
  if (cand.obsUtc == 0 || !wxFresh(&cand, now)) return false;
  if (r->code >= 0 && r->obsUtc != 0 && cand.obsUtc < r->obsUtc) return false;   // older
  *r = cand;
  return true;
}

// ---------------- when it may try ----------------

// Both callers ask this, and nothing else decides: whether the price data
// came in, a long UP, a retry of the other fetches, none of it is an input.
// One attempt an hour, worked or not (the caller stamps lastTry first).
enum { WXP_FETCH = 0, WXP_JOE };
struct WxGate {
  int path;            // WXP_FETCH: inside fetchAll, the radio already up
  bool placeSet, clockOk;
  uint32_t now, lastTry;
  bool buttonWake, prePaint, docked, night, cold;   // JOE's own fetch only
};
static inline bool wxMayTry(const WxGate &g) {
  if (!g.placeSet || !g.clockOk) return false;
  bool due = g.lastTry == 0 || g.now < g.lastTry ||
             g.now - g.lastTry >= (uint32_t)WX_EVERY_MIN * 60u;
  if (!due) return false;
  if (g.path == WXP_JOE && (g.buttonWake || g.prePaint || g.docked || g.night || g.cold))
    return false;
  return true;
}
