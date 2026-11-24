// The watch's weather request and answer handling, on the build machine,
// against the live server (wx_roots_check.sh pipes it through openssl
// s_client, which checks the chain against WX_ROOTS alone):
//   wx_live_probe request   the exact bytes the watch sends, for 0.0, 0.0
//                           (open sea: nobody's place), Celsius
//   wx_live_probe check     the server's answer on stdin, read with the
//                           watch's cap, framing and checks at this
//                           machine's clock; exit 0 only if it is taken
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "weather.h"

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "request")) {
    char q[400];
    int n = wxRequest(q, sizeof q, 0, 0, false);
    if (n <= 0 || n >= (int)sizeof q) return 1;
    fwrite(q, 1, (size_t)n, stdout);
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "check")) {
    static char buf[WX_RESP_MAX + 2];
    size_t n = fread(buf, 1, WX_RESP_MAX + 1, stdin);
    if (n > WX_RESP_MAX) { printf("wx_live_probe: over WX_RESP_MAX (%d bytes)\n", WX_RESP_MAX); return 1; }
    const char *body = nullptr;
    if (!wxHttpBody(buf, n, &body)) {
      printf("wx_live_probe: not a 200 we can read (%zu bytes):\n%.300s\n", n, buf);
      return 1;
    }
    WxReading r = {0, -1, 0};
    if (!wxParseBody(body, false, (uint32_t)time(nullptr), &r)) {
      printf("wx_live_probe: answer refused:\n%.600s\n", body);
      return 1;
    }
    printf("wx_live_probe: taken, %zu bytes in all: %.1f C, code %d, timed %u (%ld s ago)\n",
           n, r.temp10 / 10.0, (int)r.code, (unsigned)r.obsUtc, (long)time(nullptr) - (long)r.obsUtc);
    return 0;
  }
  fprintf(stderr, "usage: wx_live_probe request|check\n");
  return 2;
}
