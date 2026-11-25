// Host test for firmware/BitcoinChronograph/weather.h, run by build.sh before
// the sketch compiles:  c++ -I<sketch> -I<ArduinoJson>/src weather_test.cpp
// The answer is Open-Meteo's own (fetched 2026-11-23 04:08 UTC for
// latitude=33.9&longitude=35.5&current=temperature_2m,weather_code&timezone=GMT)
// read through the pinned ArduinoJson. Then the answers that must be
// refused with the held reading left exactly as it was (Bitcoin's reviews of
// 62ee5b0 and a990bd8 among them), the HTTP framing, the bounded read against a fake
// peer and clock, the place as typed and what the log may say about it,
// the request, freshness, and when either caller may try.
// Exit status: the number of failed checks.
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "weather.h"

static int fails = 0;
static void check(bool ok, const char *what) {
  if (!ok) { fails++; printf("FAIL %s\n", what); }
}
static bool same(const WxReading &a, const WxReading &b) {
  return a.temp10 == b.temp10 && a.code == b.code && a.obsUtc == b.obsUtc;
}

static const char *REAL =
  "{\"latitude\":33.88798,\"longitude\":35.485966,\"generationtime_ms\":0.051140785217285156,"
  "\"utc_offset_seconds\":0,\"timezone\":\"GMT\",\"timezone_abbreviation\":\"GMT\","
  "\"elevation\":4.0,\"current_units\":{\"time\":\"iso8601\",\"interval\":\"seconds\","
  "\"temperature_2m\":\"\xC2\xB0" "C\",\"weather_code\":\"wmo code\"},"
  "\"current\":{\"time\":\"2026-11-23T04:00\",\"interval\":900,"
  "\"temperature_2m\":19.1,\"weather_code\":0}}";
static const uint32_t OBS = 1795406400u;        // 2026-11-23 04:00 UTC
static const uint32_t NOW = OBS + 8 * 60;       // when it was fetched

// An answer shaped like the real one, with the parts a test changes.
static std::string answer(const char *time, const char *unit = "C", int offset = 0,
                          const char *tz = "GMT", const char *temp = "19.1", int code = 0) {
  char b[640];
  snprintf(b, sizeof b,
    "{\"latitude\":33.88798,\"longitude\":35.485966,\"utc_offset_seconds\":%d,"
    "\"timezone\":\"%s\",\"current_units\":{\"time\":\"iso8601\","
    "\"temperature_2m\":\"\xC2\xB0%s\",\"weather_code\":\"wmo code\"},"
    "\"current\":{\"time\":\"%s\",\"interval\":900,\"temperature_2m\":%s,\"weather_code\":%d}}",
    offset, tz, unit, time, temp, code);
  return b;
}

// A peer for wxReadAll: what it sends, in pieces, at given times, and
// whether it closes after. The clock only moves when the reader idles.
struct FakePeer {
  struct Piece { uint32_t at; std::string bytes; };
  std::vector<Piece> pieces;
  size_t next = 0, off = 0;
  bool closes = true;
  uint32_t *clock;
  int available() {
    if (next < pieces.size() && pieces[next].at <= *clock)
      return (int)(pieces[next].bytes.size() - off);
    return 0;
  }
  int read(uint8_t *b, size_t n) {
    int a = available();
    if (a <= 0) return -1;
    size_t k = n < (size_t)a ? n : (size_t)a;
    memcpy(b, pieces[next].bytes.data() + off, k);
    off += k;
    if (off == pieces[next].bytes.size()) { next++; off = 0; }
    return (int)k;
  }
  bool connected() { return next < pieces.size() || !closes; }
};
static int readFrom(FakePeer &p, char *buf, size_t cap, size_t *len, uint32_t *t, uint32_t ms) {
  p.clock = t;
  return wxReadAll(p, buf, cap, len, [t] { return *t; }, [t] { *t += 10; }, ms);
}

static std::string http(const char *head, const std::string &body) {
  return std::string(head) + "\r\n\r\n" + body;
}
static bool bodyOf(std::string resp, std::string *out) {
  std::vector<char> b(resp.begin(), resp.end());
  b.push_back(0);
  const char *body = nullptr;
  if (!wxHttpBody(b.data(), resp.size(), &body)) return false;
  *out = body;
  return true;
}

