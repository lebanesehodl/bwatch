// TEST IMAGE ONLY: branch fw/joe-weather-tlstest, never merged, never in
// customer firmware. It shows on the watch's own TLS (mbedTLS, core 2.0.17)
// what CI shows with OpenSSL (wx_roots_check.sh): api.open-meteo.com
// verifies against WX_ROOTS, and is refused against an unrelated root and
// for a name its certificate doesn't carry, each for its own reason.
//
// Before each weather attempt, three handshakes, one serial line each:
//   [wx] selftest <what>: <verdict> (ret -0x...., flags 0x........: ...) N ms
// The verdicts to see: "connected, as it should be" for the first,
// "refused, as it should be" for the other two. The flags are mbedTLS's
// own verify result, read before the handshake's state is freed:
// 0x00000008 not trusted (the wrong root), 0x00000004 CN mismatch (the
// wrong name).
#pragma once
#include <WiFi.h>
#include <WiFiClientSecure.h>   // ssl_client.h: sslclient_context, start_ssl_client
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "weather.h"
#include "weather_roots.h"

// Amazon Root CA 1, which signs nothing in api.open-meteo.com's chain
// (SHA-256 8E:CD:E6:88:4F:3D:87:B1:12:5B:A3:1A:C3:FC:B1:3D:70:16:DE:7F:57:CC:90:4F:E1:CB:97:C6:AE:98:19:6E)
static const char WX_TEST_WRONG_ROOT[] =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\n"
  "ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\n"
  "b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\n"
  "MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\n"
  "b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\n"
  "ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\n"
  "9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\n"
  "IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\n"
  "VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\n"
  "93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\n"
  "jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\n"
  "AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\n"
  "A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\n"
  "U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\n"
  "N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\n"
  "o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\n"
  "5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\n"
  "rqXRfboQnoZsG4q5WTP468SQvvG5\n"
  "-----END CERTIFICATE-----\n";

// One handshake to ip:443, verifying `name` against `ca`. wantFlag 0: it
// must connect; otherwise it must be refused with that verify flag set.
static void wxTlsProbe(const char *what, IPAddress ip, const char *name, const char *ca,
                       uint32_t wantFlag) {
  sslclient_context *ctx = new sslclient_context;
  ssl_init(ctx);
  ctx->socket = -1;
  ctx->handshake_timeout = WX_HANDSHAKE_S * 1000UL;
  unsigned long t0 = millis();
  int ret = start_ssl_client(ctx, ip, 443, name, WX_CONNECT_MS, ca, false,
                             NULL, NULL, NULL, NULL, false, NULL);
  uint32_t flags = mbedtls_ssl_get_verify_result(&ctx->ssl_ctx);
  char info[200] = "";
  if (flags != 0 && flags != 0xFFFFFFFFu) {
    mbedtls_x509_crt_verify_info(info, sizeof info, "", flags);
    for (char *p = info; *p; p++) if (*p == '\n') *p = ';';
  }
  char err[100] = "";
  if (ret < 0) mbedtls_strerror(ret, err, sizeof err);
  const char *verdict;
  if (wantFlag == 0) verdict = ret >= 0 ? "connected, as it should be" : "FAILED to connect";
  else if (ret >= 0) verdict = "CONNECTED: WRONG, it must be refused";
  else if (flags != 0xFFFFFFFFu && (flags & wantFlag)) verdict = "refused, as it should be";
  else verdict = "refused, but not for that reason";
  Serial.printf("[wx] selftest %s: %s (ret -0x%04x %s, flags 0x%08lx%s%s) %lu ms\n",
                what, verdict, (unsigned)(ret < 0 ? -ret : 0), err, (unsigned long)flags,
                info[0] ? ": " : "", info, millis() - t0);
  stop_ssl_socket(ctx, ca, NULL, NULL);
  delete ctx;
}

static void wxTlsSelftest() {
  IPAddress ip;
  if (!WiFi.hostByName(WX_HOSTNAME, ip)) {
    Serial.println("[wx] selftest: " WX_HOSTNAME " didn't resolve, nothing tested");
    return;
  }
  wxTlsProbe("right root, right name", ip, WX_HOSTNAME, WX_ROOTS, 0);
  wxTlsProbe("unrelated root (Amazon Root CA 1)", ip, WX_HOSTNAME, WX_TEST_WRONG_ROOT,
             MBEDTLS_X509_BADCERT_NOT_TRUSTED);
  wxTlsProbe("another name (wrong.example.com)", ip, "wrong.example.com", WX_ROOTS,
             MBEDTLS_X509_BADCERT_CN_MISMATCH);
}