int main() {
  // ---- the real answer ----
  WxReading r = {0, -1, 0};
  check(wxParseBody(REAL, false, NOW, &r), "the real answer parses");
  check(r.temp10 == 191 && r.code == 0, "19.1 C, clear sky");
  check(r.obsUtc == OBS, "timed 2026-11-23 04:00 UTC");
  check(wxIcon(r.code) == WXI_SUN, "code 0 is the sun");
  const WxReading good = r;

  // ---- refused, and the held reading kept whole ----
  const char *bad[] = {
    "",                                                         // nothing
    "<html>rate limited</html>",                                // not JSON
    "{\"error\":true,\"reason\":\"Latitude must be in range\"}", // an error
  };
  for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    char what[64]; snprintf(what, sizeof what, "bad answer %u refused, reading kept", i);
    r = good; check(!wxParseBody(bad[i], false, NOW, &r) && same(r, good), what);
  }
  struct { std::string body; const char *what; } hostile[] = {
    { answer("2026-11-22T04:00"),            "a day old (would have replaced a fresh reading)" },
    { answer("2026-11-24T04:00"),            "a day ahead" },
    { answer("2026-11-23T03:50"),            "older than the reading held" },
    { answer("2026-11-23T04:00junk"),        "a timestamp with a suffix" },
    { answer("2026-11-23T04:00+12:00"),      "a timestamp with an offset" },
    { answer("2026-11-23T04:00:00"),         "a timestamp with seconds" },
    { answer("2026-11-23T04:0"),             "a truncated timestamp" },
    { answer("2026-02-31T04:00"),            "31 February" },
    { answer("2026-11-23T04:00", "F"),       "Fahrenheit when Celsius was asked for" },
    { answer("2026-11-23T04:00", "K"),       "a unit that is neither" },
    { answer("2026-11-23T04:00", "C", 36000),"a non-zero UTC offset" },
    { answer("2026-11-23T04:00", "C", 0, "Asia/Beirut"), "a timezone other than GMT" },
    { answer("2026-11-23T04:00", "C", 0, "GMT", "75"),   "75 C" },
    { answer("2026-11-23T04:00", "C", 0, "GMT", "\"warm\""), "a temperature that is a word" },
    { answer("2026-11-23T04:00", "C", 0, "GMT", "19.1", 42), "a code with no picture" },
    { "{\"utc_offset_seconds\":0,\"timezone\":\"GMT\",\"current\":{\"time\":\"2026-11-23T04:00\","
      "\"temperature_2m\":19.1,\"weather_code\":0}}", "no units at all" },
    { "{\"timezone\":\"GMT\",\"current_units\":{\"temperature_2m\":\"\xC2\xB0" "C\"},"
      "\"current\":{\"time\":\"2026-11-23T04:00\",\"temperature_2m\":19.1,\"weather_code\":0}}",
      "no UTC offset" },
    { "{\"utc_offset_seconds\":0,\"timezone\":\"GMT\",\"current_units\":{\"temperature_2m\":\"\xC2\xB0" "C\"},"
      "\"current\":{\"time\":\"2026-11-23T04:00\",\"weather_code\":0}}", "no temperature" },
    { "{\"utc_offset_seconds\":0,\"timezone\":\"GMT\",\"current_units\":{\"temperature_2m\":\"\xC2\xB0" "C\"},"
      "\"current\":{\"temperature_2m\":19.1,\"weather_code\":0}}", "no time" },
    // Bitcoin's review of a990bd8: an escaped NUL hid a suffix from the
    // string checks, and whatever followed the object was never read
    { answer("2026-11-23T04:00\\u0000junk"),             "a timestamp, an escaped NUL, a suffix" },
    { answer("2026-11-23T04:00", "C\\u0000junk"),        "a unit, an escaped NUL, a suffix" },
    { answer("2026-11-23T04:00", "C", 0, "GMT\\u0000junk"), "a timezone, an escaped NUL, a suffix" },
    { answer("2026-11-23T04:00", "C", 0, "GMT\\u0000"),  "a timezone ending in an escaped NUL" },
    { "{\"utc_offset_seconds\":0,\"timezone\\u0000x\":\"GMT\",\"current_units\":{\"temperature_2m\":"
      "\"\xC2\xB0" "C\"},\"current\":{\"time\":\"2026-11-23T04:00\",\"temperature_2m\":19.1,"
      "\"weather_code\":0}}", "the timezone only under a key with an escaped NUL" },
    { std::string(REAL) + "BROKEN",                       "bytes after the object" },
    { std::string(REAL) + "x",                            "one byte after the object" },
    { std::string(REAL) + " \n{\"error\":true}",          "a second document after it" },
    { std::string(REAL).substr(0, strlen(REAL) - 1),      "the object never closed" },
  };
  for (auto &h : hostile) {
    r = good;
    bool took = wxParseBody(h.body.c_str(), false, NOW, &r);
    char what[120]; snprintf(what, sizeof what, "refused, reading kept whole: %s", h.what);
    check(!took && same(r, good), what);
  }
  // the newest wins; the same quarter-hour again may update it
  r = good;
  check(wxParseBody(answer("2026-11-23T04:00", "C", 0, "GMT", "19.4").c_str(), false, NOW, &r)
        && r.temp10 == 194 && r.obsUtc == OBS, "the same quarter-hour, revised, is taken");
  r = good;
  check(wxParseBody(answer("2026-11-23T04:15", "C", 0, "GMT", "18.9", 1).c_str(), false, NOW + 900, &r)
        && r.temp10 == 189 && r.code == 1 && r.obsUtc == OBS + 900, "a newer reading replaces it");
  // no clock, nothing is fresh
  r = good; check(!wxParseBody(REAL, false, 0, &r) && same(r, good), "no clock: refused");
  // whitespace around the one object is still the answer
  { WxReading w = {0, -1, 0};
    check(wxParseBody((std::string(" \r\n") + REAL + "\r\n\t ").c_str(), false, NOW, &w) && same(w, good),
          "whitespace before and after the object is taken"); }
  // the unit decides the label and the range
  { WxReading f = {0, -1, 0};
    check(wxParseBody(answer("2026-07-01T12:00", "F", 0, "GMT", "75.2", 2).c_str(), true,
                      1782907200u + 60, &f) && f.temp10 == 752 && wxIcon(f.code) == WXI_PART,
          "75.2 F, partly cloudy, when F was asked for");
    WxReading c = good;
    check(!wxParseBody(REAL, true, NOW, &c) && same(c, good),
          "Celsius refused when Fahrenheit was asked for"); }
  { WxReading c = {0, -1, 0};
    check(wxParseBody(answer("2026-01-10T06:00", "C", 0, "GMT", "-12.4", 73).c_str(), false,
                      1768024800u + 300, &c) && c.temp10 == -124 && wxIcon(c.code) == WXI_SNOW,
          "-12.4 C, snow"); }

  // ---- the timestamp, exactly ----
  check(wxIsoUtc("2026-11-23T04:00") == OBS, "the GMT shape");
  check(wxIsoUtc("2028-02-29T12:00") != 0, "29 February in a leap year");
  check(wxIsoUtc("2100-02-28T00:00") != 0 && wxIsoUtc("2100-02-29T00:00") == 0, "2100 is not a leap year");
  check(wxIsoUtc("2027-02-29T12:00") == 0, "29 February otherwise");
  check(wxIsoUtc("2026-04-31T12:00") == 0, "31 April");
  check(wxIsoUtc("2026-00-10T12:00") == 0 && wxIsoUtc("2026-13-10T12:00") == 0, "months 0 and 13");
  check(wxIsoUtc("2026-11-00T12:00") == 0, "day 0");
  check(wxIsoUtc("2026-11-23T24:00") == 0 && wxIsoUtc("2026-11-23T23:60") == 0, "24:00 and 23:60");
  check(wxIsoUtc("2026-11-23 04:00") == 0, "a space for the T");
  check(wxIsoUtc("2026-11-23T04:00Z") == 0, "a trailing Z");
  check(wxIsoUtc("2026-1a-23T04:00") == 0 && wxIsoUtc("+026-11-23T04:00") == 0, "a non-digit");
  check(wxIsoUtc("2019-12-31T23:59") == 0 && wxIsoUtc("") == 0 && wxIsoUtc(nullptr) == 0,
        "before 2020, empty, none");

  // ---- the HTTP answer ----
  std::string body = REAL, out;
  char head[160];
  snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
           "Content-Length: %zu\r\nConnection: close", body.size());
  check(bodyOf(http(head, body), &out) && out == body, "a 200 with its length");
  check(bodyOf(http("HTTP/1.0 200 OK\r\nContent-Type: application/json", body), &out) && out == body,
        "a 200 that ends when the server closes");
  check(bodyOf(http("HTTP/1.1 200 OK\r\ncontent-length: 2", "{}"), &out) && out == "{}",
        "header names in any case");
  check(!bodyOf(http(head, body.substr(0, body.size() - 5)), &out), "cut short of its length");
  check(!bodyOf(http(head, body + "{}"), &out), "longer than its length");
  check(!bodyOf(http("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2", "{}"), &out),
        "two lengths");
  check(!bodyOf(http("HTTP/1.1 200 OK\r\nContent-Length: 2x", "{}"), &out), "a length that is not a number");
  check(!bodyOf(http("HTTP/1.1 429 Too Many Requests", "{\"error\":true}"), &out), "a 429");
  check(!bodyOf(http("HTTP/1.1 302 Found\r\nLocation: http://evil/", ""), &out), "a redirect is not followed");
  check(!bodyOf(http("HTTP/1.1 2000 OK", body), &out), "a status that only starts 200");
  check(!bodyOf("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n{}", &out), "no end to the headers");
  check(!bodyOf(http("HTTP/1.1 200 OK\r\nContent-Encoding: gzip", "x"), &out), "compressed");
  check(!bodyOf(http("ICY 200 OK", body), &out) && !bodyOf("", &out), "not HTTP, or nothing");
  std::string chunked = "1a\r\n" + body.substr(0, 26) + "\r\n" + std::to_string(0) + "\r\n\r\n";
  { char sz[16]; snprintf(sz, sizeof sz, "%zx", body.size() - 26);
    chunked = "1a\r\n" + body.substr(0, 26) + "\r\n" + sz + "\r\n" + body.substr(26) + "\r\n0\r\n\r\n"; }
  check(bodyOf(http("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked", chunked), &out) && out == body,
        "chunked anyway: put back together");
  check(!bodyOf(http("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked", chunked.substr(0, chunked.size() - 5)), &out),
        "chunked, without its last chunk");
  check(!bodyOf(http("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked", "zz\r\n{}\r\n0\r\n\r\n"), &out),
        "chunked, a size that is not hex");
  check(!bodyOf(http("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked", "ffff\r\n{}\r\n0\r\n\r\n"), &out),
        "chunked, a size past the end");
  check(!bodyOf(http("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 2", chunked), &out),
        "chunked and a length");
  // Bitcoin's review of a990bd8: the last chunk's ending was never checked.
  // Each of these, framed and parsed, must leave the held reading whole.
  { const std::string te = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked";
    char sz[16]; snprintf(sz, sizeof sz, "%zx", body.size());
    const std::string one = std::string(sz) + "\r\n" + body + "\r\n";
    check(bodyOf(http(te.c_str(), one + "0\r\n\r\n"), &out) && out == body, "one chunk, then the end");
    struct { const char *end, *what; } endings[] = {
      // Lead's review of aeee647: what followed a ';' was never checked.
      // An HTTP/1.0 answer may not be chunked at all, so every extension
      // is refused, the well-formed ones too.
      { "0;\r\n\r\n",              "an extension with an empty name" },
      { "0;=x\r\n\r\n",            "an extension with no name" },
      { "0;x=\"unfinished\r\n\r\n", "an extension with an unterminated quote" },
      { "0;x=\x01\r\n\r\n",        "an extension with a control byte" },
      { "0;x=1\r\n\r\n",           "a well-formed extension" },
      { "0;x=\"ok\"\r\n\r\n",      "a well-formed quoted extension" },
      { "0\r\n",                "the last chunk without its final CRLF" },
      { "0\r\nGARBAGE",         "the last chunk, then garbage" },
      { "0NOTHEX\r\n\r\n",      "a last chunk size that is not hex" },
      { "0\r\n\r\nEXTRA",       "bytes after the end" },
      { "0\r\nX-Trailer: 1\r\n\r\n", "a trailer field (none is asked for)" },
      { "0\n\n",                "bare LFs" },
      { "",                     "no last chunk" },
    };
    for (auto &e : endings) {
      std::string resp = http(te.c_str(), one + e.end);
      std::vector<char> b(resp.begin(), resp.end()); b.push_back(0);
      const char *bd = nullptr;
      r = good;
      bool took = wxHttpBody(b.data(), resp.size(), &bd) && wxParseBody(bd, false, NOW, &r);
      char what[120]; snprintf(what, sizeof what, "refused, reading kept whole: %s", e.what);
      check(!took && same(r, good), what);
    }
    check(!bodyOf(http(te.c_str(), std::string(sz) + " x\r\n" + body + "\r\n0\r\n\r\n"), &out),
          "a chunk size, then something that is not an extension");
    check(!bodyOf(http(te.c_str(), std::string(sz) + ";x=1\r\n" + body + "\r\n0\r\n\r\n"), &out),
          "an extension on a data chunk");
    check(!bodyOf(http(te.c_str(), std::string(sz) + "\r\n" + body + "\n0\r\n\r\n"), &out),
          "a chunk's data without its CRLF"); }
  // a raw NUL anywhere: refused, so no C-string check can stop short of it
  check(!bodyOf(http("HTTP/1.0 200 OK", body + std::string("\0junk", 5)), &out), "a raw NUL in the body");
  check(!bodyOf(std::string("HTTP/1.0 200 OK\r\nX-A: ") + std::string("\0", 1) + "b\r\n\r\n" + body, &out),
        "a raw NUL in a header");
  check(!bodyOf(http("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip", body), &out), "another transfer coding");

  // ---- the read, bounded ----
  { static char buf[WX_RESP_MAX + 1];
    std::string resp = http(head, body);
    size_t n; uint32_t t = 0;
    FakePeer p; p.pieces = {{0, resp.substr(0, 100)}, {300, resp.substr(100)}};
    check(readFrom(p, buf, WX_RESP_MAX, &n, &t, WX_READ_MS) == WXR_OK && n == resp.size() &&
          !memcmp(buf, resp.data(), n), "an answer in two pieces, then a close");
    { r = good; const char *b = nullptr;
      check(wxHttpBody(buf, n, &b) && wxParseBody(b, false, NOW, &r) && same(r, good),
            "read, framed and parsed end to end"); }

    t = 0; FakePeer big; big.pieces = {{0, std::string(WX_RESP_MAX + 1, ' ')}};
    check(readFrom(big, buf, WX_RESP_MAX, &n, &t, WX_READ_MS) == WXR_TOO_BIG, "one byte over the cap");
    t = 0; FakePeer flood; for (int i = 0; i < 100; i++) flood.pieces.push_back({0, std::string(1000, 'x')});
    check(readFrom(flood, buf, WX_RESP_MAX, &n, &t, WX_READ_MS) == WXR_TOO_BIG, "a flood stops at the cap");
    t = 0; FakePeer exact; exact.pieces = {{0, std::string(WX_RESP_MAX, ' ')}};
    check(readFrom(exact, buf, WX_RESP_MAX, &n, &t, WX_READ_MS) == WXR_OK && n == WX_RESP_MAX,
          "exactly the cap, then a close");

    t = 0; FakePeer stall; stall.closes = false;
    check(readFrom(stall, buf, WX_RESP_MAX, &n, &t, WX_READ_MS) == WXR_TIMEOUT && t <= WX_READ_MS + 20,
          "a peer that never answers: the deadline");
    t = 0; FakePeer unfinished; unfinished.closes = false;
    unfinished.pieces = {{0, resp.substr(0, resp.size() - 10)}};
    check(readFrom(unfinished, buf, WX_RESP_MAX, &n, &t, WX_READ_MS) == WXR_TIMEOUT,
          "an answer left unfinished and open: the deadline");
    t = 0; FakePeer drip; drip.closes = false;
    for (uint32_t i = 0; i < 1000; i++) drip.pieces.push_back({i * 500, "x"});
    check(readFrom(drip, buf, WX_RESP_MAX, &n, &t, WX_READ_MS) == WXR_TIMEOUT && t <= WX_READ_MS + 20,
          "a byte every half second: the deadline, not the cap");
    t = 0; FakePeer late; late.pieces = {{WX_READ_MS + 500, resp}};
    check(readFrom(late, buf, WX_RESP_MAX, &n, &t, WX_READ_MS) == WXR_TIMEOUT, "an answer after the deadline");
  }

  // ---- freshness, at draw time ----
  r = good;
  check(wxFresh(&r, OBS), "fresh when timed now");
  check(wxFresh(&r, OBS + 3 * 3600), "fresh at three hours");
  check(!wxFresh(&r, OBS + 3 * 3600 + 1), "stale past three hours");
  check(wxFresh(&r, OBS - 1800), "a quarter-hour stamp a little ahead is fine");
  check(!wxFresh(&r, OBS - 3601), "a reading from over an hour in the future is not");
  { WxReading none = {0, -1, 0}; check(!wxFresh(&none, OBS), "no reading is never fresh"); }

  // ---- the place as typed ----
  int16_t la = 0, lo = 0;
  check(wxParsePlace("33.8938, 35.5018", &la, &lo) && la == 339 && lo == 355, "33.8938, 35.5018 -> 33.9, 35.5");
  check(wxParsePlace("33.9 35.5", &la, &lo) && la == 339 && lo == 355, "a space between");
  check(wxParsePlace(" -33.87,151.21 ", &la, &lo) && la == -339 && lo == 1512, "Sydney, no space");
  check(wxParsePlace("40.7128; -74.0060", &la, &lo) && la == 407 && lo == -740, "New York, semicolon");
  check(!wxParsePlace("", &la, &lo), "empty is no place");
  check(!wxParsePlace("Beirut", &la, &lo), "a name is not a place (yet)");
  check(!wxParsePlace("91, 10", &la, &lo), "latitude past the pole");
  check(!wxParsePlace("10, 181", &la, &lo), "longitude past 180");
  check(!wxParsePlace("nan, 10", &la, &lo) && !wxParsePlace("inf, 10", &la, &lo), "nan, inf");
  check(!wxParsePlace("33.9, 35.5, 12", &la, &lo), "three numbers");
  check(!wxParsePlace("33.9", &la, &lo), "one number");
  { char t[24]; wxPlaceText(t, sizeof t, 339, 355); check(!strcmp(t, "33.9, 35.5"), "shown again as typed, rounded"); }
  { char t[24]; wxPlaceText(t, sizeof t, WX_NONE, WX_NONE); check(t[0] == 0, "no place shows blank"); }

  // the setup field, and what the log may say about it
  { int16_t a = 339, o = 355;
    const char *typo = "33.8938,35.5018x";
    int e = wxPlaceEdit(typo, "33.9, 35.5", &a, &o);
    check(e == WXE_REFUSED && a == 339 && o == 355, "a typo: refused, the old place kept");
    for (int k = WXE_SAME; k <= WXE_REFUSED; k++) {
      const char *msg = wxPlaceEditLog(k);
      bool digit = false; for (const char *c = msg; *c; c++) digit |= (*c >= '0' && *c <= '9');
      char what[80]; snprintf(what, sizeof what, "log %d says nothing of what was typed", k);
      check(!digit && !strstr(msg, typo) && !strstr(msg, "33.8938"), what);
    }
    check(wxPlaceEdit("33.9, 35.5", "33.9, 35.5", &a, &o) == WXE_SAME && a == 339, "unchanged");
    check(wxPlaceEdit("48.8566, 2.3522", "33.9, 35.5", &a, &o) == WXE_SET && a == 489 && o == 24, "a new place");
    check(wxPlaceEdit("   ", "48.9, 2.4", &a, &o) == WXE_CLEARED && a == WX_NONE && o == WX_NONE, "cleared");
    check(wxPlaceEdit("", "", &a, &o) == WXE_SAME && a == WX_NONE, "still none"); }

  // the unit field: Fahrenheit unless it says C (OP's default)
  check(WX_DEFAULT_F, "Fahrenheit is the default");
  check(wxUnitIsF("F") && wxUnitIsF("f") && wxUnitIsF(" F"), "F is F");
  check(!wxUnitIsF("C") && !wxUnitIsF("c") && !wxUnitIsF("  C"), "C is C");
  check(wxUnitIsF("") && wxUnitIsF("   ") && wxUnitIsF(nullptr), "blank keeps the default");
  check(wxUnitIsF("x"), "anything else keeps the default");

  // ---- the request: the place to a tenth, nothing else about the wearer ----
  { char q[400];
    wxRequest(q, sizeof q, 339, 355, false);
    check(!strcmp(q, "GET /v1/forecast?latitude=33.9&longitude=35.5"
                     "&current=temperature_2m,weather_code&timezone=GMT HTTP/1.0\r\n"
                     "Host: api.open-meteo.com\r\nAccept: application/json\r\n"
                     "Accept-Encoding: identity\r\nConnection: close\r\n\r\n"), "the request, C");
    wxRequest(q, sizeof q, -339, 1512, true);
    check(!strcmp(q, "GET /v1/forecast?latitude=-33.9&longitude=151.2"
                     "&current=temperature_2m,weather_code&timezone=GMT&temperature_unit=fahrenheit HTTP/1.0\r\n"
                     "Host: api.open-meteo.com\r\nAccept: application/json\r\n"
                     "Accept-Encoding: identity\r\nConnection: close\r\n\r\n"), "the request, F"); }

  // ---- when it may try: one attempt an hour, on either path ----
  // WxGate has no input for the price data, a long UP or a retry: Bitcoin's
  // case (no price data, weather tried two minutes ago) can only say no.
  for (int path = WXP_FETCH; path <= WXP_JOE; path++) {
    WxGate g = {path, true, true, NOW, NOW - 120, false, false, false, false, false};
    char what[96];
    snprintf(what, sizeof what, "path %d: tried 2 minutes ago, no", path);
    check(!wxMayTry(g), what);
    g.lastTry = NOW - 3599; snprintf(what, sizeof what, "path %d: 59:59 ago, no", path); check(!wxMayTry(g), what);
    g.lastTry = NOW - 3600; snprintf(what, sizeof what, "path %d: an hour ago, yes", path); check(wxMayTry(g), what);
    g.lastTry = 0;          snprintf(what, sizeof what, "path %d: never tried, yes", path); check(wxMayTry(g), what);
    g.placeSet = false;     snprintf(what, sizeof what, "path %d: no place, never", path); check(!wxMayTry(g), what);
    g.placeSet = true; g.clockOk = false;
    snprintf(what, sizeof what, "path %d: no clock, never", path); check(!wxMayTry(g), what);
  }
  { // a failure counts: the caller stamps lastTry before it tries
    uint32_t lastTry = 0, t = NOW;
    WxGate g = {WXP_FETCH, true, true, t, lastTry, false, false, false, false, false};
    check(wxMayTry(g), "first try");
    lastTry = t;                                  // it fails
    for (uint32_t dt = 120; dt < 3600; dt += 120) {
      g.now = t + dt; g.lastTry = lastTry;
      if (wxMayTry(g)) { check(false, "a retry inside the hour after a failure"); break; }
    }
    g.now = t + 3600; check(wxMayTry(g), "the hour after a failure"); }
  { WxGate j = {WXP_JOE, true, true, NOW, 0, false, false, false, false, false};
    check(wxMayTry(j), "JOE, a minute wake");
    j.buttonWake = true; check(!wxMayTry(j), "JOE, not on a button"); j.buttonWake = false;
    j.prePaint = true;   check(!wxMayTry(j), "JOE, not pre-painting"); j.prePaint = false;
    j.docked = true;     check(!wxMayTry(j), "JOE, not docked"); j.docked = false;
    j.night = true;      check(!wxMayTry(j), "JOE, not at night"); j.night = false;
    j.cold = true;       check(!wxMayTry(j), "JOE, not in the cold"); }

  printf("weather_test: %d failed\n", fails);
  return fails;
}
