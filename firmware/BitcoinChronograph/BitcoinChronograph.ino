/*
 * BITCOIN-CHRONOGRAPH — Watchy firmware, v1-layout port
 *
 * Ports the original web prototype face to the 200x200 e-paper:
 *   - status row: clock | mode name | LIVE/DEMO
 *   - big 7-seg value + unit label
 *   - small boxed 24H% cell + blank pill
 *   - fear/greed tick gauge with needle, 0..100, FEAR/SENTIMENT/GREED
 *   - bordered 24H chart with H/L readout and sparkline
 *   - BITCOIN-CHRONOGRAPH divider + mode strip
 * No branding, no caption — just the instrument.
 *
 * Modes: HGHT / PRICE / SATS / MCAP / FEES / SUPL / HALV / WALT
 * (time lives in the status row, always visible)
 * WALT is WATCH-ONLY (zpub on device, no keys, cannot spend); inside it,
 * UP toggles balance <-> receive QR. The QR always shows the first
 * unused address and auto-advances once the previous one is paid.
 *
 * Libraries: Watchy (SQFMI), ArduinoJson, uBitcoin, QRCode (ricmoo)
 * Buttons: BACK=mode, MENU=system, UP=refresh (in WAL: toggle QR), DOWN=invert
 */

#include <Watchy.h>
// Watchy pulls in Arduino_JSON, which does `#define typeof typeof_` and
// poisons ArduinoJson + the ESP32 core's pgmspace macros. Neutralize it
// before including anything else:
#ifdef typeof
#undef typeof
#endif
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>   // NVS: customer settings survive power loss
#include <WiFiMulti.h>     // roaming: try every saved network
// (docked mode uses fast HTTP polling — no websocket library needed)
#include <Bitcoin.h>            // uBitcoin: watch-only address derivation
#include <Hash.h>               // uBitcoin: doubleSha, for the genesis boot
#include "qrcodegen_local.h"    // ricmoo/QRCode, vendored into the sketch
                                // folder because the ESP32 core ships its
                                // own qrcode.h that shadows the library

#include "DSEG7_Classic_Bold_32.h"   // big values (6 digits fit)
#include "DSEG7_Classic_Bold_25.h"   // small cell, and 7-digit values
#include "DSEG7_Classic_Bold_18.h"   // 8-digit values (full supply)
#include "DSEG7_Classic_Bold_14.h"   // DIFF%/24H% cell, last resort
// The network the watch opens for setup. The library calls it "Watchy AP";
// a buyer sees this name in their phone's WiFi list, so it should be ours.
#undef  WIFI_AP_SSID
#define WIFI_AP_SSID "BWATCH-SETUP"
#include <Fonts/FreeMonoBold9pt7b.h>

// (here, above every other function: the Arduino build inserts its
// function prototypes ahead of the first function, and they name WalletDesc)
// ==== WALLET DESCRIPTOR: BEGIN (host-tested against embit; keep portable) ====
// A multisig wallet, read from its output descriptor:
//   wsh(sortedmulti(2,[a1b2c3d4/48'/0'/0'/2']xpub.../<0;1>/*,...))#checksum
// Native segwit (wsh) and wrapped (sh(wsh(...))), multi and sortedmulti, any
// m-of-n up to 15. The watch only ever derives RECEIVE (and change) addresses
// from public keys: nothing here can spend, and nothing here should ever see
// a private key (descParse refuses one).
//
// Plain C strings and uBitcoin types only, no Arduino String, so the same
// code compiles on a desktop for testing.

static const int DESC_MAX_KEYS = 15;

struct WalletDesc {
  bool ok;                     // parsed and usable
  bool nested;                 // sh(wsh(...)) rather than wsh(...)
  bool sorted;                 // sortedmulti rather than multi
  uint8_t m, n;
  bool hasFp[DESC_MAX_KEYS];   // origin given: [fingerprint/path]
  uint8_t fp[DESC_MAX_KEYS][4];
  char tail[DESC_MAX_KEYS][9]; // the xpub's last 8 chars, when there is no origin
  // what follows each xpub: 0 = nothing (chain/index appended),
  // 1 = "/<0;1>/*" or "/0/*" (the receive branch, change = 1)
  uint8_t suffix[DESC_MAX_KEYS];
  HDPublicKey keys[DESC_MAX_KEYS];
  char checksum[9];            // computed over what was pasted
  int  given;                  // 0 none pasted, 1 matched, -1 did not match
  const char *err;             // why not, in a few words for the screen
};

// ---- BIP 380 descriptor checksum ----
static uint64_t descPolymod(uint64_t c, int val) {
  uint8_t c0 = c >> 35;
  c = ((c & 0x7ffffffffULL) << 5) ^ val;
  if (c0 & 1)  c ^= 0xf5dee51989ULL;
  if (c0 & 2)  c ^= 0xa9fdca3312ULL;
  if (c0 & 4)  c ^= 0x1bab10e32dULL;
  if (c0 & 8)  c ^= 0x3706b1677aULL;
  if (c0 & 16) c ^= 0x644d626ffdULL;
  return c;
}

// the checksum of s[0..len), into out[9]; false on a character it cannot take
static bool descChecksum(const char *s, size_t len, char out[9]) {
  static const char *IN =
    "0123456789()[],'/*abcdefgh@:$%{}"
    "IJKLMNOPQRSTUVWXYZ&+-.;<=>?!^_|~"
    "ijklmnopqrstuvwxyzABCDEFGH`#\"\\ ";
  static const char *CS = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
  uint64_t c = 1;
  int cls = 0, clscount = 0;
  for (size_t i = 0; i < len; i++) {
    const char *p = strchr(IN, s[i]);
    if (!p || !s[i]) return false;
    int pos = p - IN;
    c = descPolymod(c, pos & 31);
    cls = cls * 3 + (pos >> 5);
    if (++clscount == 3) { c = descPolymod(c, cls); cls = 0; clscount = 0; }
  }
  if (clscount > 0) c = descPolymod(c, cls);
  for (int j = 0; j < 8; j++) c = descPolymod(c, 0);
  c ^= 1;
  for (int j = 0; j < 8; j++) out[j] = CS[(c >> (5 * (7 - j))) & 31];
  out[8] = 0;
  return true;
}

static bool descStartsWith(const char *s, const char *pre) {
  return strncmp(s, pre, strlen(pre)) == 0;
}

// Something that must never be pasted: an extended private key, or a
// mnemonic (a dozen or more plain words). The setup refuses these outright.
static bool looksPrivate(const char *s) {
  static const char *PRV[] = {"xprv", "yprv", "zprv", "Yprv", "Zprv",
                              "tprv", "uprv", "vprv", "Uprv", "Vprv"};
  // only where a key starts (the beginning, or after ( , ] or a space):
  // "xprv" can turn up by chance in the middle of a perfectly good xpub
  for (const char *p = s; *p; p++) {
    if (p != s && !strchr("(,] \n\r\t", p[-1])) continue;
    for (const char *k : PRV)
      if (descStartsWith(p, k)) return true;
  }
  // words: letters and spaces only, eleven or more spaces between them
  int spaces = 0, letters = 0;
  for (const char *p = s; *p; p++) {
    if (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
      spaces++;
      while (p[1] == ' ' || p[1] == '\n' || p[1] == '\r' || p[1] == '\t') p++;
    } else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) letters++;
    else return false;
  }
  return spaces >= 11 && letters > 30;
}

static bool hexNibble(char ch, uint8_t &v) {
  if (ch >= '0' && ch <= '9') { v = ch - '0'; return true; }
  if (ch >= 'a' && ch <= 'f') { v = ch - 'a' + 10; return true; }
  if (ch >= 'A' && ch <= 'F') { v = ch - 'A' + 10; return true; }
  return false;
}

// one key expression, [fp/path]xpub<suffix>, ending at ',' or ')'
static bool descKey(WalletDesc &d, int k, const char *&p) {
  d.hasFp[k] = false; d.suffix[k] = 0; d.tail[k][0] = 0;
  if (*p == '[') {
    p++;
    for (int i = 0; i < 4; i++) {
      uint8_t hi, lo;
      if (!hexNibble(p[0], hi) || !hexNibble(p[1], lo)) { d.err = "BAD FINGERPRINT"; return false; }
      d.fp[k][i] = (hi << 4) | lo; p += 2;
    }
    d.hasFp[k] = true;
    while (*p && *p != ']') p++;          // the origin path: shown by the
    if (*p != ']') { d.err = "BAD KEY ORIGIN"; return false; }   // checksum
    p++;
  }
  const char *start = p;
  while (*p && *p != '/' && *p != ',' && *p != ')') p++;
  size_t len = p - start;
  if (len < 100 || len > 120) { d.err = "BAD KEY"; return false; }
  char x[121]; memcpy(x, start, len); x[len] = 0;
  if (!descStartsWith(x, "xpub")) { d.err = "MAINNET XPUBS ONLY"; return false; }
  d.keys[k] = HDPublicKey(x);
  if (!d.keys[k].isValid()) { d.err = "BAD KEY"; return false; }
  memcpy(d.tail[k], x + len - 8, 8); d.tail[k][8] = 0;
  // receive branch and a wildcard: what every multisig coordinator exports.
  // Anything else (a fixed key, hardened steps, deeper paths) would give
  // addresses the wallet does not, so it is refused rather than guessed at.
  if (descStartsWith(p, "/<0;1>/*")) { p += 8; d.suffix[k] = 1; }
  else if (descStartsWith(p, "/0/*"))  { p += 4; d.suffix[k] = 1; }
  else { d.err = "UNSUPPORTED KEY PATH"; return false; }
  return true;
}

// Parse a pasted descriptor. Whitespace is ignored around it; inside it,
// anything unexpected is an error rather than a guess.
static bool descParse(const char *in, WalletDesc &d) {
  d.ok = false; d.err = "NOT A DESCRIPTOR"; d.given = 0; d.checksum[0] = 0;
  if (looksPrivate(in)) { d.err = "PRIVATE KEY OR SEED"; return false; }
  while (*in == ' ' || *in == '\n' || *in == '\r' || *in == '\t') in++;
  size_t len = strlen(in);
  while (len && (in[len - 1] == ' ' || in[len - 1] == '\n' ||
                 in[len - 1] == '\r' || in[len - 1] == '\t')) len--;
  const char *hash = (const char *)memchr(in, '#', len);
  size_t body = hash ? (size_t)(hash - in) : len;
  if (!descChecksum(in, body, d.checksum)) { d.err = "BAD CHARACTER"; return false; }
  if (hash) {
    if (len - body - 1 != 8 || strncmp(hash + 1, d.checksum, 8) != 0) {
      d.given = -1; d.err = "CHECKSUM WRONG"; return false;
    }
    d.given = 1;
  }
  const char *p = in;
  d.nested = false;
  if (descStartsWith(p, "sh(wsh(")) { d.nested = true; p += 7; }
  else if (descStartsWith(p, "wsh(")) p += 4;
  else if (descStartsWith(p, "tr("))  { d.err = "TAPROOT NOT YET"; return false; }
  else return false;
  if (descStartsWith(p, "sortedmulti(")) { d.sorted = true; p += 12; }
  else if (descStartsWith(p, "multi(")) { d.sorted = false; p += 6; }
  else { d.err = "ONLY MULTI / SORTEDMULTI"; return false; }
  int m = 0;
  while (*p >= '0' && *p <= '9') m = m * 10 + (*p++ - '0');
  if (*p != ',' || m < 1) { d.err = "BAD THRESHOLD"; return false; }
  int n = 0;
  while (*p == ',') {
    p++;
    if (n >= DESC_MAX_KEYS) { d.err = "TOO MANY KEYS"; return false; }
    if (!descKey(d, n, p)) return false;
    n++;
  }
  if (*p++ != ')' || *p++ != ')' || (d.nested && *p++ != ')')) {
    d.err = "BAD BRACKETS"; return false;
  }
  if ((size_t)(p - in) != body) { d.err = "TRAILING TEXT"; return false; }
  if (m > n) { d.err = "THRESHOLD OVER KEYS"; return false; }
  d.m = m; d.n = n;
  d.ok = true; d.err = "";
  return true;
}

// The keys of one branch (0 receive, 1 change): derived once and kept, so a
// scan walking twenty addresses pays one derivation per key per address.
static void descBranch(const WalletDesc &d, int chain, HDPublicKey *out) {
  for (int k = 0; k < d.n; k++) out[k] = d.keys[k].child(chain);
}

// Address `index` from branch keys, into out; 0 on failure.
static size_t descAddressFrom(const WalletDesc &d, const HDPublicKey *branch,
                              int index, char *out, size_t outLen) {
  if (!d.ok) return 0;
  PublicKey pubs[DESC_MAX_KEYS];
  for (int k = 0; k < d.n; k++) pubs[k] = branch[k].child(index);
  // sortedmulti: keys in byte order of their compressed encoding (BIP 67).
  // Sorted HERE: uBitcoin's own sortedmulti() does not sort (its source says
  // TODO), and its addresses were wrong in every sortedmulti test vector.
  if (d.sorted) {
    uint8_t sec[DESC_MAX_KEYS][33];
    for (int k = 0; k < d.n; k++) pubs[k].sec(sec[k], 33);
    for (int i = 1; i < d.n; i++)                    // insertion sort, n <= 15
      for (int j = i; j > 0 && memcmp(sec[j - 1], sec[j], 33) > 0; j--) {
        uint8_t t[33]; memcpy(t, sec[j], 33); memcpy(sec[j], sec[j - 1], 33);
        memcpy(sec[j - 1], t, 33);
        PublicKey tp = pubs[j]; pubs[j] = pubs[j - 1]; pubs[j - 1] = tp;
      }
  }
  Script ms = multi(d.m, pubs, d.n);
  Script spk = d.nested ? sh(wsh(ms)) : wsh(ms);
  return spk.address(out, outLen);
}

// Address `index` on `chain`, the slow way (derives the branch every call)
static size_t descAddress(const WalletDesc &d, int chain, int index,
                          char *out, size_t outLen) {
  static HDPublicKey branch[DESC_MAX_KEYS];
  descBranch(d, chain, branch);
  return descAddressFrom(d, branch, index, out, outLen);
}
// ==== WALLET DESCRIPTOR: END ====

// Watchy library menu internals (globals defined in Watchy.cpp)
extern int  guiState;
extern int  menuIndex;
extern bool alreadyInMenu;
// (USB state is read directly from USB_DET_PIN — the library's cached
//  flag proved unreliable across charger power flaps)
#ifdef ARDUINO_ESP32S3_DEV
  #define BTN_ACTIVE 0
#else
  #define BTN_ACTIVE 1
#endif
// If your Watchy lib lacks the 32pt header, generate it with Adafruit
// GFX fontconvert from DSEG7Classic-Bold.ttf at 32pt.

// ---------------- WATCH-ONLY WALLET ----------------
// FACTORY DEFAULT only: customers set their real zpub on-device via
// Menu -> Setup Wallet (WiFi portal, paste from phone) — it's stored in
// NVS flash and overrides this. Watch-only: can NEVER spend.
// Never paste an xprv/zprv. Point ESPLORA_BASE at your own node for privacy.
#define WALLET_ZPUB  ""   // none: until one is set, the wallet face is the demo
// THE DEMO: with no wallet set, the wallet face watches the genesis address,
// the one block 0 paid. It has never spent and still receives tributes, so
// the demo is a real, moving balance, labelled as Satoshi's. Only Satoshi's
// key could spend from it, so anything sent there is gone: the demo never shows a
// receive QR, and it never raises PAY or CONF screens for strangers' coins.
#define DEMO_ADDR    "1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa"
// The BIP84 TEST VECTOR, the account of the published "abandon ... about"
// seed, was the default here once. Its keys are public: if a watch still has
// it stored, it is treated as no wallet at all, and the demo takes over.
#define BIP84_TEST_ZPUB "zpub6rFR7y4Q2AijBEqTUquhVz398htDFrtymD9xYYfG1m4wAcvPhXNfE3EfH1r1ADqtfSdVCToUG868RvUUkgDKf31mGDtKsAYz2oz2AGutZYs"
#define SCAN_ADDRS   10   // (legacy; the scan is now gap-limit based)
// MULTISIG (or any wallet): instead of a zpub, paste literal receive
// addresses, comma-separated, exported from your wallet software (in
// Sparrow: Receive tab, or Addresses tab -> copy). These are computed
// by your REAL descriptor, so they're correct by construction — the
// watch never derives, only watches. Leave empty to use the zpub.
// NEVER paste a single cosigner's zpub for a multisig wallet: the
// derived addresses would NOT belong to your multisig.
#define WALLET_ADDR_LIST ""   // e.g. "bc1q...,bc1q...,bc1q..."
// LIGHTNING (optional): a lightning address (user@domain) enables the
// third WAL view: the watch requests a REAL invoice from your
// address's LNURL-pay endpoint for LN_REQUEST_SATS, shows its QR, and
// polls the standard verify endpoint - triple buzz within seconds of
// settlement. Set via Menu -> Setup Wallet (stored in NVS) or here.
// LN_REQUEST_SATS = 0 shows a plain any-amount address QR instead
// (no settlement detection - an address alone can't be watched).
// LN_BOLT12: paste an offer for display-only (detection needs your
// node's API - a future integration, not faked here).
#define LN_ADDRESS       ""
// 0 (default): any-amount QR of the address itself — small code,
// instant display, payer picks the amount; no settlement buzz (an
// address alone can't be watched). Set a sats amount to switch to
// invoice mode: bigger QR, brief fetch, but the watch CAN detect
// settlement when the provider supports verify.
#define LN_REQUEST_SATS  0
#define LN_BOLT12        "" 
#define WALLET_EVERY_MIN 30
#define ESPLORA_BASE "https://blockstream.info/api/address/"

// ---------------- settings ----------------
const char *PRICE_URL =
  "https://api.coingecko.com/api/v3/simple/price"
  "?ids=bitcoin&vs_currencies=usd&include_24hr_change=true&include_market_cap=true";
const char *FNG_URL  = "https://api.alternative.me/fng/?limit=1";
const char *FEES_URL = "https://mempool.space/api/v1/fees/recommended";
const char *TIP_URL  = "https://mempool.space/api/blocks/tip/height";
const char *TIP_URL2 = "https://blockstream.info/api/blocks/tip/height";
const char *DIFF_URL = "https://mempool.space/api/v1/difficulty-adjustment";
// The three-day average: the instantaneous figure swings wildly with luck,
// and a number that jumps 30% between glances is not a reading.
const char *HASH_URL = "https://mempool.space/api/v1/mining/hashrate/3d";
// Watchy v3 routes the charger's status line to GPIO10. Charger status pins
// are open-drain: held LOW while charging, released HIGH once the charge
// terminates. That is ground truth for "is the cell actually taking charge",
// which a voltage reading alone cannot tell you.
#define CHG_STAT_PIN 10

#define FETCH_EVERY_MIN 15   // the fallback. The live interval comes from
                             // fetchEveryMin(), which follows the chain's
                             // measured pace; this is what it returns if
                             // avgBlockSec is not yet trustworthy.
// The single freshness threshold. The corner tag flips LIVE -> OLD here, and
// the height's unit line starts estimating here: both indicators must change
// together or a wearer sees LIVE next to an estimate and cannot tell which to
// believe. Must stay above FETCH_EVERY_MIN so a normal cycle never trips it.
#define STALE_AFTER_MIN 20
// ---- fetch tiers ----
// Every cycle gets what the faces show live: price, fees, tip, mempool. The
// rest changes slowly, and each request costs a TLS handshake with the radio
// on, so it is fetched on its own clock. A long-press UP, or a watch with no
// data yet, fetches everything regardless.
#define CANDLES_EVERY_MIN  30   // the 24h sparkline (hourly candles)
#define GOLD_EVERY_MIN     60   // gold spot, for the BTC/gold ratio
#define SLOW_EVERY_MIN     60   // difficulty estimate and 3-day hashrate
#define FNG_EVERY_MIN     180   // Fear & Greed is published once a day
// (block reward and miner: only when the tip has moved)
// ---- alarms and confirmations ----
#define ALARM_NEAR_BLOCKS   2   // within this many blocks of a block alarm,
#define ALARM_NEAR_EVERY    3   // fetch every 3 min so it lands on time
#define EVENT_HOLD_MIN     60   // an unacknowledged alarm screen keeps the
                                // watch from resting this long, then rest
                                // may cover it; it shows again on pickup
#define LATE_WATCH_MIN     60   // after a vigil sees nothing, keep checking
                                // that address (or invoice) at each fetch
#define TRACK_GIVEUP_MIN 4320   // stop following a payment that has not
                                // confirmed in 3 days
#define FETCH_IDLE_MIN  45   // when the watch has been still for a while it
                             // is on a table, not a wrist: nobody is reading
                             // it, so stop spending radio on their behalf
#define STILL_AFTER_MIN 60   // minutes of no movement before that kicks in
#define MOVE_THRESHOLD  90   // raw counts; a wrist easily clears this
// ---- rest and night ----
// E-paper costs nothing to hold, so the saving is in WAKES, not pixels. When
// nobody can be reading the watch it puts up a face that stays true without
// a clock (the chain, AS OF when it was fetched) and sleeps for minutes at a
// time instead of one. Any button brings it straight back.
#define NIGHT_FROM_H     0   // default night: 00:00 local ...
#define NIGHT_TO_H       6   // ... to 06:00: no radio, no minute wakes.
                             // The wearer sets their own in Menu > Night
                             // Hours (kept in flash); From = To turns it off
#define PRESS_GRACE_MIN  3   // after any press the full face stays this long,
                             // or a watch on a table would rest again at once
#define REST_WAKE_MIN    5   // resting by day: check for a pickup this often
#define FLAT_STILL_MIN  10   // flat on its back and dead still this long = on
                             // a table, not a wrist
// The next three are starting guesses. The [accel] serial line prints delta,
// x, y, z and the pose on every minute wake: wear it a day, leave it on a
// table a night, and set these from what it says.
#define STILL_DELTA     24   // raw counts; tighter than MOVE_THRESHOLD. A
                             // wrist has tremor, a table has sensor noise
#define FLAT_XY        180   // |x| and |y| under ~10 degrees off level
#define FLAT_Z         900   // and nearly all of 1 g (~1024) on z
#define FACE_UP_Z_SIGN   1   // sign of z lying face-up. If the log shows a
                             // face-up watch reading DOWN, flip this to -1
// ---- shake to wake ----
// Resting by day, the watch only looks for a pickup every REST_WAKE_MIN. With
// this on, the accelerometer watches instead: a shake or a pickup wakes it at
// once, like a button. Off by night, where a wrist in bed moves on its own.
#define SHAKE_WAKE       0   // off: untested on hardware, and a press
                             // now wakes fast anyway. 1 to try it
#define SHAKE_AT_NIGHT   0
// Starting guesses, to tune from the [shake] serial lines. The BMA423 compares
// successive samples at 50 Hz: wake when the change exceeds SHAKE_THRESH
// (1/2048 g per count, so 0x200 = 0.25 g) for SHAKE_DUR samples (20 ms each).
// Too many false wakes from bumps on a table: raise either. Shakes missed:
// lower them.
#define SHAKE_THRESH 0x200
#define SHAKE_DUR        3
// ---- haptics ----
// BUZZ_ON_BLOCK: one short tick when new block(s) are discovered at a
//   fetch. NOTE: the watch deep-sleeps, so this fires at fetch cadence
//   (every FETCH_EVERY_MIN), not at the real block moment — that's the
//   price of multi-day battery. The Metronome is the realtime sibling.
// BUZZ_PRICE_STEP: haptic when price moves this many USD from the last
//   alert anchor. Up = two quick buzzes, down = one long buzz. 0 = off.
#define BUZZ_ON_BLOCK   true
#define BUZZ_PRICE_STEP 1000
// Timezone lives in the system menu (MENU button -> Timezone),
// alongside Setup WiFi and Set Time, where settings belong.
#define SPARK_POINTS    48
#define HALVING_EPOCH   1839542400UL   // est. 2028-04-17, block 1,050,000

// ---------------- persisted across deep sleep ----------------
// RTC LAYOUT GUARD: RTC RAM survives reflashes, but the firmware's
// variable layout doesn't — after a reflash that adds/moves RTC vars,
// old bytes get reinterpreted as garbage (boot-crash-loop city). When
// the stored magic doesn't match, all volatile state resets. NVS
// settings (zpub, keychain, timezone, theme) are unaffected.
// BUMP THIS whenever RTC_DATA_ATTR variables are added/removed/moved.
// Forgetting is catastrophic-but-subtle: rtcMagic's own bytes may not
// move, the check passes, and only the NEW variables boot as garbage
// (field crash: RANGE_LB[garbage] = wild pointer, dead PRC mode).
#define RTC_LAYOUT_MAGIC 0xB17C014B
#define PARTIALS_BEFORE_FULL 30   // minute ticks between full face refreshes
RTC_DATA_ATTR uint32_t rtcMagic     = 0;
RTC_DATA_ATTR int      dispMode     = 0;
RTC_DATA_ATTR int      themeMode     = 0;   // 0 LIGHT, 1 DARK, 2 AUTO
                                             // (AUTO: dark 20:00-07:00
                                             //  in the selected timezone)
RTC_DATA_ATTR float    btcPrice     = 0;
RTC_DATA_ATTR float    btcChange24h = 0;
RTC_DATA_ATTR float    btcMcapB     = 0;
RTC_DATA_ATTR int      fearGreed    = 50;
RTC_DATA_ATTR float    fastFee      = 0;   // sat/vB. Float, because a
                                           // quiet mempool runs below 1
                                           // and an int truncates it to 0
RTC_DATA_ATTR float    spark[SPARK_POINTS] = {0};
RTC_DATA_ATTR uint8_t  sparkHead    = 0;
RTC_DATA_ATTR uint32_t lastFetchMin = 0;
RTC_DATA_ATTR bool     haveData     = false;
// wallet state (watch-only)
RTC_DATA_ATTR uint64_t walletSats    = 0;
RTC_DATA_ATTR int      recvIndex     = 0;      // first unused receive address
RTC_DATA_ATTR uint8_t  walletView    = 0;  // WAL: 0 balance, 1 chain
                                            // QR, 2 lightning QR
RTC_DATA_ATTR char     lnAddrBuf[64]  = {0};
RTC_DATA_ATTR char     lnInvoice[420] = {0};   // fetched BOLT11
RTC_DATA_ATTR char     lnVerify[200]  = {0};   // LUD-21 verify URL
RTC_DATA_ATTR uint32_t lnFetchedWake  = 0;     // invoice age (wake min)
RTC_DATA_ATTR uint32_t lastWalletMin = 0;
RTC_DATA_ATTR bool     vigilPending  = false;  // payment vigil on next
                                               // wrist QR render
RTC_DATA_ATTR long long lastTxSats    = 0;     // last observed wallet
                                               // movement (+recv/-sent)
RTC_DATA_ATTR bool     haveWallet    = false;
// timechain state
RTC_DATA_ATTR long     blockHeight   = 0;
RTC_DATA_ATTR uint32_t lastHeightWake = 0;   // height has its OWN freshness:
                                             // the tip fetch can fail while
                                             // price succeeds, and vice versa
RTC_DATA_ATTR long     lastBuzzHeight = 0;
RTC_DATA_ATTR float    priceAnchor    = 0;
RTC_DATA_ATTR float    diffChangeEst = 0;   // estimated retarget %, from mempool.space
RTC_DATA_ATTR float    medFee        = 0;   // halfHourFee
RTC_DATA_ATTR float    lowFee        = 0;   // hourFee
RTC_DATA_ATTR float    lastRewardBtc = 0;   // subsidy + fees, in BTC
RTC_DATA_ATTR long     lastTxCount   = 0;   // transactions in that block
RTC_DATA_ATTR float    netHashEH     = 0;   // network hashrate, exahash/s
RTC_DATA_ATTR uint32_t usbSeenWake   = 0;    // first wake USB was noticed, so
                                             // a fresh charge is not mistaken
                                             // for a finished one
RTC_DATA_ATTR float    minVoltSeen   = 9.9f; // the lowest true reading ever
RTC_DATA_ATTR uint32_t minVoltWake   = 0;    // and the wake it happened on
RTC_DATA_ATTR uint8_t  minVoltFlags  = 0;    // bit0 travelling, bit1 docked
RTC_DATA_ATTR long     retEpoch      = 0;   // the epoch the watch last knew it was in
RTC_DATA_ATTR uint32_t retStartT     = 0;   // when that epoch began (its first block)
RTC_DATA_ATTR long     evRetEpoch    = 0;   // the retarget recap waiting to be seen
RTC_DATA_ATTR float    evRetChange   = 0, evRetDays = 0, evRetMined = 0, evRetFees = -1;
RTC_DATA_ATTR long     evRetTxs      = 0;
RTC_DATA_ATTR bool     retPending    = false;  // read, not yet shown
RTC_DATA_ATTR long     halvEra       = 0;      // the era the watch last knew (0 = unseen)
RTC_DATA_ATTR int      evHalvN       = 0;      // the halving recap waiting to be seen
RTC_DATA_ATTR float    evHalvYears   = 0, evHalvFees = -1;
RTC_DATA_ATTR bool     halvPending   = false;
RTC_DATA_ATTR long     lastCurio     = 0;   // height of the last curiosity
RTC_DATA_ATTR long     curioSeen     = 0;   // every height up to here checked
                                            // marked, so one never repeats
RTC_DATA_ATTR char     poolName[12]  = {0};  // who mined the last block
RTC_DATA_ATTR long     rwdHeight     = 0;    // the height the reward/
                                             // miner actually describe
RTC_DATA_ATTR int16_t  lastAx = 0, lastAy = 0, lastAz = 0;
RTC_DATA_ATTR uint32_t lastMoveWake  = 0;   // last wake that saw movement
RTC_DATA_ATTR bool     accelSeeded   = false;
// A 24-hour discharge log: one raw reading an hour, kept in RTC memory so it
// survives sleep. Stored as (volts * 50) in a byte — 0.02 V resolution, which
// is finer than the ADC deserves. The point is the SHAPE: if the trace sits
// flat and then falls off a cliff, the curve is wrong, not the cell.
// Three days, one sample an hour: a full run from the charger to flat is
// longer than a day now, and a log that only holds the last 24 hours never
// shows one end to end. VLOG_CHG marks where a charge interrupted the run.
#define VLOG_N   72
#define VLOG_CHG 0xFF       // never a reading: 4.2 V stores as 210
RTC_DATA_ATTR uint8_t  vlog[VLOG_N] = {0};
RTC_DATA_ATTR uint8_t  vlogIdx  = 0;   // next slot, always < VLOG_N
RTC_DATA_ATTR uint32_t vlogWake = 0;

RTC_DATA_ATTR double   travelPastPrice = 0;   // real price at the travelled
RTC_DATA_ATTR long     travelPastFor   = -1;  // height, fetched once on
                                              // arrival. The table is only
                                              // the fallback for no network
RTC_DATA_ATTR bool     resting       = false;  // the cell ran out and the
                                               // panel is holding its last
                                               // image; wake only to check
                                               // whether a charger arrived
RTC_DATA_ATTR uint32_t candleWake    = 0;      // wakeMin each slow tier was
RTC_DATA_ATTR uint32_t goldWake      = 0;      // last fetched OK; 0 = never
RTC_DATA_ATTR uint32_t slowWake      = 0;
RTC_DATA_ATTR uint32_t fngWake       = 0;
RTC_DATA_ATTR float    dayAgoUsd     = 0;      // the close 24 h back, so the
                                               // 24h %% can follow the live price
RTC_DATA_ATTR char     fastSsid[33]  = {0};    // the access point that answered
RTC_DATA_ATTR uint8_t  fastBssid[6]  = {0};    // last time, and its channel:
RTC_DATA_ATTR uint8_t  fastChan      = 0;      // 0 = unknown, scan instead
RTC_DATA_ATTR long     alarmHeight   = 0;      // block alarm, 0 = off  (NVS)
RTC_DATA_ATTR uint16_t feeAlarm      = 0;      // sat/vB, 0 = off       (NVS)
RTC_DATA_ATTR bool     feeArmed      = true;   // false after firing, until
                                               // fees rise well above again
RTC_DATA_ATTR uint8_t  evQueue       = 0;      // EV_* bits waiting to be seen
RTC_DATA_ATTR uint8_t  evShown       = 0;      // the one on the glass now
RTC_DATA_ATTR uint32_t evWake        = 0;      // wakeMin the last one fired
RTC_DATA_ATTR long     evCurioH      = 0;      // the curiosity waiting to be seen
RTC_DATA_ATTR long     evBlockH      = 0;      // what each screen reports,
RTC_DATA_ATTR float    evFee         = 0;      // captured when it fired so a
RTC_DATA_ATTR uint16_t evFeeAt       = 0;      // later edit or a finished
RTC_DATA_ATTR uint8_t  evConfN       = 0;      // track cannot change what
RTC_DATA_ATTR long long evConfSats   = 0;      // the screen says
RTC_DATA_ATTR long     evConfBlock   = 0;
RTC_DATA_ATTR long long evPaySats    = 0;      // a payment seen landing while
RTC_DATA_ATTR bool     evPayLn       = false;  // the watch was elsewhere
RTC_DATA_ATTR uint8_t  lateKind      = 0;      // LATE_NONE / _CHAIN / _LN
RTC_DATA_ATTR int      lateIdx       = -1;     // receive index the QR showed
RTC_DATA_ATTR uint64_t lateBase      = 0;      // its funded sum when shown
RTC_DATA_ATTR uint32_t lateUntil     = 0;      // wakeMin the watch ends
RTC_DATA_ATTR bool     shipMode      = false;  // asleep in the box: only a
                                               // held button or USB wakes it
RTC_DATA_ATTR uint32_t shipSince     = 0;      // RTC epoch it went to sleep
RTC_DATA_ATTR uint8_t  trackState    = 0;      // TRACK_NONE / _RESOLVE / _ON
RTC_DATA_ATTR char     trackAddr[64] = {0};    // where the payment landed
RTC_DATA_ATTR char     trackTxid[65] = {0};
RTC_DATA_ATTR long long trackSats    = 0;      // paid to that address
RTC_DATA_ATTR long     trackHeight   = 0;      // block it confirmed in, 0 = not yet
RTC_DATA_ATTR uint8_t  trackShown    = 0;      // last count announced: 0/1/3/6
RTC_DATA_ATTR uint32_t trackWake     = 0;      // wakeMin tracking began
RTC_DATA_ATTR char     lastDoneTx[9] = {0};    // txid prefix already reported
RTC_DATA_ATTR uint8_t  restState     = 0;      // REST_NONE / _DAY / _NIGHT
RTC_DATA_ATTR int8_t   restPose      = 0;      // pose when rest began: a
                                               // turned-over watch is a
                                               // picked-up watch
RTC_DATA_ATTR uint32_t sleptSince    = 0;      // RTC epoch a long sleep began,
                                               // so wakeMin can catch up
RTC_DATA_ATTR uint16_t stillRun      = 0;      // consecutive flat dead-still wakes
RTC_DATA_ATTR uint32_t awakeTill     = 0;      // wakeMin the last press lasts to
RTC_DATA_ATTR uint8_t  joeStep       = 0;      // JOE's UP cycle: 0 time, 1 the
                                               // chain, then each person, then
                                               // the watch; a minute tick resets
RTC_DATA_ATTR int8_t   evAgeWho      = -1;     // a block birthday waiting on the
RTC_DATA_ATTR uint32_t evAgeN        = 0;      // glass: who (4 = the watch), age
RTC_DATA_ATTR bool     joeReveal     = false;  // tap UP in JOE and the date
                                               // line shows the block height
                                               // for one refresh
RTC_DATA_ATTR bool     captivePortal = false;  // associated to WiFi, but a
                                               // portal is intercepting: the
                                               // watch cannot accept terms,
                                               // and the face should say so
                                               // rather than look broken
RTC_DATA_ATTR bool     travelActive  = false;  // the whole face is simulated
RTC_DATA_ATTR long     travelHeight  = 0;      // at this height
RTC_DATA_ATTR uint32_t travelWake    = 0;      // and expires 30 min from here.
                                               // A simulation you can forget
                                               // you are in is a lie waiting
                                               // to happen
RTC_DATA_ATTR bool     sawMillion    = false;  // block 1,000,000 has been
                                               // met and marked. Stored in
                                               // NVS: it happens once and the
                                               // watch should not forget
RTC_DATA_ATTR int8_t   nightFrom     = NIGHT_FROM_H;  // the wearer's night,
RTC_DATA_ATTR int8_t   nightTo       = NIGHT_TO_H;    // whole local hours
// Which way round the panel is right now: light or dark. A partial refresh
// that flips the whole panel (theme change, a light menu over a dark face)
// under-drives every pixel, and the image fades within seconds. Any paint
// whose polarity differs from this one is done as a full refresh instead.
RTC_DATA_ATTR bool     paintedDark   = false;
RTC_DATA_ATTR int8_t   menuLevel     = 0;      // 0 main, 1 My Watch, 2 Settings
// My Watch: the wearer's choices, all kept in flash (loadPrefs/savePrefs)
RTC_DATA_ATTR int8_t   darkFrom      = 20;     // AUTO theme is dark from ...
RTC_DATA_ATTR int8_t   darkTo        = 7;      // ... to, local hours
RTC_DATA_ATTR bool     buzzBlock     = BUZZ_ON_BLOCK;   // tick on a new block
RTC_DATA_ATTR uint16_t priceStep     = BUZZ_PRICE_STEP; // USD per price buzz, 0 off
RTC_DATA_ATTR uint16_t faceHide      = 0;      // bit per face: 1 = left out
RTC_DATA_ATTR uint8_t  battProfile   = 0;      // 0 Live, 1 Balanced, 2 Saver
RTC_DATA_ATTR bool     walletPeek    = false;  // privacy: shown for one glance
RTC_DATA_ATTR bool     cleanNext     = false;  // a fast partial wake left a
                                               // ghost: next minute, full
RTC_DATA_ATTR uint8_t  lastMode      = 0;      // what the last minute wake was:
RTC_DATA_ATTR uint8_t  lastWhy       = 0;      // awake/rest/night, and if awake
                                               // when it could have rested, why
                                               // (About shows it: a watch that
                                               // stopped resting says why)
RTC_DATA_ATTR uint8_t  facePartials  = 0;      // partial face refreshes since
                                               // the last full one
RTC_DATA_ATTR int8_t   drawnMode     = -1;     // the face (and WAL view) the
RTC_DATA_ATTR int8_t   drawnView     = -1;     // panel last showed: a change is
                                               // the big redraw that ghosts
RTC_DATA_ATTR bool     fetchPending  = false;  // a long-press asked for data
                                               // and the radio has not gone
                                               // out yet. The corner says so,
                                               // rather than claiming LIVE
                                               // through several silent seconds
RTC_DATA_ATTR bool     walletPrivate = false;  // hide the balance. Long-press
                                               // DOWN in WAL toggles it; the
                                               // QR views stay usable, since
                                               // being paid is not private
RTC_DATA_ATTR uint32_t tipBlockTime = 0;     // unix time the tip was mined.
                                             // Block age is then arithmetic on
                                             // every minute wake: no fetch, no
                                             // radio, and it ticks live.
RTC_DATA_ATTR float    battFullV    = 4.20f;  // what this board reads at a
                                             // terminated charge. The library
                                             // scales the ADC with a constant
                                             // that does not match v3: a full
                                             // cell reports ~3.92 V here, so
                                             // the watch is told once what
                                             // full looks like and works from
                                             // that. Set in About: hold UP.
// Charging. The ADC sees the charger, never the cell, so the level while
// plugged in is estimated: where the cell was when the cable went in, plus
// minutes on the charger times a rate learned from real charges.
#define CHG_RATE_DEFAULT 3.00f  // %/min before any charge has been measured:
                                // ~33 min from empty. The wearer saw full at
                                // ~30 min, under a 25 min settle rule that
                                // could not show it sooner, so this is if
                                // anything slow. Replaced by learning
#define CHG_RATE_MAX     5.00f  // fastest rate learning will accept (20 min)
#define USB_SETTLE_MIN   3      // cable in this long before 'not charging'
                                // is believed to mean finished
#define CHG_EST_CAP      95     // an estimate never claims full: only the
                                // charge IC's "done" shows 100
// Hours left = the curve's share of a run still to come x how long a full
// run lasts. Measured on Live: ~42 h. Balanced and Saver start from cautious
// guesses until a run of their own is measured, then learn (blended 50/50,
// in flash), like the charge rate does.
#define RUN_H_LIVE      42.0f
#define RUN_H_BALANCED  48.0f
#define RUN_H_SAVER     54.0f
RTC_DATA_ATTR float    runH[3]      = { RUN_H_LIVE, RUN_H_BALANCED, RUN_H_SAVER };
RTC_DATA_ATTR uint32_t runStartT    = 0;    // RTC time the cable came out
RTC_DATA_ATTR int8_t   runStartPct  = -1;   // and the level then
RTC_DATA_ATTR uint8_t  runProf      = 0;    // and the battery profile
RTC_DATA_ATTR int16_t  restHrsShown = -2;   // hours printed on the rest face
RTC_DATA_ATTR int8_t   chgFromPct   = -1;   // level when the cable went in
RTC_DATA_ATTR uint32_t chgFromT     = 0;    // RTC time then (0: not charging)
RTC_DATA_ATTR bool     chgLearned   = false;// this charge already measured
RTC_DATA_ATTR int8_t   shownBattPct = -1;   // last percentage About printed.
                                           // held monotonic while discharging:
                                           // a voltage-derived figure jitters,
                                           // and a number that climbs on its
                                           // own reads as a fault
RTC_DATA_ATTR uint32_t lastNtpWake  = 0;    // wake minute of the last NTP
                                           // anchor. the pcf8563 on some
                                           // units runs minutes/day fast;
                                           // firmware cannot slow a crystal,
                                           // but it can re-anchor for free
                                           // while the radio is already up
RTC_DATA_ATTR float    goldMcapB     = 0;    // gold's mcap, billions,
                                             // in the selected currency
RTC_DATA_ATTR float    mempoolBlocks = 0;
// MEM: the newest mined blocks (0 = newest) and the next projected ones
RTC_DATA_ATTR long     recH[6]       = {0};
RTC_DATA_ATTR uint32_t recT[6]       = {0};
RTC_DATA_ATTR uint8_t  recFill[6]    = {0};    // % of the 4 MWU limit
RTC_DATA_ATTR char     recPool[6][4] = {{0}};
RTC_DATA_ATTR float    projFee[3]    = {0};    // median sat/vB, next first
RTC_DATA_ATTR uint8_t  projFill[3]   = {0};    // % of a block's 1 MvB
RTC_DATA_ATTR uint8_t  projN         = 0;   // unconfirmed vMB ~ blocks
// monotonic wake clock: +1 per RTC minute-wake. Immune to NTP resyncs,
// day/month arithmetic, and anything else wall-clock time does while
// we're not looking. All staleness math uses THIS, never the RTC.
RTC_DATA_ATTR uint32_t wakeMin       = 0;
RTC_DATA_ATTR uint32_t failCount     = 0;   // consecutive failed fetches
RTC_DATA_ATTR uint32_t nextTryWake   = 0;   // offline retry backoff
RTC_DATA_ATTR bool     forceFetch    = false;
// runtime settings, cached in RTC RAM, persisted in NVS flash
RTC_DATA_ATTR char     zpubBuf[120]  = {0};
RTC_DATA_ATTR bool     prefsLoaded   = false;
RTC_DATA_ATTR bool     haveDesc      = false;  // a multisig descriptor is set (NVS "desc")
RTC_DATA_ATTR int      qrDrawnIdx    = -1;     // the receive QR on the glass (-1: none)
RTC_DATA_ATTR uint8_t  qrPartials    = 0;      // minute redraws since its full one
// network keychain: up to 3 saved networks, most-recent first
RTC_DATA_ATTR char     wifiSsid[3][33] = {{0}};
RTC_DATA_ATTR char     wifiPass[3][65] = {{0}};
RTC_DATA_ATTR int      tzIndex       = 0;      // 0=LOC (compiled offset)
RTC_DATA_ATTR float    avgBlockSec   = 600;    // measured epoch pace

const char *TZ_NAMES[5] = {"LOC","EST","PST","UTC","GMT"};
const long  TZ_OFF  [5] = {0, -18000, -28800, 0, 0};  // fixed, no DST

// MEM sits beside HGT: the chain's two faces together. The saved
// hidden-faces mask is migrated by name from older orders (NVS "faceV").
enum { M_HGHT, M_MEMP, M_PRICE, M_SATS, M_MCAP, M_FEES, M_SUPL, M_HALV,
       M_WALT, M_JOE, NUM_MODES };
// 3-char labels while nine or fewer faces are on; all ten across 190 px
// leaves ~18 px a cell, where three letters clip, so two
const char *MODE_LABELS[NUM_MODES] =
  {"HGT","MEM","PRC","SAT","CAP","FEE","SUP","HLV","WAL","JOE"};
const char *MODE_LABELS2[NUM_MODES] =
  {"HG","ME","PR","SA","CA","FE","SU","HL","WA","JO"};
const char *MODE_NAMES [NUM_MODES] =
  {"BLOCK HEIGHT","MEMPOOL","PRICE","SATS/FIAT","MCAP","FEE SAT/VB",
   "SUPPLY","HALVING","WALLET BTC","TIME"};
const char *MODE_UNITS [NUM_MODES] =
  {"TIMECHAIN TIP","TIMECHAIN TIP","USD / BTC","SAT / USD","BILLION USD",
   "SAT/VB FAST","MILLION BTC","NEXT HALVING","BTC BALANCE",""};

// the currency dial: UP in PRC/SAT/CAP cycles it; SATS and MCAP
// inherit whatever PRICE speaks. Chart shape stays USD (Coinbase only
// has candle history for real pairs); the %% is computed per-currency.
// Twenty, all from the one Coinbase call that already priced the first six.
// The first six keep their places: curIdx is saved as an index.
#define NUM_CUR 20
const char *CUR_CODES[NUM_CUR] = {"USD","EUR","CAD","JPY","GBP","CHF",
  "AUD","CNY","HKD","SGD","INR","KRW","BRL","MXN","ARS","TRY","ZAR","NGN",
  "AED","LBP"};
const char *CUR_NAMES[NUM_CUR] = {"US DOLLAR","EURO","CANADIAN DOLLAR",
  "JAPANESE YEN","BRITISH POUND","SWISS FRANC","AUSTRALIAN DOLLAR",
  "CHINESE YUAN","HONG KONG DOLLAR","SINGAPORE DOLLAR","INDIAN RUPEE",
  "KOREAN WON","BRAZILIAN REAL","MEXICAN PESO","ARGENTINE PESO",
  "TURKISH LIRA","SA RAND","NIGERIAN NAIRA","UAE DIRHAM","LEBANESE POUND"};
RTC_DATA_ATTR uint8_t curIdx = 0;
RTC_DATA_ATTR uint32_t curOn = 0x3F;    // the ones UP cycles through: the
                                        // original six until the wearer picks
// BTC priced in all of them, fetched together once per cycle. Fiat rates
// crawl (~0.5%%/day); BTC is the only volatile leg — so the dial turns
// on cached arithmetic instead of waiting on the radio.
RTC_DATA_ATTR float   fxRate[NUM_CUR] = {0};

// PER-MODE box dial (declared after the enum that sizes it). It was
// one shared integer, and clicking FEE's tier silently re-aimed the
// halving almanac two epochs deep ("1.5625 halving in 2037").
RTC_DATA_ATTR uint8_t  cellSel[NUM_MODES] = {0};

// A tap during a wallet scan used to be missed: pressedButton() reads the pins
// at that instant, and it is only called once per address — between two HTTPS
// round trips. Unless the finger happened to be down at exactly that moment,
// nothing was seen. These latch it instead.
//
// They live at FILE scope on purpose. An IRAM_ATTR handler cannot reference a
// static class member: the linker has to place the member's address as a
// literal, and in IRAM that literal lands after the instruction using it —
// "dangerous relocation: l32r: literal placed after use".
volatile int g_isrPin = -1;   // -1 = none: on v3 the UP button IS GPIO 0
// Set by WiFiManager when the setup form is submitted. The wallet portal can
// succeed without a WiFi connection — someone pasting only a zpub — so
// "connected" is not the only way out.
bool g_portalSaved = false;

// The setup pages' look: the library's own pages, restyled in the BTC
// TECHNOLOGIES manner and given their words by a small script (it renames,
// replaces the wallet field with a box big enough for a descriptor, reads
// what is pasted live, and on save sends the phone to /exit, which ends the
// portal from inside the library's own loop: never stopped from outside).
// No web fonts: the phone on the watch's network has no internet.
// Source and phone-size screenshots: the session's scratchpad/portal.
const char SETUP_HEAD[] =
"<style>\n"
"body{background:#f4f2ec;color:#12110d;font-family:-apple-system,BlinkMacSystemFont,\"Helvetica Neue\",Arial,sans-serif;font-size:15px;margin:0;text-align:left}\n"
".wrap{display:block;max-width:440px;min-width:0;margin:0 auto;padding:22px 18px 36px;text-align:left}\n"
".wrap div,.wrap input{margin:0}\n"
"h1,h3{text-align:left}\n"
".bw-k{font:600 12px/1 ui-monospace,Menlo,Consolas,monospace;letter-spacing:3px;text-transform:uppercase;color:#57554e}\n"
".bw-top{padding:0}\n"
".bw-top h1{font-size:27px;line-height:1.15;margin:10px 0 6px;font-family:inherit}\n"
".bw-top p{margin:0;color:#3b3a35;line-height:1.45}\n"
"label{display:block;font-weight:700;margin:22px 0 6px}\n"
"label span{font-weight:400;color:#57554e}\n"
"input:not([type=checkbox]),textarea{display:block;width:100%;box-sizing:border-box;border:2px solid #12110d;border-radius:8px;padding:11px;font:14px/1.4 ui-monospace,Menlo,Consolas,monospace;background:#fff;color:#12110d}\n"
"textarea{min-height:140px;resize:vertical}\n"
"input:focus,textarea:focus{outline:0;border-color:#e5002b}\n"
"input[type=checkbox]{width:auto;margin:12px 6px 0 0;accent-color:#12110d}\n"
"label[for=showpass]{display:inline;font-weight:400;margin:0;color:#3b3a35}\n"
"button{display:block;width:100%;background:#e5002b;color:#fff;border:0;border-radius:10px;font:700 16px/1 -apple-system,BlinkMacSystemFont,\"Helvetica Neue\",Arial,sans-serif;padding:16px;margin-top:22px;opacity:1!important}\n"
"button[name=refresh]{background:transparent;color:#12110d;border:2px solid #12110d;margin-top:12px}\n"
"hr,.wrap br,.msg,form[action='/']{display:none}\n"
".wrap .bw-read br{display:inline}\n"
".bw-hint{font-size:13px;color:#57554e;margin-top:6px!important;line-height:1.4}\n"
".bw-read{border:2px dashed #12110d;border-radius:8px;padding:10px 12px!important;margin-top:10px!important;font-size:13px;line-height:1.55}\n"
".bw-read.bad{border-color:#e5002b;color:#b8001f}\n"
".bw-read code{font:12px ui-monospace,Menlo,Consolas,monospace}\n"
".bw-warn{background:#12110d;color:#f4f2ec;border-radius:8px;padding:12px!important;margin-top:20px!important;font-size:14px;line-height:1.45}\n"
".bw-warn b{color:#ff4d5e}\n"
".bw-cancel{display:block;text-align:center;margin-top:16px;color:#57554e;font-weight:400;text-decoration:none}\n"
".bw-foot{margin-top:34px!important;padding:0!important;font:600 11px/1 ui-monospace,Menlo,Consolas,monospace;letter-spacing:3px;color:#9a978d}\n"
".bw-nets{margin-top:18px!important;padding:0!important;border-top:1px solid #d5d1c5}\n"
".bw-net{display:flex;align-items:center;justify-content:space-between;padding:13px 2px!important;border-bottom:1px solid #d5d1c5}\n"
".bw-net a{color:#12110d;font-weight:600;font-size:16px;text-decoration:none;flex:1}\n"
".bw-net .q{float:none;padding:0!important;min-width:0}\n"
".bw-big{font-size:15px;line-height:1.5;margin-top:22px!important;padding:14px!important;border-left:4px solid #e5002b;background:#fff;border-radius:4px}\n"
"</style>\n"
"<script>\n"
"(function(){\n"
"var IN=\"0123456789()[],'/*abcdefgh@:$%{}IJKLMNOPQRSTUVWXYZ&+-.;<=>?!^_|~ijklmnopqrstuvwxyzABCDEFGH`#\\\"\\\\ \";\n"
"var CS=\"qpzry9x8gf2tvdw0s3jn54khce6mua7l\";\n"
"function sum(s){try{\n"
"var G=[0xf5dee51989n,0xa9fdca3312n,0x1bab10e32dn,0x3706b1677an,0x644d626ffdn];\n"
"function pm(c,v){var t=c>>35n;c=((c&0x7ffffffffn)<<5n)^BigInt(v);for(var i=0;i<5;i++)if((t>>BigInt(i))&1n)c^=G[i];return c}\n"
"var c=1n,k=0,n=0;\n"
"for(var i=0;i<s.length;i++){var p=IN.indexOf(s[i]);if(p<0)return null;c=pm(c,p&31);k=k*3+(p>>5);if(++n==3){c=pm(c,k);k=0;n=0}}\n"
"if(n)c=pm(c,k);for(var j=0;j<8;j++)c=pm(c,0);c^=1n;\n"
"var o=\"\";for(j=0;j<8;j++)o+=CS[Number((c>>BigInt(5*(7-j)))&31n)];return o}catch(e){return null}}\n"
"function el(t,c,h){var e=document.createElement(t);if(c)e.className=c;if(h!=null)e.innerHTML=h;return e}\n"
"function top(w,t,p){var d=el('div','bw-top','<div class=\"bw-k\">BWATCH &middot; SETUP</div><h1>'+t+'</h1>'+(p?'<p>'+p+'</p>':''));w.insertBefore(d,w.firstChild)}\n"
"function esc(s){return s.replace(/[&<>\"']/g,function(c){return'&#'+c.charCodeAt(0)+';'})}\n"
"function read(v){\n"
"v=v.trim();if(!v)return['','Leave it empty to keep the wallet the watch has now.'];\n"
"if(/(^|[(,\\]\\s])[xyzuvtYZUV]prv/.test(v)||/^[a-zA-Z]+(\\s+[a-zA-Z]+){11,}$/.test(v))\n"
"return['bad','<b>That looks like seed words or a private key.</b> The watch never needs them and will refuse this. Delete it, and if it was real, treat it as exposed.'];\n"
"if(v.indexOf('(')>=0){\n"
"if(/^tr\\(/.test(v))return['bad','Taproot descriptors are not supported yet.'];\n"
"var h=v.indexOf('#'),body=h<0?v:v.slice(0,h),given=h<0?'':v.slice(h+1),cs=sum(body);\n"
"var m=/(sorted)?multi\\((\\d+),/.exec(v),keys=v.match(/xpub[1-9A-HJ-NP-Za-km-z]{100,}/g)||[];\n"
"var fps=[],re=/\\[([0-9a-fA-F]{8})/g,f;while((f=re.exec(v)))fps.push(f[1].toLowerCase());\n"
"if(!m||!keys.length)return['bad','Not a multisig descriptor the watch can read. It wants <code>wsh(sortedmulti(&hellip;))</code>.'];\n"
"var o='Reads as: <b>multisig '+m[2]+' of '+keys.length+'</b>, '+(/^sh\\(wsh\\(/.test(v)?'wrapped':'native')+' SegWit<br>';\n"
"if(!cs)return['bad','It contains a character a descriptor cannot have. Check the copy.'];\n"
"if(given&&given!==cs)return['bad','Checksum <code>'+esc(given)+'</code> does not match. Something changed in the copy.'];\n"
"o+='Checksum <code>'+cs+'</code> '+(given?'&#10003;':'(computed: none was pasted)')+'<br>';\n"
"if(fps.length)o+='Keys <code>'+fps.join(' &middot; ')+'</code><br>';\n"
"return['',o+'You&rsquo;ll confirm these on the watch.'];\n"
"}\n"
"if(/^zpub/.test(v)&&v.length>100)return['','Reads as: <b>one key</b>, native SegWit. The watch shows address #0 for you to check.'];\n"
"if(/^[xy]pub/.test(v))return['bad','An '+v.slice(0,4)+': for a single-key native SegWit wallet the watch needs the <b>zpub</b>.'];\n"
"return['bad','Not something the watch can read: paste a zpub or a descriptor.'];\n"
"}\n"
"document.addEventListener('DOMContentLoaded',function(){\n"
"var b=document.body,w=document.querySelector('.wrap'),p=location.pathname,wal=/\\bbw-wal\\b/.test(b.className);\n"
"if(!w)return;\n"
"if(p=='/'||p==''){location.replace(wal?'/param':'/wifi');return}\n"
"if(p=='/param'){\n"
"top(w,'Your wallet','The watch shows your balance and receive addresses. It only ever needs public keys.');\n"
"var z=document.getElementById('zpub');\n"
"if(z){var t=el('textarea');t.id='zpub';t.name='zpub';t.rows=6;t.setAttribute('autocapitalize','off');t.setAttribute('autocorrect','off');t.spellcheck=false;\n"
"t.placeholder=\"zpub6r…  or  wsh(sortedmulti(2,[…]xpub…/<0;1>/*,…))#checksum\";\n"
"z.parentNode.replaceChild(t,z);\n"
"var lz=document.querySelector('label[for=zpub]');if(lz)lz.textContent='Wallet';\n"
"var hint=el('div','bw-hint','A zpub for a single-key wallet, or a descriptor for multisig (Sparrow: Settings &rsaquo; Export; Nunchuk: Wallet config).');\n"
"var rd=el('div','bw-read');rd.style.display='none';\n"
"t.parentNode.insertBefore(hint,t.nextSibling);hint.parentNode.insertBefore(rd,hint.nextSibling);\n"
"var up=function(){var r=read(t.value);if(!r[1]){rd.style.display='none';return}rd.className='bw-read'+(r[0]?' '+r[0]:'');rd.innerHTML=r[1];rd.style.display='block'};\n"
"t.addEventListener('input',up);up()}\n"
"var ll=document.querySelector('label[for=lnaddr]');if(ll)ll.innerHTML='Lightning address <span>(optional)</span>';\n"
"var ln=document.getElementById('lnaddr');if(ln){ln.setAttribute('autocapitalize','off');ln.setAttribute('autocorrect','off');ln.placeholder='you@example.com'}\n"
"var btn=document.querySelector('button[type=submit]');\n"
"if(btn){btn.textContent='Save to watch';\n"
"btn.parentNode.insertBefore(el('div','bw-warn','<b>Never</b> paste seed words or a private key (xprv, zprv). The watch only needs public keys and refuses anything else.'),btn)}\n"
"w.appendChild(el('a','bw-cancel','Cancel &mdash; keep the watch as it is')).href='/exit';\n"
"} else if(p=='/wifi'||p=='/0wifi'){\n"
"top(w,'Join your WiFi','Pick the network the watch should use. It remembers up to three and moves between them.');\n"
"var nets=el('div','bw-nets'),first=null;\n"
"Array.prototype.forEach.call(document.querySelectorAll('.wrap > div'),function(d){if(d.querySelector('a[onclick]')){d.className='bw-net';if(!first)first=d;nets.appendChild(d)}});\n"
"var form=document.querySelector('form[action=wifisave]');\n"
"if(nets.children.length&&form)form.parentNode.insertBefore(nets,form);\n"
"var ls=document.querySelector('label[for=s]');if(ls)ls.textContent='Network';\n"
"var b2=document.querySelector('form[action=wifisave] button[type=submit]');if(b2)b2.textContent='Connect';\n"
"var r=document.querySelector('button[name=refresh]');if(r)r.textContent='Scan again';\n"
"w.appendChild(el('a','bw-cancel','Cancel')).href='/exit';\n"
"} else if(p=='/paramsave'){\n"
"top(w,'Saved.','Back to the watch: it shows what it read, and asks you to check it.');\n"
"setTimeout(function(){location.href='/exit'},1200);\n"
"} else if(p=='/wifisave'){\n"
"top(w,'Connecting&hellip;','Watch the watch: it says whether it joined. If it did not, join BWATCH-SETUP again and re-enter the password.');\n"
"} else if(p=='/exit'){\n"
"top(w,'Done.','The watch has closed its setup network. You can close this page.');\n"
"Array.prototype.forEach.call(w.childNodes,function(n){if(n.nodeType==3)n.textContent=''});\n"
"} else top(w,'Setup','');\n"
"w.appendChild(el('div','bw-foot','BTC TECHNOLOGIES / DIV-001'));\n"
"});\n"
"})();\n"
"</script>\n";
void IRAM_ATTR btnISR0() { g_isrPin = BACK_BTN_PIN; }
void IRAM_ATTR btnISR1() { g_isrPin = MENU_BTN_PIN; }
void IRAM_ATTR btnISR2() { g_isrPin = UP_BTN_PIN;   }
void IRAM_ATTR btnISR3() { g_isrPin = DOWN_BTN_PIN; }


static const int DESC_TEXT_MAX = 2200;  // ~145 characters a key: 15 keys fit

// Turn the panel's next refresh into a full one. Whoever draws the face
// (the library's minute tick, a button, the dock loop) always sends it to the
// panel afterwards with a partial. A full refresh done inside the draw was
// therefore followed at once by a redundant partial of the same image, which
// on this panel can wash the fresh full refresh out ("it flashes, then
// fades"). Arming the driver's own "first refresh must be full" flag makes
// that one closing refresh the full one instead. (A pointer to a protected
// member, taken through a derived class, is the standard way to reach it.)
// The frame buffer is private to GxEPD2_BW; this is the standard, legal way
// to name a private member (explicit instantiation may take its address).
template<typename Tag, typename Tag::type M>
struct EpdRob { friend typename Tag::type epdGet(Tag) { return M; } };
struct EpdBufTag {
  typedef uint8_t (GxEPD2_BW<WatchyDisplay, WatchyDisplay::HEIGHT>::*type)
      [(WatchyDisplay::WIDTH / 8) * WatchyDisplay::HEIGHT];
  friend type epdGet(EpdBufTag);
};
template struct EpdRob<EpdBufTag, &GxEPD2_BW<WatchyDisplay, WatchyDisplay::HEIGHT>::_buffer>;

struct EpdFull : WatchyDisplay {
  // The driver only sends its power-off when it believes the panel is on,
  // and it believes that only after its own power-on. A full refresh right
  // after a power-off skips that (refresh(false) re-inits only from partial
  // mode), yet its 0xF4 sequence turns the high voltage on anyway: the
  // driver thinks "off", powerOff() returns without a word, the voltage stays
  // up and the fresh image fades within seconds. Tell it the truth first.
  static void markOn(WatchyDisplay &e) { e.*(&EpdFull::_power_is_on) = true; }
  // the driver's own timing print: every busy wait logs "_Update_Full : us"
  static void diag(WatchyDisplay &e) { e.*(&EpdFull::_diag_enabled) = true; }
  static bool isOn(WatchyDisplay &e) { return e.*(&EpdFull::_power_is_on); }
  // raw controller access, for the display lab only
  static void cmd(WatchyDisplay &e, uint8_t c) { (e.*(&EpdFull::_writeCommand))(c); }
  static void dat(WatchyDisplay &e, uint8_t d) {
    void (GxEPD2_EPD::*f)(uint8_t) = &EpdFull::_writeData; (e.*f)(d);
  }
};

class BitcoinChrono : public Watchy {
public:
  BitcoinChrono(const watchySettings &s) : Watchy(s) {}

  // remember a network in the keychain (most-recent-first). Called on
  // every successful join, so the list maintains itself.
  void saveNetwork(String ssid, String psk) {
    if (ssid.length() == 0) return;
    int found = -1;
    for (int i = 0; i < 3; i++)
      if (ssid.equals(wifiSsid[i])) { found = i; break; }
    if (found == 0) {
      strncpy(wifiPass[0], psk.c_str(), 64);
    } else {
      int from = (found > 0) ? found : 2;
      for (int i = from; i > 0; i--) {
        strncpy(wifiSsid[i], wifiSsid[i-1], 32);
        strncpy(wifiPass[i], wifiPass[i-1], 64);
      }
      strncpy(wifiSsid[0], ssid.c_str(), 32); wifiSsid[0][32] = 0;
      strncpy(wifiPass[0], psk.c_str(), 64); wifiPass[0][64] = 0;
      Serial.printf("[wifi] learned network: %s\n", ssid.c_str());
    }
    Preferences p; p.begin("btcchrono", false);
    for (int i = 0; i < 3; i++) {
      p.putString(("ws" + String(i)).c_str(), wifiSsid[i]);
      p.putString(("wp" + String(i)).c_str(), wifiPass[i]);
    }
    p.end();
  }

  // note the AP we just joined, for the next fast join
  void rememberAp() {
    const uint8_t *b = WiFi.BSSID();
    int ch = WiFi.channel();
    if (!b || ch <= 0 || ch > 14) { fastChan = 0; return; }
    memcpy(fastBssid, b, 6);
    fastChan = (uint8_t)ch;
    strncpy(fastSsid, WiFi.SSID().c_str(), 32); fastSsid[32] = 0;
  }

  // roaming connect: home, office, phone hotspot — whichever answers
  bool myConnectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return true;
    loadPrefs();
    WiFiMulti multi;
    bool any = false;
    for (int i = 0; i < 3; i++)
      if (wifiSsid[i][0]) { multi.addAP(wifiSsid[i], wifiPass[i]); any = true; }
    if (!any) {
      // empty keychain: fall back to the stock stored credentials,
      // and adopt them as network #1 on success. MUST call the
      // library's connect explicitly — a blanket rename once turned
      // this line into infinite recursion (85 stack frames of self-
      // call, one flickering boot loop, one very good backtrace)
      if (!Watchy::connectWiFi()) return false;
      saveNetwork(WiFi.SSID(), WiFi.psk());
      return true;
    }
    WiFi.mode(WIFI_STA);
    // Fast path: straight to the access point that answered last time, on its
    // channel, with no scan. The scan alone is a couple of seconds with the
    // radio on, every fetch, and nearly always finds the same AP. If the
    // shortcut misses, forget it and roam as before.
    if (fastChan > 0 && fastSsid[0]) {
      int k = -1;
      for (int i = 0; i < 3; i++)
        if (wifiSsid[i][0] && !strcmp(wifiSsid[i], fastSsid)) { k = i; break; }
      if (k >= 0) {
        unsigned long t1 = millis();
        WiFi.begin(wifiSsid[k], wifiPass[k], fastChan, fastBssid);
        while (WiFi.status() != WL_CONNECTED && millis() - t1 < 3000) delay(50);
        if (WiFi.status() == WL_CONNECTED) {
          Serial.printf("[wifi] fast join %s ch%u in %lu ms\n",
                        fastSsid, fastChan, millis() - t1);
          if (k != 0) saveNetwork(WiFi.SSID(), WiFi.psk());   // promote it
          return true;
        }
        Serial.println("[wifi] fast join missed - scanning");
        WiFi.disconnect();
        delay(100);
      }
      fastChan = 0;            // stale or no longer in the keychain
    }
    unsigned long t0 = millis();
    while (multi.run(4000) != WL_CONNECTED && millis() - t0 < 12000)
      delay(100);
    if (WiFi.status() == WL_CONNECTED) {
      rememberAp();
      saveNetwork(WiFi.SSID(), WiFi.psk());   // promote current to front
      return true;
    }
    WiFi.mode(WIFI_OFF); btStop();
    return false;
  }

  // price-step haptics, shared by periodic fetches and live docked feed
  void priceBuzzCheck() {
    // the $1000-step anchor tracks USD ALWAYS, whatever the face is
    // displaying — so switching to yen doesn't mute the wrist, and a
    // currency jump can never masquerade as a price move
    float p = fxRate[0] > 0 ? fxRate[0] : btcPrice;
    if (priceStep == 0 || p <= 0) return;
    if (priceAnchor <= 0) { priceAnchor = p; return; }  // arm only
    if (p - priceAnchor >= priceStep) {
      priceAnchor = p;
      vibMotor(75, 4); delay(160); vibMotor(75, 4);   // two quick: up
    } else if (priceAnchor - p >= priceStep) {
      priceAnchor = p;
      vibMotor(75, 14);                               // one long: down
    }
  }

  // parse /api/v1/blocks extras, HEIGHT-STAMPED: the extras indexer
  // lags the tip endpoint by seconds — accepting whatever's on top the
  // instant a block lands permanently displays block N-1's reward and
  // miner (field: "the info is one block behind the tip")
  long storeExtras(Stream &s) {
    JsonDocument filter;
    filter[0]["height"] = true;
    filter[0]["timestamp"] = true;      // when the tip was mined: lets the
                                        // watch age it locally, no polling
    filter[0]["tx_count"] = true;       // what the block actually carried
    filter[0]["weight"] = true;         // how full (MEM's cubes)
    filter[0]["extras"]["reward"] = true;
    filter[0]["extras"]["pool"]["name"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, s, DeserializationOption::Filter(filter)))
      return -1;
    long h = doc[0]["height"] | 0L;
    // MEM: the newest six, whatever the indexer's extras say (height and
    // time come with every block; the pool only with extras)
    for (int i = 0; i < 6; i++) {
      JsonVariant b = doc[i];
      long bh = b["height"] | 0L;
      if (bh <= 0) { recH[i] = 0; continue; }
      recH[i] = bh;
      recT[i] = b["timestamp"] | 0UL;
      long wt = b["weight"] | 0L;
      recFill[i] = wt > 0 ? (uint8_t)constrain(wt / 40000L, 0L, 100L) : 0;
      poolShort(b["extras"]["pool"]["name"] | "", recPool[i]);
    }
    if (h > rwdHeight) {
      rwdHeight = h;
      long long r = doc[0]["extras"]["reward"] | 0LL;
      if (r > 0) lastRewardBtc = r / 1e8;
      long tc = doc[0]["tx_count"] | 0L;
      if (tc > 0) lastTxCount = tc;
      uint32_t ts = doc[0]["timestamp"] | 0UL;
      if (ts > 0) tipBlockTime = ts;
      storePool(doc[0]["extras"]["pool"]["name"] | "");
    }
    return h;
  }

  // three letters for a pool, for MEM's cubes: the big ones by name, the
  // rest by their first three letters
  static void poolShort(const char *raw, char out[4]) {
    static const char *const MAP[][2] = {
      {"FOUNDRY","FDR"}, {"ANTPOOL","ANT"}, {"VIABTC","VIA"}, {"MARA","MAR"},
      {"SPIDER","SPD"}, {"F2POOL","F2P"}, {"BINANCE","BIN"}, {"BRAIINS","BRA"},
      {"SECPOOL","SEC"}, {"LUXOR","LUX"}, {"OCEAN","OCN"}, {"SBI","SBI"},
      {"ULTIMUS","ULT"}, {"POOLIN","PLN"}, {"BTC.COM","BTC"}, {"WHITE","WHT"}};
    char up[16]; int j = 0;
    for (int i = 0; raw && raw[i] && j < 15; i++)
      if (raw[i] != ' ') up[j++] = toupper(raw[i]);
    up[j] = 0;
    if (!j) { strcpy(out, "?"); return; }
    for (auto &m : MAP)
      if (!strncmp(up, m[0], strlen(m[0]))) { strcpy(out, m[1]); return; }
    strncpy(out, up, 3); out[3] = 0;
  }

  void storePool(const char *raw) {
    if (!raw || !raw[0]) return;
    int j = 0;
    for (int i = 0; raw[i] && j < 9; i++) {   // 9 chars fit beside
      char ch = raw[i];                        // the MINER label
      if (ch == ' ') continue;                 // "Foundry USA"->FOUNDRYUS
      poolName[j++] = toupper(ch);
    }
    poolName[j] = 0;
  }

  // switching currency is pure arithmetic on cached rates: no radio,
  // no wait — the same ~0.4s as every other dial on the watch
  // the face's currency, kept in flash: it used to live only in RTC memory,
  // so a reflash or a flat cell quietly put a EUR wearer back on USD
  // UP on the price faces: the next currency the wearer kept in the cycle
  uint8_t nextCur(uint8_t from) {
    for (int k = 1; k <= NUM_CUR; k++) {
      int c = (from + k) % NUM_CUR;
      if ((curOn >> c) & 1) return c;
    }
    return from;
  }
  void saveCurrency() {
    Preferences p; p.begin("btcchrono", false);
    p.putUChar("cur", curIdx);
    p.end();
  }
  // Sats per unit of fiat, or, when a unit is worth less than a sat (a won,
  // a dong, a Lebanese pound), units per sat: 0.01 sats reads as nothing,
  // 74 LBP a sat reads as something. Small values keep their decimals.
  bool satsFlip(double price) { return price > 1e8; }
  void fmtSats(char *out, size_t n, double price) {
    if (price <= 0) { snprintf(out, n, "----"); return; }
    double v = satsFlip(price) ? price / 1e8 : 1e8 / price;
    snprintf(out, n, v < 10 ? "%.2f" : (v < 100 ? "%.1f" : "%.0f"), v);
  }

  void applyCurrency() {
    if (fxRate[curIdx] > 0) btcPrice = fxRate[curIdx];
    if (blockHeight > 0)
      btcMcapB = btcPrice * supplyBTC(blockHeight) / 1e9;
  }

  // ONE call prices BTC in every currency we speak, plus gold in USD
  // (the %% GOLD ratio is currency-invariant, so USD on both legs)
  // Read BTC in every currency we speak out of Coinbase's exchange-rates
  // answer. The filter names only ours: the full answer is ~170 rates.
  bool readRates(HTTPClient &http) {
    JsonDocument filter;
    for (int i = 0; i < NUM_CUR; i++) filter["data"]["rates"][CUR_CODES[i]] = true;
    JsonDocument doc;
    // getString, NOT getStream: Coinbase answers chunked, and the
    // raw stream includes chunk-size headers the parser chokes on
    // (field: "price is 0 in all currencies")
    if (deserializeJson(doc, http.getString(),
                        DeserializationOption::Filter(filter))) return false;
    for (int i = 0; i < NUM_CUR; i++) {
      float r = atof(doc["data"]["rates"][CUR_CODES[i]] | "0");
      if (r > 0) fxRate[i] = r;
    }
    return fxRate[0] > 0;
  }

  bool fetchRates(bool ownRadio, bool wantGold = true) {
    if (ownRadio && !myConnectWiFi()) return false;
    WiFiClientSecure client; client.setInsecure();
    bool ok = false;
    { HTTPClient http; http.setConnectTimeout(5000);
      if (http.begin(client, "https://api.coinbase.com/v2/"
                             "exchange-rates?currency=BTC")
          && http.GET() == 200) {
        ok = readRates(http);
      }
      http.end(); }
    if (wantGold) { // gold, USD: XAU spot x ~6.95B above-ground ounces (216k
      // tonnes, drifts ~1.5%%/yr from mining — revisit the constant eventually)
      HTTPClient http; http.setConnectTimeout(4000);
      if (http.begin(client, "https://api.coinbase.com/v2/prices/"
                             "XAU-USD/spot") && http.GET() == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString())) {
          float oz = atof(doc["data"]["amount"] | "0");
          if (oz > 0) { goldMcapB = oz * 6.95f; goldWake = tierStamp(); }
        }
      }
      http.end(); }
    applyCurrency();
    if (ownRadio && !inDocked) { WiFi.mode(WIFI_OFF); btStop(); }
    Serial.printf("[fx] rates %s (usd %.0f, gold %.0fB)\n",
                  ok ? "OK" : "FAIL", fxRate[0], goldMcapB);
    return ok;
  }

  // Coinbase candles are arrays-of-arrays: [[t,low,high,open,close,v]].
  // ArduinoJson filters address object KEYS, not array positions — my
  // filter[0][4] matched nothing, the document came back empty, and
  // the chart went flat (field: "the 24H graph displays nothing").
  // Scan the stream by hand: no document, bounded memory, 24 closes.
  bool fetchCandles(WiFiClientSecure &client) {
    HTTPClient http; http.setConnectTimeout(6000);
    if (!http.begin(client, "https://api.exchange.coinbase.com/products/"
                            "BTC-USD/candles?granularity=3600") ||
        http.GET() != 200) { http.end(); return false; }
    String body = http.getString();   // de-chunked by HTTPClient —
    float closes[24]; int nc = 0;     // the raw stream interleaves
    int depth = 0, field = 0, nl = 0; // chunk headers with the JSON
    char num[16];
    for (unsigned int bi = 0; bi < body.length() && nc < 24; bi++) {
      char c = body[bi];
      if (c == '[') { depth++; if (depth == 2) { field = 0; nl = 0; } }
      else if (c == ']') {
        if (depth == 2 && field == 4 && nl > 0) {
          num[nl] = 0; closes[nc++] = atof(num); nl = 0;
        }
        if (--depth <= 0) break;
      }
      else if (c == ',' && depth == 2) {
        if (field == 4 && nl > 0) { num[nl] = 0; closes[nc++] = atof(num); nl = 0; }
        field++;
      }
      else if (depth == 2 && field == 4 && nl < 15 &&
               ((c >= '0' && c <= '9') || c == '.' || c == '-'))
        num[nl++] = c;
    }
    http.end();
    if (nc < 2) { Serial.println("[chart] candles FAIL"); return false; }
    // rows arrive newest-first: resample to the sparkline, oldest->newest
    for (int i = 0; i < SPARK_POINTS; i++) {
      int idx = (int)((long)i * (nc - 1) / (SPARK_POINTS - 1));
      spark[i] = closes[nc - 1 - idx];
    }
    sparkHead = 0;
    float first = closes[nc - 1], last = closes[0];
    if (first > 0) btcChange24h = ((last - first) / first) * 100.0f;
    if (first > 0) dayAgoUsd = first;   // fetchAll moves the %% with the price
    Serial.printf("[chart] %d candles, 24h %.2f%%\n", nc, btcChange24h);
    return true;   // the %% is USD-derived and reused for every
  }                // currency: fiat drift is ~0.3pp, invisible at 1dp

  // ---------------- data ----------------
  // the fetch tiers' clock: wakeMin, but never 0, which means "never"
  uint32_t tierStamp() { return wakeMin ? wakeMin : 1; }
  bool tierDue(uint32_t last, uint32_t everyMin, bool all) {
    return all || last == 0 || wakeMin < last || wakeMin - last >= everyMin;
  }

  // GET a small body over the kept-alive mempool session, read whole.
  // The core reuses an open socket without checking that the last response
  // was fully read, so anything that might leave bytes behind (an error
  // status, a failed read) closes it: the next request then starts clean
  // instead of parsing this one's leftovers. A stale kept-alive socket shows
  // up as a negative code; that gets one fresh retry.
  bool mpGet(HTTPClient &http, WiFiClientSecure &client, const char *url,
             String &out) {
    out = "";
    for (int attempt = 0; attempt < 2; attempt++) {
      if (!http.begin(client, url)) return false;
      int code = http.GET();
      if (code == HTTP_CODE_OK) {
        out = http.getString();
        int expect = http.getSize();             // -1 when chunked
        bool whole = out.length() > 0 &&
                     (expect < 0 || (int)out.length() == expect);
        if (whole) http.end();
        else { http.setReuse(false); http.end(); http.setReuse(true); }
        return whole;
      }
      http.setReuse(false); http.end(); http.setReuse(true);
      if (code > 0) return false;                // it answered: do not nag
    }
    return false;
  }

  // after a request parsed straight off the socket: how much the parser
  // left unread is unknown, so the socket was opened with Connection: close
  // and is closed here; the next mpGet opens a fresh one
  void mpStreamEnd(HTTPClient &http) {
    http.end();
    http.setReuse(true);
  }

  // ---- RETARGET: noticing the end of an epoch, and reading what it did ----
  // The tip is the clock: when its epoch (height / 2016) moves on, the
  // difficulty adjustment has happened. mempool's difficulty answer then
  // reports it (previousRetarget) and when it happened (previousTime); the
  // epoch's fees and transactions come from reward-stats, which counts back
  // from the tip, so when the watch is late (offline, asleep) the blocks
  // since the boundary are subtracted out. If the answer still describes
  // the old epoch (an indexer a few seconds behind), the next fetch retries.
  struct Retarget {
    long epoch;            // the epoch that just ended
    float change;          // difficulty, % (the adjustment that happened)
    float days;            // how long its 2,016 blocks took; 0 = unknown
    float mined, fees;     // BTC: subsidy, and fees on top; fees < 0 = unknown
    long txs;              // transactions in it; 0 = unknown
  };

  static double jnum(JsonVariantConst v) {       // numbers sent as strings
    if (v.is<const char *>()) return atof(v.as<const char *>());
    return v.as<double>();
  }

  bool rewardStats(HTTPClient &mp, WiFiClientSecure &client, long n,
                   double &fee, double &tx) {
    if (n <= 0) { fee = 0; tx = 0; return true; }
    char url[80];
    snprintf(url, 80, "https://mempool.space/api/v1/mining/reward-stats/%ld", n);
    String body;
    if (!mpGet(mp, client, url, body)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    fee = jnum(doc["totalFee"]) / 1e8;
    tx  = jnum(doc["totalTx"]);
    return true;
  }

  void retargetCheck(HTTPClient &mp, WiFiClientSecure &client) {
    if (blockHeight <= 0) return;
    long ep = blockHeight / 2016;
    if (retEpoch == 0) { retEpoch = ep; return; }  // first sight: no card
    if (ep <= retEpoch) return;
    String body;
    if (!mpGet(mp, client, DIFF_URL, body)) return;   // next fetch, then
    JsonDocument doc;
    if (deserializeJson(doc, body)) return;
    long nrh = doc["nextRetargetHeight"] | 0L;
    long boundary = ep * 2016L;
    if (nrh != boundary + 2016) {
      Serial.printf("[retarget] %ld: answer not caught up (%ld)\n", boundary, nrh);
      return;
    }
    uint32_t endT = (uint32_t)(doc["previousTime"] | 0.0);
    evRetChange = doc["previousRetarget"] | 0.0f;
    evRetEpoch  = ep - 1;                       // the one that just ended
    // how long it took: only when the watch saw it start, and only one
    // epoch back (two weeks offline: the start is the one before)
    evRetDays = (ep == retEpoch + 1 && retStartT > 0 && endT > retStartT)
                  ? (endT - retStartT) / 86400.0f : 0;
    evRetMined = supplyBTC(boundary) - supplyBTC(boundary - 2016);
    // fees and transactions: the last (late + 2016) blocks, less the late ones
    long late = blockHeight - boundary + 1;     // blocks of the new epoch
    double fA, tA, fB, tB;
    if (late < 300 && rewardStats(mp, client, late + 2016, fA, tA) &&
        rewardStats(mp, client, late, fB, tB)) {
      evRetFees = fA - fB; evRetTxs = (long)(tA - tB);
    } else { evRetFees = -1; evRetTxs = 0; }
    retEpoch = ep;
    retStartT = endT;                           // the new epoch began then
    retPending = true;
    Serial.printf("[retarget] epoch %ld: %+.2f%%, %.1f days, %.0f + %.2f BTC, %ld tx\n",
                  evRetEpoch, evRetChange, evRetDays, evRetMined, evRetFees, evRetTxs);
  }

  // The halving: the same, every 210,000 blocks. The era's span comes from
  // its first block and the new era's first; fees from reward-stats over the
  // era (it takes a count that large); the rest is the supply arithmetic the
  // watch already does.
  struct Halving {
    int n;                 // which halving (1,050,000 is the fifth)
    float subsidy;         // the new block subsidy, BTC
    float years;           // the era's span; 0 = unknown
    double mined, fees;    // BTC over the era; fees < 0 = unknown
    double supply;         // BTC in existence after it
  };

  uint32_t blockTime(HTTPClient &mp, WiFiClientSecure &client, long h) {
    char url[72]; String body;
    snprintf(url, 72, "https://mempool.space/api/block-height/%ld", h);
    if (!mpGet(mp, client, url, body) || body.length() < 64) return 0;
    String u = String("https://mempool.space/api/block/") + body.substring(0, 64);
    if (!mpGet(mp, client, u.c_str(), body)) return 0;
    JsonDocument filter; filter["timestamp"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return 0;
    return doc["timestamp"] | 0UL;
  }

  void halvingCheck(HTTPClient &mp, WiFiClientSecure &client) {
    if (blockHeight <= 0) return;
    long era = blockHeight / 210000L;
    if (halvEra == 0) { halvEra = era; return; }   // first sight: no card
    if (era <= halvEra) return;
    long boundary = era * 210000L;
    uint32_t t0 = blockTime(mp, client, boundary - 210000L);
    uint32_t t1 = blockTime(mp, client, boundary);
    evHalvYears = (t0 > 0 && t1 > t0) ? (t1 - t0) / (365.25f * 86400.0f) : 0;
    long late = blockHeight - boundary + 1;
    double fA, tA, fB, tB;
    evHalvFees = (late < 300 && rewardStats(mp, client, late + 210000L, fA, tA) &&
                  rewardStats(mp, client, late, fB, tB)) ? (float)(fA - fB) : -1;
    evHalvN = (int)era;
    halvEra = era;
    halvPending = true;
    Serial.printf("[halving] %d: %.2f years, fees %.1f\n", evHalvN, evHalvYears, evHalvFees);
  }

  Halving heldHalving() {
    long b = (long)evHalvN * 210000L;
    return Halving{evHalvN, (float)(50.0 / pow(2, evHalvN)), evHalvYears,
                   supplyBTC(b) - supplyBTC(b - 210000L), evHalvFees, supplyBTC(b)};
  }

  void halvingScreen() {
    Halving v = heldHalving();
    static const int HOLD[] = {420, 260, 380, 420};
    for (int st = 0; st < 4; st++) {
      drawHalving(v, st);
      display.display(true);
      delay(HOLD[st]);
    }
    drawHalving(v, 4);
    displayFull();
    paintedDark = true;
    for (int i = 0; i < 4; i++) { buzz(35, 2); delay(140); }
  }

  Retarget heldRetarget() {
    return Retarget{evRetEpoch, evRetChange, evRetDays, evRetMined, evRetFees, evRetTxs};
  }

  // the takeover: the face's dial grows, closes, says what happened
  void retargetScreen() {
    Retarget r = heldRetarget();
    static const int HOLD[] = {420, 260, 380, 420};
    for (int st = 0; st < 4; st++) {
      drawRetarget(r, st);
      display.display(true);
      delay(HOLD[st]);
    }
    drawRetarget(r, 4);
    displayFull();
    paintedDark = true;            // always black: the face after flips full
    buzz(35, 2); delay(140); buzz(35, 2);
  }

  bool fetchAll() {
    if (!myConnectWiFi()) {
      syncStep(SY_WIFI, "NOT FOUND", 2);
      return false;
    }
    syncStep(SY_WIFI, WiFi.SSID().substring(0, 18).c_str(), 1);
    syncStep(SY_TIME, "NTP", 0);
    // the radio is already on: re-anchor the clock every ~6 h. costs one
    // udp round trip and removes RTC drift permanently
    if (lastNtpWake == 0 || wakeMin - lastNtpWake > 360) {
      if (syncNTP()) {
        lastNtpWake = wakeMin;
        RTC.read(currentTime);
        Serial.printf("[ntp] re-anchored: %02d:%02d utc\n",
                      currentTime.Hour, currentTime.Minute);
      }
    }
    if (syncOn) {                     // the wall clock, synced or kept
      RTC.read(currentTime);
      int m = localMinuteOfDay();
      char t[24]; snprintf(t, 24, "%02d:%02d %s", m / 60, m % 60,
                           lastNtpWake == wakeMin ? "NTP" : "KEPT");
      syncStep(SY_TIME, t, 1);
      syncStep(SY_PRICE, "COINBASE", 0);
    }
    bool ok = false;
    long heightBefore = blockHeight;
    // everything when asked for (long-press UP) or when there is nothing yet
    bool all = fetchPending || !haveData;
    WiFiClientSecure client; client.setInsecure();

    if (tierDue(candleWake, CANDLES_EVERY_MIN, all) && fetchCandles(client))
      candleWake = tierStamp();                    // chart + the day-ago close
    if (fetchRates(false, tierDue(goldWake, GOLD_EVERY_MIN, all)))
      haveData = ok = true;                        // all six (+ gold when due)
    if (syncOn) {
      char t[24];
      if (ok) { char g[20]; grouped(g, 20, (long long)(btcPrice + 0.5));
                snprintf(t, 24, "%s %s", CUR_CODES[curIdx], g); }
      syncStep(SY_PRICE, ok ? t : "NO ANSWER", ok ? 1 : 2);
      syncStep(SY_TIP, "MEMPOOL", 0);
    }
    // the 24h change follows the live price between chart fetches: today's
    // price against the cached close from a day ago
    if (dayAgoUsd > 0 && fxRate[0] > 0)
      btcChange24h = ((fxRate[0] - dayAgoUsd) / dayAgoUsd) * 100.0f;

    if (tierDue(fngWake, FNG_EVERY_MIN, all)) {
      HTTPClient http; http.setConnectTimeout(4000);
      if (http.begin(client, FNG_URL) && http.GET() == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString())) {
          fearGreed = atoi(doc["data"][0]["value"] | "50");
          fngWake = tierStamp();
        }
      }
      http.end();
    }

    // ---- mempool.space: one TLS session for everything it serves ----
    // Each scoped HTTPClient used to close the socket as it went out of
    // scope, so every request paid a fresh handshake. One client, kept
    // alive, pays it once. Small bodies only (mpGet reads them whole);
    // the two parsed straight off the socket close it after (mpStreamEnd).
    { HTTPClient mp; mp.setConnectTimeout(4000); mp.setReuse(true);
      String body;
      if (mpGet(mp, client, FEES_URL, body)) {
        JsonDocument doc;
        if (!deserializeJson(doc, body)) {
          fastFee = doc["fastestFee"]  | fastFee;   // float-safe
          medFee  = doc["halfHourFee"] | medFee;
          lowFee  = doc["hourFee"]     | lowFee;
        }
      }
      const char *tips[2] = {TIP_URL, TIP_URL2};
      for (int i = 0; i < 2; i++) {               // fallback only on failure
        if (!mpGet(mp, client, tips[i], body)) continue;
        long h = body.toInt();
        if (h >= 100000) {                        // sanity: a real height
          if (h > blockHeight) blockHeight = h;   // only moves forward
          lastHeightWake = wakeMin;               // height-specific stamp
          break;
        }
      }
      retargetCheck(mp, client);                  // an epoch ended?
      halvingCheck(mp, client);                   // an era?
      if (syncOn) {                               // the height itself
        char t[16] = "NO ANSWER";
        if (lastHeightWake == wakeMin && blockHeight > 0)
          grouped(t, 16, blockHeight);
        syncStep(SY_TIP, t, lastHeightWake == wakeMin ? 1 : 2);
      }
      // mempool backlog, denominated in blocks (unconfirmed vMB)
      if (mpGet(mp, client, "https://mempool.space/api/mempool", body)) {
        JsonDocument doc;
        if (!deserializeJson(doc, body)) {
          long vs = doc["vsize"] | 0L;
          if (vs > 0) mempoolBlocks = vs / 1e6;
        }
      }
      // MEM's waiting side: the next three projected blocks. Only fetched
      // when the face is in the cycle; nobody else uses them
      if (faceShown(M_MEMP) &&
          mpGet(mp, client, "https://mempool.space/api/v1/fees/mempool-blocks", body)) {
        JsonDocument filter;
        filter[0]["medianFee"] = true;
        filter[0]["blockVSize"] = true;
        JsonDocument doc;
        if (!deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
          projN = 0;
          for (int i = 0; i < 3; i++) {
            float vs = doc[i]["blockVSize"] | 0.0f;
            if (vs <= 0) break;
            projFee[i]  = doc[i]["medianFee"] | 0.0f;
            projFill[i] = (uint8_t)constrain((long)(vs / 10000.0f), 0L, 100L);
            projN++;
          }
        }
      }
      if (tierDue(slowWake, SLOW_EVERY_MIN, all)) {
        bool gotDiff = false, gotHash = false;
        if (mpGet(mp, client, DIFF_URL, body)) {
          JsonDocument doc;
          if (!deserializeJson(doc, body)) {
            diffChangeEst = doc["difficultyChange"] | 0.0f;
            // when this epoch began: kept for the recap at its end. Only
            // for the epoch the watch thinks it is in, so a read that lands
            // after a crossing the tip has not shown yet cannot overwrite it
            long nrh = doc["nextRetargetHeight"] | 0L;
            uint32_t pt = (uint32_t)((doc["previousTime"] | 0.0) );
            if (nrh > 0 && pt > 0 && retEpoch > 0 && nrh / 2016 - 1 == retEpoch)
              retStartT = pt;
            float ta = doc["timeAvg"] | 0.0f;      // ms, current epoch
            if (ta > 0) avgBlockSec = constrain(ta / 1000.0f, 300.0f, 1200.0f);
            if (!(avgBlockSec > 300.0f && avgBlockSec < 1200.0f))
              avgBlockSec = 600;      // NaN or nonsense: back to ten minutes
            gotDiff = true;
          }
        }
        // the work behind it all. Filtered to one field: the response carries
        // a long series of historical points that would eat the heap for nothing.
        mp.setReuse(false);                       // streamed: close after
        if (mp.begin(client, HASH_URL) && mp.GET() == 200) {
          JsonDocument filter; filter["currentHashrate"] = true;
          JsonDocument doc;
          if (!deserializeJson(doc, mp.getStream(),
                               DeserializationOption::Filter(filter))) {
            double hs = doc["currentHashrate"] | 0.0;    // hashes per second
            if (hs > 0) { netHashEH = (float)(hs / 1e18); gotHash = true; }
          }
        }
        mpStreamEnd(mp);
        if (gotDiff && gotHash) slowWake = tierStamp();   // else retry next cycle
      }
      // last block's total reward (subsidy + fees) and miner: only when there
      // is a block we have not described yet. storeExtras is height-stamped,
      // so a lagging indexer just means asking again next cycle.
      if (all || blockHeight > rwdHeight) {
        mp.setReuse(false);                       // streamed: close after
        if (mp.begin(client, "https://mempool.space/api/v1/blocks") &&
            mp.GET() == 200) {
          storeExtras(mp.getStream());
        }
        mpStreamEnd(mp);
      }
    }
    lateWatchStep();              // a slow sender after the vigil, if any
    trackStep(client);            // a payment on its way in, if any
    Serial.printf("[fetch] %s tiers: chart %s gold %s slow %s fng %s\n",
                  all ? "ALL" : "due",
                  candleWake == tierStamp() ? "Y" : "-",
                  goldWake   == tierStamp() ? "Y" : "-",
                  slowWake   == tierStamp() ? "Y" : "-",
                  fngWake    == tierStamp() ? "Y" : "-");
    if (!inDocked) { WiFi.mode(WIFI_OFF); btStop(); }  // docked: stay on

    // ---- haptics (after radio off; buzzing draws its own current) ----
    if (buzzBlock && heightBefore > 0 && blockHeight > heightBefore) {
      lastBuzzHeight = blockHeight;
      vibMotor(75, 4);                       // one short tick: new block(s)
    }
    priceBuzzCheck();
    return ok;
  }

  // Has the watch moved since the last wake? One accelerometer sample per
  // minute, compared against the last. No step counting, no fitness — just
  // "is this on an arm or on a table".
  bool movedSinceLastWake() {
    Accel a;
    accDelta = -1; accPose = POSE_TILTED;      // unknown until proven
    if (!sensor.getAccel(a)) return true;      // no reading: assume worn
    long d = labs((long)a.x - lastAx) + labs((long)a.y - lastAy)
           + labs((long)a.z - lastAz);
    lastAx = a.x; lastAy = a.y; lastAz = a.z;
    // Pose: lying level (on its back or on its face) or anything else. Only a
    // level watch can be on a table; a wrist is almost never within ten
    // degrees of flat for long.
    bool level = labs((long)a.x) < FLAT_XY && labs((long)a.y) < FLAT_XY &&
                 labs((long)a.z) > FLAT_Z;
    if (level) accPose = ((long)a.z * FACE_UP_Z_SIGN > 0) ? POSE_FACE_UP
                                                          : POSE_FACE_DOWN;
    Serial.printf("[accel] d=%ld x=%d y=%d z=%d pose=%s rest=%d run=%u\n",
                  d, a.x, a.y, a.z,
                  accPose == POSE_FACE_UP ? "UP" :
                  accPose == POSE_FACE_DOWN ? "DOWN" : "TILT",
                  restState, stillRun);
    if (!accelSeeded) { accelSeeded = true; lastMoveWake = wakeMin; return true; }
    accDelta = d;
    return d > MOVE_THRESHOLD;
  }

  // ---------------- rest and night ----------------
  enum { REST_NONE = 0, REST_DAY = 1, REST_NIGHT = 2 };
  enum { POSE_TILTED = 0, POSE_FACE_UP = 1, POSE_FACE_DOWN = 2 };
  long accDelta = -1;          // this wake's movement, -1 if not measured
  int  accPose  = POSE_TILTED; // this wake's pose
  bool restExitFull = false;   // the face after a rest gets a full refresh

  // Local wall-clock minutes since midnight, the same arithmetic the status
  // row uses so the night window and the printed clock cannot disagree.
  int localMinuteOfDay() {
    time_t lt = makeTime(currentTime);
    if (tzIndex > 0) {
      time_t utc = lt - settings.gmtOffset;
      lt = utc + tzOffsetSec(tzIndex, utc);
    }
    tmElements_t dt; breakTime(lt, dt);
    return dt.Hour * 60 + dt.Minute;
  }

  bool nightOn() { return nightFrom != nightTo; }   // From = To: no night

  bool inNightWindow() {
    if (!nightOn()) return false;
    int h = localMinuteOfDay() / 60;
    return (nightFrom < nightTo) ? (h >= nightFrom && h < nightTo)
                                 : (h >= nightFrom || h < nightTo);  // 22-6
  }

  // minutes from now until the clock reads hh:00 local
  int minutesUntilHour(int hh) {
    int d = hh * 60 - localMinuteOfDay();
    return d <= 0 ? d + 1440 : d;
  }

  // A long sleep stops the minute wakes, but wakeMin is the watch's clock for
  // every cadence and every staleness: fetches, OLD tags, the hourly battery
  // sample. So on the way back in, add the minutes that actually passed.
  long catchUpWakeClock() {
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {   // a real boot ends any rest
      restState = REST_NONE; sleptSince = 0;
      return 0;
    }
    if (!sleptSince) return 0;
    tmElements_t t; RTC.read(t);
    long s = (long)makeTime(t) - (long)sleptSince;
    sleptSince = 0;
    if (s < 0 || s > 26L * 3600) return 0;   // the clock was set meanwhile:
                                             // a jump is worse than a gap
    return (s + 30) / 60;
  }

  // Everything that must keep the watch fully awake. Resting is a saving, so
  // anything doing something on the wearer's behalf wins.
  // What stops the watch resting, if anything: 0 nothing. Kept apart from
  // restAllowed() so the watch can say which one (About: "Was awake CARD").
  enum { RB_OK = 0, RB_MENU, RB_DOCK, RB_CHARGER, RB_TRAVEL, RB_FETCH, RB_DRAW,
         RB_WALLET, RB_PAYMENT, RB_QR, RB_NODATA, RB_CARD, RB_PRESSED, RB_MOVING };
  int restBlock() {
    if (guiState != WATCHFACE_STATE) return RB_MENU;
    if (inDocked)                    return RB_DOCK;
    if (chargeState() != 0)          return RB_CHARGER;
    if (travelActive)                return RB_TRAVEL;
    if (fetchPending)                return RB_FETCH;
    if (prePaint)                    return RB_DRAW;
    if (inWalletScan)                return RB_WALLET;
    if (vigilPending)                return RB_PAYMENT;
    if (dispMode == M_WALT && walletView > 0) return RB_QR;
    if (!haveData || blockHeight <= 0)        return RB_NODATA;
    if (evQueue && wakeMin - evWake < EVENT_HOLD_MIN) return RB_CARD;
    return RB_OK;
  }
  const char *blockName(int b) {
    static const char *N[] = {"", "MENU", "DOCKED", "CHARGER", "TRAVEL", "FETCH",
                              "DRAWING", "WALLET", "PAYMENT", "QR", "NO DATA",
                              "CARD", "PRESSED", "MOVING"};
    return (b >= 0 && b <= RB_MOVING) ? N[b] : "?";
  }
  enum { LM_NONE = 0, LM_AWAKE, LM_REST, LM_NIGHT };
  // note the outcome of a minute wake; say it on serial when it changes
  void noteMode(uint8_t m, uint8_t why) {
    if (m != lastMode || why != lastWhy)
      Serial.printf("[rest] now %s%s%s\n",
                    m == LM_NIGHT ? "NIGHT" : m == LM_REST ? "REST" : "AWAKE",
                    why ? ": " : "", why ? blockName(why) : "");
    lastMode = m; lastWhy = why;
  }
  bool restAllowed() { return restBlock() == RB_OK; }

  // Deep sleep for whole minutes, waking on the minute like the library does.
  // Mirrors Watchy::deepSleep() for the v3 board with a longer timer; it does
  // not return. The panel keeps its image with the power off.
  bool longSleep(uint32_t minutes) {
  #ifdef ARDUINO_ESP32S3_DEV
    if (minutes < 1) return false;
    tmElements_t t; RTC.read(t);
    sleptSince = (uint32_t)makeTime(t);
    uint64_t sec = (uint64_t)(60 - t.Second) + (uint64_t)(minutes - 1) * 60;
    Serial.printf("[rest] sleeping %lu min (state %d)\n",
                  (unsigned long)minutes, restState);
    bool shake = shakeArm();
    Serial.flush();
    display.hibernate();
    RTC.clearAlarm();
    pinMode(USB_DET_PIN, INPUT);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)USB_DET_PIN,
                                 digitalRead(USB_DET_PIN) == 1 ? LOW : HIGH);
    rtc_gpio_set_direction((gpio_num_t)USB_DET_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en((gpio_num_t)USB_DET_PIN);
    uint64_t mask = (BTN_PIN_MASK);
    if (shake) {                       // push-pull from the BMA: no pulls
      mask |= ACC_INT_MASK;
      rtc_gpio_set_direction((gpio_num_t)ACC_INT_1_PIN, RTC_GPIO_MODE_INPUT_ONLY);
      rtc_gpio_pullup_dis((gpio_num_t)ACC_INT_1_PIN);
      rtc_gpio_pulldown_dis((gpio_num_t)ACC_INT_1_PIN);
    }
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    rtc_gpio_set_direction((gpio_num_t)UP_BTN_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en((gpio_num_t)UP_BTN_PIN);
    rtc_clk_32k_enable(true);
    esp_sleep_enable_timer_wakeup(sec * 1000000ULL);
    esp_deep_sleep_start();
  #endif
    return false;      // other boards: minute wakes, but no redraws
  }

  // How long to sleep from here. By night, to the top of the next hour (so
  // the hourly battery sample keeps its hours) or to the end of the night.
  // By day, a few minutes, but never past the start of the night.
  uint32_t restSleepMinutes() {
    if (restState == REST_NIGHT) {
      int toEnd = minutesUntilHour(nightTo);
      int toHour = 60 - localMinuteOfDay() % 60;
      return (uint32_t)(toHour < toEnd ? toHour : toEnd);
    }
    if (!nightOn()) return REST_WAKE_MIN;
    int toNight = minutesUntilHour(nightFrom);
    return (uint32_t)(REST_WAKE_MIN < toNight ? REST_WAKE_MIN : toNight);
  }

  // Back to the full face. The data has aged while the radio was off, so if
  // it is past fresh, fetch before anyone has to ask.
  void endRest(const char *why) {
    Serial.printf("[rest] end (%s)\n", why);
    restState = REST_NONE;
    stillRun = 0;
    restExitFull = true;
    // Stale: fetch, but paint FIRST. forceFetch alone fetched before a single
    // pixel moved, so a press on a resting watch sat on the old rest face for
    // the whole WiFi join and TLS round (seconds) before anything answered.
    // fetchPending takes the prePaint path: the face goes up at once with
    // FTCH in the corner, then the radio, then the fresh face.
    // Unless the last tries failed: then the watch is offline (driving, say)
    // and a forced fetch is a 12 s WiFi search, FTCH in the corner, for a
    // network that was not there an hour ago either. The backoff decides
    // when to try again, on a minute wake; hold UP still forces one.
    if (failCount == 0 &&
        (staleMinutes() >= staleAfter() ||
         heightStaleMinutes() >= staleAfter())) {
      forceFetch = true;
      fetchPending = true;
    }
  }

  // ---------------- shake to wake ----------------
  // The library routes step, tilt and double-tap to the accelerometer's INT1
  // (GPIO14 on v3), active-high, and never wakes on it. A rest takes the line
  // over: only any-motion, active-LOW and latched until read, so it can sit
  // in the same ANY_LOW wake mask as the buttons. Its own small driver handle
  // (the library keeps its one private); same chip, same I2C bus.
  static uint16_t accRd(uint8_t a, uint8_t r, uint8_t *d, uint16_t n) {
    Wire.beginTransmission(a); Wire.write(r); Wire.endTransmission();
    Wire.requestFrom((uint8_t)a, (uint8_t)n);
    uint16_t i = 0;
    while (Wire.available() && i < n) d[i++] = Wire.read();
    return i == n ? 0 : 1;
  }
  static uint16_t accWr(uint8_t a, uint8_t r, uint8_t *d, uint16_t n) {
    Wire.beginTransmission(a); Wire.write(r); Wire.write(d, n);
    return Wire.endTransmission() != 0;
  }
  static void accDelay(uint32_t ms) { delay(ms); }
  struct bma4_dev accDev;
  bool accDevOk = false;
  bool accOpen() {
    if (accDevOk) return true;
    memset(&accDev, 0, sizeof accDev);
    accDev.dev_addr = BMA4_I2C_ADDR_PRIMARY;
    accDev.interface = BMA4_I2C_INTERFACE;
    accDev.bus_read = accRd; accDev.bus_write = accWr; accDev.delay = accDelay;
    accDev.read_write_len = 8;
    accDevOk = bma423_init(&accDev) == BMA4_OK;
    return accDevOk;
  }
  // read the interrupt status: releases a latched line
  void shakeClear() {
  #ifdef ARDUINO_ESP32S3_DEV
    if (!accOpen()) return;
    uint16_t st = 0;
    bma423_read_int_status(&st, &accDev);
  #endif
  }
  // Ready the detector for a rest sleep. True only when the line is configured
  // AND idle (high): a line already low would wake the chip the instant it
  // slept, and again, and again. Then the rest simply goes without it.
  bool shakeArm() {
  #if SHAKE_WAKE && defined(ARDUINO_ESP32S3_DEV)
    if (restState == REST_NIGHT && !SHAKE_AT_NIGHT) return false;
    if (!accOpen()) { Serial.println("[shake] no accelerometer"); return false; }
    uint16_t r = 0;
    r |= bma423_map_interrupt(BMA4_INTR1_MAP,
           BMA423_STEP_CNTR_INT | BMA423_ACTIVITY_INT | BMA423_TILT_INT |
           BMA423_WAKEUP_INT | BMA423_ERROR_INT, BMA4_DISABLE, &accDev);
    struct bma423_anymotion_config m;
    m.threshold = SHAKE_THRESH; m.duration = SHAKE_DUR; m.nomotion_sel = 0;
    r |= bma423_set_any_motion_config(&m, &accDev);
    r |= bma423_anymotion_enable_axis(BMA423_ALL_AXIS_EN, &accDev);
    r |= bma423_map_interrupt(BMA4_INTR1_MAP, BMA423_ANY_NO_MOTION_INT,
                              BMA4_ENABLE, &accDev);
    struct bma4_int_pin_config pc;
    pc.edge_ctrl = BMA4_LEVEL_TRIGGER; pc.lvl = BMA4_ACTIVE_LOW;
    pc.od = BMA4_PUSH_PULL; pc.output_en = BMA4_OUTPUT_ENABLE;
    pc.input_en = BMA4_INPUT_DISABLE;
    r |= bma4_set_int_pin_config(&pc, BMA4_INTR1_MAP, &accDev);
    r |= bma4_set_interrupt_mode(BMA4_LATCH_MODE, &accDev);
    shakeClear();                              // nothing old left latched
    pinMode(ACC_INT_1_PIN, INPUT);
    bool idle = digitalRead(ACC_INT_1_PIN) == HIGH;
    Serial.printf("[shake] %s (r=%u line=%s)\n",
                  (r == BMA4_OK && idle) ? "armed" : "NOT armed", r,
                  idle ? "idle" : "LOW");
    return r == BMA4_OK && idle;
  #else
    return false;
  #endif
  }

  // Called on every timer wake once the accelerometer has been read. Returns
  // true when this wake is a rest wake: the panel is handled here, and on v3
  // this does not return at all (the watch goes straight back to sleep).
  bool handleRest() {
    bool pressed = wakeMin < awakeTill;      // someone is using it
    bool night = inNightWindow() && !pressed;
    bool still = accDelta >= 0 && accDelta < STILL_DELTA;

    // a forgotten trip must not keep the watch up all night: night brings
    // it home (rest, night and the radio all wait while travelling)
    if (night && travelActive && guiState == WATCHFACE_STATE) {
      travelActive = false;
      Serial.println("[travel] night: home");
    }
    if (restState != REST_NONE) {
      if (!restAllowed()) { int b = restBlock(); endRest("busy"); noteMode(LM_AWAKE, b); return false; }
      if (restState == REST_NIGHT) {
        if (!inNightWindow()) { endRest("morning"); noteMode(LM_AWAKE, 0); return false; }
      } else {
        // a pickup: real movement, or set down a different way up
        if (accDelta < 0 || accDelta > MOVE_THRESHOLD || accPose != restPose) {
          endRest("moved"); noteMode(LM_AWAKE, 0); return false;
        }
        if (night) {                          // dozed into the night
          restState = REST_NIGHT;
          noteMode(LM_NIGHT, 0);
          drawRestFace(); displayFull();
          longSleep(restSleepMinutes());
          return true;
        }
      }
      // The AUTO theme's dark hours began or ended while resting: repaint
      // the rest face the new way round. Without this a rest that started
      // dark (say 06:10, dark until 07:00) stayed black all morning, since
      // a resting watch otherwise never redraws: it looked dead.
      if (darkNow() != paintedDark) {
        Serial.printf("[rest] theme now %s\n", darkNow() ? "dark" : "light");
        drawRestFace(); displayFull();     // full: the whole panel flips
      } else if (hoursLeft() != restHrsShown) {
        // the hours-left figure moved (every couple of hours): a resting
        // watch otherwise never redraws, and a stale "~1D" is the kind of
        // number that strands a wearer. Partial: nothing flips
        drawRestFace(); display.display(true);
      }
      noteMode(restState == REST_NIGHT ? LM_NIGHT : LM_REST, 0);
      // still resting, nothing changed: the panel already says it. Straight
      // back to sleep, no refresh at all.
      if (!longSleep(restSleepMinutes())) drawRestFace();
      return true;
    }

    // awake: should it rest?
    if (accPose == POSE_FACE_UP && still) stillRun++;
    else stillRun = 0;
    if (!restAllowed() || pressed) {
      noteMode(LM_AWAKE, pressed ? RB_PRESSED : restBlock());
      return false;
    }

    uint8_t want = REST_NONE;
    const char *why = "";
    if (night)                                           { want = REST_NIGHT; why = "night"; }
    else if (accPose == POSE_FACE_DOWN && accDelta >= 0 &&
             accDelta <= MOVE_THRESHOLD)                 { want = REST_DAY; why = "face down"; }
    else if (stillRun >= FLAT_STILL_MIN)                 { want = REST_DAY; why = "on a table"; }
    else if (watchIsIdle())                              { want = REST_DAY; why = "still an hour"; }
    if (want == REST_NONE) { noteMode(LM_AWAKE, RB_MOVING); return false; }

    Serial.printf("[rest] begin (%s)\n", why);
    noteMode(want == REST_NIGHT ? LM_NIGHT : LM_REST, 0);
    restState = want;
    restPose  = accPose;
    evShown = 0;                // the rest face covers anything waiting: it
                                // goes back up (and waits) after the rest
    drawRestFace();
    displayFull();     // full refresh: this image may hold for hours,
                                // so it starts clean of any ghosting
    longSleep(restSleepMinutes());
    return true;                // other boards: the library re-sends it
  }

  bool watchIsIdle() {
    return accelSeeded && (wakeMin - lastMoveWake) >= STILL_AFTER_MIN;
  }

  // offline: back off 5 -> 10 -> 20 -> 40 -> 60 min between radio attempts
  // instead of a 12 s WiFi search on every single wake. Shared by every path
  // that goes to the network on its own initiative, so one failure informs
  // all of them.
  void backOffRadio() {
    failCount++;
    uint32_t backoff = 5u << (failCount > 4 ? 4 : failCount - 1);
    if (backoff > 60) backoff = 60;
    nextTryWake = wakeMin + backoff;
  }

  bool radioBackedOff() {
    return failCount > 0 && wakeMin < nextTryWake;
  }

  // The moments the chain marks: curiosities, and an epoch's end. Called
  // after every fetch, on the wrist (maybeFetch) and on the dock, whose own
  // loop polls the tip every 15 s and never passes through maybeFetch, so
  // docked, a curiosity waited for the unplug and a recap was lost with the
  // run. Returns true if anything took the glass.
  bool markMoments() {
    bool shown = false;
    // and the curiosities, which are only ever marked once each
    // Every height since the last fetch, not just the tip: fetches come every
    // block and a half, and none come at night, so the tip alone skipped
    // whatever landed in between (970,969 -> 970,971 and 970,970 was never
    // seen). The newest curiosity in the gap shows, once; a restart starts
    // from the tip rather than replaying old ones.
    if (blockHeight > 0 && blockHeight > curioSeen) {
      long from = curioSeen > 0 ? curioSeen + 1 : blockHeight;
      if (blockHeight - from > 300) from = blockHeight - 300;
      for (long h = blockHeight; h >= from; h--) {
        if (h <= lastCurio) break;
        const char *why = curiosity(h);
        if (why && (evQueue & EV_CURIO) && evCurioH == 1000000L) {
          lastCurio = h;      // block 1,000,000 is still waiting to be seen:
          break;              // 1,000,001 (a palindrome) must not replace it
        }
        if (why) {
          lastCurio = h;
          curiosityScreen(h, why);
          // It used to hold 2.6 s and give way to the face. A fetch on a
          // minute wake has nobody watching, so it was seen by luck (970,970
          // came and went unseen). Now it waits on the glass like an alarm,
          // until a press. Already up: the hold just sleeps on it.
          evCurioH = h;
          fireEvent(EV_CURIO);
          evShown = EV_CURIO;
          shown = true;
          break;
        }
      }
      curioSeen = blockHeight;
    }

    // an era ended: the rarer goes first; both queue, both wait
    if (halvPending) {
      halvPending = false;
      halvingScreen();
      fireEvent(EV_HALV);
      evShown = EV_HALV;
      shown = true;
    }
    // an epoch ended: the dial takes the glass, then waits there for a press
    if (retPending) {
      retPending = false;
      retargetScreen();
      fireEvent(EV_RETG);
      evShown = EV_RETG;
      shown = true;
    }

    return shown;
  }

  void maybeFetch() {
    if (inDocked) return;   // docked loop schedules fetches itself,
                            // ONE TLS session at a time (heap can't
                            // hold the websocket + HTTPS together)
    if (travelActive && wakeMin - travelWake > 30) {
      travelActive = false;                  // half an hour is long enough to
      Serial.println("[travel] expired");    // look at the future
    }
    uint32_t every = watchIsIdle() ? FETCH_IDLE_MIN : fetchEveryMin();
    if (every < fetchFloorMin()) every = fetchFloorMin();   // battery profile
    // a block alarm about to ring: look more often, so it lands within a few
    // minutes of the block instead of up to a whole cycle late
    if (!watchIsIdle() && alarmHeight > 0 && blockHeight > 0 &&
        alarmHeight - blockHeight <= ALARM_NEAR_BLOCKS && every > ALARM_NEAR_EVERY)
      every = ALARM_NEAR_EVERY;
    bool due = forceFetch || !haveData ||
               (wakeMin - lastFetchMin >= every);
    // offline and a press woke it: the retry the backoff allows can wait for
    // the next minute wake, not hold the face up for a 12 s search
    if (pressRun && failCount > 0 && !forceFetch) due = false;
    if (due && (forceFetch || wakeMin >= nextTryWake)) {
      bool fok = fetchAll();
      syncEnd(fok && blockHeight > 0);
      if (fok) {
        lastFetchMin = wakeMin;
        failCount = 0; nextTryWake = 0;
        captivePortal = false;
        noteOpened();
      } else {
        // Associated but nothing came back? Probe for a captive portal. The
        // watch cannot tick a terms box, so the honest thing is to say a
        // login page is in the way rather than let the data quietly age.
        if (WiFi.status() == WL_CONNECTED) {
          WiFiClient plain;
          captivePortal = portalCheck(plain);
        }
        backOffRadio();
      }
    }
    // Market cap is price x supply(height), so it needs BOTH. On a reboot
    // each arrives from a different request, and if the price landed first
    // the cap sat at zero until something happened to call applyCurrency()
    // again — up to a full cycle later. Recompute once the fetch is done.
    if (btcPrice > 0 && blockHeight > 0)
      btcMcapB = btcPrice * supplyBTC(blockHeight) / 1e9;

    checkAlarms();                 // block, fee, confirmations: arithmetic

    markMoments();                 // curiosities, a retarget
    checkAges();                   // block birthdays: people, the watch

    // the milestone: checked where a new height first becomes known; after
    // the curiosities, so it is the one left waiting on the glass
    if (!sawMillion && blockHeight >= 1000000L && !travelActive) millionScreen();

    forceFetch   = false;   // wallet scanning moved to the post-render hook
    fetchPending = false;   // the radio has been out: the corner stops saying
                            // FTCH and goes back to reporting freshness
  }

  // ---------------- watch-only wallet ----------------
  void loadPrefs() {
    if (prefsLoaded) return;
    prefsLoaded = true;
    Preferences p;
    p.begin("btcchrono", true);
    bool zSet = p.isKey("zpub");          // stored, even as "": no default
    haveDesc = p.isKey("desc") && p.getString("desc", "").length() > 0;
    bool lSet = p.isKey("lnaddr");
    String z  = p.getString("zpub", "");
    int    tz = p.getInt("tz", -1);
    int    th = p.getInt("theme", -1);
    uint64_t nvWs = p.getULong64("wsat", 0);
    long long nvLt = p.getLong64("ltx", 0);
    int nvRi = p.getInt("ridx", -1);
    for (int i = 0; i < 3; i++) {
      String s = p.getString(("ws" + String(i)).c_str(), "");
      String w = p.getString(("wp" + String(i)).c_str(), "");
      strncpy(wifiSsid[i], s.c_str(), 32); wifiSsid[i][32] = 0;
      strncpy(wifiPass[i], w.c_str(), 64); wifiPass[i][64] = 0;
    }
    battFullV = p.getFloat("bfv", 4.20f);
    { const float def[3] = { RUN_H_LIVE, RUN_H_BALANCED, RUN_H_SAVER };
      for (int i = 0; i < 3; i++) {                // learned run lengths
        char k[6]; snprintf(k, 6, "runH%d", i);
        float v = p.getFloat(k, def[i]);
        runH[i] = (v >= 10.0f && v <= 200.0f) ? v : def[i];
      } }
    sawMillion = p.getBool("m1", false);
    alarmHeight = p.getLong("alH", 0);
    feeAlarm    = p.getUShort("alF", 0);
    { int nf = p.getChar("nFrom", NIGHT_FROM_H), nt = p.getChar("nTo", NIGHT_TO_H);
      nightFrom = (nf >= 0 && nf < 24) ? nf : NIGHT_FROM_H;
      nightTo   = (nt >= 0 && nt < 24) ? nt : NIGHT_TO_H; }
    { int df = p.getChar("dFrom", 20), dt = p.getChar("dTo", 7);
      darkFrom = (df >= 0 && df < 24) ? df : 20;
      darkTo   = (dt >= 0 && dt < 24) ? dt : 7; }
    buzzBlock = p.getBool("hBlk", BUZZ_ON_BLOCK);
    priceStep = p.getUShort("hPrc", BUZZ_PRICE_STEP);
    faceHide  = p.getUShort("face", 0);
    // Saved in an older face order? Re-map it bit by bit, by face. v1 had
    // no MEM (it starts shown); v2 had MEM after HLV.
    { uint8_t v = p.getUChar("faceV", 1);
      if (v < 3) {
        static const int8_t V1[] = {M_HGHT, M_PRICE, M_SATS, M_MCAP, M_FEES,
                                    M_SUPL, M_HALV, M_WALT, M_JOE};
        static const int8_t V2[] = {M_HGHT, M_PRICE, M_SATS, M_MCAP, M_FEES,
                                    M_SUPL, M_HALV, M_MEMP, M_WALT, M_JOE};
        const int8_t *old = v < 2 ? V1 : V2;
        int n = v < 2 ? 9 : 10;
        uint16_t m = 0;
        for (int i = 0; i < n; i++) if ((faceHide >> i) & 1) m |= 1 << old[i];
        faceHide = m;
      } }
    faceHide &= (1 << NUM_MODES) - 1;
    if (faceHide == (1 << NUM_MODES) - 1) faceHide = 0;  // never all hidden
    battProfile = p.getUChar("batt", 0);
    if (battProfile > 2) battProfile = 0;
    walletPrivate = p.getBool("wpriv", false);
    { uint8_t c = p.getUChar("cur", 0); curIdx = c < NUM_CUR ? c : 0;
      uint32_t on = p.getULong("curOn", 0x3F) & ((1UL << NUM_CUR) - 1);
      curOn = on ? on : 0x3F;
      curOn |= 1UL << curIdx; }              // the one showing is in the cycle
    // read BEFORE end(): it used to be read after, which always returned the
    // empty default, so a lightning address set on the watch was replaced by
    // the compiled-in one after every power loss or reflash
    String ln = p.getString("lnaddr", "");
    p.end();
    if (z.length() > 20) strncpy(zpubBuf, z.c_str(), sizeof(zpubBuf) - 1);
    else if (zSet)       zpubBuf[0] = 0;   // cleared on purpose (ship mode)
    else                 strncpy(zpubBuf, WALLET_ZPUB, sizeof(zpubBuf) - 1);
    zpubBuf[sizeof(zpubBuf) - 1] = 0;
    if (ln.indexOf('@') > 0) strncpy(lnAddrBuf, ln.c_str(), 63);
    else if (lSet) lnAddrBuf[0] = 0;
    else strncpy(lnAddrBuf, LN_ADDRESS, 63);
    lnAddrBuf[63] = 0;
    if (tz >= 0 && tz < 5) tzIndex = tz;
    if (th >= 0 && th < 3) themeMode = th;
    if (nvWs > 0 || nvRi >= 0) {               // restore wallet memory:
      walletSats = nvWs; lastTxSats = nvLt; // balance shows instantly,
      if (nvRi >= 0) recvIndex = nvRi;     // QR resumes correctly, and
      haveWallet = true;                   // the next sweep can diff
    }                                      // against it (catching
                                           // payments made while off)
  }

  void savePrefs() {
    // hydrate BEFORE persisting: after a sanitizer reset, a save that
    // runs before any load would write blank RAM over stored settings
    // (this erased a lightning address in the field). loadPrefs is a
    // no-op when already loaded, so this costs nothing normally.
    loadPrefs();
    Preferences p;
    p.begin("btcchrono", false);
    p.putString("zpub", zpubBuf);
    p.putString("lnaddr", lnAddrBuf);
    p.putInt("tz", tzIndex);
    p.putInt("theme", themeMode);
    p.putULong64("wsat", walletSats);      // wallet memory: balance,
    p.putLong64("ltx", lastTxSats);        // last movement, and next
    p.putInt("ridx", recvIndex);           // receive index survive
                                           // reflashes and power loss
    p.putFloat("bfv", battFullV);
    p.putBool("m1", sawMillion);
    p.putLong("alH", alarmHeight);
    p.putUShort("alF", feeAlarm);
    p.putChar("nFrom", nightFrom);
    p.putChar("nTo", nightTo);
    p.putChar("dFrom", darkFrom);
    p.putChar("dTo", darkTo);
    p.putBool("hBlk", buzzBlock);
    p.putUShort("hPrc", priceStep);
    p.putUShort("face", faceHide);
    p.putUChar("faceV", 3);
    p.putUChar("batt", battProfile);
    p.putBool("wpriv", walletPrivate);
    p.putUChar("cur", curIdx);
    p.putULong("curOn", curOn);
    p.end();
  }

  // The discharge log, written to flash.
  //
  // vlog lived only in RTC memory, which survives deep sleep but not a power
  // loss — so the one event the log exists to measure, a cell running flat,
  // was also the event that erased it. Three runs were lost that way before
  // anyone noticed the shape of the problem.
  //
  // Written once an hour alongside the sample, which is nothing against NVS
  // wear: a year of continuous wear is about nine thousand writes against a
  // hundred thousand cycle rating.
  void saveVlog() {
    Preferences p;
    p.begin("btcchrono", false);
    p.putBytes("vlog", (const void *)vlog, sizeof(vlog));
    p.putUChar("vidx", vlogIdx);
    p.end();
  }

  void loadVlog() {
    Preferences p;
    p.begin("btcchrono", true);
    size_t len = p.getBytesLength("vlog");
    if (len == sizeof(vlog)) {
      p.getBytes("vlog", (void *)vlog, sizeof(vlog));
      vlogIdx = p.getUChar("vidx", 0) % VLOG_N;
    } else if (len == 24) {
      // the old 24-hour ring: unroll it oldest first into the new one, so
      // the run in progress survives the upgrade
      uint8_t old[24];
      p.getBytes("vlog", (void *)old, 24);
      uint8_t oi = p.getUChar("vidx", 0);
      memset(vlog, 0, sizeof(vlog));
      vlogIdx = 0;
      for (int i = 0; i < 24; i++) {
        uint8_t v = old[(oi + i) % 24];
        if (v) vlog[vlogIdx++] = v;
      }
    }
    p.end();
  }

  // A zpub is an ACCOUNT key with two branches under it: .../0/i receive
  // and .../1/i change. Spending consumes a whole utxo from the receive
  // branch and returns the remainder to the change branch — so a wallet
  // that scans only chain 0 undercounts by the change the moment you send.
  // the wearer's own wallet, as opposed to the demo
  bool walletConfigured() {
    if (WALLET_ADDR_LIST[0]) return true;
    loadPrefs();
    if (haveDesc) return true;
    return zpubBuf[0] != 0 && strcmp(zpubBuf, BIP84_TEST_ZPUB) != 0;
  }
  bool isDemoWallet() { return !walletConfigured(); }

  // ---- multisig: the descriptor lives in NVS (too long for RTC memory) and
  // is read and parsed once per wake, the first time an address is wanted.
  // The branch keys are cached too: a scan then costs one derivation per key
  // per address instead of two.
  bool descRead = false;
  WalletDesc wdesc;
  HDPublicKey descBr[2][DESC_MAX_KEYS];
  bool descBrOk[2] = {false, false};

  const WalletDesc *walletDesc() {
    if (!haveDesc) return nullptr;
    if (!descRead) {
      descRead = true;
      Preferences p; p.begin("btcchrono", true);
      String t = p.getString("desc", "");
      p.end();
      descParse(t.c_str(), wdesc);
      descBrOk[0] = descBrOk[1] = false;
      if (!wdesc.ok) Serial.printf("[wallet] stored descriptor: %s\n", wdesc.err);
    }
    return wdesc.ok ? &wdesc : nullptr;
  }

  String descAddr(int chain, int index) {
    const WalletDesc *d = walletDesc();
    if (!d || chain < 0 || chain > 1) return "";
    if (!descBrOk[chain]) { descBranch(*d, chain, descBr[chain]); descBrOk[chain] = true; }
    char a[96];
    if (!descAddressFrom(*d, descBr[chain], index, a, sizeof a)) return "";
    return String(a);
  }

  String deriveAddress(int chain, int index) {
    loadPrefs();
    if (haveDesc) return descAddr(chain, index);
    HDPublicKey hd(zpubBuf);
    if (!hd.isValid()) return "";
    return hd.child(chain).child(index).address();
  }
  String deriveAddress(int index) { return deriveAddress(0, index); }

  // flipping QR -> balance re-arms a scan: the natural moment someone
  // checks their balance is right after getting paid
  bool lnConfigured() { return lnAddrBuf[0] != 0 || LN_BOLT12[0] != 0; }

  // UP cycles: balance -> chain QR -> lightning QR (if configured) ->
  // balance. Landing back on balance re-arms a scan. UP only ever moves the
  // view: the peek at a hidden balance is DOWN's (see downPressed), because
  // a first UP that sometimes showed the balance and sometimes the QR read
  // as the watch doing two different things.
  void walletToggleView() {
    walletView = (walletView + 1) % (lnConfigured() ? 3 : 2);
    if (walletView == 0) walletPeek = false;
    if (walletView == 0) {
      if (!inVigil)            // during a vigil the rescan waits for its end
        lastWalletMin = (wakeMin > walletEveryMin())
                          ? wakeMin - walletEveryMin() : 0;
    } else {
      vigilPending = true;    // wrist: stand watch when a QR shows
    }
  }

  // LNURL-pay: resolve the lightning address, request a real invoice
  // for LN_REQUEST_SATS, remember its LUD-21 verify URL for polling
  bool fetchLnInvoice() {
    lnInvoice[0] = 0; lnVerify[0] = 0;
    if (!lnAddrBuf[0] || LN_REQUEST_SATS <= 0) return false;
    if (!myConnectWiFi()) return false;
    String addr(lnAddrBuf);
    int at = addr.indexOf('@');
    if (at <= 0) return false;
    String url = "https://" + addr.substring(at + 1) +
                 "/.well-known/lnurlp/" + addr.substring(0, at);
    WiFiClientSecure c; c.setInsecure();
    HTTPClient http; http.setConnectTimeout(5000);
    String cb;
    long long msat = (long long)LN_REQUEST_SATS * 1000LL;
    if (http.begin(c, url) && http.GET() == 200) {
      JsonDocument doc;
      if (!deserializeJson(doc, http.getString())) {
        cb = doc["callback"] | "";
        long long mn = doc["minSendable"] | 0LL;
        long long mx = doc["maxSendable"] | 0LL;
        if (mn > 0 && msat < mn) msat = mn;
        if (mx > 0 && msat > mx) msat = mx;
      }
    }
    http.end();
    if (!cb.length()) return false;
    cb += (cb.indexOf('?') >= 0 ? "&" : "?");
    cb += "amount=" + String((long)msat);
    bool ok = false;
    HTTPClient h2; h2.setConnectTimeout(5000);
    if (h2.begin(c, cb) && h2.GET() == 200) {
      JsonDocument doc;
      if (!deserializeJson(doc, h2.getString())) {
        String pr = doc["pr"] | "";
        String vf = doc["verify"] | "";
        if (pr.startsWith("ln")) {
          strncpy(lnInvoice, pr.c_str(), sizeof(lnInvoice) - 1);
          strncpy(lnVerify, vf.c_str(), sizeof(lnVerify) - 1);
          lnFetchedWake = wakeMin;
          ok = true;
        }
      }
    }
    h2.end();
    Serial.printf("[ln] invoice %s, verify %s\n",
                  ok ? "OK" : "FAIL", lnVerify[0] ? "yes" : "no");
    return ok;
  }

  // poll LUD-21: true once the invoice settles
  bool lnSettled() {
    if (!lnVerify[0]) return false;
    WiFiClientSecure c; c.setInsecure();
    HTTPClient http; http.setConnectTimeout(4000);
    bool settled = false;
    if (http.begin(c, lnVerify) && http.GET() == 200) {
      JsonDocument doc;
      if (!deserializeJson(doc, http.getString()))
        settled = doc["settled"] | false;
    }
    http.end();
    return settled;
  }

  // PAYMENT VIGIL (wrist): showing the QR arms a 2 minute awake watch on
  // that address (or the lightning invoice). Putting the QR away does NOT
  // end it: the sender usually presses send just after it is put away, so
  // the watch keeps listening for the rest of the two minutes while every
  // button does what it does on the face. Sats landing: the payment buzz,
  // and either the wallet face (if you are on it) or a PAY screen.
  // Costs ~1.3% battery per vigil, the same as before; a payment arriving
  // later still turns up at the next wallet scan.
  bool inVigil = false;

  // UP's short press as the face does it, for the vigil, which reads the
  // buttons itself. (The long press, fetch everything, is not offered while
  // the radio is busy listening.)
  void upShortPress() {
    if (dispMode == M_JOE) joeAdvance();
    else if (dispMode == M_WALT) walletToggleView();
    else if (dispMode == M_PRICE || dispMode == M_SATS || dispMode == M_MCAP) {
      curIdx = nextCur(curIdx);
      applyCurrency();
      saveCurrency();
    } else {
      cellSel[dispMode] = (cellSel[dispMode] + 1) % cellStops(dispMode);
    }
  }

  // One poll of the buttons inside the vigil. Returns true for MENU, which
  // ends the vigil and opens the menu.
  bool vigilButtons() {
    int p = -1;                      // not 0: on v3 UP IS GPIO 0
    if      (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE) p = BACK_BTN_PIN;
    else if (digitalRead(UP_BTN_PIN)   == BTN_ACTIVE) p = UP_BTN_PIN;
    else if (digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) p = DOWN_BTN_PIN;
    else if (digitalRead(MENU_BTN_PIN) == BTN_ACTIVE) p = MENU_BTN_PIN;
    if (p < 0) return false;
    if (evQueue && evShown && !qrOnGlass()) {    // ANY BUTTON: OK
      dismissEvent();
      waitAllRelease();
    } else if (p == MENU_BTN_PIN) {
      waitAllRelease();
      return true;
    } else if (p == BACK_BTN_PIN) {
      backPressed();                 // test the hold BEFORE releasing:
      waitRelease(BACK_BTN_PIN);     // afterwards there is nothing to measure
    } else if (p == UP_BTN_PIN) {
      waitRelease(UP_BTN_PIN);
      upShortPress();
    } else {
      downPressed();
      waitRelease(DOWN_BTN_PIN);
    }
    buttonWake = true;
    drawWatchFace();
    partialRefresh();
    offIfFull();
    return false;
  }

  void paymentVigil() {
    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);
    // lightning view: poll the invoice's verify URL instead of an address —
    // settlement lands in seconds, so poll every 5
    const bool ln = (walletView == 2);
    if (ln && !lnVerify[0]) return;
    String a;
    if (!ln) {
      if (isDemoWallet()) return;                    // no QR was shown
      a = walletAddr(recvIndex); if (!a.length()) return;
    }
    if (!myConnectWiFi()) return;            // no network: sleep as usual
    Serial.printf("[vigil] armed 120s (%s)\n", ln ? "lightning" : "on-chain");
    inVigil = true;
    lateKind = 0;                    // a vigil supersedes any late watch
    bool menu = false, paid = false, polled = false;
    unsigned long start = millis(), lastPoll = 0;
    const unsigned long every = ln ? 5000UL : 15000UL;
    uint64_t base = 0; bool haveBase = false;
    while (millis() - start < 120000) {
      if (!polled || millis() - lastPoll > every) {
        polled = true;
        lastPoll = millis();
        // something on the face may have used the radio and switched it off
        // (a scheduled fetch); come back on before asking
        if (WiFi.status() == WL_CONNECTED || myConnectWiFi()) {
          if (ln) {
            if (lnSettled()) {
              paid = true;
              lnVerify[0] = 0; lnInvoice[0] = 0;   // invoice is consumed
              vibMotor(60, 4); delay(120);
              vibMotor(60, 4); delay(120);
              vibMotor(60, 8);
              Serial.println("[vigil] lightning payment settled!");
              if (dispMode == M_WALT) {            // its own moment, where
                drawLnPaid(LN_REQUEST_SATS);       // the wallet is
                delay(3500);
              } else {                             // elsewhere: a PAY screen
                evPaySats = LN_REQUEST_SATS; evPayLn = true;
                fireEvent(EV_PAY);
              }
              buttonWake = true;
              drawWatchFace();                     // fresh invoice QR, or
              partialRefresh();                    // the PAY screen
              offIfFull();
              break;
            }
          } else {
            WiFiClientSecure c; c.setInsecure();
            HTTPClient http; http.setConnectTimeout(4000);
            if (http.begin(c, String(ESPLORA_BASE) + a) && http.GET() == 200) {
              JsonDocument doc;
              if (!deserializeJson(doc, http.getString())) {
                uint64_t f =
                  (uint64_t)(doc["chain_stats"]["funded_txo_sum"] | 0ULL) +
                  (uint64_t)(doc["mempool_stats"]["funded_txo_sum"] | 0ULL);
                if (!haveBase) { base = f; haveBase = true; }
                else if (f > base) {
                  uint64_t delta = f - base;
                  http.end(); c.stop();
                  paid = true;
                  vibMotor(60, 4); delay(120);
                  vibMotor(60, 4); delay(120);
                  vibMotor(60, 8);                 // payment received!
                  Serial.printf("[vigil] payment received! +%llu sats\n",
                                (unsigned long long)delta);
                  // trust the observed delta — an instant rescan races the
                  // explorer's indexer and reads pre-payment state
                  lastTxSats = (long long)delta;
                  walletSats += delta;
                  haveWallet = true;
                  armTracking(a.c_str());          // and follow it to 6 confs
                  bool away = (dispMode != M_WALT);
                  recvIndex++;                     // address used: advance
                  walletView = 0;                  // show the new balance
                  savePrefs();                     // receipt -> NVS
                  if (away) {                      // not on the wallet: say so
                    evPaySats = (long long)delta; evPayLn = false;
                    fireEvent(EV_PAY);
                  }
                  buttonWake = true;
                  drawWatchFace();
                  partialRefresh();
                  offIfFull();
                  break;
                }
              }
            }
            http.end();
          }
        }
      }
      if (vigilButtons()) { menu = true; break; }
      delay(40);
    }
    inVigil = false;
    WiFi.mode(WIFI_OFF); btStop();
    // Nothing yet, but the sender may just be slow (a hardware wallet, a
    // careful fee choice). Keep an eye on the same address, or the same
    // invoice, at each regular fetch for the next hour.
    if (!paid && !menu && (ln ? lnVerify[0] != 0 : haveBase)) {
      lateKind  = ln ? LATE_LN : LATE_CHAIN;
      lateIdx   = recvIndex;
      lateBase  = base;
      lateUntil = wakeMin + LATE_WATCH_MIN;
      Serial.printf("[late] watching for %d min\n", LATE_WATCH_MIN);
    }
    // leaving the QR for the balance normally rescans the wallet at once. It
    // waited, because a rescan during the vigil could count a payment twice
    // (once in the sum, once as the vigil's delta). Do it now.
    if (!paid && dispMode == M_WALT && walletView == 0)
      lastWalletMin = (wakeMin > walletEveryMin()) ? wakeMin - walletEveryMin() : 0;
    Serial.printf("[vigil] done%s\n", paid ? " (paid)" : menu ? " (menu)" : "");
    if (menu) {
      // The vigil runs inside showWatchFace(), which marks the watch as on
      // its face when it returns. So show the menu and sleep from here, in
      // menu state, the way every menu wake ends.
      openMenu();
      longSleep(1);                  // v3: does not return
    }
  }

  // the addresses actually watched: the compiled list, else the demo when no
  // zpub is set, else none (the zpub derives them)
  const char *walletList() {
    if (WALLET_ADDR_LIST[0]) return WALLET_ADDR_LIST;
    loadPrefs();
    if (haveDesc) return "";                 // the descriptor derives them
    if (!zpubBuf[0] || strcmp(zpubBuf, BIP84_TEST_ZPUB) == 0) return DEMO_ADDR;
    return "";
  }

  int walletListCount() {
    const char *s = walletList();
    if (!*s) return 0;
    int n = 1;
    for (const char *p = s; *p; p++) if (*p == ',') n++;
    return n;
  }

  // i-th watched address: from the pasted list if configured (multisig-
  // safe), otherwise derived from the zpub
  String walletAddr(int chain, int index) {
    if (walletListCount() > 0) {
      if (chain != 0) return "";        // a pasted list has no change branch
      return walletAddr(index);
    }
    return deriveAddress(chain, index);
  }

  String walletAddr(int index) {
    if (walletListCount() > 0) {
      String all(walletList());
      int start = 0, idx = 0;
      for (int i = 0; i <= (int)all.length(); i++) {
        if (i == (int)all.length() || all[i] == ',') {
          if (idx == index) {
            String a = all.substring(start, i); a.trim(); return a;
          }
          idx++; start = i + 1;
        }
      }
      return "";
    }
    return deriveAddress(index);
  }

  // Long network loops must stay interruptible. The gap-limit scan can make
  // sixty sequential requests; without this the watch ignores every button
  // for the duration, which reads as a freeze.
  void watchButtonsDuringScan(bool on) {
    if (on) {
      g_isrPin = -1;
      int edge = (BTN_ACTIVE == 0) ? FALLING : RISING;
      attachInterrupt(digitalPinToInterrupt(BACK_BTN_PIN), btnISR0, edge);
      attachInterrupt(digitalPinToInterrupt(MENU_BTN_PIN), btnISR1, edge);
      attachInterrupt(digitalPinToInterrupt(UP_BTN_PIN),   btnISR2, edge);
      attachInterrupt(digitalPinToInterrupt(DOWN_BTN_PIN), btnISR3, edge);
    } else {
      detachInterrupt(digitalPinToInterrupt(BACK_BTN_PIN));
      detachInterrupt(digitalPinToInterrupt(MENU_BTN_PIN));
      detachInterrupt(digitalPinToInterrupt(UP_BTN_PIN));
      detachInterrupt(digitalPinToInterrupt(DOWN_BTN_PIN));
    }
  }

  int pressedButton() {
    if (g_isrPin >= 0) { int p = g_isrPin; g_isrPin = -1; return p; }  // caught
    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);
    if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE) return BACK_BTN_PIN;
    if (digitalRead(MENU_BTN_PIN) == BTN_ACTIVE) return MENU_BTN_PIN;
    if (digitalRead(UP_BTN_PIN)   == BTN_ACTIVE) return UP_BTN_PIN;
    if (digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) return DOWN_BTN_PIN;
    return -1;                       // not 0: that is the UP button on v3
  }

  bool fetchWallet() {
    // GAP-LIMIT scan, like real wallet software: walk forward until 5
    // consecutive unused addresses (hard cap 60). A fixed window fails
    // the moment a wallet has history — payments land on address #10
    // while a 10-address sweep stares forever at #0-#9.
    if (!myConnectWiFi()) return false;
    const int GAP = 5, HARD_CAP = 60;
    uint64_t total = 0;
    int lastUsed = -1, firstUnused = -1;
    int listN = walletListCount();
    WiFiClientSecure client; client.setInsecure();
    // BOTH branches: chain 0 is where payments arrive, chain 1 is where your
    // own change comes back. Only chain 0 sets the next receive index.
    int chains = (listN > 0) ? 1 : 2;
    int scanned = 0;                 // addresses walked across both branches
    String incoming;                 // first receive address with mempool funds
    for (int chain = 0; chain < chains; chain++) {
    int unusedRun = 0, i = 0;
    while (true) {
      if (listN > 0) { if (i >= listN) break; }        // list: scan all
      else if (unusedRun >= GAP || i >= HARD_CAP) break;
      String addr = walletAddr(chain, i);
      if (addr.length() == 0) return false;
      HTTPClient http; http.setConnectTimeout(4000);
      bool got = false;
      if (http.begin(client, String(ESPLORA_BASE) + addr) &&
          http.GET() == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString())) {
          got = true;
          uint64_t funded = doc["chain_stats"]["funded_txo_sum"] | 0ULL;
          uint64_t spent  = doc["chain_stats"]["spent_txo_sum"]  | 0ULL;
          uint64_t mFund  = doc["mempool_stats"]["funded_txo_sum"] | 0ULL;
          uint64_t mSpent = doc["mempool_stats"]["spent_txo_sum"]  | 0ULL;
          total += (funded + mFund) - (spent + mSpent);
          // money arriving at a receive address that no vigil saw land
          if (chain == 0 && mFund > 0 && !incoming.length())
            incoming = addr;
          long txs = (doc["chain_stats"]["tx_count"] | 0L)
                   + (doc["mempool_stats"]["tx_count"] | 0L);
          if (txs > 0) { if (chain == 0) lastUsed = i; unusedRun = 0; }
          else {
            if (chain == 0 && firstUnused < 0) firstUnused = i;
            unusedRun++;
          }
        }
      }
      http.end();
      // fail HARD on any request failure: a partial sum silently
      // undercounts, which is worse than an honest stale balance
      if (!got) { if (!inDocked) { WiFi.mode(WIFI_OFF); btStop(); }
                  return false; }
      i++;
      // a press cancels the scan. nothing is committed: a partial sum is a
      // wrong balance, and the wrist wants the button more than the number
      int btn = pressedButton();
      if (btn >= 0) {
        scanCancelled = btn;
        if (!inDocked) { WiFi.mode(WIFI_OFF); btStop(); }
        Serial.printf("[wallet] scan cancelled at %d/%d\n", chain, i);
        return false;
      }
    }
    Serial.printf("[wallet] chain %d: %d addrs walked\n", chain, i);
    scanned += i;
    }
    if (!inDocked) { WiFi.mode(WIFI_OFF); btStop(); }
    // the scan is the authoritative count: a late watch still running would
    // add the same payment a second time
    if (lateKind == LATE_CHAIN) { lateKind = LATE_NONE; Serial.println("[late] scan took over"); }
    if (incoming.length()) armTracking(incoming.c_str());   // resolved at
                                                            // the next fetch
    if (haveWallet && total != walletSats)
      lastTxSats = (long long)total - (long long)walletSats;
    walletSats = total;
    // next receive: wallet convention is (highest used + 1) for zpub
    // derivation; for a pasted list, the first unused entry
    recvIndex = (listN > 0)
                  ? (firstUnused >= 0 ? firstUnused : 0)
                  : lastUsed + 1;
    haveWallet = true;
    savePrefs();                           // wallet memory -> NVS
    Serial.printf("[wallet] scanned %d addrs (both chains), lastUsed %d, "
                  "next #%d, TOTAL %llu sats\n",
                  scanned, lastUsed, recvIndex,
                  (unsigned long long)walletSats);
    Serial.print("[wallet] addr#0 = ");
    Serial.println(walletAddr(0));   // compare against your wallet app!
    return true;
  }


  // ---------------- helpers ----------------
  bool themeDark = false;   // cached once per render

  bool darkNow() {
    if (themeMode == 1) return true;
    if (themeMode == 2) {
      time_t lt = makeTime(currentTime);
      time_t utc = lt - settings.gmtOffset;
      lt = utc + tzOffsetSec(tzIndex, utc);
      tmElements_t dt; breakTime(lt, dt);
      int h = dt.Hour;                     // the wearer's dark hours
      if (darkFrom == darkTo) return false;
      return (darkFrom < darkTo) ? (h >= darkFrom && h < darkTo)
                                 : (h >= darkFrom || h < darkTo);
    }
    return false;
  }

  uint16_t fg() { return themeDark ? GxEPD_WHITE : GxEPD_BLACK; }
  uint16_t bg() { return themeDark ? GxEPD_BLACK : GxEPD_WHITE; }

  long staleMinutes() {
    long e = (long)wakeMin - (long)lastFetchMin;
    return e < 0 ? 0 : e;
  }

  long heightStaleMinutes() {
    long e = (long)wakeMin - (long)lastHeightWake;
    return e < 0 ? 0 : e;
  }

  // the timechain keeps ticking even when we can't hear it: estimate
  // the current height from the last fetched height + wake-minutes at
  // ~10 min/block. Corrected on the next successful fetch. Estimation
  // is capped at 24h of drift — beyond that an estimate is fiction,
  // so the dials hold at last-known + 144 rather than spinning away.
  // Counting forward at the CURRENT epoch's pace is right until the estimate
  // crosses a retarget, after which the chain runs at a rate we already know
  // is different: diffChangeEst is the adjustment mempool.space projects, and
  // block time moves inversely with difficulty. So the walk is done in two
  // legs — blocks remaining in this epoch at avgBlockSec, the rest at the
  // corrected pace. Immaterial over an hour; worth a block or two over a day.
  float blocksForward(long minutes) {
    if (minutes <= 0) return 0;
    float secs      = minutes * 60.0f;
    long  intoEpoch = blockHeight % 2016L;
    float leftHere  = (float)(2016L - intoEpoch);          // blocks to retarget
    float secsHere  = leftHere * avgBlockSec;
    if (secs <= secsHere) return secs / avgBlockSec;       // never gets there

    float nextSec = avgBlockSec * (1.0f + diffChangeEst / 100.0f);
    nextSec = constrain(nextSec, 300.0f, 1200.0f);
    return leftHere + (secs - secsHere) / nextSec;
  }

  // The power-law model, anchored to the last real price we fetched: the
  // exponent extrapolates, the anchor keeps the near end honest. Returns 0
  // when it cannot be computed, so callers can dash the value.
  // Above a thousand billion the number stops meaning anything: 1,600 BILLION
  // is 1.6 TRILLION, and in yen the billions run to six figures. One rule for
  // the magnitude, consulted by both the value and the unit line.
  // Box money. Grouped with separators up to a million, abbreviated beyond —
  // one coin in yen is eight figures and will not fit a 96 px box at any
  // sensible size. Text font, because DSEG has no comma glyph.
  void fmtMoney(char *out, size_t n, double v) {
    if (v < 0) v = 0;
    if (v >= 1e9) { snprintf(out, n, "%.2f B", v / 1e9); return; }
    if (v >= 1e6) { snprintf(out, n, "%.2f M", v / 1e6); return; }
    long whole = (long)(v + 0.5);
    if (whole < 1000) { snprintf(out, n, "%ld", whole); return; }
    char raw[16]; snprintf(raw, 16, "%ld", whole);
    int len = strlen(raw), o = 0;
    for (int i = 0; i < len && o < (int)n - 2; i++) {
      out[o++] = raw[i];
      int left = len - i - 1;
      if (left > 0 && left % 3 == 0 && o < (int)n - 2) out[o++] = ',';
    }
    out[o] = 0;
  }

  bool capInTrillions(double capB) { return capB >= 1000.0; }
  void fmtCap(char *out, size_t n, double capB) {
    if (capInTrillions(capB)) {
      double t = capB / 1000.0;
      snprintf(out, n, t < 10 ? "%.2f" : (t < 100 ? "%.1f" : "%.0f"), t);
    } else {
      snprintf(out, n, capB < 100 ? "%.1f" : "%.0f", capB);
    }
  }

  // THE PAST IS RECORDED. Travelling forward, price can only be modelled;
  // travelling back, it actually happened. A handful of anchors — the four
  // halvings and a few well-known heights between them — with log-linear
  // interpolation, because price moves in orders of magnitude. The final
  // anchor is today's live price at today's height, so the recent end is
  // never stale and the table never needs updating.
  //
  // These figures are approximate: they are what the market was around, not
  // a close on a given day. Labelled RECORDED, never LIVE.
  struct PriceAnchor { long h; float usd; };
  static const int N_ANCHORS = 10;
  // The past is recorded, so fetch it rather than approximating: the block's
  // own timestamp, then mempool's historical price at that moment. One-shot,
  // on arrival, cached against the height it was fetched for. No network
  // means the table below still answers, marked APPROX.
  bool fetchPastPrice(long h) {
    if (h <= 0 || h >= blockHeight) return false;
    // Away from every saved network (a demo at a friend's), each trip back
    // used to spend a full 12 s WiFi search before giving up. If the last
    // attempt already failed, the anchor table answers at once, marked APPROX.
    if (radioBackedOff()) {
      Serial.println("[past] offline backoff - using the table");
      return false;
    }
    if (!myConnectWiFi()) { backOffRadio(); return false; }
    WiFiClientSecure client; client.setInsecure();
    long ts = 0;

    { HTTPClient http; http.setConnectTimeout(4000);
      String u = "https://mempool.space/api/block-height/" + String(h);
      if (http.begin(client, u) && http.GET() == 200) {
        String hash = http.getString(); hash.trim();
        http.end();
        if (hash.length() == 64) {
          HTTPClient h2; h2.setConnectTimeout(4000);
          if (h2.begin(client, "https://mempool.space/api/block/" + hash) &&
              h2.GET() == 200) {
            JsonDocument d;
            if (!deserializeJson(d, h2.getString())) ts = d["timestamp"] | 0L;
          }
          h2.end();
        }
      } else http.end();
    }
    if (ts <= 0) {                       // fall back to our own reckoning
      time_t now = time(nullptr);
      if (now < 1700000000) { tmElements_t tn; RTC.read(tn); now = makeTime(tn); }
      ts = (long)(now + (h - blockHeight) * (double)avgBlockSec);
    }

    bool ok = false;
    { HTTPClient http; http.setConnectTimeout(4000);
      String u = "https://mempool.space/api/v1/historical-price?currency=USD"
                 "&timestamp=" + String(ts);
      if (http.begin(client, u) && http.GET() == 200) {
        JsonDocument d;
        if (!deserializeJson(d, http.getString())) {
          double p = d["prices"][0]["USD"] | 0.0;
          if (p > 0) { travelPastPrice = p; travelPastFor = h; ok = true; }
        }
      }
      http.end(); }
    if (!inDocked) { WiFi.mode(WIFI_OFF); btStop(); }
    Serial.printf("[past] block %ld ts=%ld -> %s%.2f\n", h, ts,
                  ok ? "" : "FAILED ", travelPastPrice);
    return ok;
  }

  // A captive portal accepts the association and then intercepts everything.
  // The standard probe is an endpoint that returns 204 with no body: anything
  // other than a bare 204 means something is sitting in the way.
  bool portalCheck(WiFiClient &plain) {
    HTTPClient http; http.setConnectTimeout(3000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    bool portal = false;
    if (http.begin(plain, "http://connectivitycheck.gstatic.com/generate_204")) {
      int code = http.GET();
      portal = (code != 204);
      Serial.printf("[net] portal probe -> %d%s\n", code,
                    portal ? "  CAPTIVE" : "");
    }
    http.end();
    return portal;
  }

  // What each currency was worth at the four halvings, in units per USD:
  // MARKET rates (the street rate for ARS, NGN and LBP, which had an
  // official one far from it for years). Rough levels, like the price
  // anchors: enough to say what a unit of money used to buy.
  //  2012-11-28  2016-07-09  2020-05-11  2024-04-20
  static const float *fxRow(int c) {
    static const float T[NUM_CUR][4] = {
      {     1.0f,     1.0f,      1.0f,      1.0f },   // USD
      {   0.773f,   0.903f,    0.924f,    0.938f },   // EUR
      {   0.995f,   1.310f,    1.400f,    1.375f },   // CAD
      {    82.1f,   100.6f,    107.6f,    154.6f },   // JPY
      {   0.625f,   0.773f,    0.808f,    0.809f },   // GBP
      {   0.931f,   0.983f,    0.972f,    0.911f },   // CHF
      {   0.959f,   1.326f,    1.535f,    1.558f },   // AUD
      {    6.23f,    6.69f,     7.09f,     7.24f },   // CNY
      {    7.75f,    7.76f,     7.75f,     7.83f },   // HKD
      {   1.221f,   1.350f,    1.414f,    1.361f },   // SGD
      {    54.9f,    67.2f,     75.6f,     83.4f },   // INR
      {  1084.0f,  1157.0f,   1222.0f,   1378.0f },   // KRW
      {    2.10f,    3.32f,     5.80f,     5.21f },   // BRL
      {   12.98f,    18.6f,     24.0f,    17.07f },   // MXN
      {    6.46f,    15.2f,    119.5f,   1000.0f },   // ARS (blue)
      {    1.79f,    2.95f,     7.05f,     32.5f },   // TRY
      {    8.87f,    14.9f,     18.5f,    19.15f },   // ZAR
      {   160.0f,   372.0f,    440.0f,   1125.0f },   // NGN (parallel)
      {   3.673f,   3.673f,    3.673f,    3.673f },   // AED
      {  1507.5f,  1507.5f,   4000.0f,  89500.0f },   // LBP (street)
    };
    return T[(c >= 0 && c < NUM_CUR) ? c : 0];
  }
  static bool fxMarket(int c) { return c == 14 || c == 17 || c == 19; }  // ARS NGN LBP
  // units of currency c per USD at height h: interpolated between the
  // halvings, then toward today's live cross rate
  double fxThen(int c, long h) {
    if (c <= 0 || c >= NUM_CUR) return 1.0;
    static const long FH[4] = {210000L, 420000L, 630000L, 840000L};
    const float *T = fxRow(c);
    if (h <= FH[0]) return T[0];
    double nowR = (fxRate[0] > 0 && fxRate[c] > 0) ? (double)fxRate[c] / fxRate[0] : T[3];
    long hh[5] = {FH[0], FH[1], FH[2], FH[3], blockHeight > FH[3] ? blockHeight : FH[3] + 1};
    double vv[5] = {T[0], T[1], T[2], T[3], nowR};
    for (int k = 0; k < 4; k++) {
      if (h <= hh[k + 1]) {
        double f = (double)(h - hh[k]) / (double)(hh[k + 1] - hh[k]);
        return exp(log(vv[k]) + f * (log(vv[k + 1]) - log(vv[k])));
      }
    }
    return nowR;
  }
  // the unit SAT compares: k of the face currency, k a power of ten so that
  // today it buys at least 100 sats. Writes "$1" / "1 EUR" / "1,000 LBP".
  double satUnit(char *o, size_t n) {
    double k = 1;
    while (btcPrice > 0 && k < 1e7 && k * 1e8 / btcPrice < 100) k *= 10;
    char kg[16]; grouped(kg, 16, (long long)k);
    if (curIdx == 0 && k == 1) snprintf(o, n, "$1");
    else snprintf(o, n, "%s %s", kg, CUR_CODES[curIdx]);
    return k;
  }
  // the past price in the face's currency
  double pastPriceCur(long h) { return pastPrice(h) * fxThen(curIdx, h); }

  double pastPrice(long targetH) {
    if (travelPastFor == targetH && travelPastPrice > 0)
      return travelPastPrice;            // fetched: the real thing
    static const PriceAnchor A[N_ANCHORS] = {
      { 100000L,      0.20f },   // Dec 2010
      { 210000L,     12.0f  },   // Nov 2012 — first halving
      { 300000L,    950.0f  },   // Dec 2013
      { 420000L,    650.0f  },   // Jul 2016 — second halving
      { 500000L,   7900.0f  },   // Nov 2017
      { 550000L,   4300.0f  },   // Nov 2018
      { 630000L,   8600.0f  },   // May 2020 — third halving
      { 700000L,  45000.0f  },   // Sep 2021
      { 750000L,  19000.0f  },   // Oct 2022
      { 840000L,  63800.0f  },   // Apr 2024 — fourth halving
    };
    if (targetH <= A[0].h) return A[0].usd;

    // the live present is the last anchor
    long  hiH = blockHeight > 0 ? blockHeight : A[N_ANCHORS-1].h;
    float hiP = fxRate[0] > 0 ? fxRate[0]                 // USD, like the anchors
              : (curIdx == 0 && btcPrice > 0) ? (float)btcPrice : A[N_ANCHORS-1].usd;

    long  loH = A[0].h;  float loP = A[0].usd;
    for (int i = 0; i < N_ANCHORS; i++) {
      if (A[i].h <= targetH) { loH = A[i].h; loP = A[i].usd; }
      else { hiH = A[i].h; hiP = A[i].usd; break; }
    }
    if (hiH <= loH || loP <= 0 || hiP <= 0) return loP;
    double f = (double)(targetH - loH) / (double)(hiH - loH);
    return exp(log(loP) + f * (log(hiP) - log(loP)));   // log-linear
  }

  // Seconds from now until a given height. The CURRENT epoch runs at the
  // measured pace, but everything past the next retarget reverts to ten
  // minutes — difficulty exists precisely to pull it back. Projecting a fast
  // epoch across forty more of them put the 2028 halving in January.
  double secondsUntil(long targetH) {
    long here = estHeight();
    if (targetH <= here) return 0;
    long toRetarget = 2016L - (here % 2016L);        // rest of this epoch
    long atPace = targetH - here;
    if (atPace > toRetarget) atPace = toRetarget;
    double s = atPace * (double)avgBlockSec;
    long beyond = (targetH - here) - atPace;
    if (beyond > 0) s += beyond * 600.0;             // the protocol's target
    return s;
  }

  double modelPrice(long targetH) {
    const double N = 5.82;                 // published fit, not a fact
    const double GENESIS = 1231006505.0;   // 2009-01-03 18:15:05 UTC
    if (btcPrice <= 0 || blockHeight <= 0) return 0;
    time_t now = time(nullptr);
    if (now < 1700000000) { tmElements_t tn; RTC.read(tn); now = makeTime(tn); }
    double dNow = (now - GENESIS) / 86400.0;
    double dThen = dNow + (targetH - blockHeight) * (double)avgBlockSec / 86400.0;
    if (dNow < 1 || dThen < 1) return 0;
    return (btcPrice / pow(dNow, N)) * pow(dThen, N);
  }

  long estHeight() {
    if (travelActive) return travelHeight;   // everything derived from height
                                             // follows: supply, epoch, almanac
    if (blockHeight <= 0) return 0;
    if (inDocked) return blockHeight;  // docked: the socket streams the
                                       // true tip in realtime; estimating
                                       // on top of it invents blocks that
                                       // haven't been mined yet
    long e = heightStaleMinutes();   // NOT the price stamp: if only the
    if (e > 1440) e = 1440;          // tip fetch fails, keep estimating

    // While the fetch cycle is still healthy, show the number we were TOLD,
    // not one we inferred. Estimating between fetches makes the height climb
    // at ~10 min, then fall back when the next fetch disagrees — a block
    // counter that runs backwards, and briefly reports a block nobody mined.
    // Lagging the chain is honest; overshooting it is not.
    if (e < staleAfter()) return blockHeight;

    return blockHeight + (long)blocksForward(e);
  }

  // Blocks arrive as a Poisson process, so an estimate of n blocks carries a
  // standard error of about sqrt(n) blocks. Guessing 36 blocks over six hours
  // means the true tip is plausibly +/-6 away. The watch should say so rather
  // than print a confident number that is quietly wrong.
  // How long the current tip has stood. Counted from the block's own
  // timestamp against the RTC, so it advances every minute without asking
  // anyone. Returns -1 when no timestamp has been seen yet.
  // How often to go out, from how fast the chain is actually running.
  //
  // A fixed fifteen minutes is right at a ten-minute pace and wrong at both
  // ends: when the chain speeds up the watch falls further behind between
  // fetches, and when it slows down we spend radio for nothing. So the
  // interval follows the measured pace — one and a half blocks' worth —
  // which lands on the old fifteen at 10 min/blk and tightens on its own
  // when the chain runs hot.
  //
  // Proportional rather than a threshold on purpose: a cliff at 9 min/blk
  // would thrash every time avgBlockSec wobbled across the line.
  //
  // The ceiling is 18, not 20: STALE_AFTER_MIN is 20, and a normal cycle
  // must never trip the LIVE -> OLD flip on its own.
  // Battery profile. Live is the watch as it always was; Balanced and Saver
  // stretch the gaps between fetches and wallet sweeps, which are what the
  // battery pays for. Block alarms keep their own short cadence near the
  // block, and the dock (on USB) is untouched.
  enum { BATT_LIVE = 0, BATT_BALANCED = 1, BATT_SAVER = 2 };
  uint32_t fetchFloorMin() {
    if (inDocked) return 0;           // on USB: the dock keeps its own pace
    return battProfile == BATT_SAVER ? 60 : battProfile == BATT_BALANCED ? 30 : 0;
  }
  uint32_t walletEveryMin() {
    if (inDocked) return WALLET_EVERY_MIN;
    return battProfile == BATT_SAVER ? 120 : battProfile == BATT_BALANCED ? 60
                                                                         : WALLET_EVERY_MIN;
  }
  // When the face stops saying LIVE: a cycle actually missed, so it follows
  // the profile's interval (a Saver watch is not stale at 25 minutes).
  long staleAfter() {
    uint32_t f = fetchFloorMin();
    return f ? (long)f + 5 : STALE_AFTER_MIN;
  }

  uint32_t fetchEveryMin() {
    float mins = (avgBlockSec / 60.0f) * 1.5f;
    if (!(mins > 0)) return FETCH_EVERY_MIN;      // NaN guard
    if (mins < 10) mins = 10;
    if (mins > 18) mins = 18;
    return (uint32_t)(mins + 0.5f);
  }

  int blockAgeMin() {
    if (tipBlockTime == 0) return -1;
    // the system clock is unset until NTP anchors it — after a reflash that
    // can be a quarter of an hour of "--". The RTC has known UTC the whole
    // time, so fall back to it rather than refusing to answer.
    time_t now = time(nullptr);
    if (now < 1700000000) {
      tmElements_t t; RTC.read(t);
      now = makeTime(t);                      // RTC keeps UTC
    }
    if (now < 1700000000) return -1;          // genuinely no clock
    long a = ((long)now - (long)tipBlockTime) / 60;
    if (a < 0)   a = 0;
    if (a > 999) a = 999;
    return (int)a;
  }

  int estBlocksAhead() {
    if (blockHeight <= 0 || inDocked) return 0;
    long e = heightStaleMinutes();
    if (e > 1440) e = 1440;
    if (e < staleAfter()) return 0;    // same gate as estHeight: no
    return (int)blocksForward(e);         // estimate while the cycle is healthy
  }
  int estUncertainty() {
    int n = estBlocksAhead();
    if (n <= 0) return 0;
    int u = (int)(sqrtf((float)n) + 0.5f);
    return u < 1 ? 1 : u;
  }

  // DSEG's glyph range is 0x2D..0x3A — digits, minus, period, colon. No
  // comma, because seven-segment displays never had one. So the separators
  // are drawn: a 2x2 block on the baseline with a tail, sized to the font.
  // Grouping is applied to whole numbers of five digits or more, where it
  // earns its space; 20074579 is unreadable, 20,074,579 is not.
  // width of a value once its separators are added
  // Must measure what drawGrouped actually draws: whole part in the given
  // face plus its commas, fraction in the smaller face after a gap. Measuring
  // the raw string in the big font overestimates and picks a size too small.
  // how far the cursor moves per digit in a given face — the same for every
  // digit in DSEG, and the only figure that predicts the drawn width
  // A DSEG '1' is 4 px of ink pushed 18 px into a 25 px cell. Centring the
  // ADVANCE box therefore leaves a value like 1264 visibly right of centre,
  // while centring the INK breaks the grouped layout. So: lay out on
  // advances, then shift by the bearings of the first and last glyphs.
  int glyphLeft(const GFXfont *f, char c) {
    int first = (int)pgm_read_byte(&f->first);
    if (c < first) return 0;
    return (int)(int8_t)pgm_read_byte(&f->glyph[(int)c - first].xOffset);
  }
  int glyphRightGap(const GFXfont *f, char c) {
    int first = (int)pgm_read_byte(&f->first);
    if (c < first) return 0;
    int adv = (int)pgm_read_byte(&f->glyph[(int)c - first].xAdvance);
    int w   = (int)pgm_read_byte(&f->glyph[(int)c - first].width);
    int xo  = (int)(int8_t)pgm_read_byte(&f->glyph[(int)c - first].xOffset);
    return adv - (xo + w);
  }

  int digitAdvance(const GFXfont *f) {
    return (int)pgm_read_byte(&f->glyph[(int)'0' - (int)pgm_read_byte(&f->first)].xAdvance);
  }

  int groupedWidthImpl(const char *s, const GFXfont *f, int *commasOut) {
    char whole[16]; char frac[8]; frac[0] = 0;
    const char *dot = strchr(s, '.');
    if (dot) {
      int n = (int)(dot - s); if (n > 15) n = 15;
      memcpy(whole, s, n); whole[n] = 0;
      snprintf(frac, 8, "%s", dot + 1);
    } else {
      snprintf(whole, 16, "%s", s);
    }

    int digits = 0;
    for (const char *p = whole; *p; p++) if (*p >= '0' && *p <= '9') digits++;
    int commas = (digits >= 5) ? (digits - 1) / 3 : 0;
    int commaW = (f == &DSEG7_Classic_Bold_32) ? 6
               : (f == &DSEG7_Classic_Bold_25) ? 4 : 3;
    const GFXfont *sf = &DSEG7_Classic_Bold_14;   // matches drawGrouped

    // getTextBounds measures INK, not advance — and in DSEG a '1' is 4 px of
    // ink inside a 25 px cell while '.' advances zero. So identical-length
    // values measured differently and one of them overflowed. Every digit
    // advances the same amount, so compute it: digits x advance, exactly.
    int total;
    if (frac[0] && digits < 5) {
      int n = 0;                              // one size, point costs nothing
      for (const char *p = s; *p; p++) if (*p >= '0' && *p <= '9') n++;
      total = n * digitAdvance(f);
    } else {
      total = (int)strlen(whole) * digitAdvance(f) + commas * commaW;
      if (frac[0]) total += 1 + 2 + (int)strlen(frac) * digitAdvance(sf);
    }
    if (commasOut) *commasOut = commas;
    return total;
  }

  // Largest face that still fits the panel. Measuring beats guessing by digit
  // count: 20,073,300.00 is thirteen characters but only ten of them are full
  // width, so it sets larger than a naive ladder would allow.
  const GFXfont *fitFont(const char *s) {
    const GFXfont *ladder[] = { &DSEG7_Classic_Bold_32,
                                &DSEG7_Classic_Bold_25,
                                &DSEG7_Classic_Bold_18 };
    for (int i = 0; i < 3; i++)
      if (groupedWidthImpl(s, ladder[i], nullptr) <= 194) return ladder[i];
      // 188 of 200: about 6 px of margin each side. Tight enough that the
      // eight-digit supply figure holds the 25 px face rather than dropping
    return &DSEG7_Classic_Bold_14;
  }

  void drawGrouped(const char *s, int y, const GFXfont *f) {
    // GFX wraps by default: an overlong value folded its last digit onto the
    // next line and landed on top of EPOC. Clipping is the lesser failure,
    // and fitFont below should prevent either.
    display.setTextWrap(false);
    // Whole part big and grouped, fraction small and set apart. DSEG's decimal
    // point is drawn to overlay the preceding digit's corner, so printing it
    // inline character by character reads as a gap in the wrong place —
    // 924,731.25 came out looking like 924,73 1.25. Splitting the two also
    // gives the decimals somewhere to breathe.
    char whole[16]; char frac[8]; frac[0] = 0;
    const char *dot = strchr(s, '.');
    if (dot) {
      int n = (int)(dot - s); if (n > 15) n = 15;
      memcpy(whole, s, n); whole[n] = 0;
      snprintf(frac, 8, "%s", dot + 1);
    } else {
      snprintf(whole, 16, "%s", s);
    }

    int digits = 0;
    for (const char *p = whole; *p; p++) if (*p >= '0' && *p <= '9') digits++;
    int commas  = (digits >= 5) ? (digits - 1) / 3 : 0;
    int commaW  = (f == &DSEG7_Classic_Bold_32) ? 6
                : (f == &DSEG7_Classic_Bold_25) ? 4 : 3;
    // always the smallest face. Stepping it up to 18 made it the same size as
    // an 18 px whole part, and cost the width that kept the whole part at 25.
    const GFXfont *sf = &DSEG7_Classic_Bold_14;
    const int GAP = 1;                       // small type needs little air

    // advance-based, exactly as groupedWidthImpl computes it: ink measurement
    // varies with which digits a value contains and the two disagreed
    // A short value like 1.60 or 0.25 IS its decimals — shrinking them makes
    // the number look like it has a footnote. Only split the fraction when
    // the whole part is long enough to need separators; otherwise set the
    // lot at one size and let DSEG's own decimal point do the work: it has
    // zero advance and sits in the preceding digit's corner, the way a real
    // seven-segment display shows it.
    if (frac[0] && digits < 5) {
      int n = 0;
      for (const char *p = s; *p; p++) if (*p >= '0' && *p <= '9') n++;
      int adv  = digitAdvance(f);
      // centre the INK, both ends: correcting only the leading bearing left
      // a short value a few pixels left of centre, which is exactly what the
      // long path already handles
      int lead = glyphLeft(f, s[0]);
      int trail = glyphRightGap(f, s[strlen(s) - 1]);
      int x = (200 - (n * adv - lead - trail)) / 2 - lead;
      if (x < 0) x = 0;
      display.setFont(f);
      display.setCursor(x, y);
      display.print(s);
      return;
    }

    int glyphs = (int)strlen(whole);          // dashes and minus signs count
    int wholeW = glyphs * digitAdvance(f) + commas * commaW;
    int fracW  = 0;
    if (frac[0]) {
      fracW = GAP + 2 + (int)strlen(frac) * digitAdvance(sf);
    }
    display.setFont(f);
    // shift so the INK is centred, not the advance box
    int lead = glyphLeft(f, whole[0]);
    char lastCh = frac[0] ? frac[strlen(frac) - 1]
                          : whole[strlen(whole) - 1];
    int trail = glyphRightGap(frac[0] ? sf : f, lastCh);
    int x = (200 - (wholeW + fracW - lead - trail)) / 2 - lead;
    if (x < 0) x = 0;

    display.setFont(f);
    for (int i = 0; whole[i]; i++) {
      char c[2] = { whole[i], 0 };
      display.setCursor(x, y);
      display.print(c);
      x += digitAdvance(f);                  // uniform: never trust the cursor
      int left = digits - i - 1;
      if (left > 0 && left % 3 == 0) {       // a grouping boundary
        int cw = (commaW - 2) / 2;
        display.fillRect(x + cw, y - 3, 2, 3, fg());
        display.drawPixel(x + cw, y, fg());
        display.drawPixel(x + cw - 1, y + 1, fg());
        x += commaW;
      }
    }

    if (frac[0]) {                            // a drawn point, then the digits
      x += GAP;
      display.fillRect(x, y - 2, 2, 2, fg());
      x += 2;
      display.setFont(sf);
      display.setCursor(x, y);
      display.print(frac);
    }
  }

  void centerText(const String &s, int y, const GFXfont *f) {
    int16_t x1, y1; uint16_t w, h;
    display.setFont(f);
    display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((200 - w) / 2 - x1, y);
    display.print(s);
  }

  void sparkHiLo(float &hi, float &lo, int &n) {
    hi = -1e12; lo = 1e12; n = 0;
    for (int i = 0; i < SPARK_POINTS; i++)
      if (spark[i] > 0) { hi = max(hi, spark[i]); lo = min(lo, spark[i]); n++; }
  }

  // total issued supply is a pure function of height — computed on-device,
  // no API needed. ("Bitcoin is time": the height IS the calendar.)
  double supplyBTC(long h) {
    if (h <= 0) return 0;
    uint64_t sats = 0, subsidy = 5000000000ULL;  // 50 BTC in sats
    long remaining = h + 1;                       // blocks incl. genesis
    while (remaining > 0 && subsidy > 0) {
      long era = remaining < 210000L ? remaining : 210000L;
      sats += subsidy * (uint64_t)era;
      remaining -= era;
      subsidy >>= 1;
    }
    return sats / 1e8;
  }

  // The cap is NOT 21,000,000. Each subsidy is an integer number of satoshis
  // and halving truncates, so the schedule tops out at 20,999,999.9769 BTC —
  // 0.0231 short. Measuring against a round 21 million overstates what is
  // left to mine, and the error grows as a share of the remainder: by the
  // 2130s the watch would be promising hundredths of a coin that cannot exist.
  double supplyCapBTC() {
    static double cap = 0;
    if (cap == 0) cap = supplyBTC(6930000L * 2);   // past the last paying era
    return cap;
  }

  // ---- RETARGET: the EPOC dial, blown up to the whole panel at 100 ----
  // The little dial spends two weeks filling; when it fills, it takes the
  // glass and says what the network just did to itself. stage 0: the ring
  // nearly full, 1: closed at 100, 2: the recap inside it.

  // the dial at any size: the face's small EPOC dial, scaled. The arc and
  // ring keep their proportions; the label sits inside when there is room.
  void drawDialRing(int cx, int cy, int R, float pct, const char *name) {
    display.drawCircle(cx, cy, R, GxEPD_WHITE);
    int tk = R > 40 ? 4 : 3;
    for (int a = 0; a < 360; a += 90) {            // the quarter ticks
      float rad = a * DEG_TO_RAD;
      display.drawLine(cx + cosf(rad) * (R + 1), cy + sinf(rad) * (R + 1),
                       cx + cosf(rad) * (R + tk), cy + sinf(rad) * (R + tk),
                       GxEPD_WHITE);
    }
    int outer = R - (R > 40 ? 3 : 2), inner = R - (R > 40 ? 10 : 5);
    // the arc, solid: a filled ring (spokes at this radius leave gaps)
    display.fillCircle(cx, cy, outer, GxEPD_WHITE);
    display.fillCircle(cx, cy, inner, GxEPD_BLACK);
    if (pct < 1.0f)                               // what is still to go
      for (float a = -90 + pct * 360.0f; a <= 270; a += 0.25f) {
        float rad = a * DEG_TO_RAD;
        display.drawLine(cx + cosf(rad) * (inner - 1), cy + sinf(rad) * (inner - 1),
                         cx + cosf(rad) * (outer + 1), cy + sinf(rad) * (outer + 1),
                         GxEPD_BLACK);
      }
    char p[8]; snprintf(p, 8, "%d", (int)(pct * 100 + 0.001f));
    display.setFont(NULL); display.setTextSize(1);
    if (R > 40) {
      char l[16]; snprintf(l, 16, "%s %s", name, p);
      display.setCursor(cx - (int)strlen(l) * 3, cy + R - 32);
      display.print(l);
    } else {                                       // the face's own layout
      display.setCursor(cx - (int)strlen(p) * 3, cy - 3); display.print(p);
      display.setCursor(cx - (int)strlen(name) * 3, cy - R - 12);
      display.print(name);
    }
  }

  // stage 0: the face's small dial, 1: halfway out, 2: full size at 97,
  // 3: closed at 100, 4: the recap inside it
  void drawRetarget(const Retarget &r, int stage) {
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    static const int CX[] = {26, 62, 100}, CY[] = {80, 90, 100}, RR[] = {16, 52, 95};
    int k = stage < 2 ? stage : 2;
    drawDialRing(CX[k], CY[k], RR[k], stage < 3 ? 0.97f : 1.0f, "EPOC");
    if (stage < 4) return;

    char s[34];
    snprintf(s, 34, "EPOCH %ld", r.epoch);
    centerSmall(s, 30);
    centerSmall("DIFFICULTY ADJUSTED", 44);

    // the change, big: digits in the face's 7-segment, sign and % beside
    // (the font carries digits, '-' and '.', no '+' or '%')
    char v[12]; snprintf(v, 12, "%.2f", fabsf(r.change));
    int16_t x1, y1; uint16_t w, h;
    display.setFont(&DSEG7_Classic_Bold_32);
    display.getTextBounds(v, 0, 0, &x1, &y1, &w, &h);
    int total = 14 + w + 18, x = (200 - total) / 2;
    display.setFont(NULL); display.setTextSize(2);
    display.setCursor(x, 67); display.print(r.change < 0 ? "-" : "+");
    display.setTextSize(1);                       // a GFX font scales too
    display.setFont(&DSEG7_Classic_Bold_32);
    display.setCursor(x + 14 - x1, 90); display.print(v);
    display.setFont(NULL); display.setTextSize(2);
    display.setCursor(x + 14 + w + 6, 67); display.print("%");
    display.setTextSize(1);

    display.drawFastHLine(70, 102, 60, GxEPD_WHITE);
    if (r.days > 0) {
      snprintf(s, 34, "2,016 BLOCKS IN %.1f DAYS", r.days);
      centerSmall(s, 110);
    } else centerSmall("2,016 BLOCKS", 110);
    { char g[12]; grouped(g, 12, (long long)(r.mined + 0.5));
      snprintf(s, 34, "%s BTC MINED", g);
      centerSmall(s, 122); }
    int y = 134;
    if (r.fees >= 0) {
      snprintf(s, 34, "+ %.1f IN FEES", r.fees);
      centerSmall(s, y); y += 12;
    }
    if (r.txs > 0) {
      snprintf(s, 34, "%.1fM TRANSACTIONS", r.txs / 1e6);
      centerSmall(s, y);
    }
  }

  // ---- HALVING: the HALV dial, the same way, once every four years ----
  // stage 0: the face's dial (right side), 1: halfway out, 2: full at 97,
  // 3: closed at 100, 4: the recap of the era that just ended
  void drawHalving(const Halving &v, int stage) {
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    static const int CX[] = {174, 138, 100}, CY[] = {80, 90, 100}, RR[] = {16, 52, 95};
    int k = stage < 2 ? stage : 2;
    drawDialRing(CX[k], CY[k], RR[k], stage < 3 ? 0.97f : 1.0f, "HALV");
    if (stage < 4) return;

    char s[34];
    snprintf(s, 34, "HALVING %d", v.n);
    centerSmall(s, 32);

    // the new subsidy, big; what it was, small, under it
    char b[12]; snprintf(b, 12, "%.8g", v.subsidy);
    centerText(b, 78, &DSEG7_Classic_Bold_32);
    display.setFont(NULL); display.setTextSize(1);
    char was[12]; snprintf(was, 12, "%.8g", v.subsidy * 2);
    snprintf(s, 34, "BTC A BLOCK, WAS %s", was);
    centerSmall(s, 86);

    display.drawFastHLine(70, 99, 60, GxEPD_WHITE);
    if (v.years > 0) {
      snprintf(s, 34, "210,000 BLOCKS, %.1f YEARS", v.years);
      centerSmall(s, 106);
    } else centerSmall("210,000 BLOCKS", 106);
    { char g[16]; grouped(g, 16, (long long)(v.mined + 0.5));
      snprintf(s, 34, "%s BTC MINED", g);
      centerSmall(s, 118); }
    int y = 130;
    if (v.fees >= 0) {
      char g[16]; grouped(g, 16, (long long)(v.fees + 0.5));
      snprintf(s, 34, "+ %s IN FEES", g);
      centerSmall(s, y); y += 12;
    }
    { char g[16]; grouped(g, 16, (long long)(v.supply + 0.5));
      snprintf(s, 34, "%s OF 21M", g);
      centerSmall(s, y); }
  }

  // ---------------- the faces' lower halves (FEE, SUP, HLV, MEM) --------------
  // Built-in font throughout; y 100-173, between the dials and the divider.
  void lowAt(const char *t, int x, int y) { display.setCursor(x, y); display.print(t); }
  void lowRight(const char *t, int y) { lowAt(t, 194 - (int)strlen(t) * 6, y); }

  // a price in the chosen currency, with cents when it is small
  void fiatSmall(char *o, size_t n, double v) {
    if (v <= 0) { snprintf(o, n, "----"); return; }
    char m[20];
    if (v < 100) snprintf(m, 20, "%.2f", v); else fmtMoney(m, 20, v);
    snprintf(o, n, "%s %s", m, CUR_CODES[curIdx]);
  }

  // FEE: what a payment costs now, and how much is waiting
  void drawFeeLower() {
    display.setFont(NULL); display.setTextColor(fg());
    lowAt("A TYPICAL SEND, 141 VB", 8, 104);   // 1 input, 2 outputs, segwit
    const char *T[3] = {"FAST", "30 MIN", "1 HOUR"};
    float r[3] = {fastFee, medFee, lowFee};
    for (int i = 0; i < 3; i++) {
      int y = 115 + i * 10;
      char a[16], f[24];
      lowAt(T[i], 8, y);
      if (r[i] > 0) {
        long sats = (long)(r[i] * 141.0f + 0.5f);
        snprintf(a, 16, "%ld SAT", sats);
        fiatSmall(f, 24, btcPrice > 0 ? sats / 1e8 * btcPrice : 0);
      } else { strcpy(a, "----"); strcpy(f, "----"); }
      lowAt(a, 64, y);
      lowRight(f, y);
    }
    display.drawFastHLine(6, 147, 188, fg());
    lowAt("MEMPOOL", 8, 153);
    int full = mempoolBlocks > 0 ? (int)ceilf(mempoolBlocks) : 0;
    for (int i = 0; i < 10; i++) {
      int x = 56 + i * 9;
      if (i < full) display.fillRect(x, 152, 7, 9, fg());
      else display.drawRect(x, 152, 7, 9, fg());
    }
    char t[34];
    if (full > 10) snprintf(t, 34, "%d+", full); else snprintf(t, 34, "%d BLK", full);
    lowRight(full > 0 ? t : "----", 153);
    if (full > 0) {
      long mins = (long)(mempoolBlocks * avgBlockSec / 60.0f + 0.5f);
      if (mins >= 60) snprintf(t, 34, "~%ldH %02ldM OF BLOCKS WAITING", mins / 60, mins % 60);
      else snprintf(t, 34, "~%ld MIN OF BLOCKS WAITING", mins);
      lowAt(t, 8, 164);
    }
  }

  // SUP: the road to 21M, and the pace along it
  // SUP's second view (UP, with BTC REMAINING): how hot the issuance still
  // is. A block's "temperature" is its new coins over all coins mined: 1 at
  // genesis (50 of 50), cooling toward 0 in 2140. On a log scale it steps
  // down one notch per halving. The line under it says the same in words:
  // one new coin for every N already mined, and N goes up by exactly one
  // every block (each block adds its reward to the total) until a halving
  // doubles it.
  void drawSupCooling() {
    display.setFont(NULL); display.setTextColor(fg());
    long h = estHeight();
    int era = (int)(h / 210000L);
    double sub = subsidySats(era) / 1e8, all = supplyBTC(h);
    lowAt("GENESIS: HOT", 8, 100); lowRight("2140: COLD", 100);
    const int x0 = 12, x1 = 188, y = 114;
    const double CMAX = 15.5;                  // the last 1-sat era: 10^-15.3
    auto X = [&](double c) { return x0 + 8 + (int)(c / CMAX * (x1 - x0 - 10)); };
    display.drawRoundRect(x0, y, x1 - x0, 10, 5, fg());
    display.fillCircle(x0 + 4, y + 5, 7, fg());                 // the bulb
    for (int k = 1; k <= LAST_HALVING; k++) {                   // a notch per halving
      double ck = -log10((subsidySats(k) / 1e8) / supplyBTC(210000L * k));
      display.drawFastVLine(X(ck), y + 11, (k % 4 == 0) ? 4 : 2, fg());
    }
    int mx;
    if (sub > 0 && all > 0) mx = X(-log10(sub / all));
    else mx = x1 - 3;                                            // cold
    display.fillRect(x0 + 4, y + 3, mx - x0 - 4, 4, fg());       // the mercury
    display.fillTriangle(mx - 3, y - 6, mx + 3, y - 6, mx, y - 1, fg());
    lowAt("NOW", constrain(mx - 9, 2, 176), y + 17);
    display.drawFastHLine(6, 142, 188, fg());
    if (sub > 0) {
      char g[16], a[34];
      grouped(g, 16, (long long)(all / sub + 0.5));
      centerSmall("1 NEW COIN FOR EVERY", 148);
      snprintf(a, 34, "%s ALREADY MINED", g);
      centerSmall(a, 159);
    } else {
      centerSmall("NO NEW COINS ANY MORE", 148);
      centerSmall("COLD: FEES ONLY", 159);
    }
  }

  void drawSupLower() {
    if (cellSel[M_SUPL] % 2) { drawSupCooling(); return; }   // with REMAINING
    display.setFont(NULL); display.setTextColor(fg());
    long h = estHeight();
    double cap = supplyCapBTC(), issued = supplyBTC(h);
    // The last million: 0-21M showed a bar that was simply full. From 20M
    // the remaining wait is visible, and so is each halving adding half
    // what the one before did. The break mark says it does not start at 0.
    // Before 20M (time travel) it falls back to the whole road.
    const double lo = issued >= 20e6 ? 20e6 : 0;
    lowAt(lo > 0 ? "20M" : "0", 8, 103); lowRight("21M", 103);
    int x0 = 8, W = 184, y = 113;
    display.drawRect(x0, y, W, 10, fg());
    display.fillRect(x0, y, (int)(W * (issued - lo) / (cap - lo) + 0.5), 10, fg());
    if (lo > 0) {
      display.fillRect(x0 - 2, y - 1, 5, 12, bg());
      display.drawLine(x0 - 2, y + 11, x0 + 1, y - 1, fg());
      display.drawLine(x0 + 1, y + 11, x0 + 4, y - 1, fg());
    }
    int labelled = 0, lastLx = -100;
    for (int k = 1; k <= 12; k++) {                 // where each halving lands
      double sk = supplyBTC(210000L * k);
      if (sk < lo) continue;
      int x = x0 + (int)(W * (sk - lo) / (cap - lo) + 0.5);
      if (x > x0 + W - 1) x = x0 + W - 1;
      display.drawFastVLine(x, y + 11, 4, fg());
      // the next three halvings by year (they get closer: 4 years apart)
      // Each halving adds half the last, so far ahead they crowd into the
      // bar's last few pixels: a year is only written where it has room.
      int lx = x - 12; if (lx > 194 - 24) lx = 194 - 24;
      if (lo > 0 && labelled < 3 && 210000L * k > h && lx >= lastLx + 28) {
        char yr[6]; snprintf(yr, 6, "%d", 2008 + 4 * k);   // 2012, 2016 ...
        lowAt(yr, lx, y + 17);
        labelled++; lastLx = lx;
      }
    }
    if (lo == 0) lowAt("EACH TICK A HALVING", 8, 131);
    display.drawFastHLine(6, 142, 188, fg());
    int era = (int)(h / 210000L);
    double sub = era < 33 ? 50.0 / (double)(1LL << era) : 0;
    char a[24], b[24];
    long long daySats = subsidySats(era) * 144;
    if (daySats == 0)                 snprintf(a, 24, "NO MORE BTC");
    else if (daySats < 1000000LL)     snprintf(a, 24, "%lld SATS A DAY", daySats);
    else snprintf(a, 24, sub * 144 >= 10 ? "%.0f BTC A DAY" : "%.2f BTC A DAY", sub * 144);
    double pct = issued > 0 ? sub * 144 * 365.25 / issued * 100 : 0;
    if (daySats == 0)       snprintf(b, 24, "FEES ONLY");
    else if (pct < 0.005)   snprintf(b, 24, "~0%% A YEAR");
    else                    snprintf(b, 24, "%.2f%% A YEAR", pct);
    lowAt(a, 8, 148); lowRight(b, 148);
    // the last sat: block 6,930,000, at ten minutes a block from the real
    // tip (a fact about now, wherever Time Travel has gone)
    // (the same clock as HGT and the travel title, so they agree on 2140)
    int64_t tEnd = heightUtc(6930000L), tNow = heightUtc(blockHeight);
    double years = tEnd > tNow ? (double)(tEnd - tNow) / (365.25 * 86400.0) : 0;
    snprintf(a, 24, "LAST SAT ~%d", civil64(tEnd).y);
    snprintf(b, 24, "IN %d YEARS", (int)(years + 0.5));
    if (daySats == 0) {                    // travelled past it: it is behind you
      snprintf(a, 24, "LAST SAT MINED");
      snprintf(b, 24, "~%d", civil64(tEnd).y);
    }
    lowAt(a, 8, 160); lowRight(b, 160);
  }

  // the block subsidy of era k in sats, as consensus computes it (a shift)
  static long long subsidySats(int k) { return (k >= 0 && k < 64) ? (5000000000LL >> k) : 0; }
  // sats, short: 78M, 9.8M, 610K, 1.2K, 37
  static void satsShort(char *o, size_t n, long long v) {
    if (v >= 10000000LL)    snprintf(o, n, "%lldM", v / 1000000LL);
    else if (v >= 1000000LL) snprintf(o, n, "%.1fM", v / 1e6);
    else if (v >= 10000LL)   snprintf(o, n, "%lldK", v / 1000LL);
    else if (v >= 1000LL)    snprintf(o, n, "%.1fK", v / 1e3);
    else                     snprintf(o, n, "%lld", v);
  }

  // HLV: the subsidy, era by era, and where we are on it
  void drawHalvLower() {
    display.setFont(NULL); display.setTextColor(fg());
    long h = estHeight();
    int era = (int)(h / 210000L);
    char a[24], g[16];
    bool over = era > LAST_HALVING;                 // 6,930,000 on: reward 0
    if (over) {
      lowAt("NO MORE BTC", 8, 103);
      lowRight("FEES ONLY", 103);
    } else {
      snprintf(a, 24, "ERA %d OF 33", era + 1);
      lowAt(a, 8, 103);
      grouped(g, 16, (long long)((era + 1) * 210000L - h));
      snprintf(a, 24, "%s TO GO", g);
      lowRight(a, 103);
    }
    int first = era < 4 ? 0 : era - 4;              // NOW stays in the window
    if (first > LAST_HALVING - 6) first = LAST_HALVING - 6;   // ends on 1 sat
    bool inSats = subsidySats(first) < 100000000LL; // under a coin: count sats
    const int base = 152, bw = 24, gap = 4, x = 4;
    for (int i = 0; i < 7; i++) {
      int k = first + i;
      int hgt = 5 + (6 - i) * 5;                    // log scale: each halves
      int xx = x + i * (bw + gap);
      if (k <= era) display.fillRect(xx, base - hgt, bw, hgt, fg());
      else display.drawRect(xx, base - hgt, bw, hgt, fg());
      if (k == era) lowAt("NOW", xx + 3, base - hgt - 10);
      double v = 50.0 / (double)(1LL << k);
      char t[8];
      if (inSats)                  satsShort(t, 8, subsidySats(k));
      else if (v >= 10 && v == (long)v) snprintf(t, 8, "%.0f", v);
      else if (v >= 10)            snprintf(t, 8, "%.1f", v);
      else if (v >= 1)             snprintf(t, 8, "%.2f", v + 1e-9);  // 3.13, not 3.12
      else                         snprintf(t, 8, ".%02d", (int)(v * 100 + 0.5));
      lowAt(t, xx + (bw - (int)strlen(t) * 6) / 2, base + 3);
    }
    lowRight(inSats ? "SATS/BLOCK" : "BTC/BLOCK", base - 38);
  }

  // digits grouped in threes, in a 7-segment face, commas as pixels; returns
  // the width (measure only when draw is false)
  int segGrouped(const char *d, int x, int y, const GFXfont *f, int cw, bool draw = true) {
    display.setFont(f);
    int n = strlen(d), cx = x;
    for (int i = 0; i < n; i++) {
      GFXglyph *g = f->glyph + (d[i] - f->first);
      if (draw) { char c[2] = {d[i], 0}; display.setCursor(cx, y); display.print(c); }
      cx += g->xAdvance;
      if ((n - 1 - i) % 3 == 0 && i < n - 1) {
        if (draw) display.fillRect(cx + cw / 2 - 1, y - 1, 2, cw > 3 ? 4 : 3, fg());
        cx += cw;
      }
    }
    display.setFont(NULL);
    return cx - x;
  }

  // SAT: what a dollar used to buy, at each halving, in sats. The halving
  // prices are the Time Travel anchors (USD, rough levels), so the table is
  // in dollars whatever the face's currency; TODAY is live.
  void drawSatLower() {
    display.setFont(NULL); display.setTextColor(fg());
    // in the face's currency (UP turns it): the price anchors are USD, the
    // currency's own past rate carries them over (market rates)
    char unit[20], t0[34];
    double k = satUnit(unit, 20);
    snprintf(t0, 34, "WHAT %s USED TO BUY", unit);
    lowAt(t0, 8, 103);
    if (strlen(t0) <= 22) lowRight("IN SATS", 103);
    const long HH[4] = {210000L, 420000L, 630000L, 840000L};
    const char *YY[5] = {"2012", "2016", "2020", "2024", "TODAY"};
    double sats[5];
    for (int i = 0; i < 4; i++) { double p = pastPriceCur(HH[i]); sats[i] = p > 0 ? k * 1e8 / p : 0; }
    sats[4] = btcPrice > 0 ? k * 1e8 / btcPrice : 0;
    // Log scale from just below today's value rather than from 1 sat: from
    // 1, 8.3M sats got 70 px and today's 820 still 29, a gentle slope for
    // a 99.99% fall. From half of today, 2012 is the full width and today
    // a stub. Still log underneath, so 2016 and 2020 stay readable.
    double top = sats[0] > 1 ? sats[0] : 1;
    double floorS = sats[4] > 2 ? sats[4] / 2 : 1;
    // bars from x = 44, stopping short of the widest number (a weak currency
    // in 2012 runs to 1,289,989,672 sats)
    int numW = 0;
    for (int i = 0; i < 5; i++) {
      char g[16]; grouped(g, 16, (long long)(sats[i] + 0.5));
      if ((int)strlen(g) * 6 > numW) numW = strlen(g) * 6;
    }
    int BW = 194 - numW - 6 - 44; if (BW > 86) BW = 86; if (BW < 30) BW = 30;
    for (int i = 0; i < 5; i++) {
      int y = 113 + i * 9;
      lowAt(YY[i], 8, y);
      char g[16];
      if (sats[i] > 0) grouped(g, 16, (long long)(sats[i] + 0.5)); else strcpy(g, "----");
      lowRight(g, y);
      if (sats[i] >= 1 && top > floorS) {
        int w = (int)(BW * (log10(sats[i]) - log10(floorS)) /
                           (log10(top) - log10(floorS)) + 0.5);
        if (w < 3) w = 3;
        if (i == 4) display.fillRect(44, y, w, 7, fg());
        else display.drawRect(44, y, w, 7, fg());
      }
    }
    display.drawFastHLine(6, 160, 188, fg());
    if (sats[0] > 0 && sats[4] > 0) {
      char t[34];
      double lost = (1.0 - sats[4] / sats[0]) * 100.0;
      char who[20];
      if (curIdx == 0) snprintf(who, 20, "THE DOLLAR");
      else if (fxMarket(curIdx)) snprintf(who, 20, "%s (MARKET)", CUR_CODES[curIdx]);
      else snprintf(who, 20, "THE %s", CUR_CODES[curIdx]);
      // as many decimals as it takes not to round to -100%: 1,000 ARS in
      // 2012 bought 12.9 BTC, and today 585 sats
      char pc[16]; int d = 2;
      do { snprintf(pc, 16, "%.*f", d, lost); } while (atof(pc) >= 100.0 && ++d <= 7);
      snprintf(t, 34, d > 2 ? "%s, IN BTC: -%s%%" : "%s, IN BITCOIN: -%s%%", who, pc);
      if (strlen(t) > 31) {                    // too long: the code alone
        snprintf(who, 20, "%s", curIdx == 0 ? "THE DOLLAR" : CUR_CODES[curIdx]);
        snprintf(t, 34, d > 2 ? "%s, IN BTC: -%s%%" : "%s, IN BITCOIN: -%s%%", who, pc);
      }
      centerSmall(t, 164);
    }
  }

  // CAP: the two monies on one scale, and the price at which they meet.
  // Gold is fetched in USD (spot x ~6.95B oz, the same constant as % GOLD)
  // and carried into the face's currency by the cross rate.
  void fmtTB(char *o, size_t n, double b) {        // billions in, short out
    // B, T, Q: a weak unit (LBP) puts gold in the thousands of quadrillions
    const char *U[3] = {"B", "T", "Q"};
    int u = 0;
    while (b >= 1000 && u < 2) { b /= 1000; u++; }
    if (b >= 100 || u == 0) snprintf(o, n, "%.0f%s", b, U[u]);
    else if (b >= 10)       snprintf(o, n, "%.1f%s", b, U[u]);
    else                    snprintf(o, n, "%.2f%s", b, U[u]);
  }
  void drawCapLower() {
    display.setFont(NULL); display.setTextColor(fg());
    double cross = (fxRate[0] > 0 && fxRate[curIdx] > 0) ? fxRate[curIdx] / fxRate[0] : 0;
    double goldB = (goldMcapB > 0 && cross > 0) ? goldMcapB * cross : 0;
    double btcB  = btcMcapB;
    double big   = goldB > btcB ? goldB : btcB;
    char t[24];
    lowAt("GOLD", 8, 105); lowAt("BTC", 8, 119);
    if (big > 0 && goldB > 0) {                 // no gold yet: no scale either
      int gw = goldB > 0 ? (int)(120 * goldB / big + 0.5) : 0;
      int bw = btcB > 0 ? (int)(120 * btcB / big + 0.5) : 0;
      display.drawRect(40, 105, 120, 8, fg()); if (gw) display.fillRect(40, 105, gw, 8, fg());
      display.drawRect(40, 119, 120, 8, fg()); if (bw) display.fillRect(40, 119, bw > 1 ? bw : 2, 8, fg());
    }
    if (goldB > 0) { fmtTB(t, 24, goldB); lowRight(t, 105); } else lowRight("----", 105);
    if (btcB > 0)  { fmtTB(t, 24, btcB);  lowRight(t, 119); } else lowRight("----", 119);
    display.drawFastHLine(6, 134, 188, fg());
    long h = blockHeight > 0 ? blockHeight : estHeight();
    double supply = supplyBTC(h);
    if (goldB <= 0 || supply <= 0 || btcPrice <= 0) {
      lowAt("PRICE AT GOLD PARITY", 8, 140);
      lowAt("WAITING FOR GOLD'S PRICE", 8, 156);
      return;
    }
    double parity = goldB * 1e9 / supply;
    if (btcB >= goldB) {                         // the day it happens
      lowAt("BITCOIN HAS PASSED GOLD", 8, 140);
      snprintf(t, 24, "%.2fX GOLD", btcB / goldB);
      lowAt(t, 8, 156);
      return;
    }
    lowAt("PRICE AT GOLD PARITY", 8, 140);
    if (parity < 1e7) {                          // spelled out: 1,249,000
      char d[16]; snprintf(d, 16, "%.0f", parity);
      int w = segGrouped(d, 8, 170, &DSEG7_Classic_Bold_18, 3);
      lowAt(CUR_CODES[curIdx], 14 + w, 162);
    } else {                                     // a weak unit: 110.83 T LBP
      char m[20];
      if (parity >= 1e12) snprintf(m, 20, "%.2f T", parity / 1e12);
      else                fmtMoney(m, 20, parity);
      snprintf(t, 24, "%s %s", m, CUR_CODES[curIdx]);
      lowAt(t, 8, 158);
    }
    snprintf(t, 24, parity / btcPrice < 100 ? "%.1fX" : "%.0fX", parity / btcPrice);
    lowRight(t, 151); lowRight("AWAY", 162);
  }

  // ---------------- Time Travel lower halves (PRC, SAT, CAP, FEE, WAL) -------
  // Each answers "what was it like then, against now?". The past uses the
  // Time Travel anchors (USD, rough levels: RECORDED); the future the power
  // law model in the face's currency, always named as a model. What cannot
  // be known (past fees, past gold, the mempool) says so.
  bool travelPast() { return estHeight() < blockHeight; }
  // When a height happened (or would): the past from the blocks' recorded
  // dates, interpolated, since early blocks came far faster than today's
  // pace (counting back at today's pace put block 100,000 in June 2010,
  // not December); the future at the measured pace, as the title does.
  // time_t is 32 bits on this core: it runs out in January 2038, and the
  // chain runs to 2140. Every date past the present goes through these two
  // in 64 bits (2140 through time_t came out as 2094, "in 0 years").
  struct Ymd { int y, m, d; };
  static Ymd civil64(int64_t t) {                 // days to civil, Hinnant
    int64_t z = (t >= 0 ? t / 86400 : (t - 86399) / 86400) + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    Ymd r;
    r.d = (int)(doy - (153 * mp + 2) / 5 + 1);
    r.m = (int)(mp < 10 ? mp + 3 : mp - 9);
    r.y = (int)(yoe + era * 400 + (r.m <= 2));
    return r;
  }
  int64_t heightUtc(long h) {
    int64_t now = (int64_t)time(nullptr);
    if (now < 1700000000) { tmElements_t tn; RTC.read(tn); now = (int64_t)makeTime(tn); }
    if (blockHeight <= 0) return now + (int64_t)((h - blockHeight) * 600.0);
    if (h >= blockHeight) {        // this epoch at its pace, then ten minutes
      long dh = h - blockHeight, toRet = 2016L - (blockHeight % 2016L);
      long atPace = dh < toRet ? dh : toRet;
      return now + (int64_t)(atPace * (double)avgBlockSec + (dh - atPace) * 600.0);
    }
    static const struct { long h; uint32_t t; } R[] = {
      {      0L, 1231006505UL },   // 3 Jan 2009
      {     170L, 1231731025UL },   // 12 Jan 2009 (the landmarks' own dates,
      {  57043L, 1274552191UL },   // 22 May 2010  so the picker names them
      {  74638L, 1281891957UL },   // 15 Aug 2010  in the right month)
      { 100000L, 1293623863UL },   // 29 Dec 2010
      { 210000L, 1354116278UL },   // 28 Nov 2012
      { 300000L, 1399703554UL },   // 10 May 2014
      { 420000L, 1468082773UL },   // 9 Jul 2016
      { 481824L, 1503539857UL },   // 24 Aug 2017
      { 500000L, 1513622125UL },   // 18 Dec 2017
      { 630000L, 1589225023UL },   // 11 May 2020
      { 700000L, 1631333672UL },   // 11 Sep 2021
      { 709632L, 1636866927UL },   // 14 Nov 2021
      { 840000L, 1713571767UL },   // 20 Apr 2024
    };
    const int NR = sizeof(R) / sizeof(R[0]);
    long loH = R[0].h; double loT = R[0].t, hiT = (double)now; long hiH = blockHeight;
    for (int i = 0; i < NR; i++) {
      if (R[i].h > blockHeight) break;
      if (R[i].h <= h) { loH = R[i].h; loT = R[i].t; }
      else { hiH = R[i].h; hiT = R[i].t; break; }
    }
    if (hiH <= loH) return (int64_t)loT;
    return (int64_t)(loT + (hiT - loT) * (double)(h - loH) / (double)(hiH - loH));
  }
  void travelMonYear(char *o, size_t n) {
    static const char *MON[12] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                  "JUL","AUG","SEP","OCT","NOV","DEC"};
    Ymd c = civil64(heightUtc(travelHeight));
    snprintf(o, n, "%s %d", MON[c.m - 1], c.y);
  }
  void priceTxt(char *o, size_t n, double v, const char *code) {
    if (v <= 0) { snprintf(o, n, "----"); return; }
    char m[20];
    if (v < 100) snprintf(m, 20, "%.2f", v); else fmtMoney(m, 20, v);
    snprintf(o, n, "%s %s", m, code);
  }
  void ratioTxt(char *o, size_t n, double r) {      // 188X, 4.7X
    snprintf(o, n, r < 100 ? "%.1fX" : "%.0fX", r);
  }
  double usdPerCur() {                              // face currency -> USD
    return (btcPrice > 0 && fxRate[0] > 0) ? fxRate[0] / btcPrice : 0;
  }

  // Landmarks: blocks worth travelling to. A few moments from the record,
  // every halving (computed), and the end of the subsidy. They mark HGT's
  // travel line and are a step in the Time Travel picker.
  struct Landmark { long h; const char *name; };
  // (a function's static: the core's C++ needs a class constexpr array
  // defined out of line, and this keeps it all in one place)
  static const Landmark *landmarks() {
    static const Landmark L[] = {
      {       0L, "GENESIS"          },
      {     170L, "THE FIRST SEND"   },   // Satoshi to Hal Finney, Jan 2009
      {   57043L, "PIZZA DAY"        },   // 10,000 BTC for two pizzas
      {   74638L, "THE OVERFLOW BUG" },   // 184 billion BTC, Aug 2010
      {  481824L, "SEGWIT"           },
      {  709632L, "TAPROOT"          },
      { 6930000L, "THE SUBSIDY ENDS" },   // reward 0 from here on
    };
    return L;
  }
  static const int N_LANDMARKS = 7;
  static const int LAST_HALVING = 32;   // 6,720,000: the reward becomes 1 sat
  // the name of a landmark at exactly h, into o; false if h is none
  bool landmarkName(long h, char *o, size_t n) {
    loadPeople();
    for (int i = 0; i < MAX_PEOPLE; i++)            // births are landmarks too
      if (pplBirth[i] && !bornBefore(i) && heightAtUtc(birthUtc(i)) == h) {
        snprintf(o, n, "%s IS BORN", pplName[i]); return true; }
    for (int i = 0; i < N_LANDMARKS; i++)
      if (landmarks()[i].h == h) { snprintf(o, n, "%s", landmarks()[i].name); return true; }
    if (h > 0 && h % 210000L == 0 && h / 210000L <= LAST_HALVING) {
      int k = (int)(h / 210000L);
      const char *suf = (k % 100 >= 11 && k % 100 <= 13) ? "TH"
                      : k % 10 == 1 ? "ST" : k % 10 == 2 ? "ND" : k % 10 == 3 ? "RD" : "TH";
      snprintf(o, n, "THE %d%s HALVING", k, suf);
      return true;
    }
    return false;
  }
  // the nearest landmark strictly after (dir > 0) or before h; -1 if none
  long landmarkNext(long h, int dir) {
    long best = -1;
    loadPeople();
    for (int i = 0; i < MAX_PEOPLE; i++) {
      if (!pplBirth[i] || bornBefore(i)) continue;
      long c = heightAtUtc(birthUtc(i));
      if (dir > 0 && c > h && (best < 0 || c < best)) best = c;
      if (dir < 0 && c < h && (best < 0 || c > best)) best = c;
    }
    for (int i = 0; i < N_LANDMARKS; i++) {
      long c = landmarks()[i].h;
      if (dir > 0 && c > h && (best < 0 || c < best)) best = c;
      if (dir < 0 && c < h && (best < 0 || c > best)) best = c;
    }
    long k = dir > 0 ? h / 210000L + 1 : (h - 1) / 210000L;
    if (h <= 0 && dir < 0) k = 0;
    if (k >= 1 && k <= LAST_HALVING) {
      long c = k * 210000L;
      if (dir > 0 && (best < 0 || c < best)) best = c;
      if (dir < 0 && (best < 0 || c > best)) best = c;
    }
    return best;
  }

  // HGT: where on the chain you are. Distance in blocks and years, the chain
  // from 2009 with you on it, and the landmarks either side of you.
  void drawHgtTravel() {
    display.setFont(NULL); display.setTextColor(fg());
    long th = estHeight(), now = blockHeight > 0 ? blockHeight : th;
    bool past = th < now;
    char a[40], g[16];
    grouped(g, 16, past ? now - th : th - now);
    snprintf(a, 40, "%s BLOCKS %s", g, past ? "AGO" : "AHEAD");
    lowAt(a, 8, 102);
    double yrs = fabs((double)(heightUtc(th) - heightUtc(now))) / (365.25 * 86400.0);
    snprintf(a, 40, yrs < 10 ? "~%.1f YRS" : "~%.0f YRS", yrs);
    lowRight(a, 102);

    long R = past ? now : th;                         // the line's far end
    if (R <= 0) R = 1;
    const int x0 = 10, x1 = 190, y = 131;
    auto X = [&](long hh) { return x0 + (int)((double)hh / R * (x1 - x0)); };
    int nx = X(now), tx = X(th);
    int solidTo = past ? tx : nx;                     // lived: solid
    display.drawFastHLine(x0, y, solidTo - x0, fg());
    display.drawFastHLine(x0, y + 1, solidTo - x0, fg());
    for (int x = solidTo; x <= x1; x += 3) display.drawPixel(x, y, fg());   // to come
    for (int i = 0; i < N_LANDMARKS; i++) {           // landmarks: short ticks
      if (landmarks()[i].h > R) break;
      int x = X(landmarks()[i].h);
      display.drawFastVLine(x, y - 2, 2, fg()); display.drawFastVLine(x, y + 2, 1, fg());
    }
    for (int k = 1; k <= LAST_HALVING && k * 210000L <= R; k++) {   // halvings: tall
      int x = X(k * 210000L);
      display.drawFastVLine(x, y - 4, 4, fg()); display.drawFastVLine(x, y + 2, 3, fg());
    }
    display.drawFastVLine(x0, y - 5, 12, fg());
    int yx = tx - 9; if (yx < 2) yx = 2; if (yx > 176) yx = 176;
    display.fillTriangle(tx - 4, y - 9, tx + 4, y - 9, tx, y - 4, fg());
    lowAt("YOU", yx, y - 18);
    lowAt("2009", x0 - 2, y + 7);
    if (past) {
      display.drawFastVLine(x1, y - 5, 12, fg());
      lowAt("NOW", x1 + 2 - 18, y + 7);
    } else {
      display.fillRect(nx - 1, y - 3, 3, 8, fg());
      int lx = nx - 9; if (lx > yx - 22) lx = yx - 22;
      lowAt("NOW", lx, y - 18);
      char w[12]; travelMonYear(w, 12);
      lowAt(w + 4, x1 + 2 - 24, y + 7);              // the arrival year
    }
    display.drawFastHLine(6, 148, 188, fg());

    if (landmarkName(th, a, 40)) {
      centerSmall("YOU ARE AT", 153);
      centerSmall(a, 164);
    } else {
      char nm[24];
      long pv = landmarkNext(th, -1), nxl = landmarkNext(th, 1);
      if (pv >= 0 && landmarkName(pv, nm, 24)) {
        grouped(g, 16, th - pv); snprintf(a, 40, "%s AFTER %s", g, nm); centerSmall(a, 153);
      }
      if (nxl >= 0 && landmarkName(nxl, nm, 24)) {
        grouped(g, 16, nxl - th); snprintf(a, 40, "%s BEFORE %s", g, nm); centerSmall(a, 164);
      }
    }
  }

  // PRC: the price history on a log scale, the trip marked
  void drawPrcTravel() {
    display.setFont(NULL); display.setTextColor(fg());
    bool past = travelPast();
    long th = estHeight(), nowH = blockHeight;
    double nowUsd = btcPrice > 0 ? btcPrice : 0;
    // all in the face's currency now (the past via its own old rate)
    double thenUsd = past ? pastPriceCur(th) : modelPrice(th);
    static const long AH[10] = {100000L, 210000L, 300000L, 420000L, 500000L,
                                550000L, 630000L, 700000L, 750000L, 840000L};
    long rightH = past ? nowH : th;
    double hiP = (past ? nowUsd : (thenUsd > nowUsd ? thenUsd : nowUsd));
    double loP = pastPriceCur(100000L); if (loP <= 0) loP = 0.2;   // 2010, here
    if (hiP <= loP || rightH <= 100000L) return;
    const int x0 = 8, y0 = 104, W = 184, h = 40;
    display.drawRect(x0 - 2, y0 - 2, W + 4, h + 4, fg());
    auto X = [&](long hh) { return x0 + 2 + (int)((double)(hh - 100000L) / (rightH - 100000L) * (W - 5)); };
    auto Y = [&](double pr) {
      if (pr < loP) pr = loP;
      return y0 + h - (int)((log10(pr) - log10(loP)) / (log10(hiP) - log10(loP)) * h);
    };
    int px = -1, py = -1;
    for (int i = 0; i < 10; i++) {                 // the record, then today
      if (AH[i] > nowH) break;
      int x = X(AH[i]), y = Y(pastPriceCur(AH[i]));
      if (px >= 0) display.drawLine(px, py, x, y, fg());
      px = x; py = y;
    }
    int nx = X(nowH), ny = Y(nowUsd);
    if (px >= 0) display.drawLine(px, py, nx, ny, fg());
    display.fillCircle(nx, ny, 2, fg());
    int mx = X(th), my = Y(thenUsd);
    if (past) {
      for (int yy = y0; yy < y0 + h; yy += 2) display.drawPixel(mx, yy, fg());
      display.fillCircle(mx, my, 2, fg());
    } else {                                      // the model: dotted, open
      for (int t = 0; t <= 20; t += 2) {
        int x = nx + (mx - nx) * t / 20, y = ny + (my - ny) * t / 20;
        display.drawPixel(x, y, fg()); display.drawPixel(x, y + 1, fg());
      }
      for (int yy = y0; yy < y0 + h; yy += 2) display.drawPixel(mx, yy, fg());
      display.drawCircle(mx, my, 2, fg());
    }
    char a[24], b[24];
    const char *code = CUR_CODES[curIdx];
    double nowShown  = btcPrice;
    double thenShown = thenUsd;
    if (past) {
      priceTxt(a, 24, thenShown, code); lowAt("THEN", 8, 152); lowAt(a, 44, 152);
      priceTxt(b, 24, nowShown, code);  lowAt("NOW", 8, 163);  lowAt(b, 44, 163);
      if (thenShown > 0) { ratioTxt(a, 24, nowShown / thenShown); lowRight(a, 152); lowRight("SINCE", 163); }
    } else {
      priceTxt(a, 24, nowShown, code);  lowAt("NOW", 8, 152);   lowAt(a, 44, 152);
      priceTxt(b, 24, thenShown, code); lowAt("MODEL", 8, 163); lowAt(b, 44, 163);
      if (nowShown > 0) { ratioTxt(a, 24, thenShown / nowShown); lowRight(a, 152); lowRight("IF IT HOLDS", 163); }
    }
  }

  // two log bars against each other, measured from half the smaller
  void twoBars(double a, double b, int y1, int y2, bool solidSecond, int textW) {
    double lo = (a < b ? a : b) / 2, hi = a > b ? a : b;
    if (lo <= 0 || hi <= lo) return;
    int bw = 194 - textW - 8 - 64; if (bw > 70) bw = 70; if (bw < 20) bw = 20;
    int wa = (int)(bw * (log10(a) - log10(lo)) / (log10(hi) - log10(lo)) + 0.5);
    int wb = (int)(bw * (log10(b) - log10(lo)) / (log10(hi) - log10(lo)) + 0.5);
    if (wa < 3) wa = 3; if (wb < 3) wb = 3;
    display.drawRect(64, y1, wa, 7, fg());
    if (solidSecond) display.fillRect(64, y2, wb, 7, fg());
    else display.drawRect(64, y2, wb, 7, fg());
  }

  // SAT: what a dollar bought then (or would buy), against today
  void drawSatTravel() {
    display.setFont(NULL); display.setTextColor(fg());
    bool past = travelPast();
    long th = estHeight();
    char when[12]; travelMonYear(when, 12);
    char a[34], g[16] = "";
    if (past) {
      char unit[20];
      double k = satUnit(unit, 20);
      double pp = pastPriceCur(th);
      double thenS = pp > 0 ? k * 1e8 / pp : 0;
      double nowS  = btcPrice > 0 ? k * 1e8 / btcPrice : 0;
      snprintf(a, 34, "WHAT %s BOUGHT", unit);
      lowAt(a, 8, 104);
      if (strlen(a) <= 22) lowRight("IN SATS", 104);
      lowAt(when, 8, 117);  grouped(g, 16, (long long)(thenS + 0.5)); lowRight(thenS > 0 ? g : "----", 117);
      int tw = strlen(g) * 6;
      lowAt("TODAY", 8, 129); grouped(g, 16, (long long)(nowS + 0.5)); lowRight(nowS > 0 ? g : "----", 129);
      if ((int)strlen(g) * 6 > tw) tw = strlen(g) * 6;
      if (thenS > 0 && nowS > 0) {
        twoBars(thenS, nowS, 117, 129, true, tw);
        display.drawFastHLine(6, 142, 188, fg());
        if (curIdx == 0) snprintf(a, 34, "SINCE THEN THE DOLLAR");
        else if (fxMarket(curIdx)) snprintf(a, 34, "SINCE THEN THE %s (MARKET)", CUR_CODES[curIdx]);
        else snprintf(a, 34, "SINCE THEN THE %s", CUR_CODES[curIdx]);
        centerSmall(a, 148);
        double lost = (1 - nowS / thenS) * 100;
        if (nowS < thenS) snprintf(a, 34, lost >= 99.99 ? "LOST %.4f%% IN BITCOIN"
                                                        : "LOST %.2f%% IN BITCOIN", lost);
        else              snprintf(a, 34, "BUYS %.0f%% MORE SATS", (nowS / thenS - 1) * 100);
        centerSmall(a, 160);
      }
    } else {
      double k = 1;                                  // 1, 10, ... 100,000
      while (btcPrice > 0 && k < 1e7 && k * 1e8 / btcPrice < 100) k *= 10;
      double nowS  = btcPrice > 0 ? k * 1e8 / btcPrice : 0;
      double m = modelPrice(th);
      double thenS = m > 0 ? k * 1e8 / m : 0;
      char unit[20], kg[16]; grouped(kg, 16, (long long)k);
      if (curIdx == 0 && k == 1) strcpy(unit, "$1");
      else snprintf(unit, 20, "%s %s", kg, CUR_CODES[curIdx]);
      snprintf(a, 34, "WHAT %s WOULD BUY", unit);
      lowAt(a, 8, 104);
      if (strlen(a) <= 22) lowRight("IN SATS", 104);   // room for both
      lowAt("TODAY", 8, 117); grouped(g, 16, (long long)(nowS + 0.5)); lowRight(nowS > 0 ? g : "----", 117);
      lowAt(when, 8, 129);
      if (thenS >= 1) { grouped(g, 16, (long long)(thenS + 0.5)); snprintf(a, 34, "~%s", g); }
      else if (thenS > 0) snprintf(a, 34, "~%.2f", thenS);
      else strcpy(a, "----");
      lowRight(a, 129);
      int tw = strlen(a) * 6; if ((int)strlen(g) * 6 > tw) tw = strlen(g) * 6;
      if (thenS > 0 && nowS > 0) {
        twoBars(nowS, thenS, 117, 129, true, tw);
        display.drawFastHLine(6, 142, 188, fg());
        if (curIdx == 0 && k == 1) snprintf(a, 34, "IF THE MODEL HOLDS, A DOLLAR");
        else snprintf(a, 34, "IF THE MODEL HOLDS, %s", unit);
        centerSmall(a, 148);
        snprintf(a, 34, "BUYS ~%.0f%% FEWER SATS", (1 - thenS / nowS) * 100);
        centerSmall(a, 160);
      }
    }
  }

  // CAP: the network then (or modelled) against now, and gold where known
  void drawCapTravel() {
    display.setFont(NULL); display.setTextColor(fg());
    bool past = travelPast();
    long th = estHeight();
    char t[34];
    if (past) {
      double thenB = supplyBTC(th) * pastPriceCur(th) / 1e9;   // face currency
      double nowB  = btcMcapB;
      double big = thenB > nowB ? thenB : nowB;
      lowAt("THEN", 8, 105); lowAt("NOW", 8, 119);
      display.drawRect(40, 105, 110, 8, fg()); display.drawRect(40, 119, 110, 8, fg());
      if (big > 0) {
        int a = (int)(110 * thenB / big + 0.5), b = (int)(110 * nowB / big + 0.5);
        display.fillRect(40, 105, a > 2 ? a : 3, 8, fg());
        display.fillRect(40, 119, b > 2 ? b : 3, 8, fg());
      }
      fmtTB(t, 34, thenB); lowRight(thenB > 0 ? t : "----", 105);
      fmtTB(t, 34, nowB);  lowRight(nowB > 0 ? t : "----", 119);
      display.drawFastHLine(6, 134, 188, fg());
      if (thenB > 0 && nowB > 0) {
        centerSmall("THE WHOLE NETWORK WAS", 141);
        if (thenB < nowB) snprintf(t, 34, "WORTH %.2f%% OF TODAY", thenB / nowB * 100);
        else              snprintf(t, 34, "WORTH %.1fX TODAY", thenB / nowB);
        centerSmall(t, 153);
      }
      centerSmall("GOLD THEN: NOT ON THE WATCH", 165);
    } else {
      double m = modelPrice(th);
      double thenB = m > 0 ? supplyBTC(th) * m / 1e9 : 0;
      double nowB  = btcMcapB;
      double cross = (fxRate[0] > 0 && fxRate[curIdx] > 0) ? fxRate[curIdx] / fxRate[0] : 0;
      double goldB = (goldMcapB > 0 && cross > 0) ? goldMcapB * cross : 0;
      double big = thenB; if (nowB > big) big = nowB; if (goldB > big) big = goldB;
      char yr[8]; { char w[12]; travelMonYear(w, 12); snprintf(yr, 8, "%s", w + 4); }
      lowAt("NOW", 8, 105); lowAt(yr, 8, 119); lowAt("GOLD", 8, 133);
      display.drawRect(40, 105, 110, 8, fg()); display.drawRect(40, 119, 110, 8, fg());
      display.drawRect(40, 133, 110, 8, fg());
      if (big > 0) {
        int a = (int)(110 * nowB / big + 0.5), b = (int)(110 * thenB / big + 0.5);
        display.fillRect(40, 105, a > 2 ? a : 3, 8, fg());
        for (int x = 40; x < 40 + b; x += 2) display.drawFastVLine(x, 119, 8, fg());   // modelled
        if (goldB > 0) display.fillRect(40, 133, (int)(110 * goldB / big + 0.5), 8, fg());
      }
      fmtTB(t, 34, nowB); lowRight(nowB > 0 ? t : "----", 105);
      if (thenB > 0) { char u[20]; fmtTB(u, 20, thenB); snprintf(t, 34, "~%s", u); lowRight(t, 119); }
      else lowRight("----", 119);
      if (goldB > 0) { fmtTB(t, 34, goldB); lowRight(t, 133); } else lowRight("----", 133);
      display.drawFastHLine(6, 147, 188, fg());
      if (thenB > 0 && goldB > 0) {
        if (thenB < goldB) snprintf(t, 34, "%.0f%% OF GOLD'S VALUE TODAY", thenB / goldB * 100);
        else               snprintf(t, 34, "%.1fX GOLD'S VALUE TODAY", thenB / goldB);
        centerSmall(t, 153);
      } else if (thenB > 0) centerSmall("GOLD: NO PRICE YET", 153);
      centerSmall("STAY HUMBLE, STACK SATS", 165);
    }
  }

  // sats as BTC, every digit that is there and none that is not: 3.125 BTC
  void btcExact(char *o, size_t n, long long sats) {
    if (sats <= 0) { snprintf(o, n, "0 BTC"); return; }
    char f[12]; snprintf(f, 12, "%08lld", sats % 100000000LL);
    int e = 7; while (e >= 0 && f[e] == '0') f[e--] = 0;
    char w[16]; grouped(w, 16, sats / 100000000LL);
    if (f[0]) snprintf(o, n, "%s.%s BTC", w, f); else snprintf(o, n, "%s BTC", w);
  }

  // FEE: fees cannot be known, then or ahead; the subsidy always can
  void drawFeeTravel() {
    display.setFont(NULL); display.setTextColor(fg());
    bool past = travelPast();
    long th = estHeight();
    int eraT = (int)(th / 210000L), eraN = (int)(blockHeight / 210000L);
    long long subSats = eraT < 64 ? (5000000000LL >> eraT) : 0;   // exact, as consensus has it
    char a[34], b[24];
    btcExact(a, 34, subSats);
    btcExact(b, 24, subSats * 144);
    if (past) {
      lowAt("FEES THEN", 8, 105); lowRight("UNKNOWN", 105);
      lowAt("THE WATCH KEEPS NO FEE HISTORY,", 8, 116);
      lowAt("SO IT DOES NOT PRETEND.", 8, 126);
      display.drawFastHLine(6, 137, 188, fg());
      lowAt("WHAT MINERS EARNED FOR SURE", 8, 142);
      lowAt("A BLOCK", 8, 153); lowRight(a, 153);
      lowAt("A DAY", 8, 163);   lowRight(b, 163);
    } else {
      lowAt("FEES THEN", 8, 105); lowRight("UNKNOWABLE", 105);
      lowAt("WHAT MINERS WILL EARN FOR SURE", 8, 116);
      lowAt("A BLOCK", 8, 127); lowRight(a, 127);
      lowAt("A DAY", 8, 137);   lowRight(b, 137);
      display.drawFastHLine(6, 148, 188, fg());
      int k = eraT - eraN;
      if (k <= 0) centerSmall("THE SAME SUBSIDY AS TODAY", 154);
      else {
        if (k == 1) snprintf(a, 34, "HALF OF TODAY: FEES MUST");
        else        snprintf(a, 34, "1/%lld OF TODAY: FEES MUST", 1LL << (k > 40 ? 40 : k));
        centerSmall(a, 154);
        centerSmall("CARRY MORE OF THE WORK", 164);
      }
    }
  }

  // WAL: today's sats at the trip's price (never "what you had": the watch
  // cannot know a past balance), in the open only when the balance is
  void drawWalTravel() {
    display.setFont(NULL); display.setTextColor(fg());
    if (isDemoWallet() || !haveWallet) {
      centerSmall("SET UP A WALLET TO SEE", 120);
      centerSmall("WHAT YOUR SATS WERE WORTH", 132);
      return;
    }
    bool hidden = walletPrivate && !walletPeek;
    bool past = travelPast();
    long th = estHeight();
    double btc = walletSats / 1e8;
    char when[12]; travelMonYear(when, 12);
    char a[34];
    if (past) {
      double thenV = btc * pastPriceCur(th), nowV = btc * btcPrice;
      lowAt("YOUR SATS, WORTH", 8, 104);
      lowAt(when, 8, 116);  priceTxt(a, 34, thenV, CUR_CODES[curIdx]); lowRight(hidden ? "-----" : a, 116);
      lowAt("TODAY", 8, 128); priceTxt(a, 34, nowV, CUR_CODES[curIdx]); lowRight(hidden ? "-----" : a, 128);
      display.drawFastHLine(6, 141, 188, fg());
      if (thenV > 0 && nowV > 0) {
        centerSmall("THE SAME SATS,", 147);
        const char *mon = curIdx == 0 ? "DOLLARS" : CUR_CODES[curIdx];
        if (nowV >= thenV) { char r[12]; ratioTxt(r, 12, nowV / thenV); snprintf(a, 34, "%s THE %s", r, mon); }
        else snprintf(a, 34, "%.0f%% OF THE %s", nowV / thenV * 100, mon);
        centerSmall(a, 159);
      }
    } else {
      double m = modelPrice(th);
      double thenV = btc * m, nowV = btc * btcPrice;
      lowAt("YOUR SATS, IF THE MODEL HOLDS", 8, 104);
      lowAt("TODAY", 8, 116); priceTxt(a, 34, nowV, CUR_CODES[curIdx]); lowRight(hidden ? "-----" : a, 116);
      lowAt(when, 8, 128);
      if (thenV > 0) { char v[30]; priceTxt(v, 30, thenV, CUR_CODES[curIdx]); snprintf(a, 34, "~%s", v); }
      else strcpy(a, "----");
      lowRight(hidden ? "-----" : a, 128);
      display.drawFastHLine(6, 141, 188, fg());
      if (thenV > 0 && nowV > 0) {
        char r[12]; ratioTxt(r, 12, thenV / nowV);
        snprintf(a, 34, "THE SAME SATS, ~%s", r);
        centerSmall(a, 147);
      }
      centerSmall("STAY HUMBLE, STACK SATS", 159);
    }
  }

  // MEM: a block as a small cube (front, top, shaded side); fill = how full
  void memCube(int x, int y, int w, int h, int dp, int fillPct, bool solid,
               const char *lab) {
    uint16_t F = fg(), Bg = bg();
    if (solid) {
      for (int i = 0; i <= dp; i++) {
        display.drawFastHLine(x + i, y - i, w + 1, F);
        display.drawFastVLine(x + w + i, y - i, h + 1, F);
      }
      display.fillRect(x, y, w + 1, h + 1, F);
      display.setTextColor(Bg);
      int lw = strlen(lab) * 6;
      lowAt(lab, x + (w + 1 - lw) / 2, y + 3);
      display.setTextColor(F);
      return;
    }
    display.drawLine(x, y, x + dp, y - dp, F);
    display.drawLine(x + w, y, x + w + dp, y - dp, F);
    display.drawFastHLine(x + dp, y - dp, w, F);
    display.drawLine(x + w, y + h, x + w + dp, y + h - dp, F);
    display.drawFastVLine(x + w + dp, y - dp, h, F);
    for (int i = 2; i < dp; i += 2) display.drawFastVLine(x + w + i, y - i + 2, h - 3, F);
    display.fillRect(x, y, w + 1, h + 1, Bg);
    display.drawRect(x, y, w + 1, h + 1, F);
    int fh = (h - 2) * fillPct / 100;
    for (int yy = y + h - 1; yy > y + h - 1 - fh; yy--)
      for (int xx = x + 1 + (yy & 1); xx < x + w; xx += 2) display.drawPixel(xx, yy, F);
    int lw = strlen(lab) * 6, lx = x + (w + 1 - lw) / 2, ly = y + 3;
    display.fillRect(lx - 1, ly - 1, lw + 1, 9, Bg);
    lowAt(lab, lx, ly);
  }
  // a block that does not exist yet: the same cube, dotted
  void memGhost(int x, int y, int w, int h, int dp, int fillPct, const char *lab) {
    uint16_t F = fg();
    for (int i = 0; i <= w; i += 2) {
      display.drawPixel(x + i, y, F); display.drawPixel(x + i, y + h, F);
      display.drawPixel(x + i + dp, y - dp, F);
    }
    for (int j = 0; j <= h; j += 2) {
      display.drawPixel(x, y + j, F); display.drawPixel(x + w, y + j, F);
      display.drawPixel(x + w + dp, y - dp + j, F);
    }
    for (int i = 0; i <= dp; i += 2) {
      display.drawPixel(x + i, y - i, F); display.drawPixel(x + w + i, y - i, F);
      display.drawPixel(x + w + i, y + h - i, F);
    }
    int fh = (h - 2) * fillPct / 100;
    for (int yy = y + h - 1; yy > y + h - 1 - fh; yy--)
      if (yy % 2 == 0)
        for (int xx = x + 2 + ((yy >> 1) & 1) * 2; xx < x + w - 1; xx += 4)
          display.drawPixel(xx, yy, F);
    int lw = strlen(lab) * 6, lx = x + (w + 1 - lw) / 2, ly = y + 3;
    display.fillRect(lx - 1, ly - 1, lw + 1, 9, bg());
    lowAt(lab, lx, ly);
  }

  // MEM: waiting on the left, NOW at x = 100, mined on the right, newest
  // beside the line: the mempool emptying into the chain, left to right
  // MEM's divider label: NOW while live; once the data is old, how old, since
  // the queue on the left is a snapshot from then, not the mempool now
  bool memOld() { return !inDocked && heightStaleMinutes() >= staleAfter(); }
  const char *memTitle() {
    static char t[16];
    if (!memOld()) return "NOW";
    long e = heightStaleMinutes();
    if (e < 60) snprintf(t, 16, "%ld MIN AGO", e);
    else        snprintf(t, 16, "%ldH AGO", e / 60 > 48 ? 48 : e / 60);
    return t;
  }
  void drawMemLower() {
    display.setFont(NULL); display.setTextColor(fg());
    // how long since the tip, against the ten minutes (bar: 0-20 min).
    // Old data: it is the last block SEEN; later ones almost surely exist
    lowAt(memOld() ? "LAST BLOCK SEEN" : "LAST BLOCK", 8, 103);
    long ago = -1;
    if (tipBlockTime > 0) {
      // the RTC keeps UTC plus the fixed gmtOffset (the zone is applied on
      // top only for display), the same conversion the halving date uses
      time_t nowUtc = makeTime(currentTime) - settings.gmtOffset;
      ago = ((long)nowUtc - (long)tipBlockTime) / 60;
      if (ago < 0) ago = 0;
    }
    char t[24];
    if (ago < 0) strcpy(t, "----");
    else if (ago < 120) snprintf(t, 24, "%ld MIN AGO", ago);
    else snprintf(t, 24, "%ldH AGO", ago / 60);
    lowRight(t, 103);
    int x0 = 8, W = 184, y = 113;
    display.drawRect(x0, y, W, 8, fg());
    if (ago > 0) display.fillRect(x0, y, (int)(W * (ago > 20 ? 20 : ago) / 20), 8, fg());
    display.drawFastVLine(x0 + W / 2, y - 3, 14, fg());

    const int w = 21, h = 19, dp = 4, gap = 6, cy = 142;
    int x = 100 - 7 - (3 * (w + dp) + 2 * gap);
    // waiting: furthest left, the next block nearest the line
    for (int i = 0; i < 3; i++) {
      int k = 2 - i;                               // projection index
      const char *top = k == 0 ? "NEXT" : k == 1 ? "+2" : "+3";
      lowAt(top, x + (w + dp - (int)strlen(top) * 6) / 2, cy - dp - 10);
      if (k < projN) {
        char f[6]; snprintf(f, 6, "%d", (int)(projFee[k] + 0.5f) < 1 ? 1 : (int)(projFee[k] + 0.5f));
        memGhost(x, cy, w, h, dp, projFill[k], f);
        char m[8]; snprintf(m, 8, "~%dM", (int)((k + 1) * avgBlockSec / 60 + 0.5f));
        lowAt(m, x + (w - (int)strlen(m) * 6) / 2 + 1, cy + h + 4);
      } else memGhost(x, cy, w, h, dp, 0, "-");
      x += w + dp + gap;
    }
    x -= gap; x += 7;
    for (int j = cy - dp - 12; j < 174; j += 2) display.drawPixel(x, j, fg());   // NOW
    x += 7;
    // mined: the newest beside the line
    for (int i = 0; i < 3; i++) {
      if (recH[i] > 0) {
        char hs[8]; snprintf(hs, 8, "%03ld", recH[i] % 1000);
        lowAt(hs, x + (w + dp - 18) / 2, cy - dp - 10);
        memCube(x, cy, w, h, dp, recFill[i], i == 0, recPool[i][0] ? recPool[i] : "?");
        if (recT[i] > 0 && recT[i + 1] > 0 && recT[i] >= recT[i + 1]) {
          char m[8]; snprintf(m, 8, "%luM", (unsigned long)((recT[i] - recT[i + 1] + 30) / 60));
          lowAt(m, x + (w - (int)strlen(m) * 6) / 2 + 1, cy + h + 4);
        }
      } else memGhost(x, cy, w, h, dp, 0, "-");
      if (i < 2) {
        int lx = x + w + dp + 1, ly = cy + h / 2;
        display.drawFastHLine(lx, ly, gap - 1, fg());
        display.drawFastHLine(lx, ly - 1, gap - 1, fg());
      }
      x += w + dp + gap;
    }
  }

  // round ticker: ring dial with quarter ticks + progress arc + % in center
  void drawSubdial(int cx, int cy, int r, float pct, const char *label) {
    display.drawCircle(cx, cy, r, fg());
    for (int a = 0; a < 360; a += 90) {          // quarter ticks, outside
      float rad = a * DEG_TO_RAD;
      display.drawLine(cx + cosf(rad)*(r+1), cy + sinf(rad)*(r+1),
                       cx + cosf(rad)*(r+3), cy + sinf(rad)*(r+3), fg());
    }
    // progress arc: thick ring segment from 12 o'clock, clockwise
    for (float a = -90; a < -90 + pct * 360.0f; a += 3) {
      float rad = a * DEG_TO_RAD;
      display.drawLine(cx + cosf(rad)*(r-5), cy + sinf(rad)*(r-5),
                       cx + cosf(rad)*(r-2), cy + sinf(rad)*(r-2), fg());
    }
    // percent, centered
    char p[4]; snprintf(p, 4, "%d", (int)(pct * 100));
    int16_t x1,y1; uint16_t w,h;
    display.setFont(NULL);
    display.getTextBounds(p,0,0,&x1,&y1,&w,&h);
    display.setCursor(cx - w/2, cy - 3);
    display.print(p);
    // label above the dial
    display.getTextBounds(label,0,0,&x1,&y1,&w,&h);
    display.setCursor(cx - w/2, cy - r - 12);
    display.print(label);
  }

  // ---- US daylight saving: 2nd Sunday of March -> 1st Sunday of Nov ----
  bool usDST(time_t localStd) {
    tmElements_t t; breakTime(localStd, t);
    if (t.Month < 3 || t.Month > 11) return false;
    if (t.Month > 3 && t.Month < 11) return true;
    tmElements_t f = t; f.Day = 1; f.Hour = 12; f.Minute = 0; f.Second = 0;
    breakTime(makeTime(f), f);                 // weekday of the 1st
    int firstSunday = 1 + ((8 - f.Wday) % 7);  // Wday: 1 = Sunday
    if (t.Month == 3)  return t.Day >= firstSunday + 7;  // 2nd Sunday on
    return t.Day < firstSunday;                          // 1st Sunday off
  }

  long tzOffsetSec(int idx, time_t utc) {
    if (idx == 0) return settings.gmtOffset;
    long off = TZ_OFF[idx];
    if ((idx == 1 || idx == 2) && usDST(utc + off)) off += 3600;  // spring fwd
    return off;
  }

  const char *tzName(int idx, time_t utc) {
    if (idx == 1) return usDST(utc + TZ_OFF[1]) ? "EDT" : "EST";
    if (idx == 2) return usDST(utc + TZ_OFF[2]) ? "PDT" : "PST";
    return TZ_NAMES[idx];
  }

  void buildValue(char *big) {   // buffer is 16
    // In travel, only the height-derived modes mean anything. A real price
    // beside a simulated height is exactly the confusion worth avoiding, so
    // these blank rather than showing yesterday's number.
    if (travelActive) {
      long th = estHeight();
      bool past = (th < blockHeight);
      double mp = past ? pastPriceCur(th) : modelPrice(th);
      switch (dispMode) {
        case M_PRICE:                       // modelled, not fetched
          if (mp <= 0) { strcpy(big, "----"); return; }
          snprintf(big, 16, "%.0f", mp);    // spelled out: 6,450,000, not 6.45
          return;
        case M_SATS:                        // sats per unit of fiat
          fmtSats(big, 16, mp);
          return;
        case M_MCAP: {                      // supply is exact, price is modelled
          if (mp <= 0) { strcpy(big, "----"); return; }
          fmtCap(big, 16, supplyBTC(th) * mp / 1e9);
          return;
        }
        case M_FEES:                        // no model, no pretending
          strcpy(big, "----"); return;
        default: break;                     // HGT, SUP, HLV, WAL unchanged
      }
    }
    switch (dispMode) {
    }
    switch (dispMode) {
      case M_HGHT:  snprintf(big, 12, "%ld", estHeight()); break;
      // MEM shows only what it has seen: the last fetched tip, frozen when
      // the data goes old (HGT is the one that dead-reckons ahead)
      case M_MEMP:  snprintf(big, 12, "%ld", travelActive ? estHeight() : blockHeight); break;
      case M_PRICE:
        // spelled out, whatever the currency: 12,760,000 rather than 12.76.
        // The comment this replaces said eight digits would not fit — true
        // before fitFont existed, but the face now steps the numerals down a
        // size instead of the number stepping down a scale.
        snprintf(big, 14, "%.0f", btcPrice);
        break;
      case M_SATS:  fmtSats(big, 12, btcPrice); break;
      case M_MCAP:  fmtCap(big, 14, btcMcapB); break;
      case M_FEES:  fmtFee(big, 14, fastFee); break;
      case M_SUPL: {
        // every coin issued, and every coin still to come. Both move by one
        // subsidy per block — one climbing, one falling — so the decimals are
        // the part worth watching.
        double issued = supplyBTC(estHeight());
        double rem = supplyCapBTC() - issued;
        if (cellSel[M_SUPL] % 2) {
          if (rem < 0.5e-8) snprintf(big, 14, "0");  // not 0.00: none at all
          else snprintf(big, 14, "%.2f", rem);
        }
        else                     snprintf(big, 14, "%.2f", issued);
        break;
      }
      case M_HALV: {
        // the DATE of the selected halving (UP walks future epochs),
        // projected at the chain's MEASURED pace, not idealized 10 min.
        // Big display carries the year; the unit line the day.
        if (estHeight() > 0 &&
            estHeight() / 210000L + 1 + cellSel[dispMode] % 6 > LAST_HALVING + 1) {
          snprintf(big, 12, "----");                // past the 33rd: none
        } else if (estHeight() > 0) {
          int t = cellSel[dispMode] % 6;
          int era = (int)(estHeight() / 210000L);
          long targetH = (long)(era + 1 + t) * 210000L;
          long blocksLeft = targetH - estHeight();
          if (blocksLeft < 0) blocksLeft = 0;
          // base the projection on WHEN YOU ARE, not on today. Travelling,
          // the blocks remaining are counted from the target height, so
          // adding them to the present would land the halving too early by
          // exactly the distance travelled.
          int64_t utc = (int64_t)makeTime(currentTime) - settings.gmtOffset;
          if (travelActive)                         // 64-bit: past 2038
            utc += (int64_t)((travelHeight - blockHeight) * (double)avgBlockSec);
          int64_t when = utc + (int64_t)(blocksLeft * (double)avgBlockSec);
          Ymd hd = civil64(when);
          Serial.printf("[hlv] est=%ld left=%ld avg=%.0f -> %d\n",
                        estHeight(), blocksLeft, avgBlockSec, hd.y);
          snprintf(big, 12, "%d", hd.y);
        } else snprintf(big, 12, "----");
        break; }
      case M_WALT: {
        if (walletPrivate && !walletPeek) { snprintf(big, 12, "------"); break; }
        if (!haveWallet) { snprintf(big, 12, "-----"); break; }
        if (walletSats < 1000000ULL) {
          // under 0.01 BTC, four decimals renders real sats as 0.0000
          // — a Bitcoin watch that can't show sats is broken. Small
          // balances are denominated in sats.
          snprintf(big, 12, "%llu", (unsigned long long)walletSats);
          break;
        }
        double btc = walletSats / 1e8;
        if (btc >= 10)      snprintf(big, 12, "%.2f", btc);
        else if (btc >= 1)  snprintf(big, 12, "%.3f", btc);
        else                snprintf(big, 12, "%.4f", btc);
        break; }
    }
  }

  // lightning settled: its OWN moment. LN sats live at your provider,
  // not in the zpub — flipping to the chain balance (which won't move)
  // would perfectly reenact the "paid but balance didn't update" bug,
  // as a feature. Different layer, different screen.
  void drawLnPaid(long sats) {
    display.fillScreen(bg());
    display.setTextColor(fg());
    display.setFont(NULL);
    { int16_t x1,y1; uint16_t w,h;
      display.getTextBounds("LIGHTNING PAID",0,0,&x1,&y1,&w,&h);
      display.setCursor((200-w)/2, 30); display.print("LIGHTNING PAID"); }
    char amt[12]; snprintf(amt, 12, "%ld", sats);
    centerText(amt, 100, &DSEG7_Classic_Bold_32);
    String dom(lnAddrBuf);
    int at = dom.indexOf('@');
    String where = "SATS SETTLED AT " + (at > 0 ? dom.substring(at + 1) : dom);
    where.toUpperCase();
    { int16_t x1,y1; uint16_t w,h;
      display.getTextBounds(where.c_str(),0,0,&x1,&y1,&w,&h);
      display.setCursor((200-w)/2, 124); display.print(where); }
    { int16_t x1,y1; uint16_t w,h;
      display.getTextBounds("(NOT IN THE CHAIN BALANCE)",0,0,&x1,&y1,&w,&h);
      display.setCursor((200-w)/2, 136);
      display.print("(NOT IN THE CHAIN BALANCE)"); }
    displayFull();   // full refresh: a celebration deserves
  }                           // a clean frame, not ghost soup

  // ---------------- LIGHTNING: invoice / address / offer QR ----------
  void drawLightning() {
    display.fillScreen(bg());       // ALWAYS clear: the cached-invoice
    display.setTextColor(fg());     // path skips the fetch branch that
    display.setFont(NULL);          // used to do this — QR over old frame
    // priority: BOLT12 offer (display-only) > fetched invoice
    // (settlement-watched) > plain address (any amount, no confirm)
    String payload, label, hint;
    if (LN_BOLT12[0]) {
      payload = LN_BOLT12; label = "BOLT12 OFFER";
      hint = "ANY AMOUNT (NO CONFIRM)";
    } else if (LN_REQUEST_SATS > 0 && lnAddrBuf[0]) {
      bool stale = !lnInvoice[0] ||
                   (wakeMin - lnFetchedWake > 10) ||
                   !lnVerify[0];   // no verify -> can't know it's been
                                   // paid; NEVER re-show one (the
                                   // "already processed" field bug)
      if (stale) {
        // no fake progress bar — two HTTP round-trips have no
        // measurable progress. Say what's happening instead.
        display.fillScreen(bg());
        display.setTextColor(fg());
        centerText("LN", 84, &DSEG7_Classic_Bold_32);
        { const char *m = "REQUESTING INVOICE...";
          int16_t x1,y1; uint16_t w,h;
          display.getTextBounds(m,0,0,&x1,&y1,&w,&h);
          display.setCursor((200-w)/2, 108); display.print(m); }
        String dom(lnAddrBuf);
        int at2 = dom.indexOf('@');
        if (at2 > 0) {
          String d = "FROM " + dom.substring(at2 + 1);
          d.toUpperCase();
          int16_t x1,y1; uint16_t w,h;
          display.getTextBounds(d.c_str(),0,0,&x1,&y1,&w,&h);
          display.setCursor((200-w)/2, 120); display.print(d.c_str());
        }
        display.display(true);
        fetchLnInvoice();
        display.fillScreen(bg());
        display.setTextColor(fg());
      }
      if (lnInvoice[0]) {
        payload = lnInvoice;
        label = String(LN_REQUEST_SATS) + " SATS INVOICE";
        hint = lnVerify[0] ? "BUZZ ON SETTLE"
                           : "NO CONFIRM";
        for (unsigned i = 0; i < payload.length(); i++)   // bolt11 is
          payload[i] = toupper(payload[i]);               // case-safe;
      } else {                                            // upper = smaller QR
        payload = "lightning:" + String(lnAddrBuf);
        label = "LIGHTNING ADDRESS";
        hint = "INVOICE FAILED - ANY AMOUNT";
      }
    } else if (lnAddrBuf[0]) {
      payload = "lightning:" + String(lnAddrBuf);
      label = "LIGHTNING ADDRESS";
      hint = "ANY AMOUNT (NO CONFIRM)";
    } else {
      centerText("NO LN", 100, &DSEG7_Classic_Bold_25);
      drawModeStrip();
      return;
    }
    // status row
    char clk[6];
    time_t lt = makeTime(currentTime);
    if (tzIndex > 0) {
      time_t utc = lt - settings.gmtOffset;
      lt = utc + tzOffsetSec(tzIndex, utc);
    }
    tmElements_t dt; breakTime(lt, dt);
    snprintf(clk, 6, "%02d:%02d", dt.Hour, dt.Minute);
    display.setCursor(10, 3); display.print(clk);
    { int16_t x1,y1; uint16_t w,h;
      display.getTextBounds("LIGHTNING",0,0,&x1,&y1,&w,&h);
      display.setCursor((200-w)/2, 3); display.print("LIGHTNING"); }
    // QR (invoices are long: the generator picks the version)
    QRCode qr;
    uint8_t ver = payload.length() > 220 ? 13 : (payload.length() > 120 ? 9 : 6);
    uint8_t buf[qrcode_getBufferSize(13)];
    if (qrcode_initText(&qr, buf, ver, ECC_LOW, payload.c_str()) == 0) {
      int scale = 150 / qr.size; if (scale < 1) scale = 1;
      int qs = qr.size * scale;
      int pad = 4 * scale;               // the QR spec's quiet zone is
      int ox = (200 - qs) / 2;           // FOUR modules — the old 4px
      int oy = 14 + pad;                 // border was half that at this
                                         // density, and scanners balked
      display.fillRect(ox - pad, oy - pad, qs + 2*pad, qs + 2*pad,
                       GxEPD_WHITE);
      for (int y = 0; y < qr.size; y++)
        for (int x = 0; x < qr.size; x++)
          if (qrcode_getModule(&qr, x, y))
            display.fillRect(ox + x*scale, oy + y*scale, scale, scale,
                             GxEPD_BLACK);
      // one caption line below the card, theme colors, clear of strip
      String cap = label + " - " + hint;
      if (cap.length() > 31) cap = cap.substring(0, 31);
      display.setCursor(10, oy + qs + pad + 4);
      display.print(cap);
    } else {
      centerText("QR ERR", 100, &DSEG7_Classic_Bold_25);
    }
    drawModeStrip();
    // FULL refresh: partials leave the LN... fetch frame ghosting
    // under the QR. One clean flash beats a haunted invoice.
    displayFull();
  }

  // ---------------- RECV: QR screen (v1 chrome: status row + strip) ----
  void drawReceive() {
    display.setFont(NULL);
    // No wallet, or the public demo one: say so instead of a QR. The demo's
    // keys are published, so a payment to it is a payment to anyone.
    if (isDemoWallet()) {
      centerText("SATOSHI'S WALLET", 20, NULL);
      centerSmall("THE GENESIS ADDRESS. ONLY", 40);
      centerSmall("SATOSHI'S KEY SPENDS FROM IT.", 52);
      centerSmall("TREAT ANY COINS SENT HERE", 68);
      centerSmall("AS LOST, OR A DONATION.", 80);
      // BitcoinTalk, 21 June 2010, quoted as he wrote it
      centerSmall("\"LOST COINS ONLY MAKE", 98);
      centerSmall("EVERYONE ELSE'S COINS WORTH", 110);
      centerSmall("SLIGHTLY MORE.\"", 122);
      centerSmall("- SATOSHI, 2010", 134);
      centerSmall("ADD YOURS: MENU > SETUP WALLET", 158);
      drawModeStrip();
      return;
    }
    String addr = walletAddr(recvIndex);   // always the first UNUSED
                                              // address; auto-advances once
                                              // the previous one is paid
    if (addr.length() == 0) {
      centerText("BAD ZPUB", 100, &DSEG7_Classic_Bold_25);
      drawModeStrip();
      return;
    }
    String up = addr; up.toUpperCase();   // alphanumeric-mode QR
    QRCode qr;
    uint8_t qrData[qrcode_getBufferSize(4)];
    qrcode_initText(&qr, qrData, 4, ECC_LOW, up.c_str());
    int scale = 132 / qr.size;
    int qs = qr.size * scale;
    int ox = (200 - qs) / 2, oy = 14;
    display.fillRect(ox - 4, oy - 4, qs + 8, qs + 8, GxEPD_WHITE);
    for (int y = 0; y < qr.size; y++)
      for (int x = 0; x < qr.size; x++)
        if (qrcode_getModule(&qr, x, y))
          display.fillRect(ox + x * scale, oy + y * scale, scale, scale, GxEPD_BLACK);
    display.setFont(NULL);
    display.setTextColor(fg());
    String shortAddr = addr.substring(0, 14) + ".." + addr.substring(addr.length() - 6);
    display.setCursor(14, oy + qs + 8);  display.print(shortAddr);
    display.setCursor(14, oy + qs + 17);
    display.print("RECV #"); display.print(recvIndex);
    display.print(lnConfigured() ? " UP:LN" : " UP:BAL");
    display.print(" DOWN:VERIFY");
    drawModeStrip();
  }

  // DSEG's '.' glyph has zero xAdvance at 18px — print one char at a
  // time and step the cursor past the dot manually
  // the box's second line on HGT and MEM, where two stops share it
  void boxLineBlock(char *o) {         // TXS: which block carried them
    if (blockHeight > 0) snprintf(o, 20, "IN BLOCK %ld", blockHeight);
  }
  void boxLineFees(char *o) {          // BTC RWD: the part that was fees
    if (blockHeight <= 0 || lastRewardBtc <= 0) return;
    double fees = lastRewardBtc - subsidySats((int)(blockHeight / 210000L)) / 1e8;
    if (fees >= 0) snprintf(o, 20, "INCL %.3f FEES", fees);
  }
  void boxLineRetarget(char *o) {      // RETARGET: blocks until it happens
    long h = estHeight(); if (h <= 0) return;
    long left = 2016 - h % 2016;
    snprintf(o, 20, left == 1 ? "%% IN 1 BLOCK" : "%% IN %ld BLOCKS", left);
  }
  void printCellValue(const char *s, int x, int y) {
    display.setFont(&DSEG7_Classic_Bold_14);
    display.setCursor(x, y);
    for (const char *p = s; *p; p++) {
      if (*p == '+') {
        // a real 7-segment display can't draw a plus, so neither can
        // DSEG — build one from pixels, sized to the 14px digits
        int cx = display.getCursorX();
        display.fillRect(cx + 4, y - 12, 2, 10, fg());
        display.fillRect(cx,     y - 8, 10, 2,  fg());
        display.setCursor(cx + 13, y);
        continue;
      }
      display.print(*p);
      if (*p == '.' || *p == ':')
        display.setCursor(display.getCursorX() + 4, y);
    }
  }

  // one battery model for the whole firmware: the glyph on the face and the
  // number in About read from this, so they cannot drift apart.
  // LiPo under a light load, approximated piecewise — the cell spends most
  // of its life on the flat part of the curve, so a straight 3.3-4.2 line
  // reports "half empty" while the watch is really near full.
  // 0 = running on the cell, 1 = charging, 2 = on charge and terminated.
  //
  // The status line is read with a pull-up, so an UNCONNECTED pin also reads
  // HIGH — indistinguishable from "charge complete". A watch plugged in at
  // 71% would then claim to be full instantly, which is what it did. So HIGH
  // only counts as terminated when the cell's voltage agrees; otherwise we
  // assume charging, which is the safe reading and true far more often.
  int chargeState() {
    static uint32_t usbSeenMs = 0;        // this boot only; millis() resets
    pinMode(USB_DET_PIN, INPUT);
    if (digitalRead(USB_DET_PIN) != 1) { usbSeenWake = 0; usbSeenMs = 0; return 0; }
    if (usbSeenWake == 0) { usbSeenWake = wakeMin ? wakeMin : 1; usbSeenMs = millis(); }
    pinMode(CHG_STAT_PIN, INPUT_PULLUP);
    delayMicroseconds(60);
    if (digitalRead(CHG_STAT_PIN) == LOW) return 1;      // asserted: charging

    // CHG_STAT high means "not actively charging", which is true both of a
    // finished charge and of the first moments after plugging in, before the
    // IC has started. The voltage cannot break the tie: with USB attached the
    // ADC reads the charger rail, not the resting cell, so it shows about
    // 4.2 V the instant the cable goes in whatever the cell holds. That is
    // why a watch at 60% jumped straight to 100%.
    //
    // So a terminated charge has to earn it: the cable has to have been in
    // for a while, and only then does the voltage get a say.
    // Two clocks, because neither runs in every case. wakeMin advances only
    // on minute wakes, which stop while the watch is docked; millis() runs
    // while awake but resets on every boot, and a flat cell means a boot.
    // Either reaching USB_SETTLE_MIN is enough. It was 25, which covered the
    // IC's start-up many times over and cost a reflash on a full, plugged-in
    // watch 25 minutes of a low figure: a charge IC starts within seconds of
    // power, so a status still high minutes later means it has finished (or
    // the cell is above its recharge threshold, which is full for us).
    uint32_t onWakes = (wakeMin >= usbSeenWake) ? wakeMin - usbSeenWake : 0;
    uint32_t onMs    = millis() - usbSeenMs;
    if (onWakes < USB_SETTLE_MIN &&
        onMs < USB_SETTLE_MIN * 60UL * 1000UL) return 1;           // too soon
    return (battVoltsTrue() >= 4.05f) ? 2 : 1;
  }

  // one ADC sample moves a few points on noise; the median of five drops
  // outliers without averaging a spike back in
  // The library scales a raw ADC count with a fixed constant. The ESP32 has
  // per-chip calibration burned in at the factory, reachable through
  // analogReadMilliVolts(), which is both more accurate and immune to the
  // reference drifting. Watchy has a known history here: batteries reading
  // far from full while the charge circuit knew otherwise.
  float rawVolts() {
  #if defined(ADC_PIN)
    uint32_t mv = analogReadMilliVolts(ADC_PIN);
    if (mv > 100) return (mv * 2.0f) / 1000.0f;   // 1:1 divider on the pin
  #endif
    return getBatteryVoltage();                   // fall back to the library
  }

  float batteryVolts() {
    float s[5];
    for (int i = 0; i < 5; i++) { s[i] = rawVolts(); delay(2); }
    for (int i = 1; i < 5; i++) {
      float k = s[i]; int j = i - 1;
      while (j >= 0 && s[j] > k) { s[j+1] = s[j]; j--; }
      s[j+1] = k;
    }
    return s[2];
  }

  // the calibrated reading: what the cell would measure if the board's
  // divider matched the library's constant. Everything that reasons about
  // real lithium voltages must use this, not the raw figure.
  float battVoltsTrue() {
    float f = (battFullV > 3.0f) ? battFullV : 4.20f;
    return batteryVolts() * (4.20f / f);
  }

  // the discharge curve, from a calibrated (true) cell voltage to 0..1
  // Measured, not assumed: two full runs from the 72-hour log (Oct 2026).
  // This cell holds 4.1-4.2 V for most of its life and then falls about six
  // times faster below 4.0 V: run 2 took 29 h from the charger to 3.97 V and
  // only 11 h more to 3.56 V (run 1: 10 h for the same stretch). The old
  // textbook curve called 3.97 V 80% when about a quarter of the time was
  // left, so "85% at bedtime" was nearly empty by morning. These points are
  // the share of the run's TIME still to come at each voltage, so the figure
  // reads as "how long have I got", which is what a wearer means by it.
  float curvePct(float v) {
    static const float V[] = { 4.20f, 4.16f, 4.12f, 4.08f, 4.03f, 3.97f,
                               3.90f, 3.80f, 3.70f, 3.60f, 3.50f, 3.36f };
    static const float P[] = { 1.00f, 0.85f, 0.72f, 0.55f, 0.43f, 0.32f,
                               0.27f, 0.20f, 0.13f, 0.08f, 0.04f, 0.00f };
    const int n = sizeof(V) / sizeof(V[0]);
    if (v >= V[0])     return 1.0f;
    if (v <= V[n - 1]) return 0.0f;
    for (int i = 1; i < n; i++)
      if (v >= V[i])
        return P[i] + (v - V[i]) * (P[i - 1] - P[i]) / (V[i - 1] - V[i]);
    return 0.0f;
  }

  // The newest on-cell sample in the hourly log, as a percentage, or -1. The
  // log lives in flash, so this survives the flat cell that wiped RTC memory:
  // a watch that ran dead and was plugged in still knows roughly where it was.
  int lastLoggedPct() {
    for (int i = 1; i <= VLOG_N; i++) {
      uint8_t x = vlog[(vlogIdx + VLOG_N - i) % VLOG_N];
      if (x == 0) return -1;
      if (x == VLOG_CHG) continue;      // this charge's own mark: the sample
                                        // before it is the cell, last seen
      float f = (battFullV > 3.0f) ? battFullV : 4.20f;
      return (int)(curvePct((x / 50.0f) * (4.20f / f)) * 100.0f + 0.5f);
    }
    return -1;
  }

  float chgRateCache = -1.0f;           // %/min, from flash, once per boot
  float chgRate() {
    if (chgRateCache < 0) {
      Preferences p; p.begin("btcchrono", true);
      chgRateCache = p.getFloat("chgRate", CHG_RATE_DEFAULT);
      p.end();
      if (chgRateCache < 0.2f || chgRateCache > CHG_RATE_MAX) chgRateCache = CHG_RATE_DEFAULT;
    }
    return chgRateCache;
  }

  uint32_t rtcNow() { tmElements_t t; RTC.read(t); return (uint32_t)makeTime(t); }

  bool battEstimated = false;           // the last figure was an estimate

  // The cable went in: if a long enough run on one profile just ended, it
  // says how long a full run lasts. Only runs of 12 h+ that used 40%+ count,
  // so a quick top-up or a reflash on the cable teaches nothing.
  void endRun(int endPct) {
    if (runStartT && runStartPct > 0 && endPct >= 0 && runProf == battProfile &&
        battProfile < 3) {
      float hrs = (rtcNow() - runStartT) / 3600.0f;
      float used = (runStartPct - endPct) / 100.0f;
      if (hrs >= 12.0f && used >= 0.40f) {
        float est = hrs / used;
        if (est >= 10.0f && est <= 200.0f) {
          float was = runH[battProfile];
          runH[battProfile] = 0.5f * was + 0.5f * est;
          Preferences p; p.begin("btcchrono", false);
          char k[6]; snprintf(k, 6, "runH%d", battProfile);
          p.putFloat(k, runH[battProfile]);
          p.end();
          Serial.printf("[run] %.0f h used %d%%: full run %.0f h, was %.0f, now %.0f\n",
                        hrs, (int)(used * 100), est, was, runH[battProfile]);
        }
      }
    }
    runStartT = 0;
  }

  // hours of battery left, or -1 on the cable (the cell is not being drained)
  // A stored RTC time that is in the future, or weeks old, is not a time:
  // drop it rather than compute from it.
  bool timeSane(uint32_t t) {
    uint32_t now = rtcNow();
    return t != 0 && t <= now && now - t < 30UL * 86400UL;
  }

  int hoursLeft() {
    if (chargeState() != 0) return -1;
    int p = batteryPctShown();
    if (p < 0) return -1;
    float h = runH[battProfile < 3 ? battProfile : 0];
    return (int)(p / 100.0f * h + 0.5f);
  }
  void fmtHours(char *out, size_t n, int h) {
    if (h < 1)       snprintf(out, n, "<1H");
    else if (h < 24) snprintf(out, n, "~%dH", h);
    else             snprintf(out, n, "~%dD %dH", h / 24, h % 24);
  }

  float batteryPct() {
    if (chargeState() == 2) return 1.0f;   // terminated: the guard now lives
                                           // inside chargeState itself                         // terminated AND the cell agrees
    return curvePct(battVoltsTrue());
  }

  // What About prints: rounded to 5%, because a voltage-derived reading is
  // not accurate to 1%, and never rising while on battery. The latch releases
  // as soon as a charger is attached so it can climb again.
  int batteryPctShown() {
    // Keep the worst reading the watch has ever taken, and the circumstances.
    // Four log lines could not tell a flat cell from a bad sample; this can.
    { float v = battVoltsTrue();
      if (v > 2.0f && v < minVoltSeen) {
        minVoltSeen = v; minVoltWake = wakeMin;
        minVoltFlags = (travelActive ? 1 : 0) | (inDocked ? 2 : 0);
      } }
    int raw     = (int)(batteryPct() * 100.0f + 0.5f);
    int rounded = ((raw + 2) / 5) * 5;
    if (rounded > 100) rounded = 100;
    if (rounded < 0)   rounded = 0;
    int cs = chargeState();
    battEstimated = false;
    if (chgFromT && !timeSane(chgFromT)) { chgFromT = 0; chgFromPct = -1; }
    if (runStartT && !timeSane(runStartT)) runStartT = 0;
    if (cs != 0 && chgFromT == 0) {
      // the cable just went in: note where the cell was, from the latch, else
      // from the hourly log in flash (a flat cell wipes the latch, not that)
      chgFromT = rtcNow();
      chgFromPct = (shownBattPct >= 0) ? shownBattPct : (int8_t)lastLoggedPct();
      chgLearned = false;
      endRun(chgFromPct);
      Serial.printf("[chg] start at %d%%\n", chgFromPct);
    }
    if (cs == 2) {                                     // the IC says finished
      // Learn the rate from a real charge: only one with a decent span, so
      // the taper at the end does not dominate, and blended, so one odd
      // charge moves it half way and no further.
      if (!chgLearned && chgFromPct >= 0 && chgFromPct <= 60 && chgFromT) {
        float mins = (rtcNow() - chgFromT) / 60.0f;
        if (mins >= 10.0f && mins <= 600.0f) {
          float r = (100 - chgFromPct) / mins;
          if (r >= 0.2f && r <= CHG_RATE_MAX) {
            chgRateCache = 0.5f * chgRate() + 0.5f * r;
            Preferences p; p.begin("btcchrono", false);
            p.putFloat("chgRate", chgRateCache); p.end();
            Serial.printf("[chg] full: %d%% -> 100%% in %.0f min, rate %.2f -> %.2f %%/min\n",
                          chgFromPct, mins, r, chgRateCache);
          }
        }
        chgLearned = true;
      }
      shownBattPct = 100; return 100;
    }
    if (cs == 1) {
      // Charging. The ADC is on the charger rail, so anything derived from it
      // describes the cable. Estimate instead, and say so (About shows ~).
      // Unknown start (a new watch, a wiped log): say so rather than guess.
      // Already finished once on this plug-in: this is the IC topping the
      // cell back up after it sagged, not a charge. Still full.
      if (chgLearned) { shownBattPct = 100; return 100; }
      if (chgFromPct < 0) return -1;
      float mins = (rtcNow() - chgFromT) / 60.0f;
      if (mins < 0) mins = 0;
      int est = chgFromPct + (int)(chgRate() * mins);
      est = ((est + 2) / 5) * 5;
      if (est > CHG_EST_CAP) est = CHG_EST_CAP;
      if (est < chgFromPct) est = chgFromPct;
      battEstimated = true;
      return est;
    }
    if (chgFromT != 0) {
      // Off the charger: measure the cell afresh. The latch only ever steps
      // down on the cell, so without this a charge cut short left it stuck at
      // the level from before the cable went in.
      chgFromT = 0; chgFromPct = -1;
      shownBattPct = rounded;
      runStartT = rtcNow(); runStartPct = rounded; runProf = battProfile;
      return rounded;
    }
    if (shownBattPct < 0) { shownBattPct = rounded; return rounded; }
    // A real cell does not fall twenty points between two wakes. If it seems
    // to, that is a bad reading, not a battery — step down by twenty and let
    // the next wake confirm it. A genuine collapse still gets there, one wake
    // later; a glitch never poisons the latch again.
    if (rounded < shownBattPct) {
      int step = shownBattPct - rounded;
      shownBattPct = (step > 20) ? shownBattPct - 20 : rounded;
    }
    return shownBattPct;
  }

  // Battery glyph. Three states, because "high voltage" and "charging" are
  // not the same thing: a cell on the charger reads 4.2 V whether it has
  // been there a minute or an hour.
  //   charging, not yet full : outline + bolt
  //   charging, full         : solid fill
  //   on battery             : proportional fill, '!' below 15%
  void drawBattery(int x, int y) {
    // One source of truth. This used to read batteryPct() while About read
    // batteryPctShown(), so on the charger the glyph filled from the rail
    // voltage while the number held the cell's last honest figure — a full
    // glyph above a 60% reading. Both now come from the same place: the
    // latched, five-percent-quantised value.
    int shown = batteryPctShown();
    bool known = (shown >= 0);           // -1: charging with nothing to go on
    float pct = known ? shown / 100.0f : 0.0f;
    int cs = chargeState();
    bool charging = (cs == 1);           // the bolt means charging, not plugged in

    display.drawRect(x, y, 14, 8, fg());
    display.fillRect(x + 14, y + 2, 2, 4, fg());   // terminal nub

    const int INNER_X = x + 2, INNER_W = 11;       // 100% fills edge to edge
    if (charging && pct < 0.99f) {
      display.drawFastVLine(x + 8, y + 2, 2, fg());   // hand-drawn bolt
      display.drawFastVLine(x + 7, y + 3, 2, fg());
      display.drawFastVLine(x + 6, y + 4, 2, fg());
      display.drawFastVLine(x + 9, y + 3, 1, fg());
      display.drawFastVLine(x + 8, y + 4, 2, fg());
      display.drawFastVLine(x + 7, y + 5, 1, fg());
      display.drawPixel(x + 5, y + 5, fg());
    } else {
      int fill = (int)(INNER_W * pct + 0.5f);
      if (pct >= 0.99f) fill = INNER_W;            // full means full
      if (fill > 0) display.fillRect(INNER_X, y + 2, fill, 4, fg());
    }

    if (cs == 0 && pct < 0.15f) {                  // low-battery mark
      display.drawFastVLine(x + 19, y, 5, fg());
      display.drawPixel(x + 19, y + 7, fg());
    }
  }


  void drawModeStrip() {
    // Nine cells across 190 px is 21.1 each, and integer division threw the
    // remainder away: the box came out 19 px while a three-character label at
    // a 2 px inset needs 20, so every label crossed its own border. Compute
    // each cell's edges from the full width so the remainder is spread, then
    // centre the label in whatever width that cell actually got.
    const int sy = 186, x0 = 5, span = 190;
    display.setFont(NULL);
    int shown = 0;                             // only the faces in the cycle
    for (int i = 0; i < NUM_MODES; i++) if (faceShown(i)) shown++;
    if (shown == 0) shown = 1;
    int c = 0;
    for (int i = 0; i < NUM_MODES; i++) {
      if (!faceShown(i)) continue;
      int a = x0 + (c * span) / shown;
      int b = x0 + ((c + 1) * span) / shown;
      c++;
      int w = b - a - 1;                       // 1 px gap between cells
      if (i == dispMode) {
        display.fillRect(a, sy, w, 13, fg());
        display.setTextColor(bg());
      } else {
        display.drawRect(a, sy, w, 13, fg());
        display.setTextColor(fg());
      }
      const char *lab = (shown >= 10) ? MODE_LABELS2[i] : MODE_LABELS[i];
      int tw = strlen(lab) * 6;                // built-in font: 6 px per char
      display.setCursor(a + (w - tw) / 2, sy + 3);
      display.print(lab);
    }
    display.setTextColor(fg());
  }

  // ---------------- face (v1 layout, 200x200) ----------------
  // boot splash, shown once per power-on (survives deep sleep via RTC flag)
  // A tiny Bitcoin B, 7 x 11, drawn in eight rectangles. It stands in for the
  // twelve o'clock marker on the plain face and in the boot animation: anyone
  // glancing sees a tick, anyone looking sees what the watch actually is.
  void drawTinyB(int x, int y) {
    display.fillRect(x + 1, y + 1, 2, 9, fg());        // stem
    display.fillRect(x + 1, y + 1, 4, 2, fg());        // upper bowl
    display.fillRect(x + 4, y + 2, 2, 2, fg());
    display.fillRect(x + 1, y + 4, 4, 2, fg());        // waist
    display.fillRect(x + 4, y + 6, 2, 2, fg());        // lower bowl
    display.fillRect(x + 1, y + 8, 4, 2, fg());
    display.fillRect(x + 2, y - 1, 1, 2, fg());        // serifs through it
    display.fillRect(x + 2, y + 10, 1, 2, fg());
  }

  // The last thing it does. E-paper holds its image with the power off, so
  // a watch that has run out does not have to freeze mid-face or brown out
  // halfway through a refresh — it can put up something it meant to say and
  // stop. The height is the one it last saw, which is the point: the chain
  // carried on without it.
  void restingScreen() {
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.drawRect(0, 0, 200, 200, GxEPD_WHITE);
    display.drawRect(2, 2, 196, 196, GxEPD_WHITE);

    display.setFont(NULL);
    display.setTextSize(2);
    const char *l1 = "BITCOIN";
    const char *l2 = "IS TIME";
    display.setCursor((200 - (int)strlen(l1) * 12) / 2, 72); display.print(l1);
    display.setCursor((200 - (int)strlen(l2) * 12) / 2, 96); display.print(l2);
    display.setTextSize(1);

    if (blockHeight > 0) {
      char h[20]; snprintf(h, 20, "%ld", blockHeight);
      char g[24]; int o = 0, len = strlen(h);
      for (int i = 0; i < len && o < 22; i++) {          // group the height
        g[o++] = h[i];
        int left = len - i - 1;
        if (left > 0 && left % 3 == 0 && o < 22) g[o++] = ',';
      }
      g[o] = 0;
      display.setCursor((200 - o * 6) / 2, 128); display.print(g);
      display.setCursor((200 - 15 * 6) / 2, 142);
      display.print("LAST BLOCK SEEN");
    }
    display.setCursor((200 - 16 * 6) / 2, 176); display.print("CHARGE TO RESUME");
    displayFull();            // full refresh: this one has to last
    Serial.println("[batt] resting");
    paintedDark = true;          // black: the face after a charge flips full
  }

  // Boot. E-paper cannot move, so nothing here pretends to: each partial
  // refresh is about a third of a second, and the watch uses them to ASSEMBLE
  // itself instead — the ring fills a sixth at a time, the marker at twelve
  // turns out to be a B, then the words arrive. Roughly three seconds, once
  // per power-on, and it ends on a full refresh so no ghosting is left.
  void bootRing() {
    const float CX = 100, CY = 100;
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    displayFull();                     // clean slate

    // the ring, ten marks at a time
    for (int step = 20; step <= 60; step += 20) {
      for (int m = step - 20; m < step; m++) {
        if (m == 0) continue;                   // twelve is spoken for
        float a = (m * 6.0f - 90.0f) * 0.01745329f;
        bool major = (m % 5 == 0);
        int x1 = (int)(CX + cosf(a) * 94), y1 = (int)(CY + sinf(a) * 94);
        int x2 = (int)(CX + cosf(a) * (major ? 84 : 89));
        int y2 = (int)(CY + sinf(a) * (major ? 84 : 89));
        display.drawLine(x1, y1, x2, y2, GxEPD_WHITE);
        if (major) display.drawLine(x1 + 1, y1, x2 + 1, y2, GxEPD_WHITE);
      }
      display.display(true);
    }

    // drawTinyB draws in fg(): on this always-black screen that has to be
    // white, or in the light theme the B was black on black and never showed
    { bool t = themeDark; themeDark = true; drawTinyB(97, 7); themeDark = t; }
    display.display(true);

    display.setFont(NULL);
    display.setTextSize(2);
    display.setCursor((200 - 7 * 12) / 2, 86);  display.print("BITCOIN");
    display.display(true);
    display.setCursor((200 - 7 * 12) / 2, 110); display.print("IS TIME");
    display.display(true);
    display.setTextSize(1);

    // No height here: RTC memory does not survive a power-on, so at boot
    // there is nothing to show yet — the line was always skipped and the
    // final refresh redrew the same frame, which read as the animation
    // stalling. It ends on the words instead.
    displayFull();                     // full refresh: no ghosts
    delay(600);
  }

  // Curiosities. A palindrome comes round about once a week, a repdigit once
  // in a lifetime. Neither means anything — that is rather the point. The
  // watch spends the rest of its time being strict about numbers, and this is
  // the one place it is allowed to enjoy one.
  //
  // It can only mark what it actually saw: the tip is read at fetch cadence,
  // so a block that came and went between fetches passes unremarked. Better
  // that than pretending to have witnessed it.
  const char *curiosity(long h) {
    if (h < 1000) return nullptr;
    char s[16]; snprintf(s, 16, "%ld", h);
    int n = strlen(s);
    bool pal = true, rep = true;
    for (int i = 0; i < n / 2; i++) if (s[i] != s[n-1-i]) { pal = false; break; }
    for (int i = 1; i < n; i++)     if (s[i] != s[0])     { rep = false; break; }
    // twin: the same digits twice over, 970,970 (or 98,9898... any even
    // length whose halves match). Not a palindrome: backwards it is 079,079
    bool twin = (n % 2 == 0) && strncmp(s, s + n / 2, n / 2) == 0;
    if (rep)  return "REPDIGIT BLOCK";
    if (pal)  return "PALINDROME BLOCK";
    if (twin) return "TWIN BLOCK";
    return nullptr;
  }

  // ---- the curiosity card: the date cards' layout (time line, big word, big
  // number, rule, two lines, double border), with brackets under the digits
  // that show what makes the number what it is. Staged in like the dates.
  int curioKind(long h) {                   // 0 palindrome, 1 twin, 2 repdigit
    const char *w = curiosity(h);
    if (!w) return -1;
    return w[0] == 'P' ? 0 : w[0] == 'T' ? 1 : 2;
  }
  // the next height of the same kind, after h (-1: none worth naming)
  long curioNext(int kind, long h) {
    char s[16]; snprintf(s, 16, "%ld", h);
    int n = strlen(s);
    if (kind == 2) {                        // d x 11..1, same length, then longer
      for (int len = n; len <= 9; len++) {
        long ones = 0; for (int i = 0; i < len; i++) ones = ones * 10 + 1;
        for (int dg = 1; dg <= 9; dg++) if (ones * dg > h) return ones * dg;
      }
      return -1;
    }
    if (kind == 1) {                        // the halves: 970|970 -> 971|971
      long p10 = 1;
      if (n % 2 == 0) {
        for (int i = 0; i < n / 2; i++) p10 *= 10;
        long half = h / p10;
        long c = half * p10 + half;
        if (c > h) return c;
        if (half + 1 < p10) return (half + 1) * p10 + (half + 1);
        n += 1;                             // 999|999: no more at this length
      }
      p10 = 1; for (int i = 0; i < (n + 1) / 2; i++) p10 *= 10;
      return (p10 / 10) * p10 + p10 / 10;   // 1000|1000
    }
    for (long c = h + 1; c < h + 20000; c++)  // palindromes: 1,100 apart at most
      if (curioKind(c) == 0) return c;      // at 6 digits, 11,000 at 7
    return -1;
  }
  // "IN ~17 HOURS": blocks at ten minutes
  void curioEta(char *o, size_t n, long blocks) {
    double m = blocks * 10.0;
    if (m < 120)              snprintf(o, n, "IN ~%.0f MIN", m);
    else if (m < 48 * 60)     snprintf(o, n, "IN ~%.0f HOURS", m / 60);
    else if (m < 730 * 1440)  snprintf(o, n, "IN ~%.0f DAYS", m / 1440);
    else                      snprintf(o, n, "IN ~%.0f YEARS", m / 525960);
  }
  // when it was mined, and how long ago in blocks: the tip and the last few
  // blocks carry their real timestamps; older ones are estimated (~)
  void curioWhen(char *o, size_t n, long h) {
    uint32_t t = 0; bool exact = false;
    if (h == blockHeight && tipBlockTime > 0) { t = tipBlockTime; exact = true; }
    for (int i = 0; i < 6 && !exact; i++)
      if (recH[i] == h && recT[i] > 0) { t = recT[i]; exact = true; }
    if (!t && tipBlockTime > 0)
      t = tipBlockTime - (uint32_t)((blockHeight - h) * (double)avgBlockSec);
    long ago = blockHeight - h; if (ago < 0) ago = 0;
    time_t nowUtc = makeTime(currentTime) - settings.gmtOffset;
    if (!t) { snprintf(o, n, ago ? "MINED %ld BLOCKS AGO" : "JUST MINED", ago); return; }
    if (ago == 0 && (long)nowUtc - (long)t < 15 * 60) { snprintf(o, n, "JUST MINED"); return; }
    tmElements_t a, b;
    breakTime(localFromUtc((time_t)t), a);
    breakTime(localFromUtc(nowUtc), b);
    char hm[8]; snprintf(hm, 8, "%s%02d:%02d", exact ? "" : "~", a.Hour, a.Minute);
    long dayDiff = (long)(localFromUtc(nowUtc) / 86400) - (long)(localFromUtc((time_t)t) / 86400);
    static const char *DN[8] = {"", "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    if (dayDiff <= 0) {
      if (ago == 1)    snprintf(o, n, "MINED %s, 1 BLOCK AGO", hm);
      else if (ago)    snprintf(o, n, "MINED %s, %ld BLOCKS AGO", hm, ago);
      else             snprintf(o, n, "MINED %s", hm);
    } else if (dayDiff == 1) snprintf(o, n, "MINED YESTERDAY %s", hm);
    else if (dayDiff < 7)    snprintf(o, n, "MINED %s %s", DN[a.Wday <= 7 ? a.Wday : 0], hm);
    else                     snprintf(o, n, "MINED %ld BLOCKS AGO", ago);
  }
  // the whole card at a stage: 0 the number, 1 half the brackets, 2 all of
  // them, 3 the title, 4 the details and the frame (what stays up)
  void drawCurioCard(long h, int stage) {
    static const char *T[3] = {"PALINDROME", "TWIN", "REPDIGIT"};
    static const char *L1[3] = {"READS THE SAME BOTH WAYS",
                                "THE SAME DIGITS, TWICE OVER",
                                "ONE DIGIT, ALL THE WAY ACROSS"};
    int kind = curioKind(h); if (kind < 0) kind = 0;
    char s[16], line[34];
    snprintf(s, 16, "%ld", h);
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setTextWrap(false);
    display.setFont(NULL); display.setTextSize(1);
    curioWhen(line, 34, h);
    centerSmall(line, 14);
    // the number, digits with pixel commas, and each digit's centre
    const GFXfont *F = strlen(s) > 6 ? &DSEG7_Classic_Bold_25 : &DSEG7_Classic_Bold_32;
    int n = strlen(s), cw = 6, w = 0, cx[12];
    for (int i = 0; i < n; i++) {
      w += F->glyph[s[i] - F->first].xAdvance;
      if ((n - 1 - i) % 3 == 0 && i < n - 1) w += cw;
    }
    int x = (200 - w) / 2, yb = 108;
    display.setFont(F);
    for (int i = 0; i < n; i++) {
      GFXglyph *g = F->glyph + (s[i] - F->first);
      char c[2] = {s[i], 0};
      display.setCursor(x, yb); display.print(c);
      cx[i] = x + g->xOffset + g->width / 2;
      x += g->xAdvance;
      if ((n - 1 - i) % 3 == 0 && i < n - 1) {
        display.fillRect(x + cw / 2 - 1, yb - 1, 3, 5, GxEPD_WHITE); x += cw;
      }
    }
    display.setFont(NULL);
    // the brackets: what pairs with what
    int y = 118, P = kind == 0 ? n / 2 : kind == 1 ? 2 : 1;
    int upto = stage >= 2 ? P : stage == 1 ? (P + 1) / 2 : 0;
    if (kind == 0) {                         // mirrors, outermost first
      for (int i = 0; i < P && i < upto; i++) {
        int dp = 4 + (P - 1 - i) * 5, a = cx[i], b = cx[n - 1 - i];
        display.fillRect(a - 1, y, 2, dp + 1, GxEPD_WHITE);
        display.fillRect(b - 1, y, 2, dp + 1, GxEPD_WHITE);
        display.fillRect(a - 1, y + dp - 1, b - a + 2, 2, GxEPD_WHITE);
      }
      if (n % 2 && upto >= P) display.fillCircle(cx[n / 2], y + 3, 2, GxEPD_WHITE);
    } else if (kind == 1) {                  // two halves, equal
      int hf = n / 2;
      for (int k = 0; k < 2 && k < upto; k++) {
        int a = cx[k * hf], b = cx[k * hf + hf - 1];
        display.fillRect(a - 1, y, 2, 7, GxEPD_WHITE);
        display.fillRect(b - 1, y, 2, 7, GxEPD_WHITE);
        display.fillRect(a - 1, y + 6, b - a + 2, 2, GxEPD_WHITE);
      }
      if (upto >= 2) {
        int m = (cx[hf - 1] + cx[hf]) / 2;
        display.fillRect(m - 3, y + 2, 7, 2, GxEPD_WHITE);
        display.fillRect(m - 3, y + 6, 7, 2, GxEPD_WHITE);
      }
    } else if (upto) {                       // every digit, one line
      for (int i = 0; i < n; i++) display.fillRect(cx[i] - 1, y, 2, 6, GxEPD_WHITE);
      display.fillRect(cx[0] - 1, y + 5, cx[n - 1] - cx[0] + 2, 2, GxEPD_WHITE);
    }
    if (stage >= 3) {
      display.setTextSize(3);
      display.setCursor((200 - (int)strlen(T[kind]) * 18) / 2, 34);
      display.print(T[kind]);
      display.setTextSize(1);
      centerSmall("BLOCK", 62);
    }
    if (stage >= 4) {
      display.drawFastHLine(30, 142, 140, GxEPD_WHITE);
      centerSmall(L1[kind], 154);
      long nx = curioNext(kind, h);
      if (nx > 0) {
        char g[16], eta[20];
        grouped(g, 16, nx);
        long from = blockHeight > 0 ? blockHeight : h;
        if (nx > from) { curioEta(eta, 20, nx - from); snprintf(line, 34, "NEXT: %s %s", g, eta); }
        else snprintf(line, 34, "NEXT: %s", g);
        centerSmall(line, 170);
      }
      display.drawRect(0, 0, 200, 200, GxEPD_WHITE);
      display.drawRect(2, 2, 196, 196, GxEPD_WHITE);
    }
  }

  void curiosityScreen(long h, const char *why) {
    // staged in, a partial at each step, like a find in an old handheld game:
    // the number, the brackets closing in, the name, then the card settles
    drawCurioCard(h, 0); display.display(true); delay(260);
    drawCurioCard(h, 1); display.display(true); delay(200);
    drawCurioCard(h, 2); display.display(true); delay(260);
    drawCurioCard(h, 3); display.display(true); delay(320);
    drawCurioCard(h, 4);
    displayFull();
    paintedDark = true;           // white on black, whatever the theme: the
                                  // face after it must not flip by partial
    buzz(35, 2);
    Serial.printf("[curio] %ld — %s\n", h, why);
  }

  // The finished card, whole: the end of the animation, and what an event
  // redraws while it waits to be seen (its time line is worked out again)
  void drawCurioFrame(long h, const char *why) {
    (void)why;
    if (h == 1000000L) drawMillionCard(2, 1000000L, 10);   // the milestone
    else drawCurioCard(h, 4);
  }

  // Bitcoin dates. The block curiosities are found in the chain; these are on
  // the calendar. Once each, on the first press of the day: a press means
  // someone is looking, which a timer wake at 4 am does not. The press that
  // brings the card up does only that, like the press that ends a rest.
  // Remembered in NVS, not RTC, so a reflash on the day does not replay it.
  struct BtcDate {
    uint8_t m, d; uint16_t y;
    const char *title, *l1, *l2;
    const char *l3;              // optional: a signature, say
  };
  // today's date on the wall clock, or nothing when the clock can't be trusted
  bool localDate(tmElements_t &dt) {
    time_t lt = makeTime(currentTime);
    if (tzIndex > 0) {
      time_t utc = lt - settings.gmtOffset;
      lt = utc + tzOffsetSec(tzIndex, utc);
    }
    breakTime(lt, dt);
    return tmYearToCalendar(dt.Year) >= 2026;   // an unset RTC reads 1970/2000
  }

  const BtcDate *dateCardDue(uint32_t &key) {
    static const BtcDate BTC_DATES[] = {
      { 1,  3, 2009, "GENESIS",    "BLOCK 0 WAS MINED",     "THE TIMES 03/JAN/2009" },
      { 1, 12, 2009, "FIRST SEND", "SATOSHI TO HAL FINNEY", "10 BTC IN BLOCK 170" },
      { 4, 23, 2011, "FAREWELL",   "\"I'VE MOVED ON TO OTHER THINGS", "IT'S IN GOOD HANDS.\"",
                                 "- SATOSHI NAKAMOTO" },
      { 5, 22, 2010, "PIZZA DAY",  "10,000 BTC FOR",        "TWO PIZZAS" },
      {10, 31, 2008, "WHITEPAPER", "A PEER-TO-PEER",        "ELECTRONIC CASH SYSTEM" },
      {12, 12, 2010, "LAST POST",  "SATOSHI'S LAST FORUM POST", "BITCOINTALK, V0.3.19" },
    };
    tmElements_t dt;
    if (!localDate(dt)) return nullptr;
    for (const BtcDate &b : BTC_DATES) {
      if (b.m != dt.Month || b.d != dt.Day) continue;
      key = tmYearToCalendar(dt.Year) * 10000UL + dt.Month * 100 + dt.Day;
      Preferences p; p.begin("btcchrono", true);
      uint32_t seen = p.getULong("dateSeen", 0);
      p.end();
      return seen == key ? nullptr : &b;
    }
    return nullptr;
  }

  // a big price in a few characters: 1.24B, 87.3T
  void fmtWorth(char *o, size_t n, double v) {
    if      (v >= 1e12) snprintf(o, n, "%.1fT", v / 1e12);
    else if (v >= 1e9)  snprintf(o, n, "%.2fB", v / 1e9);
    else if (v >= 1e6)  snprintf(o, n, "%.1fM", v / 1e6);
    else                snprintf(o, n, "%.0f",  v);
  }

  void dateCardScreen(const BtcDate &b, uint32_t key) {
    static const char *MON[] = {"", "JANUARY", "FEBRUARY", "MARCH", "APRIL",
      "MAY", "JUNE", "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER",
      "DECEMBER"};
    int years = key / 10000 - b.y;
    char s[34];
    guiState = APP_STATE;
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setFont(NULL);

    // 1. the date it happened
    snprintf(s, 34, "%d %s %d", b.d, MON[b.m], b.y);
    centerSmall(s, 14);
    display.display(true);
    delay(240);

    // 2. what happened
    display.setTextSize(3);
    display.setCursor((200 - (int)strlen(b.title) * 18) / 2, 34);
    display.print(b.title);
    display.setTextSize(1);
    display.display(true);
    delay(240);

    // 3. how long ago
    snprintf(s, 34, "%d", years);
    centerText(s, 108, &DSEG7_Classic_Bold_32);
    display.setFont(NULL);
    centerSmall(years == 1 ? "YEAR AGO TODAY" : "YEARS AGO TODAY", 118);
    display.display(true);
    delay(240);

    // 4. the detail, and the clean frame that stays up
    display.drawFastHLine(30, 138, 140, GxEPD_WHITE);
    centerSmall(b.l1, 150);
    centerSmall(b.l2, 162);
    if (b.l3) centerSmall(b.l3, 178);
    else if (b.m == 5 && b.d == 22 && btcPrice > 0) {   // the pizzas, priced live
      char w[16]; fmtWorth(w, 16, 10000.0 * btcPrice);
      snprintf(s, 34, "NOW %s %s", w, CUR_CODES[curIdx]);
      centerSmall(s, 178);
    }
    display.drawRect(0, 0, 200, 200, GxEPD_WHITE);
    display.drawRect(2, 2, 196, 196, GxEPD_WHITE);
    displayFull();
    paintedDark = true;            // always black: the face after flips full
    buzz(35, 2);
    Serial.printf("[date] %s, %d years\n", b.title, years);

    Preferences p; p.begin("btcchrono", false);
    p.putULong("dateSeen", key);
    p.end();

    // holds until a press, or a minute
    pinMode(BACK_BTN_PIN, INPUT); pinMode(MENU_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);
    unsigned long t0 = millis();
    while (millis() - t0 < 60000) {
      if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE ||
          digitalRead(MENU_BTN_PIN) == BTN_ACTIVE ||
          digitalRead(UP_BTN_PIN)   == BTN_ACTIVE ||
          digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) { waitAllRelease(); break; }
      delay(50);
    }
    guiState = WATCHFACE_STATE;
  }

  // true when a card went up (and the face after it): the press is spent
  bool maybeDateCard() {
    if (maybeBirthdayCard()) return true;
    uint32_t key = 0;
    const BtcDate *b = dateCardDue(key);
    if (!b) return false;
    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);
    waitAllRelease();
    dateCardScreen(*b, key);
    RTC.read(currentTime);
    showFaceFull();
    return true;
  }

  // There was a clearPanel() here: a full inverse refresh before the face,
  // to scrub the residue a black menu left on a white panel. It went when the
  // menu started following the theme — with both in the same palette there is
  // no full-panel inversion to leave residue, and the face's own full refresh
  // on exit is enough. One flash instead of two.

  // ---------------- startup sequences ----------------
  // One per true boot (power-on, reflash, crash), picked at random, so the
  // watch introduces itself a little differently each time. They share the ring's
  // rules: e-paper does not move, so each one ASSEMBLES a picture in partial
  // refreshes (about a third of a second each), lasts about three seconds,
  // and ends on a full refresh so nothing ghosts. None of them can use live
  // data: RTC memory is empty at a cold boot. Everything shown is either
  // timeless or computed here and now.
  static const int BOOT_SEQS = 4;

  // At random, but never the one shown last time: true randomness repeats
  // itself one boot in four, and a repeat reads as a watch that is stuck.
  // esp_random() is the chip's hardware generator. Except: the first boot of
  // a new firmware always opens with GENESIS, the best of them. "New" is the
  // build's own timestamp differing from the one stored last boot, so a
  // crash or a power cycle on the same firmware stays random.
  void bootAnimation() {
    Preferences p;
    p.begin("btcchrono", false);
    uint8_t last = p.getUChar("bootLast", 0xFF);  // 0xFF: none yet
    const char *build = __DATE__ " " __TIME__;
    bool newFirmware = (p.getString("bootFw", "") != String(build));
    uint8_t pick;
    uint8_t chosen = p.getUChar("bootPick", 0);    // Settings > Start Screen
    if (newFirmware) p.putString("bootFw", build);
    if (chosen >= 1 && chosen <= BOOT_SEQS) {
      pick = chosen - 1;                           // the wearer's choice
    } else if (newFirmware) {
      pick = 1;                                    // GENESIS
    } else if (last >= BOOT_SEQS) pick = esp_random() % BOOT_SEQS;
    else {
      pick = esp_random() % (BOOT_SEQS - 1);       // one of the other three
      if (pick >= last) pick++;
    }
    p.putUChar("bootLast", pick);
    p.end();
    Serial.printf("[boot] sequence %u (last %u)%s\n", pick, last,
                  newFirmware ? " - new firmware" : "");
    switch (pick) {
      case 0:  bootSegmentTest(); break;
      case 1:  bootGenesis();     break;
      case 2:  bootHalvings();    break;
      default: bootRing();        break;
    }
  }

  // white on black, like the ring: the boot does not follow the theme
  void bootClear() {
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setFont(NULL);
    display.setTextSize(1);
  }

  // A line along the bottom of whatever boot screen is up, for the wait
  // that follows it. Every sequence keeps y 184-199 clear for this (the
  // lowest boot text, SEGMENT TEST's BITCOIN IS TIME, ends at 183).
  void bootStatus(const char *s) {
    display.fillRect(0, 184, 200, 16, GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    bootCenter(s, 186, 1);
    display.display(true);
  }

  // SYNCING TO THE TIP: the first fetch after a boot, step by step. A
  // restart has no data, and that fetch (WiFi, then every source: 10-20 s)
  // used to be one line under the boot frame that sat there looking frozen.
  // Here each step ticks as it lands, and the last is the height itself:
  // the moment the watch knows where it is. A step that fails says so, and
  // where, instead of a face of dashes. Black like every boot sequence, so
  // it is all partial refreshes; the face after it is the full one. Only
  // ever up for a boot sync: ordinary fetches never call it (syncOn).
  bool syncOn = false;
  enum { SY_WIFI, SY_TIME, SY_PRICE, SY_TIP, SY_ROWS };
  static const int SY_Y0 = 64, SY_DY = 26;

  void syncBegin() {
    static const char *L[SY_ROWS] = {"WIFI", "TIME", "PRICE", "TIP"};
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    bootCenter("SYNCING", 12, 2);
    bootCenter("TO THE TIP", 32, 1);
    display.drawFastHLine(12, 48, 176, GxEPD_WHITE);
    for (int i = 0; i < SY_ROWS; i++) {
      display.setTextSize(1);
      display.setCursor(12, SY_Y0 + i * SY_DY);
      display.print(L[i]);
    }
    display.drawFastHLine(12, SY_Y0 + SY_ROWS * SY_DY - 6, 176, GxEPD_WHITE);
    display.display(true);
    syncOn = true;
    syncStep(SY_WIFI, "SEARCHING", 0);
  }

  // state: 0 working on it, 1 done, 2 failed
  void syncStep(int row, const char *val, int state) {
    if (!syncOn) return;
    int y = SY_Y0 + row * SY_DY;
    display.fillRect(52, y - 4, 140, 16, GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setFont(NULL); display.setTextSize(1);
    display.setCursor(56, y);
    display.print(val);
    if (state == 0) {                                  // three dots: working
      for (int k = 0; k < 3; k++) display.fillRect(172 + k * 5, y + 5, 2, 2, GxEPD_WHITE);
    } else if (state == 1) {                           // a tick
      for (int t = 0; t < 2; t++) {
        display.drawLine(174, y + 3 + t, 177, y + 6 + t, GxEPD_WHITE);
        display.drawLine(177, y + 6 + t, 184, y - 1 + t, GxEPD_WHITE);
      }
    } else {                                           // a cross
      for (int t = 0; t < 2; t++) {
        display.drawLine(175 + t, y - 1, 183 + t, y + 7, GxEPD_WHITE);
        display.drawLine(183 + t, y - 1, 175 + t, y + 7, GxEPD_WHITE);
      }
    }
    display.display(true);
  }

  // 970962 -> "970,962"
  void grouped(char *o, size_t n, long long v) {
    char d[24]; snprintf(d, 24, "%lld", v);
    int len = strlen(d), j = 0;
    for (int i = 0; i < len && j < (int)n - 1; i++) {
      if (i > 0 && (len - i) % 3 == 0 && j < (int)n - 2) o[j++] = ',';
      o[j++] = d[i];
    }
    o[j] = 0;
  }

  // the steps the fetch did not reach, and a moment to read it
  void syncEnd(bool ok) {
    if (!syncOn) return;
    bootCenter(ok ? "TICK TOCK NEXT BLOCK" : "WILL TRY AGAIN SHORTLY", 176, 1);
    display.display(true);
    delay(ok ? 900 : 2500);
    syncOn = false;
  }

  void bootCenter(const char *s, int y, int size) {     // built-in font
    display.setFont(NULL);
    display.setTextSize(size);
    display.setCursor((200 - (int)strlen(s) * 6 * size) / 2, y);
    display.print(s);
    display.setTextSize(1);
  }

  // __DATE__ is "Oct  7 2026": the day the firmware was built, as a stamp
  void buildStamp(char *out, int n) {
    const char *d = __DATE__;
    char mon[4] = { (char)toupper(d[0]), (char)toupper(d[1]), (char)toupper(d[2]), 0 };
    int day = atoi(d + 4);
    snprintf(out, n, "BUILT %02d %s %s", day, mon, d + 7);
  }

  // SEGMENT TEST. What the first digital watches did at power-on: light
  // every segment so you can see none is dead, then show who they are.
  void bootSegmentTest() {
    bootClear();
    displayFull();                       // clean slate

    // everything on: the clock, every indicator, every cell, every digit
    display.setFont(&DSEG7_Classic_Bold_14);
    display.setCursor(6, 17); display.print("88:88");
    display.drawRect(64, 6, 14, 8, GxEPD_WHITE);  // battery, full
    display.fillRect(78, 8, 2, 4, GxEPD_WHITE);
    display.fillRect(66, 8, 11, 4, GxEPD_WHITE);
    display.fillRect(150, 4, 44, 11, GxEPD_WHITE);   // the tag, lit solid
    { bool t = themeDark; themeDark = true;         // and the alarm bell
      drawBell(140, 5); themeDark = t; }
    centerText("88:88", 92, &DSEG7_Classic_Bold_32);
    centerText("888888", 138, &DSEG7_Classic_Bold_25);
    for (int i = 0; i < 9; i++) {                 // the mode strip, all on
      int a = 5 + (i * 190) / 9, b = 5 + ((i + 1) * 190) / 9;
      display.fillRect(a, 186, b - a - 1, 13, GxEPD_WHITE);
    }
    display.display(true);
    delay(250);

    // then only the middle segment of each digit: the classic dash pass
    display.fillScreen(GxEPD_BLACK);
    display.setFont(&DSEG7_Classic_Bold_14);
    display.setCursor(6, 17); display.print("--:--");
    centerText("--:--", 92, &DSEG7_Classic_Bold_32);
    centerText("------", 138, &DSEG7_Classic_Bold_25);
    for (int i = 0; i < 9; i++) {
      int a = 5 + (i * 190) / 9, b = 5 + ((i + 1) * 190) / 9;
      display.drawRect(a, 186, b - a - 1, 13, GxEPD_WHITE);
    }
    display.display(true);
    delay(150);

    // and who it is
    bootClear();
    bootCenter("BWATCH", 64, 3);
    display.drawFastHLine(40, 96, 120, GxEPD_WHITE);
    bootCenter("SEGMENT TEST OK", 106, 1);
    display.display(true);
    char b[32]; buildStamp(b, sizeof(b));
    bootCenter(b, 124, 1);
    bootCenter("BITCOIN IS TIME", 176, 1);
    display.display(true);

    displayFull();                       // full refresh: no ghosts
    delay(600);
  }

  // GENESIS. The watch re-mines block 0: the real 80-byte genesis header,
  // double SHA-256, for the last five nonces before Satoshi's. The first
  // four come out ordinary; the fifth comes out with ten leading zeros,
  // because it does. Nothing here is a picture of a hash.
  void bootGenesis() {
    bootClear();
    displayFull();

    // version 1 | previous block: none | merkle root | time | bits | nonce
    static const uint8_t HDR[76] = {
      0x01,0x00,0x00,0x00,
      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      0x3b,0xa3,0xed,0xfd,0x7a,0x7b,0x12,0xb2,0x7a,0xc7,0x2c,0x3e,0x67,0x76,0x8f,0x61,
      0x7f,0xc8,0x1b,0xc3,0x88,0x8a,0x51,0x32,0x3a,0x9f,0xb8,0xaa,0x4b,0x1e,0x5e,0x4a,
      0x29,0xab,0x5f,0x49,                        // 2009-01-03 18:15:05 UTC
      0xff,0xff,0x00,0x1d };                      // bits 0x1d00ffff
    const uint32_t NONCE = 2083236893UL;

    // first, that this is a watch starting up, and where it starts from:
    // the same card SEGMENT TEST ends on, with the line that makes the
    // mining that follows make sense
    bootCenter("BWATCH", 64, 3);
    display.drawFastHLine(40, 96, 120, GxEPD_WHITE);
    bootCenter("STARTING FROM BLOCK 0", 106, 1);
    display.display(true);
    delay(600);
    display.fillScreen(GxEPD_BLACK);

    bootCenter("BLOCK 0", 8, 2);
    bootCenter("03 JAN 2009", 30, 1);
    bootCenter("MINING THE GENESIS HEADER", 46, 1);
    display.drawFastHLine(10, 58, 180, GxEPD_WHITE);
    display.display(true);
    delay(200);                       // read the title before the hashing

    uint8_t hdr[80];
    memcpy(hdr, HDR, 76);
    for (int k = 4; k >= 0; k--) {
      uint32_t nonce = NONCE - k;
      hdr[76] = nonce & 0xff;         hdr[77] = (nonce >> 8) & 0xff;
      hdr[78] = (nonce >> 16) & 0xff; hdr[79] = (nonce >> 24) & 0xff;
      uint8_t h[32];
      doubleSha(hdr, 80, h);
      char hex[13];                               // shown big-endian, as
      for (int i = 0; i < 6; i++)                 // block hashes always are
        snprintf(hex + i * 2, 3, "%02x", h[31 - i]);
      char line[34];
      snprintf(line, sizeof(line), "%10lu %s", (unsigned long)nonce, hex);
      int y = 66 + (4 - k) * 14;
      if (k == 0) {                               // the one that worked
        display.fillRect(10, y - 3, 180, 13, GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
      }
      display.setFont(NULL);
      display.setCursor((200 - (int)strlen(line) * 6) / 2, y);
      display.print(line);
      display.setTextColor(GxEPD_WHITE);
      display.display(true);
      // paced so each try lands: it raced through in ~2 s and then sat on
      // the last frame through the first fetch, which read as frozen
      delay(k == 0 ? 350 : 175);
    }

    display.drawFastHLine(10, 140, 180, GxEPD_WHITE);
    bootCenter("BLOCK 0 FOUND", 148, 1);
    bootCenter("THE TIMES 03/JAN/2009", 162, 1);   // up a little: y 186+
    bootCenter("CHANCELLOR ON BRINK...", 173, 1);  // is the status line's
    display.display(true);

    displayFull();
    delay(600);
  }

  // HALVINGS. Bitcoin's own calendar: every 210,000 blocks the subsidy
  // halves, about every four years. The counter walks the epochs so far and
  // the next one; the bar is all 33 epochs that will ever pay a subsidy.
  void bootHalvings() {
    // drawGrouped draws its separators in fg(): white on this black, whatever
    // the face's theme is, and the theme comes back after
    bool savedTheme = themeDark;
    themeDark = true;
    bootClear();
    displayFull();

    static const char *YEAR[6]    = { "2009", "2012", "2016", "2020", "2024", "~2028" };
    static const char *SUBSIDY[6] = { "50", "25", "12.5", "6.25", "3.125", "1.5625" };
    bootCenter("HALVINGS", 10, 2);
    // 33 epochs across the bottom, 4 px each with a 1 px gap, clear of the
    // bezel on both sides
    const int CW = 4, CP = 5, BW = 33 * CP - 1;
    const int BX = (200 - BW) / 2, BY = 160;
    for (int e = 0; e < 33; e++)
      display.drawRect(BX + e * CP, BY, CW, 9, GxEPD_WHITE);
    display.setCursor(BX, BY + 13);                 display.print("EPOCH 1");
    display.setCursor(BX + BW - 2 * 6 + 1, BY + 13); display.print("33");

    for (int i = 0; i < 6; i++) {
      display.fillRect(0, 34, 200, 120, GxEPD_BLACK);   // the number area
      char h[12]; snprintf(h, sizeof(h), "%ld", (long)i * 210000L);
      drawGrouped(h, 84, fitFont(h));
      display.setFont(NULL);
      char l[28];
      snprintf(l, sizeof(l), "BLOCK HEIGHT - %s", YEAR[i]);
      bootCenter(l, 98, 1);
      snprintf(l, sizeof(l), "%s BTC PER BLOCK", SUBSIDY[i]);
      bootCenter(l, 116, 1);
      if (i == 5) bootCenter("NEXT", 134, 1);
      // fill the epoch this height opens (the next one only outlined)
      if (i < 5) display.fillRect(BX + i * CP, BY, CW, 9, GxEPD_WHITE);
      display.display(true);
      delay(i == 0 || i == 5 ? 250 : 80);         // linger on the ends
    }

    display.fillRect(0, 34, 200, 120, GxEPD_BLACK);
    bootCenter("BITCOIN", 70, 2);
    bootCenter("IS TIME", 94, 2);
    display.display(true);

    displayFull();
    delay(600);
    themeDark = savedTheme;
  }

  void drawSplash() {
    display.fillScreen(bg());
    display.setTextColor(fg());
    display.drawRect(0, 0, 200, 200, fg());
    display.drawRect(2, 2, 196, 196, fg());
    display.setFont(NULL);
    display.setTextSize(2);                       // 12x16 built-in
    const char *l1 = "BITCOIN";
    const char *l2 = "IS TIME";
    display.setCursor((200 - strlen(l1)*12) / 2, 74);  display.print(l1);
    display.setCursor((200 - strlen(l2)*12) / 2, 96);  display.print(l2);
    display.setTextSize(1);
    const char *l3 = "TICK TOCK NEXT BLOCK";
    display.setCursor((200 - strlen(l3)*6) / 2, 126);  display.print(l3);
    if (blockHeight > 0) {
      char h[16]; snprintf(h, 16, "%ld", blockHeight);
      display.setCursor((200 - strlen(h)*6) / 2, 140); display.print(h);
    }
    display.display(true);
    delay(2200);
  }

  bool buttonWake = false;   // set by handleButtonPress; not an RTC wake
  bool pressRun = false;     // this run began with a press (never cleared)
  bool showThemeName = false; // DOWN was pressed: announce the theme
                              // in the divider title for one render
                              // (the status row has no free pixels)
  bool inDocked   = false;   // true while the docked realtime loop runs
  bool dockNetDown = false;  // docked but WiFi has dropped
  // How many stops each instrument's box dial has. The increment used to be
  // a flat %6 for every mode while each renderer applied its own modulo, so
  // a 5-stop dial took six presses with the first repeating.
  // DOWN cycles the theme. On WAL it is the wallet's own extra action:
  //   receive QR      VERIFY
  //   hidden balance  tap: peek for this look
  //   shown balance   hold: hide it (and keep it hidden: the privacy setting)
  // Hiding is quick, for the moment someone looks over a shoulder; showing
  // it for good again is deliberate, in My Watch > Privacy.
  void downPressed() {
    if (dispMode == M_WALT && walletView == 0) {
      bool hidden = walletPrivate && !walletPeek;
      bool held = heldFor(DOWN_BTN_PIN, 700);
      if (held) {
        if (!hidden) {
          walletPrivate = true; walletPeek = false;
          savePrefs();
          buzz(40, 2);
          Serial.println("[wal] balance hidden");
        }
        return;                        // held on a hidden balance: nothing
      }
      if (hidden) { walletPeek = true; return; }
      // a tap on a shown balance: the theme, as everywhere
    }
    if (dispMode == M_WALT && walletView == 1 && !isDemoWallet()) {
      verifyScreen();                  // the receive QR: check it, by eye
      return;
    }
    themeMode = (themeMode + 1) % 3;
    savePrefs();
    showThemeName = true;
  }

  // Below 10 sat/vB the decimals are the whole story — 0.25 and 0.9 are
  // different fees, and both used to print as 0.
  void fmtFee(char *out, size_t n, float f) {
    if (f <= 0)      snprintf(out, n, "--");
    else if (f < 10) snprintf(out, n, "%.2f", f);
    else             snprintf(out, n, "%.0f", f);
  }

  // vibMotor() toggles the pin `length` times starting from OFF, so an ODD
  // length ends with the motor still running and nothing switches it back.
  // That is a runaway buzz you have to reset the watch to stop. Always go
  // through this: even count, and the pin driven low afterwards regardless.
  void buzz(uint8_t ms, uint8_t times) {
    if (times & 1) times++;                 // odd would end ON
    vibMotor(ms, times);
    pinMode(VIB_MOTOR_PIN, OUTPUT);
    digitalWrite(VIB_MOTOR_PIN, LOW);       // belt and braces
  }

  int cellStops(int m) {
    if (m == M_HGHT) {              // reward, miner, retarget, min/blk,
      if (travelActive) return 1;   // travelling: subsidy only
      return 7;                     // transactions, hashrate, unconfirmed.
                                    // Block age came out: the corner tag
                                    // already says how old everything is.
    }
    if (m == M_FEES) return 3;      // high, medium, low
    if (m == M_SUPL) return 2;      // issued, remaining
    if (m == M_MEMP) return travelActive ? 1 : 7;   // waiting, then the
                                    // last block and the chain's pace
    return 6;                       // halving almanac, currency dials
  }

  bool prePaint = false;      // re-entering the render to show FTCH before
                              // the radio goes out; suppresses the fetch
  bool walletScanArmed = false, inWalletScan = false;
  bool walletScanAsked = false;  // the wearer asked (long UP): goes regardless
  bool walletDwell = false;      // stopped on WAL for a few seconds: sweep
#define WAL_DWELL_MS 3000        // how long on WAL, untouched, before a sweep
  int  scanCancelled = -1;     // pin of the button that interrupted a scan

  void sanitizeState() {
    if (rtcMagic == RTC_LAYOUT_MAGIC) return;
    rtcMagic = RTC_LAYOUT_MAGIC;
    Serial.begin(115200);
    Serial.println("[rtc] layout changed - state reset");
    // Everything RTC-resident added since must be reset here too, or a
    // reflash leaves it holding the old layout's bytes (a charge "started"
    // 9 years in the future, a run that began never). Settings kept in
    // flash come back through loadPrefs; these are working state.
    chgFromT = 0; chgFromPct = -1; chgLearned = false;
    runStartT = 0; runStartPct = -1; runProf = 0;
    restHrsShown = -2; cleanNext = false; paintedDark = false; facePartials = 0;
    lastMode = 0; lastWhy = 0;
    drawnMode = -1; drawnView = -1;
    dispMode = M_HGHT; themeMode = 0; tzIndex = 0;
    btcPrice = 0; btcChange24h = 0; btcMcapB = 0;
    fearGreed = 50; fastFee = 0.0f;
    for (int i = 0; i < SPARK_POINTS; i++) spark[i] = 0;
    sparkHead = 0;
    lastFetchMin = 0; haveData = false;
    walletSats = 0; recvIndex = 0; walletView = 0;
    lnInvoice[0] = 0; lnVerify[0] = 0;
    lastWalletMin = 0; vigilPending = false;
    lastTxSats = 0; haveWallet = false;
    blockHeight = 0; lastHeightWake = 0; lastBuzzHeight = 0;
    priceAnchor = 0; diffChangeEst = 0;
    for (int i = 0; i < NUM_MODES; i++) cellSel[i] = 0;
    curIdx = 0;
    for (int i = 0; i < NUM_CUR; i++) fxRate[i] = 0;
    lastRewardBtc = 0; lastTxCount = 0; netHashEH = 0; lastCurio = 0; curioSeen = 0;
    retEpoch = 0; retStartT = 0; evRetEpoch = 0; evRetChange = 0; evRetDays = 0;
    evRetMined = 0; evRetFees = -1; evRetTxs = 0; retPending = false;
    halvEra = 0; evHalvN = 0; evHalvYears = 0; evHalvFees = -1; halvPending = false;
    minVoltSeen = 9.9f; minVoltWake = 0; minVoltFlags = 0;
    usbSeenWake = 0;
    mempoolBlocks = 0;
    for (int i = 0; i < 6; i++) { recH[i] = 0; recT[i] = 0; recFill[i] = 0; recPool[i][0] = 0; }
    for (int i = 0; i < 3; i++) { projFee[i] = 0; projFill[i] = 0; }
    projN = 0;
    poolName[0] = 0; rwdHeight = 0;
    goldMcapB = 0; lastNtpWake = 0; tipBlockTime = 0; fetchPending = false;
    travelActive = false; travelHeight = 0; travelWake = 0;
    travelPastPrice = 0; travelPastFor = -1;
    walletPrivate = false;          // loadPrefs restores the saved choice
    walletPeek = false;
    lastAx = lastAy = lastAz = 0; lastMoveWake = 0; accelSeeded = false;
    shownBattPct = -1;      // let the latch re-learn from the next reading
    medFee = 0; lowFee = 0;
    wakeMin = 0; failCount = 0; nextTryWake = 0; forceFetch = false;
    zpubBuf[0] = 0; lnAddrBuf[0] = 0;
    // These were added later and never reset here, so a layout change left
    // them holding whatever occupied those bytes before. A stale avgBlockSec
    // is the worst of them: every halving date, every estimate and the
    // MIN/BLK readout are computed from it, so a garbage value moves the
    // 2028 halving by months without anything looking obviously broken.
    avgBlockSec = 600;
    joeReveal = false; captivePortal = false; resting = false;
    joeStep = 0; evAgeWho = -1; evAgeN = 0;
    restState = 0; restPose = 0; sleptSince = 0; stillRun = 0;
    candleWake = 0; goldWake = 0; slowWake = 0; fngWake = 0; dayAgoUsd = 0;
    alarmHeight = 0; feeAlarm = 0; feeArmed = true;   // NVS reloads the alarms
    evQueue = 0; evShown = 0; evWake = 0; evCurioH = 0; evBlockH = 0; evFee = 0; evFeeAt = 0;
    evConfN = 0; evConfSats = 0; evConfBlock = 0; evPaySats = 0; evPayLn = false;
    lateKind = 0; lateIdx = -1; lateBase = 0; lateUntil = 0;
    shipMode = false; shipSince = 0;
    trackState = 0; trackAddr[0] = 0; trackTxid[0] = 0; trackSats = 0;
    trackHeight = 0; trackShown = 0; trackWake = 0; lastDoneTx[0] = 0;
    fastSsid[0] = 0; fastChan = 0; memset(fastBssid, 0, sizeof(fastBssid));
    awakeTill = 0;
    lnFetchedWake = 0;      // a stale stamp can make an old invoice
                            // look fresh after a layout change
    sawMillion = false;
    // The log is not cleared here. RTC memory has just been thrown away, but
    // the samples live in flash and are the whole point of collecting them —
    // a layout change or a flat cell should not cost a discharge run. Only
    // the hourly timer resets, so the next sample lands on a fresh hour.
    vlogWake = 0;
    loadVlog();
    prefsLoaded = false;                    // NVS reloads the real ones
    haveDesc = false;                       // (and says if there is one)
    qrDrawnIdx = -1; qrPartials = 0;
    memset(wifiSsid, 0, sizeof(wifiSsid));
    memset(wifiPass, 0, sizeof(wifiPass));
    avgBlockSec = 600;
    guiState = 0;                          // library state too:
    menuIndex = 0;                         // WATCHFACE_STATE, top item
    menuLevel = 0;
    alreadyInMenu = true;
  }

  // Stencil numerals. Each digit is a solid slab with its counters cut back
  // out of it, the way a stencil is made — not a grid of little bricks. At
  // 86 x 81 on e-paper that is the difference between a number that reads
  // across a room and one that reads as texture.
  //
  //   T   stroke thickness, as a fraction of the digit
  //   UY  top of the upper counter        LY  top of the lower counter
  //   UH  counter height — what is left after three strokes
  void drawStencilDigit(int X, int Y, int W, int H, int d) {
    const float T = 0.24f;
    const float UH = (1.0f - 3.0f * T) / 2.0f;
    const float UY = T, LY = (1.0f + T) / 2.0f;
    auto fill = [&](float x, float y, float w, float h, uint16_t c) {
      display.fillRect(X + (int)(x * W + 0.5f), Y + (int)(y * H + 0.5f),
                       (int)(w * W + 0.5f),     (int)(h * H + 0.5f), c);
    };
    // (x, width) of each counter: where the digit is open
    float ux, uw, lx, lw;
    bool slab = true;
    switch (d) {
      case 0: fill(0,0,1,1,fg()); fill(T,T,1-2*T,1-2*T,bg()); return;
      case 2: ux=0;   uw=1-T;   lx=T;   lw=1-T;   break;
      case 3: ux=0;   uw=1-T;   lx=0;   lw=1-T;   break;
      case 5: ux=T;   uw=1-T;   lx=0;   lw=1-T;   break;
      case 6: ux=T;   uw=1-T;   lx=T;   lw=1-2*T; break;
      case 8: ux=T;   uw=1-2*T; lx=T;   lw=1-2*T; break;
      case 9: ux=T;   uw=1-2*T; lx=0;   lw=1-T;   break;
      default: slab = false; break;
    }
    if (slab) {
      fill(0,0,1,1,fg());
      fill(ux,UY,uw,UH,bg());
      fill(lx,LY,lw,UH,bg());
      return;
    }
    // the three that are strokes rather than a slab
    if (d == 1) { fill(1-T,0,T,1,fg()); fill(1-2.1f*T,0,1.1f*T,T,fg()); }
    else if (d == 4) { fill(0,0,T,(1+T)/2,fg()); fill(1-T,0,T,1,fg());
                       fill(0,(1-T)/2,1,T,fg()); }
    else if (d == 7) { fill(0,0,1,T,fg()); fill(1-T,0,T,1,fg()); }
  }

  // The resting face. Everything on it has to stay true with nobody redrawing
  // it for half an hour or a whole night, so nothing on it is a clock: the
  // chain as it was last heard, and AS OF when that was. The tag sits where
  // LIVE does, and the strip that normally steers between modes says the one
  // thing anyone picking it up needs to know.
  // What the chain will do before night ends: up to two lines for the night
  // face, "~02:10  RETARGET ~-1.9%". All worked out from the height and the
  // measured pace, no radio; times are estimates. Returns how many.
  int nightAhead(char out[2][34]) {
    long h0 = estHeight();
    if (h0 <= 0) return 0;
    long windowSec = (long)minutesUntilHour(nightTo) * 60L;
    struct Ev { long h; char t[26]; } ev[11]; int n = 0;
    long r = (h0 / 2016L + 1) * 2016L;                       // retarget
    snprintf(ev[n].t, 26, diffChangeEst != 0 ? "RETARGET ~%+.1f%%" : "RETARGET",
             diffChangeEst);
    ev[n++].h = r;
    long hv = (h0 / 210000L + 1) * 210000L;                  // halving
    if (hv <= 6930000L) { snprintf(ev[n].t, 26, "HALVING"); ev[n++].h = hv; }
    if (h0 < 1000000L) { snprintf(ev[n].t, 26, "BLOCK 1,000,000"); ev[n++].h = 1000000L; }
    { long best = -1; int bk = 0;                            // curiosities
      for (int k = 0; k < 3; k++) {
        long c = curioNext(k, h0);
        if (c > 0 && (best < 0 || c < best)) { best = c; bk = k; }
      }
      if (best > 0 && best != 1000000L) {
        static const char *K[3] = {"PALINDROME", "TWIN", "REPDIGIT"};
        char g[16]; grouped(g, 16, best);
        snprintf(ev[n].t, 26, "%s %s", K[bk], g); ev[n++].h = best;
      } }
    if (alarmHeight > h0) {                                  // the radio is off:
      snprintf(ev[n].t, 26, "ALARM, RINGS %02d:00", nightTo); // it rings on the
      ev[n++].h = alarmHeight;                                // morning fetch
    }
    for (int i = 1; i < n; i++)                              // soonest first
      for (int j = i; j > 0 && ev[j].h < ev[j - 1].h; j--) { Ev t = ev[j]; ev[j] = ev[j - 1]; ev[j - 1] = t; }
    // block birthdays: as heights for real-block ages, so they sort with the
    // rest (a ten-minute age is turned into the height it lands at)
    loadPeople();
    for (int who = 0; who <= MAX_PEOPLE && n < 11; who++) {
      long long a, nx; bool ten; long at;
      if (who == WATCH_WHO) {
        a = watchAge(h0); if (a < 0) continue; ten = false;
        nx = milestoneAbove(a, false); at = watchSince() + (long)nx;
      } else {
        if (!pplBirth[who]) continue;
        ten = bornBefore(who);
        int64_t now64 = nowUtc64();
        a = ageOf(who, h0, now64); if (a < 0) continue;
        nx = milestoneAbove(a, ten);
        at = ten ? h0 + (long)(((birthUtc(who) + nx * 600) - now64) / 600)
                 : heightAtUtc(birthUtc(who)) + (long)nx;
      }
      char g[16]; grouped(g, 16, nx);
      snprintf(ev[n].t, 26, "%s %s", who == WATCH_WHO ? "BWATCH" : pplName[who], g);
      ev[n++].h = at;
    }
    for (int i = 1; i < n; i++)                              // soonest first
      for (int j = i; j > 0 && ev[j].h < ev[j - 1].h; j--) { Ev t = ev[j]; ev[j] = ev[j - 1]; ev[j - 1] = t; }
    int shown = 0, now = localMinuteOfDay();
    for (int i = 0; i < n && shown < 2; i++) {
      double sec = secondsUntil(ev[i].h);
      if (sec <= 0 || sec > windowSec) continue;
      int m = (now + (int)(sec / 60.0 + 0.5)) % 1440;
      snprintf(out[shown++], 34, "~%02d:%02d  %s", m / 60, m % 60, ev[i].t);
    }
    return shown;
  }

  void drawRestFace() {
    qrDrawnIdx = -1;
    bool night = (restState == REST_NIGHT);
    themeDark = darkNow();
    paintedDark = themeDark;          // its callers refresh it full
    display.setFullWindow();
    display.fillScreen(bg());
    display.setTextColor(fg());
    display.setFont(NULL);

    drawBattery(10, 3);
    restHrsShown = hoursLeft();             // how long it has: the question a
    if (restHrsShown >= 0) {                // wearer picking it up is asking
      char hb[12]; fmtHours(hb, 12, restHrsShown);
      display.setCursor(32, 3);
      display.print(hb);
    }
    const char *tag = night ? "NIGHT" : "REST";
    display.setCursor(188 - (int)strlen(tag) * 6, 3);
    display.print(tag);
    if (alarmsArmed()) drawBell(188 - (int)strlen(tag) * 6 - 9, 3);

    if (night) {                                   // a crescent, two circles
      display.fillCircle(100, 34, 9, fg());
      display.fillCircle(105, 30, 8, bg());
    }

    // the height we were TOLD, never an estimate: AS OF dates it
    char big[16]; snprintf(big, 16, "%ld", blockHeight);
    int base = night ? 92 : 80;
    drawGrouped(big, base, fitFont(big));
    display.setFont(NULL);

    // AS OF the older of the two figures on the face, so the stamp is true
    // of both of them
    long age = staleMinutes();
    if (heightStaleMinutes() > age) age = heightStaleMinutes();
    int asOf = localMinuteOfDay() - (int)age;
    while (asOf < 0) asOf += 1440;
    char line[26];
    snprintf(line, 26, "AS OF %02d:%02d", asOf / 60, asOf % 60);
    centerSmall(line, base + 10);

    if (btcPrice > 0) {
      char p[16]; snprintf(p, 16, "%ld", (long)(btcPrice + 0.5f));
      char g[20]; int o = 0, len = strlen(p);
      for (int i = 0; i < len && o < 18; i++) {      // group the thousands
        g[o++] = p[i];
        int left = len - i - 1;
        if (left > 0 && left % 3 == 0) g[o++] = ',';
      }
      g[o] = 0;
      snprintf(line, 26, "%s %s", g, CUR_CODES[curIdx < NUM_CUR ? curIdx : 0]);
      centerSmall(line, night ? 126 : 136);
    }
    if (night) {
      snprintf(line, 26, "RADIO OFF TO %02d:00", nightTo);
      centerSmall(line, 146);
      // and what the chain does while you sleep (the cards wait for morning)
      char ahead[2][34];
      int na = nightAhead(ahead);
      if (na > 0) {
        display.drawFastHLine(40, 157, 120, fg());
        for (int i = 0; i < na; i++) centerSmall(ahead[i], 162 + i * 11);
      }
    }

    display.drawRect(5, 186, 190, 13, fg());       // where the mode strip was
    centerSmall("ANY BUTTON WAKES", 189);
  }

  // A full refresh, then the panel's high voltage OFF. The Watchy driver
  // ends a full refresh with 0x22/0xF4, which leaves the analog supply on
  // (the stock GxEPD2 driver for this panel ends with 0xF7, which turns it
  // off), and its hibernate() skips the power-off. Harmless when deep sleep
  // follows within milliseconds; but awake after it (the menu's 5 s, the
  // payment vigil, the dock) the freshly driven image faded to almost
  // nothing within a couple of seconds. Off costs one re-power on the next
  // refresh, ~0.1 s.
  // every power-off goes through here: forced, so it is never skipped
  // DIAGNOSTIC (Oct 16): every refresh and power-off is logged with a
  // millis() stamp, the radio state and the driver's busy timings, to find
  // why fresh images fade on the glass
  void epdLog(const char *what) {
    EpdFull::diag(display.epd2);
    Serial.printf("[epd] %7lu %s  wifi=%d dock=%d on=%d\n", millis(), what,
                  WiFi.status() == WL_CONNECTED, inDocked,
                  EpdFull::isOn(display.epd2));
  }
  void panelOff() {
    unsigned long t = millis();
    EpdFull::markOn(display.epd2); display.powerOff();
    Serial.printf("[epd] %7lu off (%lu ms)\n", millis(), millis() - t);
  }
  // The controller diffs a partial against its "previous" memory. The stock
  // driver for this panel rewrites that memory with the shown image after
  // every refresh; the Watchy driver writes only "current". So "previous"
  // drifted from what was on the glass: a partial right after a full greyed
  // black areas, and card after card kept bits of the cards before. After
  // every refresh of ours, both memories get the image on the glass.
  void syncPrev() {
    uint8_t *fb = display.*epdGet(EpdBufTag());
    display.epd2.writeImageForFullRefresh(fb, 0, 0, WatchyDisplay::WIDTH,
                                          WatchyDisplay::HEIGHT);
  }
  void displayFull() {
    epdLog("FULL start");
    display.display(false);
    syncPrev();
    epdLog("FULL done");
    panelOff();
    justFull = true;
  }
  void partialSync() { display.display(true); syncPrev(); }
  // THE FADE (found with the serial log, Oct 16): a full refresh came out
  // crisp, and the partial refresh sent ~0.5 s later on the SAME image greyed
  // its black areas within half a second. So after a full, the identical
  // partial that would follow is not sent. justFull is cleared at the start
  // of every draw, so a later partial with real changes still goes out.
  bool justFull = false;
  void partialRefresh() {
    if (justFull) { justFull = false; epdLog("partial skipped: just full"); return; }
    partialSync();
  }
  // inside a face draw: the caller's closing refresh becomes the full one.
  // That refresh arrives as display(true), which (unlike display(false))
  // never turns the panel's high voltage off: left on while the watch stays
  // awake, the fresh image fades within seconds (see displayFull). So it is
  // remembered, and whoever sent it powers the panel off straight after.
  bool fullArmed = false;
  // Turning the caller's partial into a full (the driver's "first refresh"
  // flag) drove light faces fine but not big black ones: a flip to dark came
  // out grey and faded. The black cards, sent by displayFull(), were always
  // crisp. So a face that wants a full now gets exactly that, from the end
  // of drawWatchFace(): displayFull(), then the caller's own partial lands
  // on an identical image and changes nothing.
  void fullOnClose() { fullArmed = true; }
  // After ANY refresh while the watch stays awake (button loop, dock, payment
  // vigil, wallet scan): the panel's high voltage off. A partial leaves it on
  // too, and on the dock the LN QR, freshly drawn, faded to grey in ~15 s.
  // Before deep sleep it does not matter (the panel's sleep cuts it); awake,
  // it does. ~150 ms, and the next refresh powers back up by itself.
  void offIfFull() { panelOff(); }
  // our own calls get the power-off; the library's minute tick calls its own
  // version and goes straight to deep sleep, which hibernates the panel
  // The library's version, with our refreshes: no identical partial after a
  // full, and the panel off after (we stay awake for more presses)
  bool ownShow = false;
  void showWatchFace(bool partial) {
    epdLog(partial ? "face (partial)" : "face (full)");
    display.setFullWindow();
    display.epd2.asyncPowerOn();
    ownShow = true;
    drawWatchFace();
    ownShow = false;
    if (partial) partialRefresh();
    else if (justFull) justFull = false;      // the draw already sent it full
    else { display.display(false); syncPrev(); justFull = true; }
    guiState = WATCHFACE_STATE;
    offIfFull();
  }
  void showFaceFull() {
    callerFull = true; showWatchFace(false); callerFull = false; panelOff();
  }

  void centerSmall(const char *s, int y) {          // built-in 6x8 font
    display.setCursor((200 - (int)strlen(s) * 6) / 2, y);
    display.print(s);
  }

  // JOE. Four digits, each one filling a quarter of the panel, with nothing
  // drawn between them — the gaps do the dividing. Below them a strip: the
  // date, the cell as a bar, and the mark. Nothing here comes off the chain.
  // ---------------- people: ages in blocks ----------------
  // Up to four people (name and birthday), entered on the phone (Setup
  // Wallet's page) or under My Watch > Birthdays, kept in NVS. Someone born
  // before Bitcoin has an age in ten-minute blocks counted back past genesis
  // (every block ever mined happened in their lifetime, so "blocks seen" would
  // just be the height); someone born after it has seen real blocks, counted
  // from the height at their birth. The watch counts its own: blocks since it
  // was unboxed. Round ages ("block birthdays") get a card.
  static const int MAX_PEOPLE = 4;
  static const int WATCH_WHO = 4;                   // the watch, in milestones
  char pplName[MAX_PEOPLE][9];
  uint32_t pplBirth[MAX_PEOPLE];                    // yyyymmdd, 0 = empty
  uint32_t pplMile[MAX_PEOPLE + 1];                 // last age celebrated
  bool peopleLoaded = false;
  static const int64_t GENESIS_UTC = 1231006505LL;  // 3 Jan 2009 18:15:05

  void loadPeople() {
    if (peopleLoaded) return;
    peopleLoaded = true;
    Preferences p; p.begin("btcchrono", true);
    for (int i = 0; i < MAX_PEOPLE; i++) {
      char k[6];
      snprintf(k, 6, "pN%d", i);
      String n = p.getString(k, "");
      strncpy(pplName[i], n.c_str(), 8); pplName[i][8] = 0;
      snprintf(k, 6, "pB%d", i); pplBirth[i] = p.getULong(k, 0);
    }
    for (int i = 0; i <= MAX_PEOPLE; i++) {
      char k[6]; snprintf(k, 6, "pM%d", i); pplMile[i] = p.getULong(k, 0);
    }
    p.end();
  }
  void savePeople() {
    Preferences p; p.begin("btcchrono", false);
    for (int i = 0; i < MAX_PEOPLE; i++) {
      char k[6];
      snprintf(k, 6, "pN%d", i); p.putString(k, pplName[i]);
      snprintf(k, 6, "pB%d", i); p.putULong(k, pplBirth[i]);
    }
    for (int i = 0; i <= MAX_PEOPLE; i++) {
      char k[6]; snprintf(k, 6, "pM%d", i); p.putULong(k, pplMile[i]);
    }
    p.end();
  }
  int peopleCount() {
    loadPeople();
    int n = 0;
    for (int i = 0; i < MAX_PEOPLE; i++) if (pplBirth[i]) n++;
    return n;
  }
  // the i-th entered person, skipping empty slots; -1 if none
  int personAt(int k) {
    loadPeople();
    for (int i = 0; i < MAX_PEOPLE; i++) if (pplBirth[i] && k-- == 0) return i;
    return -1;
  }
  static int64_t daysFromCivil(int y, int m, int d) {    // Hinnant
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
  }
  // local midnight of the birthday, as UTC seconds
  int64_t birthUtc(int i) {
    uint32_t b = pplBirth[i];
    return daysFromCivil(b / 10000, (b / 100) % 100, b % 100) * 86400LL
           - settings.gmtOffset;
  }
  int64_t nowUtc64() { return (int64_t)makeTime(currentTime) - settings.gmtOffset; }
  // the height at a moment: the inverse of heightUtc() (recorded dates in the
  // past, ten minutes a block beyond the tip)
  long heightAtUtc(int64_t t) {
    if (t <= GENESIS_UTC || blockHeight <= 0) return 0;
    int64_t now = heightUtc(blockHeight);
    if (t >= now) return blockHeight + (long)((t - now) / 600);
    long lo = 0, hi = blockHeight;
    while (hi - lo > 1) {
      long mid = (lo + hi) / 2;
      if (heightUtc(mid) <= t) lo = mid; else hi = mid;
    }
    return lo;
  }
  bool bornBefore(int i) { return birthUtc(i) < GENESIS_UTC; }
  // where "now" is: the trip's destination, or the real present
  long ctxHeight() { return travelActive ? travelHeight : (blockHeight > 0 ? blockHeight : estHeight()); }
  int64_t ctxUtc() { return travelActive ? heightUtc(travelHeight) : nowUtc64(); }
  // the age shown: ten-minute blocks (born before genesis) or real blocks
  // seen (after). Negative: not born yet (a trip into the past).
  long long ageOf(int i, long h, int64_t t) {
    if (bornBefore(i)) return (long long)((t - birthUtc(i)) / 600);
    return (long long)h - heightAtUtc(birthUtc(i));
  }
  // the block the watch started counting from: unboxed, or first seen
  long watchSince() {
    Preferences p; p.begin("btcchrono", true);
    long o = p.getLong("openH", 0), f = p.getLong("h1st", 0);
    p.end();
    return o > 0 ? o : f;
  }
  void noteFirstHeight() {                     // once, for a watch never sealed
    if (blockHeight <= 0 || travelActive) return;
    Preferences p; p.begin("btcchrono", false);
    if (p.getLong("openH", 0) == 0 && p.getLong("h1st", 0) == 0) p.putLong("h1st", blockHeight);
    p.end();
  }
  // round ages: ten-minute ages every 100,000; real-block ages (kids, the
  // watch) at 1,000 / 10,000 / 50,000 / 100,000 and every 100,000 after;
  // and same-digit ages (222,222, 1,111,111 ...) from six digits up
  static bool isRep(long long a) {
    if (a < 111111) return false;
    long long d = a % 10; if (!d) return false;
    while (a) { if (a % 10 != d) return false; a /= 10; }
    return true;
  }
  static bool isMilestone(long long a, bool ten) {
    if (a <= 0) return false;
    if (isRep(a)) return true;
    if (a % 100000 == 0) return true;
    return !ten && (a == 1000 || a == 10000 || a == 50000);
  }
  static long long milestoneAtOrBelow(long long a, bool ten) {
    if (a <= 0) return 0;
    long long best = 0;
    long long c = (a / 100000) * 100000;
    if (c > 0) best = c;
    if (!ten) { if (a >= 1000 && best < 1000) best = 1000;
                if (a >= 10000 && best < 10000) best = 10000;
                if (a >= 50000 && best < 50000) best = 50000; }
    for (int len = 6; len <= 8; len++) {
      long long ones = 0; for (int k = 0; k < len; k++) ones = ones * 10 + 1;
      for (int dg = 1; dg <= 9; dg++) { long long r = ones * dg; if (r <= a && r > best) best = r; }
    }
    return best;
  }
  static long long milestoneAbove(long long a, bool ten) {
    long long best = ((a / 100000) + 1) * 100000;
    if (!ten) { if (a < 1000) best = 1000; else if (a < 10000) best = 10000;
                else if (a < 50000) best = 50000; }
    for (int len = 6; len <= 8; len++) {
      long long ones = 0; for (int k = 0; k < len; k++) ones = ones * 10 + 1;
      for (int dg = 1; dg <= 9; dg++) { long long r = ones * dg; if (r > a && r < best) best = r; }
    }
    return best;
  }
  // the watch's age now, in blocks witnessed
  long long watchAge(long h) { long s0 = watchSince(); return s0 > 0 ? (long long)h - s0 : -1; }

  // After every fetch: a person (or the watch) who crossed a round age since
  // the last check gets a card. A new person starts at their current age, so
  // adding someone does not replay every milestone they ever passed.
  void checkAges() {
    if (blockHeight <= 0 || travelActive) return;
    loadPeople();
    noteFirstHeight();
    int64_t now = nowUtc64();
    bool dirty = false;
    for (int who = 0; who <= MAX_PEOPLE; who++) {
      long long a; bool ten;
      if (who == WATCH_WHO) { a = watchAge(blockHeight); ten = false; }
      else { if (!pplBirth[who]) continue; ten = bornBefore(who); a = ageOf(who, blockHeight, now); }
      if (a <= 0) continue;
      long long m = milestoneAtOrBelow(a, ten);
      if (pplMile[who] == 0) { pplMile[who] = m ? (uint32_t)m : 1; dirty = true; continue; }
      if (m > (long long)pplMile[who]) {
        pplMile[who] = (uint32_t)m; dirty = true;
        if (!(evQueue & EV_AGE)) {                 // one card at a time
          evAgeWho = who; evAgeN = (uint32_t)m;
          fireEvent(EV_AGE);
          buzz(35, 2);
          Serial.printf("[age] %s: %llu blocks\n",
                        who == WATCH_WHO ? "watch" : pplName[who], (unsigned long long)m);
        }
      }
    }
    if (dirty) savePeople();
  }

  // The card all of these share: the curiosity cards' layout
  void drawStatCard(const char *top, const char *title, long long num,
                    const char *l1, const char *l2, const char *l3) {
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setTextWrap(false);
    display.setFont(NULL); display.setTextSize(1);
    centerSmall(top, 14);
    int tl = strlen(title); int sz = tl <= 10 ? 3 : 2;
    display.setTextSize(sz);
    display.setCursor((200 - tl * 6 * sz) / 2, 34);
    display.print(title);
    display.setTextSize(1);
    char s[24]; snprintf(s, 24, "%lld", num < 0 ? -num : num);
    const GFXfont *F = strlen(s) > 7 ? &DSEG7_Classic_Bold_25 : &DSEG7_Classic_Bold_32;
    int n = strlen(s), cw = 6, w = 0;
    for (int i = 0; i < n; i++) {
      w += F->glyph[s[i] - F->first].xAdvance;
      if ((n - 1 - i) % 3 == 0 && i < n - 1) w += cw;
    }
    int x = (200 - w) / 2;
    display.setFont(F);
    for (int i = 0; i < n; i++) {
      char c[2] = {s[i], 0};
      display.setCursor(x, 104); display.print(c);
      x += F->glyph[s[i] - F->first].xAdvance;
      if ((n - 1 - i) % 3 == 0 && i < n - 1) {
        display.fillRect(x + cw / 2 - 1, 103, 3, 5, GxEPD_WHITE); x += cw;
      }
    }
    display.setFont(NULL);
    centerSmall(l1, 114);
    display.drawFastHLine(30, 134, 140, GxEPD_WHITE);
    centerSmall(l2, 146);
    centerSmall(l3, 160);
    display.drawRect(0, 0, 200, 200, GxEPD_WHITE);
    display.drawRect(2, 2, 196, 196, GxEPD_WHITE);
  }

  // JOE's person card: at the trip's destination when travelling
  void drawPersonCard(int i, int k, int n) {
    char top[34], l1[34], l2[34], l3[34], g[16];
    long h = ctxHeight(); int64_t t = ctxUtc();
    if (travelActive) { grouped(g, 16, h); snprintf(top, 34, "AT BLOCK %s", g); }
    else snprintf(top, 34, "%d OF %d - UP FOR NEXT", k, n);
    long long a = ageOf(i, h, t);
    l3[0] = 0;
    if (bornBefore(i)) {
      snprintf(l1, 34, "BLOCKS OLD, AT TEN MINUTES");
      grouped(g, 16, (long long)((GENESIS_UTC - birthUtc(i)) / 600));
      snprintf(l2, 34, "BORN %s BEFORE GENESIS", g);
      double share = (double)(t - GENESIS_UTC) / (double)(t - birthUtc(i));
      if (share > 0) snprintf(l3, 34, "BITCOIN IS %d%% OF YOUR LIFE", (int)(share * 100 + 0.5));
    } else {
      long bh = heightAtUtc(birthUtc(i));
      grouped(g, 16, bh);
      if (a < 0) {
        snprintf(l1, 34, "BLOCKS UNTIL BORN");
        snprintf(l2, 34, "BORN AT BLOCK %s", g);
      } else {
        snprintf(l1, 34, "BLOCKS SEEN");
        snprintf(l2, 34, "BORN AT BLOCK %s", g);
        if (h > 0) snprintf(l3, 34, "SEEN %d%% OF THE CHAIN", (int)(100.0 * a / h + 0.5));
      }
    }
    drawStatCard(top, pplName[i][0] ? pplName[i] : "SOMEONE", a, l1, l2, l3);
  }
  void drawWatchCard() {
    char top[34], l2[34], l3[34], g[16];
    long h = ctxHeight(); int64_t t = ctxUtc();
    long s0 = watchSince();
    if (travelActive) { grouped(g, 16, h); snprintf(top, 34, "AT BLOCK %s", g); }
    else snprintf(top, 34, "THIS WATCH");
    grouped(g, 16, s0);
    snprintf(l2, 34, "UNBOXED AT BLOCK %s", g);
    long long a = (long long)h - s0;
    double days = (double)(t - heightUtc(s0)) / 86400.0;
    if (a < 0) snprintf(l3, 34, "NOT MADE YET");
    else snprintf(l3, 34, "%d DAYS ON YOUR WRIST", (int)(days + 0.5));
    drawStatCard(top, "BWATCH", a, a < 0 ? "BLOCKS UNTIL UNBOXED" : "BLOCKS WITNESSED", l2, l3);
  }
  // how many steps JOE's UP cycle has: time, chain, people, watch
  int joeSteps() { return 2 + peopleCount() + (watchSince() > 0 ? 1 : 0); }
  void joeAdvance() {
    joeStep = (joeStep + 1) % joeSteps();
    joeReveal = (joeStep == 1);
  }

  // the block-birthday card (an event, waiting on the glass)
  void drawAgeCard() {
    loadPeople();
    int who = evAgeWho; long long n = evAgeN;
    char top[34], l1[34], l2[34], l3[34], g[16], eta[20];
    bool watch = (who == WATCH_WHO);
    bool ten = !watch && who >= 0 && bornBefore(who);
    if (ten) {                                    // exact: birth + n x 600 s
      int64_t at = birthUtc(who) + n * 600;
      int64_t now = nowUtc64();
      tmElements_t a, b;
      breakTime(localFromUtc((time_t)at), a); breakTime(localFromUtc((time_t)now), b);
      long dd = (long)(localFromUtc((time_t)now) / 86400) - (long)(localFromUtc((time_t)at) / 86400);
      if (now - at < 15 * 60) snprintf(top, 34, "JUST NOW");
      else if (dd <= 0) snprintf(top, 34, "AT %02d:%02d TODAY", a.Hour, a.Minute);
      else if (dd == 1) snprintf(top, 34, "YESTERDAY AT %02d:%02d", a.Hour, a.Minute);
      else snprintf(top, 34, "%ld DAYS AGO", dd);
    } else {                                      // a real block: when it was mined
      long bh = watch ? watchSince() : heightAtUtc(birthUtc(who));
      curioWhen(top, 34, bh + (long)n);
    }
    snprintf(l1, 34, watch ? "BLOCKS WITNESSED" : ten ? "BLOCKS OLD" : "BLOCKS SEEN");
    snprintf(l2, 34, "HAPPY BLOCKDAY");
    long long nx = milestoneAbove(n, ten);
    grouped(g, 16, nx);
    double mins = (double)(nx - n) * 10.0;
    if (mins < 48 * 60)          snprintf(eta, 20, "~%.0f HOURS", mins / 60);
    else if (mins < 730 * 1440)  snprintf(eta, 20, "~%.0f DAYS", mins / 1440);
    else                         snprintf(eta, 20, "~%.1f YEARS", mins / 525960);
    snprintf(l3, 34, "NEXT: %s IN %s", g, eta);
    drawStatCard(top, watch ? "BWATCH" : (who >= 0 && pplName[who][0] ? pplName[who] : "SOMEONE"),
                 n, l1, l2, l3);
  }

  // Once a year: the birthday itself, on the first press of the day
  bool maybeBirthdayCard() {
    tmElements_t dt;
    if (!localDate(dt)) return false;
    loadPeople();
    uint32_t today = tmYearToCalendar(dt.Year) * 10000UL + dt.Month * 100 + dt.Day;
    for (int i = 0; i < MAX_PEOPLE; i++) {
      uint32_t b = pplBirth[i];
      if (!b || (b / 100) % 100 != dt.Month || b % 100 != dt.Day) continue;
      char k[6]; snprintf(k, 6, "bdS%d", i);
      Preferences p; p.begin("btcchrono", false);
      bool seen = p.getULong(k, 0) == today;
      if (!seen) p.putULong(k, today);
      p.end();
      if (seen) continue;
      int years = tmYearToCalendar(dt.Year) - (int)(b / 10000);
      static const char *MON[] = {"", "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY",
        "JUNE", "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"};
      char top[34], l2[34], l3[34], g[16];
      snprintf(top, 34, "%d %s", dt.Day, MON[dt.Month]);
      long long a = ageOf(i, blockHeight, nowUtc64());
      grouped(g, 16, a);
      snprintf(l2, 34, bornBefore(i) ? "%s BLOCKS OLD" : "%s BLOCKS SEEN", g);
      snprintf(l3, 34, "HAPPY BIRTHDAY");
      guiState = APP_STATE;
      drawStatCard(top, pplName[i][0] ? pplName[i] : "SOMEONE", years,
                   years == 1 ? "YEAR OLD TODAY" : "YEARS OLD TODAY", l2, l3);
      displayFull();
      paintedDark = true;
      buzz(35, 2);
      pinMode(BACK_BTN_PIN, INPUT); pinMode(MENU_BTN_PIN, INPUT);
      pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);
      waitAllRelease();
      unsigned long t0 = millis();
      while (millis() - t0 < 60000) {
        if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE || digitalRead(MENU_BTN_PIN) == BTN_ACTIVE ||
            digitalRead(UP_BTN_PIN) == BTN_ACTIVE || digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) {
          waitAllRelease(); break; }
        delay(50);
      }
      guiState = WATCHFACE_STATE;
      RTC.read(currentTime);
      showFaceFull();
      return true;
    }
    return false;
  }

  void drawJoeFace() {
    if (!buttonWake) { joeStep = 0; joeReveal = false; }   // one glance only
    if (joeStep >= joeSteps()) joeStep = 0;
    if (joeStep >= 2) {                                    // a person, or the watch
      int k = joeStep - 2, n = peopleCount();
      if (k < n) drawPersonCard(personAt(k), k + 1, n);
      else drawWatchCard();
      if (!paintedDark) fullOnClose();                     // black card: a flip
      paintedDark = true;
      return;
    }
    if (paintedDark != themeDark) fullOnClose();           // back from a card
    paintedDark = themeDark;
    display.fillScreen(bg());
    display.setTextColor(fg());
    RTC.read(currentTime);
    time_t lt = makeTime(currentTime);
    if (tzIndex > 0) {
      time_t utc = lt - settings.gmtOffset;
      lt = utc + tzOffsetSec(tzIndex, utc);
    }
    tmElements_t dt; breakTime(lt, dt);

    // A digit fills most of its quarter but not all of it. The cell gutter and
    // the gap between quarters were the same 4 px at first, which made the
    // four digits merge into one grid — the margin is what separates them.
    const int QW = 98, QH = 90, GUT = 4;
    const int DW = 86, DH = 81;                // the digit, inside its quarter
    int digits[4] = { dt.Hour / 10, dt.Hour % 10,
                      dt.Minute / 10, dt.Minute % 10 };
    for (int q = 0; q < 4; q++) {
      int ox = (q % 2) * (QW + GUT) + (QW - DW) / 2;
      int oy = (q / 2) * (QH + GUT) + (QH - DH) / 2;
      drawStencilDigit(ox, oy, DW, DH, digits[q]);
    }

    // ── the strip ──────────────────────────────────────────────────
    display.setFont(NULL);
    const int SY = 188;
    drawTinyB(1, SY);                                    // the mark

    char d[10]; snprintf(d, 10, "%02d %02d", dt.Day, dt.Month);
    display.setCursor(12, SY + 3); display.print(d);

    // the cell, drawn the way the strip wants it: a rule with a fill under it
    display.fillRect(78, SY, 46, 3, fg());
    int bp = batteryPctShown();
    int fw = (bp >= 0) ? (int)(46.0f * bp / 100.0f + 0.5f) : 0;
    if (fw > 0) display.fillRect(78, SY + 5, fw, 5, fg());

    char right[16];
    if (joeReveal) snprintf(right, 16, "%ld", estHeight());
    else           snprintf(right, 16, "%s", "");
    if (right[0]) {
      int16_t x1, y1; uint16_t w, h;
      display.getTextBounds(right, 0, 0, &x1, &y1, &w, &h);
      display.setCursor(198 - w, SY + 3); display.print(right);
    }
  }

  bool callerFull = false;
  // Not depth-counted: the dock loop runs INSIDE a draw and redraws by calling
  // this again, so "outermost only" skipped every full while docked (a flip
  // to dark there came out as a grey partial). The one nested draw that must
  // not send is the FTCH pre-paint, and it says so with prePaint.
  int drawDepth = 0;
  bool menuOnGlass = false;            // the menu timed out on a minute tick
  void drawWatchFace() override {
    EpdFull::diag(display.epd2);
    justFull = false;                  // a new image: its partial is real
    // the library's own minute tick (or USB plug) draws through here and then
    // sends a partial we cannot stop: nobody else is calling
    bool libTick = drawDepth == 0 && !ownShow && !inDocked && !inVigil;
    drawDepth++;
    drawFaceBody();
    drawDepth--;
    esp_sleep_wakeup_cause_t why = esp_sleep_get_wakeup_cause();
    if (libTick && !prePaint &&
        (why == ESP_SLEEP_WAKEUP_TIMER || why == ESP_SLEEP_WAKEUP_EXT0)) {
      // the library's minute tick (or USB plug): it would now send its own
      // partial, without the "previous" sync. Send ours and sleep instead,
      // exactly as it would have right after it.
      bool full = fullArmed || menuOnGlass;
      fullArmed = false; menuOnGlass = false;
      if (full) displayFull();
      else if (!justFull) partialSync();   // a rest face may have gone full
      Serial.flush();
      deepSleep();
    }
    if (!prePaint && fullArmed) {
      fullArmed = false;
      if (!callerFull) displayFull();  // showFaceFull sends a full anyway
    }
    if (!prePaint) menuOnGlass = false;
  }
  void drawFaceBody() {
    sanitizeState();
    if (shipMode) {                     // USB in: out of the box. Anything else
      if (usbPresent()) { leaveShipMode("usb"); bootGenesis(); firstStartWifi(); }
      else { shipSleep(); return; }     // (a reset) goes back to sleep
    }

    // Out of charge: put up the resting screen once and stop. Checked before
    // anything else is drawn, because a panel refresh at 3.3 V is exactly
    // what browns the chip out mid-frame and leaves half an image on the
    // glass. Once resting, wakes only look for a charger.
    {
      int cs = chargeState();
      if (cs != 0 && resting) {           // plugged in: back to life
        resting = false;
      } else if (cs == 0 && battVoltsTrue() < 3.36f) {
        if (!resting) { resting = true; restingScreen(); }
        return;                           // nothing else gets drawn
      }
    }

    if (dispMode == M_JOE) { drawJoeFace(); return; }
    loadPrefs();
    themeDark = darkNow();
    long carried = catchUpWakeClock();   // minutes a long sleep skipped
    if (!buttonWake) {
      wakeMin += carried > 1 ? carried : 1;   // one tick per minute-wake,
                                              // or as many as were slept
      joeReveal = false;               // the reveal lasts one glance only
      // and so does a peek at the balance (not on the re-render after a wallet
      // sweep, which also arrives here without a button: that would undo
      // the peek the wearer just asked for)
      if (!inWalletScan) walletPeek = false;
    } else {
      wakeMin += carried;
    }
    // Hourly discharge sample.
    //
    // Only while running on the cell. It used to log on any wake an hour
    // apart, charger or not, so a run that ended on the charger filled the
    // last hours with 4.1 V readings — and those are the shape of a charge,
    // not a discharge. The log exists to fit the percentage curve, and a
    // curve fitted to that is worse than no curve.
    if ((wakeMin - vlogWake >= 60 || vlogWake == 0) && chargeState() == 0) {
      vlogWake = wakeMin;
      float v = batteryVolts();     // already the median of five
      vlog[vlogIdx] = (uint8_t)(v * 50.0f);
      // wrap explicitly: the index is a byte, and 256 is not a multiple of
      // the ring, so letting it overflow scrambled the order once in a while
      vlogIdx = (vlogIdx + 1) % VLOG_N;
      saveVlog();                   // so a flat cell cannot erase the evidence
      Serial.printf("[vlog] %02d  %.3fV raw  %.3fV true  %d%%\n",
                    vlogIdx, v, battVoltsTrue(), batteryPctShown());
    } else if (chargeState() != 0 &&
               vlog[(vlogIdx + VLOG_N - 1) % VLOG_N] != VLOG_CHG &&
               vlog[(vlogIdx + VLOG_N - 1) % VLOG_N] != 0) {
      // on the charger after a run: mark the break once, so three days of
      // log never read as one impossibly slow discharge
      vlog[vlogIdx] = VLOG_CHG;
      vlogIdx = (vlogIdx + 1) % VLOG_N;
      saveVlog();
      Serial.println("[vlog] charge mark");
    }
    if (!accelSeeded) scanAdcPins();   // once per cold boot
    chargeDiag();                      // silent unless a charger is present
    bool timerWake = !buttonWake && !prePaint &&
                     esp_reset_reason() == ESP_RST_DEEPSLEEP;
    if (!buttonWake && !inDocked) {
      bool wasIdle = watchIsIdle();
      if (movedSinceLastWake()) {
        lastMoveWake = wakeMin;
        // picked up after a long rest: the face is stale, so refresh now
        // rather than making the wearer wait for the next slot
        // (not when the last tries failed: offline, the backoff decides)
        if (wasIdle && failCount == 0) {
          forceFetch = true; Serial.println("[accel] picked up");
        }
      }
    }
    buttonWake = false;
    // Rest and night. Only on the minute wake: a button press always gets the
    // full face, and the press itself ends a rest (see handleButtonPress).
    if (timerWake && !inDocked && handleRest()) return;
    // BITCOIN IS TIME on every true boot (power-on, reflash, crash) —
    // but never on the once-a-minute deep-sleep wake. Reset reason has
    // real semantics; a sticky RTC flag survives too many resets.
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
      bootAnimation();
      paintedDark = true;             // every boot sequence ends on black
      qrDrawnIdx = -1;                // and nothing it drew is a QR
      // A restart has no data, so the first fetch (WiFi, then every
      // source: 10-20 s) runs before the face can draw. Say so on the boot
      // screen, or the last frame just sits there looking frozen.
      if (!haveData || forceFetch) syncBegin();
    }
    // wallet scans are slow (10 sequential TLS requests) — never do them
    // before the face is on screen. Render first, scan after (see the
    // hook at the end of this function). SCAN in the tag meanwhile.
    if (dispMode < 0 || dispMode >= NUM_MODES) dispMode = 0;
    if (dispMode == M_MEMP && travelActive) dispMode = M_HGHT;
    if (!faceShown(dispMode)) dispMode = nextFace(dispMode);
    if (tzIndex < 0 || tzIndex > 4) tzIndex = 0;
    if (themeMode < 0 || themeMode > 2) themeMode = 0;
    for (int i = 0; i < NUM_MODES; i++)  // index-ish RTC values must clamp,
      if (cellSel[i] >= cellStops(i)) cellSel[i] = 0;   // survives a stop count change
    if (curIdx >= NUM_CUR) curIdx = 0;
    if (walletView > 2) walletView = 0;  // never dereference

    // Due, and not while the radio is backed off: offline, a scan is the
    // same 12 s WiFi search the fetch backoff exists to stop, and a failed
    // scan used to stay due, so the wallet face searched on every wake.
    // And never on a button press: cycling faces with BACK and passing over
    // WAL used to stop there for a whole sweep. A press shows the balance
    // the watch already has; if the wearer is still on WAL at the next
    // minute wake, that wake sweeps. A long UP (forceFetch) still goes now.
    walletScanAsked = forceFetch;
    walletScanArmed = !inWalletScan && !inVigil && (dispMode == M_WALT) &&
      (forceFetch || ((timerWake || inDocked || walletDwell) && !radioBackedOff() &&
                      (!haveWallet || wakeMin - lastWalletMin >= walletEveryMin())));

    // A requested fetch paints FIRST, then goes to the radio. maybeFetch()
    // lives inside this function, so fetching here meant the request was
    // serviced and the flag cleared before a single pixel was drawn: FTCH
    // only ever showed on the dock, where the loop redraws, and never on the
    // wrist, where there is one draw per wake. Re-enter once to paint the
    // pending state, then fetch, then fall through and paint the result.
    // a fast partial wake from rest left a faint ghost: clean it now, on a
    // minute wake nobody is waiting on
    // ...and never while someone is looking at it: the press grace has to have
    // run out (a flash mid-demo, a minute after showing a face, was the
    // complaint), so the clean happens once the wrist is down
    if (timerWake && cleanNext && wakeMin >= awakeTill) { cleanNext = false; restExitFull = true; }
    if (fetchPending && !prePaint && !syncOn) {   // the sync screen says it
      prePaint = true;
      drawWatchFace();                // corner reads FTCH; no fetch inside
      // partial: fast, the full comes after. Unless it flips the panel:
      // then partial is exactly what fades, so this frame goes full
      if (themeDark == paintedDark) display.display(true);
      else displayFull();             // a flip is full, and full needs its off
      paintedDark = themeDark;
      prePaint = false;
    }
    if (!prePaint) maybeFetch();
    syncOn = false;                   // a boot that did not fetch: no screen
    // the fetch may just have failed and backed the radio off: then the
    // wallet sweep waits too (unless the wearer asked), and the corner must
    // not say SCAN for a sweep that will not happen
    if (!prePaint && walletScanArmed && !walletScanAsked && radioBackedOff()) {
      walletScanArmed = false;
      Serial.println("[wallet] sweep skipped: offline, backing off");
    }
    // an alarm someone has not seen yet takes the glass until a press
    if (!prePaint && evQueue && !qrOnGlass() && showEvent(timerWake)) return;
    display.fillScreen(bg());
    display.setTextColor(fg());
    display.setFont(NULL);   // 6x8 built-in for all small text

    // --- status row: clock | mode | LIVE/DEMO ---
    char clk[6];
    time_t lt = makeTime(currentTime);
    if (tzIndex > 0) {
      time_t utc = lt - settings.gmtOffset;
      lt = utc + tzOffsetSec(tzIndex, utc);
    }
    tmElements_t dt; breakTime(lt, dt);
    snprintf(clk, 6, "%02d:%02d", dt.Hour, dt.Minute);
    display.setCursor(10, 3);  display.print(clk);
    drawBattery(44, 3);
    { int16_t x1,y1; uint16_t w,h;
      const char *hdr = MODE_NAMES[dispMode];
      display.getTextBounds(hdr,0,0,&x1,&y1,&w,&h);
      display.setCursor((200-w)/2, 3); display.print(hdr); }
    // status tag: LIVE when fresh, OLD nH when stale, DEMO when never
    char tag[8];
    if (walletScanArmed) strcpy(tag, "SCAN");
    else if (captivePortal) strcpy(tag, "PRTL");  // a login page is in the way
    else if (travelActive) strcpy(tag, "TRVL");   // nothing here is now
    else if (fetchPending)
      strcpy(tag, (dispMode == M_WALT) ? "SCAN" : "FTCH");   // asked for, not
                                                            // yet delivered
    else if (inDocked) strcpy(tag, dockNetDown ? "DK-" : "DOCK");
    else if (chargeState() != 0) strcpy(tag, "CHG");   // asleep on purpose, so
                                                       // the cell gets the
                                                       // charger's current
    else if (!haveData) strcpy(tag, "DEMO");
    else {
      // the fetch cadence is 15 min, so 20 means a cycle was actually
      // missed. an hour of "LIVE" would cover three failures in a row.
      long e = (dispMode == M_HGHT || dispMode == M_MEMP) ? heightStaleMinutes()
                                                          : staleMinutes();
      if (e < staleAfter()) strcpy(tag, "LIVE");
      else if (e < 60) snprintf(tag, 8, "OLD %ldM", e);
      else             snprintf(tag, 8, "OLD %ldH", e / 60 > 48 ? 48 : e / 60);
    }
    display.setCursor(188 - (int)strlen(tag) * 6, 3);
    display.print(tag);
    if (alarmsArmed()) drawBell(188 - (int)strlen(tag) * 6 - 9, 3);

    if (dispMode == M_WALT && walletView > 0) {
      if (walletView == 2) drawLightning();
      else drawReceive();
      // FULL refresh when a QR first goes up: partial updates smear large
      // black<->white flips (ghost "LN..." fragments over the QR in dark
      // mode) and a QR must scan crisply. But the chain QR then sat there
      // flashing the whole panel every minute for the sake of the clock.
      // Same address, same theme: the minute's change is a few digits, so
      // partial, with a full one every 15 minutes to keep the code clean.
      bool sameQr = (walletView == 1 && qrDrawnIdx == recvIndex &&
                     paintedDark == themeDark && !restExitFull &&
                     qrPartials < 15);
      // A payment vigil about to start holds this wake for two minutes, so
      // the QR must be on the glass now, not when the caller sends it
      bool vigilNow = vigilPending && !inDocked && !inVigil;
      if (sameQr) { if (vigilNow) display.display(true); qrPartials++; }
      else if (vigilNow) {
        displayFull();
        qrPartials = 0;
        qrDrawnIdx = (walletView == 1) ? recvIndex : -1;
      } else {                        // the caller's partial is the minute
                                      // redraw (it used to be sent twice)
        fullOnClose();
        qrPartials = 0;
        qrDrawnIdx = (walletView == 1) ? recvIndex : -1;
      }
      restExitFull = false;
      paintedDark = themeDark;
      if (vigilPending && !inDocked && !inVigil) {
        vigilPending = false;
        paymentVigil();                   // stand watch ~2 min
      }
      return;
    }

    qrDrawnIdx = -1;                    // whatever QR was up, it is not now

    // --- big value + unit ---
    char big[16]; buildValue(big);
    // Eight digits will not fit at 32 px on a 200 px panel, and forcing them
    // into the box collided with its label. Long values step down a size
    // instead: the number stays the headline, it just sets smaller.
    drawGrouped(big, 46, fitFont(big));
    display.setFont(NULL);
    { const char *unit = MODE_UNITS[dispMode];
      static char ub[24];
      // only once an update has actually FAILED, not merely because we are
      // between fetches: at a 15 min cadence a >0 test fires for the last
      // five minutes of every normal cycle, which makes a healthy watch look
      // like a broken one. This also lines the line up with the LIVE -> OLD
      // tag, so both indicators change state together.
      if (dispMode == M_MEMP && !travelActive && !inDocked &&
          heightStaleMinutes() >= staleAfter()) {
        long e = heightStaleMinutes();
        if (e < 60) snprintf(ub, 24, "LAST SEEN %ld MIN AGO", e);
        else        snprintf(ub, 24, "LAST SEEN %ldH AGO", e / 60 > 48 ? 48 : e / 60);
        unit = ub;
      }
      if (dispMode == M_SUPL) {
        bool none = estHeight() >= 210000L * (LAST_HALVING + 1);
        unit = (cellSel[M_SUPL] % 2) ? (none ? "NO MORE BTC" : "BTC REMAINING")
                                     : (none ? "BTC ISSUED - ALL OF IT" : "BTC ISSUED");
      }
      if (!travelActive && dispMode == M_HGHT && !inDocked &&
          heightStaleMinutes() >= staleAfter() && estBlocksAhead() > 0) {
        // dead-reckoning: name the estimate and its error bar
        // ASCII only: the built-in GFX font maps high bytes via CP437 and
        // would draw a shaded block instead of a proper plus-minus
        snprintf(ub, 24, "EST +%d BLK +/-%d",
                 estBlocksAhead(), estUncertainty());
        unit = ub;
      }
      if (travelActive) {
        static char tu[26];
        // the past is recorded in dollars (the anchors and mempool's
        // history both are), so say USD there whatever the dial is on;
        // ahead, the model scales today's price, in today's currency
        bool past = estHeight() < blockHeight;
        const char *cc = CUR_CODES[curIdx];   // the past via the currency's own old rate
        double tp = past ? pastPriceCur(estHeight()) : modelPrice(estHeight());
        if (dispMode == M_PRICE) {
          snprintf(tu, 26, "%s / BTC", cc);
          unit = tu;
        } else if (dispMode == M_SATS) {
          snprintf(tu, 26, satsFlip(tp) ? "%s / SAT" : "SAT / %s", cc); unit = tu;
        } else if (dispMode == M_MCAP) {
          double cb = supplyBTC(estHeight()) * tp / 1e9;
          snprintf(tu, 26, capInTrillions(cb) ? "TRILLION %s" : "BILLION %s",
                   cc); unit = tu;
        } else if (dispMode == M_FEES) {
          unit = "NOT DERIVABLE";
        }
        // everything below is a present-mode unit: it does not apply here
      } else if (dispMode == M_PRICE) {
        snprintf(ub, 24, "%s / BTC", CUR_CODES[curIdx]);
        unit = ub;
      } else if (dispMode == M_SATS) {
        snprintf(ub, 24, satsFlip(btcPrice) ? "%s / SAT" : "SAT / %s",
                 CUR_CODES[curIdx]);
        unit = ub;
      } else if (dispMode == M_MCAP) {
        snprintf(ub, 24, capInTrillions(btcMcapB) ? "TRILLION %s" : "BILLION %s",
                 CUR_CODES[curIdx]);
        unit = ub;
      } else if (!travelActive && dispMode == M_HALV && estHeight() > 0) {
        static const char *MN[13] = {"","JAN","FEB","MAR","APR","MAY",
          "JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
        int t = cellSel[dispMode] % 6;
        int era = (int)(estHeight() / 210000L);
        long targetH = (long)(era + 1 + t) * 210000L;
        long blocksLeft = targetH - estHeight();
        if (blocksLeft < 0) blocksLeft = 0;
        int64_t utc = (int64_t)makeTime(currentTime) - settings.gmtOffset;
        if (travelActive)                           // 64-bit: past 2038
          utc += (int64_t)((travelHeight - blockHeight) * (double)avgBlockSec);
        int64_t when = utc + (int64_t)secondsUntil(targetH);
        Ymd hd = civil64(when);
        // Precision the projection can actually carry. Hashrate growth has
        // landed every halving so far ahead of the ten-minute estimate, and
        // over tens of thousands of blocks that is weeks, not days — so a
        // day is only shown once the target is close enough to mean it.
        if (blocksLeft < 4032)                    // within two epochs: a date
          snprintf(ub, 24, "#%d - %s %d",
                   era + 1 + t, MN[hd.m], hd.d);
        else                                      // beyond that: the month
          snprintf(ub, 24, "#%d - %s %d",
                   era + 1 + t, MN[hd.m], hd.y);
        unit = ub;
      }
      if (dispMode == M_WALT && haveWallet && walletSats < 1000000ULL)
        unit = "SATS BALANCE";
      if (dispMode == M_WALT && isDemoWallet())
        unit = "SATOSHI - GENESIS";     // never mistaken for the wearer's

      int16_t x1,y1; uint16_t w,h;
      display.getTextBounds(unit,0,0,&x1,&y1,&w,&h);
      display.setCursor((200-w)/2, 49); display.print(unit); }

    // --- middle band: EPOCH subdial | cell | HALVING subdial ---
    // (the difficulty adjustment "readjusts every 2016 ticks" — the
    //  conductor of the orchestra; the halving epoch is the calendar year)
    long eh = estHeight();
    float epochPct = eh > 0 ? (eh % 2016L)   / 2016.0f   : 0;
    float halvPct  = eh > 0 ? (eh % 210000L) / 210000.0f : 0;
    drawSubdial(26, 80, 16, epochPct, "EPOC");    // 4 px out from the box
    drawSubdial(174, 80, 16, halvPct, "HALV");

    // single-line cell: [ value   LABEL ] — label beside the number,
    // bottoms aligned, instead of stacked underneath
    // HGT and MEM (live): a taller, wider box with a second line that says
    // what the number means. Elsewhere the one-line box, as before.
    const bool tall = (dispMode == M_HGHT || dispMode == M_MEMP) && !travelActive;
    const int bx = tall ? 50 : 52, bw = tall ? 101 : 96;
    const int vy = tall ? 81 : 84, ly = tall ? 74 : 77;   // value baseline, label row
    if (tall) display.drawRect(50, 63, 101, 34, fg());
    else      display.drawRect(52, 66, 96, 22, fg());
    char line2[20] = "";
    char cell[18]; const char *cellLabel;   // 18: "POWER LAW MODEL" plus
                                           // room, not 12 which truncates it
    int hashExp = -1;          // MEM HASHES: the power of ten after the digits
    bool cellIsText = false;   // pool names are letters: the 7-seg
                               // font is digits-only, like real ones
    // THE BOX: each mode's contextual sub-dial; UP turns it.
    if (travelActive && (dispMode == M_PRICE || dispMode == M_SATS ||
                         dispMode == M_MCAP)) {
      // one statement of provenance for the whole trip, so the unit lines do
      // not have to carry "- MODEL" on every mode
      cellLabel = "";
      strcpy(cell, estHeight() >= blockHeight ? "POWER LAW MODEL"
             : (travelPastFor == estHeight() && travelPastPrice > 0)
                 ? "RECORDED" : "RECORDED, APPROX");
      cellIsText = true;
    } else if (travelActive && dispMode == M_FEES) {
      cellLabel = "";                  // no model for congestion, and none
      strcpy(cell, "NOT DERIVABLE");   // invented
      cellIsText = true;
    } else if (travelActive && dispMode == M_WALT) {
      // what the balance is worth at that date, in the selected currency
      double mp = estHeight() < blockHeight ? pastPriceCur(estHeight())
                                            : modelPrice(estHeight());
      cellLabel = CUR_CODES[curIdx];
      cellIsText = true;                 // separators need the text font
      if (mp <= 0 || !haveWallet) strcpy(cell, "----");
      else fmtMoney(cell, 18, (double)walletSats / 1e8 * mp);
    } else if (dispMode == M_MEMP) {   // the queue, then HGT's chain stats
      // UP walks: what's waiting, what the last block carried and paid, how
      // fast blocks come, what the retarget will do, the hashrate. (MINER
      // stays on HGT: the cubes below already name every pool.)
      int t = cellSel[M_MEMP] % cellStops(M_MEMP);
      if (t == 1) {
        cellLabel = "TXS";
        if (lastTxCount > 0) snprintf(cell, 18, "%ld", lastTxCount);
        else strcpy(cell, "-");
        boxLineBlock(line2);
      } else if (t == 2) {
        cellLabel = "BTC RWD";
        snprintf(cell, 18, "%.2f", lastRewardBtc);
        boxLineFees(line2);
      } else if (t == 3) {
        cellLabel = "MIN/BLK";
        snprintf(cell, 18, "%.1f", avgBlockSec / 60.0f);
        strcpy(line2, "AIM: 10 MINUTES");
      } else if (t == 4) {
        cellLabel = "RETARGET";
        snprintf(cell, 18, fabs(diffChangeEst) >= 10 ? "%s%.0f" : "%s%.1f",
                 diffChangeEst < 0 ? "-" : "", fabs(diffChangeEst));
        boxLineRetarget(line2);
      } else if (t == 5) {
        strcpy(line2, "ALL MINERS NOW");
        cellLabel = "EH/S";
        if (netHashEH >= 1000)  snprintf(cell, 18, "%.0f", netHashEH);
        else if (netHashEH > 0) snprintf(cell, 18, "%.1f", netHashEH);
        else strcpy(cell, "-");
      } else if (t == 6) {
        // the work since the last block: hashrate x time, "5.5x10^23", each
        // one a guess at the next block; it starts over when one lands. Only
        // while live: from old data it would count past blocks never seen.
        cellLabel = "HASHES";
        strcpy(cell, "-");
        strcpy(line2, "SINCE LAST BLOCK");
        if (tipBlockTime > 0 && netHashEH > 0 && !memOld()) {
          time_t nowUtc = makeTime(currentTime) - settings.gmtOffset;
          double sec = (double)((long)nowUtc - (long)tipBlockTime);
          if (sec < 1) sec = 1;
          double hs = netHashEH * 1e18 * sec;
          int e = (int)floor(log10(hs));
          double m = hs / pow(10.0, e);
          if (m >= 9.95) { m /= 10; e++; }
          snprintf(cell, 18, "%.1f", m);
          hashExp = e;
        }
      } else {
        cellLabel = "BLK WAITING";
        if (mempoolBlocks > 0) snprintf(cell, 18, "%d", (int)ceilf(mempoolBlocks));
        else strcpy(cell, "-");        // "----" would run into the label
        if (mempoolBlocks > 0 && avgBlockSec > 0) {     // at the chain's pace
          long mins = (long)(ceilf(mempoolBlocks) * avgBlockSec / 60.0f + 0.5f);
          if (mins < 100) snprintf(line2, 20, "~%ld MIN TO CLEAR", mins);
          else            snprintf(line2, 20, "~%ldH TO CLEAR", (mins + 30) / 60);
        }
      }
    } else if (dispMode == M_FEES) {   // fee tiers, same fetch
      int t = cellSel[dispMode] % 3;
      cellLabel = t == 0 ? "HIGH PRI" : t == 1 ? "MED PRI" : "LOW PRI";
      fmtFee(cell, 12, t == 0 ? fastFee : t == 1 ? medFee : lowFee);
    } else if (dispMode == M_SUPL) {
      cellLabel = "% MINED";        // the box stays fixed; the dial now moves
      snprintf(cell, 18, "%.2f",    // the headline instead
               supplyBTC(estHeight()) / supplyCapBTC() * 100.0);
    } else if (dispMode == M_HALV) {   // the subsidy the SELECTED
      int t = cellSel[dispMode] % 6;            // halving brings — box and date
      int era = estHeight() > 0 ? (int)(estHeight() / 210000L) : 4;
      int k = era + 1 + t;
      double sub = 50.0 / (double)(1LL << (k < 62 ? k : 62));
      long long ss = subsidySats(k);
      static const char *HL[6] =
        {"B NEXT", "B +2", "B +3", "B +4", "B +5", "B +6"};
      static const char *HS[6] =
        {"S NEXT", "S +2", "S +3", "S +4", "S +5", "S +6"};
      if (ss == 0) {                            // 0.0000 was true, and useless
        cellLabel = ""; strcpy(cell, "NO MORE BTC"); cellIsText = true;
      } else if (ss < 1000000LL) {              // under 0.01: whole sats
        cellLabel = HS[t]; snprintf(cell, 18, "%lld", ss);
      } else {
        cellLabel = HL[t];
        snprintf(cell, 18, sub >= 1 ? "%.3f" : "%.4f", sub);
      }
    } else if (dispMode == M_SATS) {
      // ONE SAT, in the face's currency: the other side of the big number,
      // and the useful one for small payments (a 2,000-sat zap is about
      // 2.40 USD). Moscow time used to sit here; it was the big number
      // again, set as a clock. The currency is named just above.
      cellLabel = "/SAT";
      double pr = !travelActive ? btcPrice
                : (estHeight() < blockHeight ? pastPriceCur(estHeight())
                                             : modelPrice(estHeight()));
      double v = pr > 0 ? pr / 1e8 : 0;
      if (v <= 0)        strcpy(cell, "----");
      else if (v >= 100) snprintf(cell, 18, "%.0f", v);
      else if (v >= 1)   snprintf(cell, 18, "%.2f", v);
      else if (v >= 0.1) snprintf(cell, 18, "%.3f", v);
      else               snprintf(cell, 18, "%.4f", v);
    } else if (dispMode == M_MCAP) {
      cellLabel = "% GOLD";                // the thesis meter: BTC's
      float usdMcapB = fxRate[0] > 0 && blockHeight > 0
                       ? fxRate[0] * supplyBTC(blockHeight) / 1e9 : 0;
      if (goldMcapB > 0 && usdMcapB > 0)   // mcap vs all mined gold,
        snprintf(cell, 18, "%.1f", usdMcapB / goldMcapB * 100.0f);
                                           // USD both legs: the ratio
                                           // is currency-invariant
      else snprintf(cell, 18, "--.-");
    } else if (dispMode == M_HGHT && travelActive) {
      // The subdials travel by themselves — they read estHeight(). The box
      // cannot: reward-with-fees, miner, retarget, pace, block age and the
      // mempool are all facts about the present tip. But the SUBSIDY at any
      // height is exact arithmetic, so that is what the box shows out there.
      int era = (int)(estHeight() / 210000L);
      double sub = 50.0; for (int i = 0; i < era && i < 33; i++) sub /= 2.0;
      cellLabel = "SUB";
      // eight decimals in DSEG is 110 px inside a 96 px box — it ran straight
      // through the HALV dial. Trim the trailing zeros and set it as text.
      snprintf(cell, 18, "%.8f", sub);
      { int L = strlen(cell);
        while (L > 1 && cell[L-1] == '0') cell[--L] = 0;
        if (L > 1 && cell[L-1] == '.') cell[--L] = 0; }
      cellIsText = true;
    } else if (dispMode == M_HGHT) {
      // the timechain box, in the order that matters: what the last
      // block PAID, WHO found it, what the retarget will DO, what's
      // WAITING. SAT/CAP carry the 3-stop version (24H%% mid-slot).
      int t = cellSel[dispMode] % cellStops(M_HGHT);   // seven stops
      if (t == 0) {
        cellLabel = "BTC RWD";         // subsidy + tx fees
        snprintf(cell, 18, "%.2f", lastRewardBtc);
        boxLineFees(line2);
      } else if (t == 1 && dispMode == M_HGHT) {
        cellLabel = "MINER";           // who found the last block
        snprintf(cell, 18, "%s", poolName[0] ? poolName : "...");
        cellIsText = true;
        if (recPool[0][0]) {           // its share of the cubes on MEM
          int n = 0, of = 0;
          for (int i = 0; i < 6; i++) if (recH[i] > 0 && recPool[i][0]) {
            of++; if (!strcmp(recPool[i], recPool[0])) n++;
          }
          if (of > 1) snprintf(line2, 20, "%d OF THE LAST %d", n, of);
        }
      } else if ((t == 2 && dispMode == M_HGHT) ||
                 (t == 1 && dispMode != M_HGHT)) {
        if (dispMode == M_HGHT) {
          cellLabel = "RETARGET";      // names the event, not jargon.
          // decimals yield when magnitude goes double-digit, or the
          // value collides with the 8-char label (-12.3 vs RETARGET)
          snprintf(cell, 18, fabs(diffChangeEst) >= 10 ? "%s%.0f" : "%s%.1f",
                   diffChangeEst < 0 ? "-" : "", fabs(diffChangeEst));
          boxLineRetarget(line2);
        } else {
          cellLabel = "24H %";
          snprintf(cell, 18, "%s%.1f",
                   btcChange24h < 0 ? "-" : "", fabs(btcChange24h));
        }
      } else if (t == 4 && dispMode == M_HGHT) {
        // what the block carried. The reward says what it paid, the miner
        // says who found it; this says how much work it actually settled.
        cellLabel = "TXS";
        if (lastTxCount > 0) snprintf(cell, 18, "%ld", lastTxCount);
        else                 snprintf(cell, 18, "%s", "----");
        boxLineBlock(line2);
      } else if (t == 5 && dispMode == M_HGHT) {
        // what is actually defending the chain. Exahash keeps it to three or
        // four digits for years; below 1000 EH/s show a decimal, above it
        // drop to whole units so the cell never overflows.
        cellLabel = "EH/S";
        strcpy(line2, "ALL MINERS NOW");
        if (netHashEH >= 1000)     snprintf(cell, 18, "%.0f", netHashEH);
        else if (netHashEH > 0)    snprintf(cell, 18, "%.1f", netHashEH);
        else                       snprintf(cell, 18, "%s", "----");
      } else if (t == 3 && dispMode == M_HGHT) {
        // how fast the chain is actually running: the same figure the
        // dead-reckoning uses, so a wearer can see why an estimate drifts
        cellLabel = "MIN/BLK";
        snprintf(cell, 18, "%.1f", avgBlockSec / 60.0f);
        strcpy(line2, "AIM: 10 MINUTES");
      } else {
        strcpy(line2, "BLOCKS WAITING");
        cellLabel = "UNCONF";          // blocks-worth, abbreviated:
        snprintf(cell, 18, "%.1f", mempoolBlocks); // UNCONFIRMED
      }                                            // overflows the box
    } else if (dispMode == M_WALT && walletPrivate && !walletPeek) {
      cellLabel = "PRIVATE";    // the last movement is as telling as the
      strcpy(cell, "----");     // balance, so it hides behind the same switch
    } else if (dispMode == M_WALT) {
      cellLabel = "TX";         // the wallet's last movement: received
      long long shown = lastTxSats;
      static char confLbl[6];
      if (trackState != TRACK_NONE) {   // a payment on its way in: its
        snprintf(confLbl, 6, "%d/6", trackConfs());   // confirmations so far
        cellLabel = confLbl;
        if (trackState == TRACK_ON && trackSats > 0) shown = trackSats;
      }
      long long a = shown < 0 ? -shown : shown;
      const char *plus = shown > 0 ? "+" : "";   // deposits wear
      if (shown == 0) snprintf(cell, 18, "----"); // their sign
      else if (a < 100000)
        snprintf(cell, 18, "%s%lld", plus, shown);
      else {                    // large moves read better in BTC
        double b = shown / 1e8;
        snprintf(cell, 18, (fabs(b) < 10) ? "%s%.3f" : "%s%.1f", plus, b);
      }
    } else {
      cellLabel = "24H %";
      snprintf(cell, 18, "%s%.1f", btcChange24h < 0 ? "-" : "", fabs(btcChange24h));
    }
    if (cellIsText) {
      display.setFont(NULL);
      display.setCursor(tall ? 55 : 57, ly);   // letters: small caps font,
      display.print(cell);             // aligned with the label row
    } else {
      // HASHES: two pixels further left and a 3 x 3 times sign, so
      // "5.5x10^23" clears the label
      printCellValue(cell, bx + (hashExp >= 0 ? 4 : 5), vy);
      if (hashExp >= 0) {              // "x10" small, the power raised
        int ex = display.getCursorX() + 1;
        for (int i = 0; i < 3; i++) {
          display.drawPixel(ex + i, ly + 2 + i, fg());
          display.drawPixel(ex + 2 - i, ly + 2 + i, fg());
        }
        char es[6]; snprintf(es, 6, "%d", hashExp);
        display.setFont(NULL);
        lowAt("10", ex + 4, ly);
        lowAt(es, ex + 15, ly - 4);
      }
    }
    display.setFont(NULL);
    display.setCursor((tall ? 148 : 146) - (int)strlen(cellLabel) * 6, ly);
    display.print(cellLabel);
    if (tall && line2[0]) {            // centred under the value and label
      display.setCursor(bx + (bw - (int)strlen(line2) * 6) / 2 + 1, 86);
      display.print(line2);
    }

    // The lower half belongs to the face where the gauge and the 24H chart
    // say nothing about it. SUP and HLV are arithmetic on the height, so
    // they travel; FEE and MEM are about now, so travelling they fall
    // through to the travel view below. HGT, PRC, SAT, CAP, WAL unchanged.
    if (dispMode == M_SUPL) drawSupLower();
    else if (dispMode == M_HALV) drawHalvLower();
    else if (dispMode == M_FEES && !travelActive) drawFeeLower();
    else if (dispMode == M_MEMP && !travelActive) drawMemLower();
    else if (dispMode == M_SATS && !travelActive) drawSatLower();
    else if (dispMode == M_MCAP && !travelActive) drawCapLower();
    else if (travelActive && dispMode == M_PRICE) drawPrcTravel();
    else if (travelActive && dispMode == M_SATS)  drawSatTravel();
    else if (travelActive && dispMode == M_MCAP)  drawCapTravel();
    else if (travelActive && dispMode == M_FEES)  drawFeeTravel();
    else if (travelActive && dispMode == M_WALT)  drawWalTravel();
    else if (travelActive) {
      // Sentiment and the 24h chart are facts about now; the supply bar and
      // emission curve that stood here live on SUP and HLV. HGT says where
      // on the chain you are.
      drawHgtTravel();
    } else {

    // --- fear/greed gauge ---
    int ry = 115;                      // 2 px under the taller HGT box
    display.drawFastHLine(6, ry, 188, fg());
    for (int i = 0; i <= 20; i++)
      display.drawFastVLine(6 + i * 188 / 20, ry - (i%5==0 ? 8 : 4),
                            (i%5==0 ? 8 : 4), fg());
    int nx = 6 + fearGreed * 188 / 100;
    display.fillTriangle(nx-4, ry-15, nx+4, ry-15, nx, ry-11, fg());
    const char *nums[5] = {"0","25","50","75","100"};
    for (int i = 0; i < 5; i++) {
      int x = 6 + i * 188 / 4;
      int16_t x1,y1; uint16_t w,h;
      display.getTextBounds(nums[i],0,0,&x1,&y1,&w,&h);
      display.setCursor(constrain(x - (int)w/2, 2, 198-(int)w), ry + 3);
      display.print(nums[i]);
    }
    display.setCursor(6, ry + 13);  display.print("FEAR");
    char fgs[16]; snprintf(fgs, 16, "SENTIMENT %d", fearGreed);
    { int16_t x1,y1; uint16_t w,h;
      display.getTextBounds(fgs,0,0,&x1,&y1,&w,&h);
      display.setCursor((200-w)/2, ry + 13); display.print(fgs); }
    display.setCursor(194 - 5*6, ry + 13); display.print("GREED");

    // --- boxed 24H chart with H/L ---
    int cy = 138, ch = 36;
    display.drawRect(6, cy, 188, ch, fg());
    float hi, lo; int n; sparkHiLo(hi, lo, n);
    if (!n) { display.setCursor(10, cy + 3); display.print("24H"); }
    if (n) {
      float rng = (hi - lo) > 1 ? (hi - lo) : 1;

      // Where do the H/L labels go? They live top-right by default, but a
      // rising 24h puts the trace exactly there and the two collide. So look
      // at where the line actually runs in the right-hand third and move the
      // labels to the bottom-left when that corner is occupied.
      // Rather than guess from the trace's extremes, test the label boxes
      // against the trace itself: walk the line once, and for each candidate
      // position ask whether any point falls inside it. First clear slot wins.
      // A corner can look free while the line runs straight through the
      // middle of it, which is how the labels kept landing on the chart.
      char hstr[16], lstr[16];
      snprintf(hstr, 16, "H %.0f", hi);
      snprintf(lstr, 16, "L %.0f", lo);
      int pw = (int)(strlen(hstr) > strlen(lstr) ? strlen(hstr) : strlen(lstr)) * 6;

      // Both arrays use ONE convention: 0 = top right, 1 = top left,
      // 2 = bottom right, 3 = bottom left. They did not before — the caption's
      // x array ran left-first — so a preference list written for one was
      // silently mirrored for the other, and "top left" asked for top right.
      int pairX[4] = { 188 - pw, 8,        188 - pw,     8            };
      int pairY[4] = { cy + 2,   cy + 2,   cy + ch - 21, cy + ch - 21 };
      int tagX[4]  = { 170,      8,        170,          8            };
      int tagY[4]  = { cy + 2,   cy + 2,   cy + ch - 10, cy + ch - 10 };

      int  pairHits[4] = { 0, 0, 0, 0 };
      int  tagHits[4]  = { 0, 0, 0, 0 };
      { int k2 = 0, px2 = -1, py2 = -1;
        for (int i = 0; i < SPARK_POINTS; i++) {
          int idx = (sparkHead + i) % SPARK_POINTS;
          if (spark[idx] <= 0) continue;
          int x = 9 + (k2 * 182) / max(n - 1, 1);
          int y = cy + ch - 4 - (int)((spark[idx] - lo) / rng * (ch - 10));

          // Test the SEGMENT, not the point. Twenty-four samples across
          // 182 px sit 8 px apart, so a line can cross a label box without
          // any sampled point landing inside it — which is how the trace kept
          // running through 24H while the corner tested clear.
          int ax = px2 < 0 ? x : px2, ay = px2 < 0 ? y : py2;
          int lx = ax < x ? ax : x, hx = ax < x ? x : ax;
          int ly = ay < y ? ay : y, hy = ay < y ? y : ay;
          for (int c = 0; c < 4; c++) {
            if (hx >= pairX[c] - 2 && lx <= pairX[c] + pw + 2 &&
                hy >= pairY[c] - 1 && ly <= pairY[c] + 18) pairHits[c]++;
            if (hx >= tagX[c] && lx <= tagX[c] + 18 &&
                hy >= tagY[c] && ly <= tagY[c] + 8)        tagHits[c]++;
          }
          px2 = x; py2 = y; k2++;
        }
      }

      // 24H belongs top left and stays there unless the trace actually runs
      // through it; the H/L pair belongs on the right, dropping to the bottom
      // right when a rising trace takes the top. Both only move for a real
      // overlap, so the layout is steady between refreshes.
      const int tagPref[4]  = { 1, 3, 0, 2 };   // TL, BL, TR, BR
      const int pairPref[4] = { 0, 2, 3, 1 };   // TR, BR, BL, TL

      // First choice: the preferred corner the trace does not cross at all.
      // If it crosses every one — a busy day fills the box — take the corner
      // it crosses LEAST, rather than falling back on a fixed default and
      // drawing straight through the line.
      int tag = -1;
      for (int i = 0; i < 4 && tag < 0; i++)
        if (tagHits[tagPref[i]] == 0) tag = tagPref[i];
      if (tag < 0) {
        tag = tagPref[0];
        for (int i = 1; i < 4; i++)
          if (tagHits[tagPref[i]] < tagHits[tag]) tag = tagPref[i];
      }

      int pair = -1, best = -1;
      for (int i = 0; i < 4; i++) {
        int c = pairPref[i];
        bool clashTag = (pairX[c] < tagX[tag] + 22 &&
                         pairX[c] + pw > tagX[tag] - 4 &&
                         pairY[c] < tagY[tag] + 10 &&
                         pairY[c] + 18 > tagY[tag] - 2);
        if (clashTag) continue;                    // never over the caption
        if (pairHits[c] == 0) { pair = c; break; } // clean: take it
        if (best < 0 || pairHits[c] < pairHits[best]) best = c;
      }
      if (pair < 0) pair = (best >= 0) ? best : pairPref[0];

      display.setCursor(tagX[tag], tagY[tag] + 1);
      display.print("24H");
      display.setCursor(pairX[pair], pairY[pair] + 1);  display.print(hstr);
      display.setCursor(pairX[pair], pairY[pair] + 10); display.print(lstr);

      // sparkline inside the box
      int px = -1, py = -1, k = 0;
      for (int i = 0; i < SPARK_POINTS; i++) {
        int idx = (sparkHead + i) % SPARK_POINTS;
        if (spark[idx] <= 0) continue;
        int x = 9 + (k * 182) / max(n - 1, 1);
        int y = cy + ch - 4 - (int)((spark[idx] - lo) / rng * (ch - 10));
        if (px >= 0) display.drawLine(px, py, x, y, fg());
        px = x; py = y; k++;
      }
    }

    }   // end travel-face swap

    // --- divider title + mode strip ---
    const char *THEME_NAMES[3] = {"THEME: LIGHT","THEME: DARK","THEME: AUTO"};
    // the mantra lives in every mode — the blocks tick whether you're
    // watching them or not. The brand keeps the splash and the case;
    // the owner doesn't need their own watch introduced six modes a day.
    // Travelling, the divider carries where you have gone: the destination
    // projected from the chain's own measured pace, not from ten minutes.
    // It sits in the mantra's slot, which is the right trade — the mantra is
    // about now, and now is the one thing this face is not showing.
    static char destLine[26];
    if (travelActive) {
      long dB = travelHeight - blockHeight;
      static const char *MON[12] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                    "JUL","AUG","SEP","OCT","NOV","DEC"};
      Ymd c = civil64(heightUtc(travelHeight));
      snprintf(destLine, 26, "%s %s %d",
               dB >= 0 ? "ARRIVING" : "ARRIVED", MON[c.m - 1], c.y);
    }
    const char *title = travelActive     ? destLine
                      : showThemeName    ? THEME_NAMES[themeMode]
                      : (dispMode == M_WALT && walletPrivate && !walletPeek &&
                         walletView == 0) ? "DOWN = PEEK"
                      : (dispMode == M_WALT) ? "UP = RECEIVE QR"
                      : (dispMode == M_MEMP) ? memTitle()
                                             : "TICK TOCK NEXT BLOCK";
    showThemeName = false;
    { int16_t x1,y1; uint16_t w,h;
      display.getTextBounds(title,0,0,&x1,&y1,&w,&h);
      int tx = (200-w)/2;
      // centred in the band between the chart's bottom edge (173) and the
      // mode strip (186): an 8 px line at 176 leaves 2 px either side, where
      // 178 left 4 above and nothing below
      display.setCursor(tx, 176); display.print(title);
      display.drawFastHLine(6, 179, tx - 10, fg());
      display.drawFastHLine(tx + w + 4, 179, 194 - (tx + w + 4), fg()); }
    drawModeStrip();
    // the theme changed since the last paint (DOWN, or AUTO at its hours):
    // a whole-panel flip, which a partial refresh only half drives
    if (!prePaint && themeDark != paintedDark) restExitFull = true;
    // Partial refreshes only nudge the pixels that change, and on this panel
    // the rest drift: after an hour of minute ticks (or a trip through the
    // menu) the face goes grey and earlier screens show through. So every
    // PARTIALS_BEFORE_FULL minute ticks, the tick is a full refresh instead.
    // Only on a timer wake: never in the middle of someone pressing buttons.
    if (!prePaint && timerWake && facePartials < 255) facePartials++;
    if (!prePaint && timerWake && facePartials >= PARTIALS_BEFORE_FULL &&
        wakeMin >= awakeTill)                     // not while being looked at
      restExitFull = true;
    // A different face (or WAL view) went up by partial: that is the redraw
    // that leaves the old one showing through. Clean once nobody is looking.
    // Small presses on the same face (UP cycling the box) leave no trace.
    if (!prePaint && (drawnMode != dispMode || drawnView != walletView)) {
      if (drawnMode >= 0 && !restExitFull) cleanNext = true;
      drawnMode = dispMode; drawnView = walletView;
    }
    if (restExitFull && !prePaint) facePartials = 0;
    if (restExitFull && !prePaint) { // first face after a rest: full refresh,
      restExitFull = false;          // so the old image leaves no ghost (not
                                     // the FTCH frame: that one is the fast
                                     // answer, the full comes with the data)
      fullOnClose();                 // the caller's refresh, made full: one
                                     // refresh, not a full and then a partial
    }
    if (!prePaint) paintedDark = themeDark;   // what the panel now shows

    // first WAL entry / due rescan: the face above goes to the panel
    // NOW, then the slow address sweep runs, then we re-render.
    if (walletScanArmed && !inWalletScan) {
      inWalletScan = true;
      scanCancelled = -1;
      watchButtonsDuringScan(true);
      display.display(true);              // instant frame, tag = SCAN
      offIfFull();                        // the scan keeps it awake
      bool ok = fetchWallet();
      watchButtonsDuringScan(false);       // disarm on EVERY path out, or the
                                           // handlers stay live and fire
                                           // during ordinary button use
      if (ok) lastWalletMin = wakeMin;
      else if (scanCancelled < 0) backOffRadio();   // failed, not cancelled:
                                                    // wait like the fetch does
      fetchPending = false;                // the sweep is over either way
      Serial.printf("[wallet] sweep %s\n",
                    ok ? "OK" : (scanCancelled >= 0 ? "CANCELLED" : "FAIL"));
      // the press that cancelled the scan still has to do its job, or the
      // watch just sits there having ignored you
      if (scanCancelled >= 0) {
        int btn = scanCancelled; scanCancelled = -1;
        waitRelease(btn);
        buttonWake = true;
        if (btn == BACK_BTN_PIN) {
          backPressed();                           // next instrument, or home
          lastWalletMin = wakeMin;                 // do not immediately rescan
        } else if (btn == UP_BTN_PIN) {
          walletToggleView();
        } else if (btn == DOWN_BTN_PIN) {
          downPressed();
        } else if (btn == MENU_BTN_PIN) {
          inWalletScan = false;
          openMenu();
          return;
        }
      }
      drawWatchFace();                    // fresh balance (or honest stale)
      inWalletScan = false;
      return;                             // caller displays the re-render
    }

    #ifdef ARDUINO_ESP32S3_DEV
    // on USB power, hand over to the realtime docked loop. Read the
    // pin DIRECTLY: the library's cached USB flag can go stale after
    // a charger flap, stranding a plugged watch in deep sleep. Ground
    // truth every wake means worst-case dock recovery is one minute.
    if (!inDocked && guiState == WATCHFACE_STATE) {
      pinMode(USB_DET_PIN, INPUT);
      if (digitalRead(USB_DET_PIN) == 1) dockedLoop();
    }
    #endif
  }

  // ---------------- menu: stock items + Timezone ----------------
  // Two levels. The main menu holds what gets used; Settings holds what gets
  // set once and left (eleven items in one list had become a scroll).
  static const int MENU_ROWS   = 8;        // 22 px a row from y 22: a ninth
                                           // row's descenders run off the panel
  static const int MAIN_LEN = 6, MY_LEN = 8, SET_LEN = 7;
  // most used first; About, a status page, last
  static const int MYWATCH_POS = 2, SETTINGS_POS = 4;
  const char *const MAIN_ITEMS[MAIN_LEN] = {
    "Alarms", "Time Travel", "My Watch  >", "Setup Wallet", "Settings  >",
    "About BWATCH"};
  const char *const MY_ITEMS[MY_LEN] = {
    "Faces", "Currency", "Dark Hours", "Night Hours", "Haptics", "Battery",
    "Privacy", "Birthdays"};
  const char *const SET_ITEMS[SET_LEN] = {
    "Set Time", "Set Timezone", "Sync NTP", "Setup WiFi", "Networks",
    "Start Screen", "Ship Mode"};
  int menuLen() { return menuLevel == 1 ? MY_LEN : menuLevel == 2 ? SET_LEN : MAIN_LEN; }
  int levelPos() { return menuLevel == 1 ? MYWATCH_POS : SETTINGS_POS; }
  void menuStep(int d) {
    int n = menuLen();
    menuIndex = ((menuIndex + d) % n + n) % n;
    myShowMenu(menuIndex, true);
  }
  // From the face: always the main menu. A wake that timed out inside
  // Settings would otherwise reopen the submenu with no way to tell.
  void openMenu() {
    if (menuLevel) { menuIndex = levelPos(); menuLevel = 0; }
    if (menuIndex >= MAIN_LEN) menuIndex = 0;
    myShowMenu(menuIndex, false);
  }
  // BACK in a menu: Settings goes up to the main menu (true); the main menu
  // leaves to the face (false: the caller does that, as it always has).
  bool menuBack() {
    if (!menuLevel) return false;
    menuIndex = levelPos(); menuLevel = 0;
    myShowMenu(menuIndex, false);
    return true;
  }

  void myShowMenu(byte idx, bool partial) {
    epdLog("menu");
    menuOnGlass = true;
    qrDrawnIdx = -1;                    // the menu covers any QR
    const int len = menuLen();
    const char *const *items = menuLevel == 1 ? MY_ITEMS
                             : menuLevel == 2 ? SET_ITEMS : MAIN_ITEMS;
    if (idx >= len) idx = menuIndex = 0;
    // themeDark is worked out when the face draws, and a button wake straight
    // into the menu never drew one: work it out here, or a dark-theme wearer
    // gets a white menu over a black face
    themeDark = darkNow();
    // The menu follows the theme, like everything else. It used to be black
    // whatever the face was doing, which meant a light-mode wearer went from
    // a white face to a black menu and back — a full-panel inversion twice,
    // and the residue from it was the "some of it fades" effect on return.
    display.setFullWindow();
    display.fillScreen(bg());
    display.setFont(&FreeMonoBold9pt7b);
    int16_t x1, y1; uint16_t w, h;
    // more items than rows: the list scrolls so the selection stays on screen
    int first = (idx >= MENU_ROWS) ? idx - (MENU_ROWS - 1) : 0;
    for (int i = first; i < len && i < first + MENU_ROWS; i++) {
      int16_t yPos = 22 + 22 * (i - first);
      display.setCursor(0, yPos);
      if (i == idx) {
        display.getTextBounds(items[i], 0, yPos, &x1, &y1, &w, &h);
        display.fillRect(x1 - 1, y1 - 10, 200, h + 15, fg());
        display.setTextColor(bg());
        display.println(items[i]);
      } else {
        display.setTextColor(fg());
        display.println(items[i]);
      }
    }
    if (menuLevel) {                       // where you are, and the way out
      display.setFont(NULL);
      display.setTextColor(fg());
      display.drawFastHLine(6, 180, 188, fg());
      centerSmall(menuLevel == 1 ? "MY WATCH  -  BACK: MAIN MENU"
                                 : "SETTINGS  -  BACK: MAIN MENU", 186);
    }
    // partial: the entry flash annoyed the owner, and the menu shares the
    // face's palette, so normally nothing flips. If the panel is the other
    // way round (the theme changed since it was painted), full, once.
    justFull = false;
    if (themeDark == paintedDark) { partialSync(); panelOff(); }   // the menu waits 5 s awake
    else displayFull();                    // full, then the voltage off
    paintedDark = themeDark;
    guiState = MAIN_MENU_STATE;
    alreadyInMenu = false;
  }

  // Timezone picker, in the style of the stock setTime() app
  // Saved networks, and how to drop one. The keychain fills up silently —
  // three slots, most-recent first — and until now the only way to evict a
  // network was to add three more. UP/DOWN to pick, hold MENU to forget.
  void showNetworks() {
    guiState = APP_STATE;
    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);
    unsigned long t0 = millis();
    while ((digitalRead(MENU_BTN_PIN) == BTN_ACTIVE ||
            digitalRead(BACK_BTN_PIN) == BTN_ACTIVE ||
            digitalRead(UP_BTN_PIN)   == BTN_ACTIVE ||
            digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) &&
           millis() - t0 < 2000) delay(20);

    int sel = 0; bool redraw = true;
    unsigned long lastActivity = millis();
    display.setFullWindow();
    while (1) {
      if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE) { waitAllRelease(); break; }

      if (digitalRead(MENU_BTN_PIN) == BTN_ACTIVE) {
        if (heldFor(MENU_BTN_PIN, 800) && wifiSsid[sel][0]) {
          // shuffle the rest up so the list stays packed, newest first
          for (int i = sel; i < 2; i++) {
            strncpy(wifiSsid[i], wifiSsid[i+1], 32);
            strncpy(wifiPass[i], wifiPass[i+1], 64);
          }
          wifiSsid[2][0] = 0; wifiPass[2][0] = 0;
          savePrefs();
          buzz(40, 2);
          Serial.println("[wifi] network forgotten");
          redraw = true; lastActivity = millis();
        }
        waitAllRelease();
      }
      if (digitalRead(UP_BTN_PIN) == BTN_ACTIVE) {
        sel = (sel + 2) % 3; redraw = true; lastActivity = millis(); delay(150); }
      if (digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) {
        sel = (sel + 1) % 3; redraw = true; lastActivity = millis(); delay(150); }
      if (millis() - lastActivity > 20000) break;

      if (redraw) {
        redraw = false;
        display.fillScreen(bg());
        display.setTextColor(fg());
        display.setFont(&FreeMonoBold9pt7b);
        display.setCursor(0, 22); display.print("NETWORKS");
        display.drawFastHLine(0, 30, 200, fg());
        for (int i = 0; i < 3; i++) {
          int y = 56 + i * 30;
          if (i == sel) {
            display.fillRect(0, y - 15, 200, 22, fg());
            display.setTextColor(bg());
          } else display.setTextColor(fg());
          display.setCursor(4, y);
          if (wifiSsid[i][0]) {
            char s[15]; snprintf(s, 15, "%d %s", i + 1, wifiSsid[i]);
            display.print(s);
          } else {
            char s[12]; snprintf(s, 12, "%d -empty-", i + 1);
            display.print(s);
          }
        }
        display.setTextColor(fg());
        display.setFont(NULL);
        display.setCursor(0, 168); display.print("UP/DOWN  SELECT");
        display.setCursor(0, 180); display.print("HOLD MENU  FORGET");
        display.setCursor(0, 192); display.print("BACK  DONE");
        display.display(true);
      }
      delay(30);
    }
    myShowMenu(menuIndex, false);
  }

  // ---------------- My Watch screens ----------------
  // Two whole local hours, From and To, for a window of the day: the night
  // (kind 0: radio off, a wake an hour) or the AUTO theme's dark hours
  // (kind 1). From = To is no window at all. UP/DOWN change the hour, MENU
  // moves From -> To -> save, BACK leaves without changing anything (so does
  // 30 s of nothing).
  void hourWindowScreen(int kind) {
    guiState = APP_STATE;
    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);
    waitAllRelease();                 // the press that chose this item
    int v[2] = { kind ? darkFrom : nightFrom, kind ? darkTo : nightTo };
    int field = 0;
    bool redraw = true, save = false;
    unsigned long last = millis();
    display.setFullWindow();
    while (1) {
      if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE) break;
      if (digitalRead(MENU_BTN_PIN) == BTN_ACTIVE) {
        if (field == 0) { field = 1; redraw = true; last = millis();
                          waitRelease(MENU_BTN_PIN); }
        else { save = true; break; }
      }
      if (digitalRead(UP_BTN_PIN) == BTN_ACTIVE) {
        v[field] = (v[field] + 1) % 24; redraw = true; last = millis(); delay(150); }
      if (digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) {
        v[field] = (v[field] + 23) % 24; redraw = true; last = millis(); delay(150); }
      if (millis() - last > 30000) break;
      if (redraw) {
        redraw = false;
        display.fillScreen(bg());
        display.setFont(&FreeMonoBold9pt7b);
        display.setTextColor(fg());
        display.setCursor(0, 22); display.println(kind ? "Dark Hours" : "Night Hours");
        const char *lab[2] = { "From", "To" };
        for (int i = 0; i < 2; i++) {
          int y = 60 + i * 30;
          char line[20];
          snprintf(line, 20, "%-5s %02d:00", lab[i], v[i]);
          display.setCursor(16, y);
          if (i == field) {
            display.fillRect(8, y - 15, 184, 21, fg());
            display.setTextColor(bg());
          } else display.setTextColor(fg());
          display.println(line);
        }
        display.setTextColor(fg());
        display.setFont(NULL);
        int hrs = (v[1] - v[0] + 24) % 24;
        char a[34], b[34];
        if (kind == 0) {
          if (hrs == 0) { snprintf(a, 34, "NIGHT MODE OFF"); snprintf(b, 34, "(FROM = TO)"); }
          else { snprintf(a, 34, "%d H: NO RADIO, FACE HOLDS", hrs);
                 snprintf(b, 34, "ANY BUTTON STILL WAKES IT"); }
        } else {
          if (hrs == 0) snprintf(a, 34, "AUTO THEME NEVER DARK");
          else          snprintf(a, 34, "%d H OF DARK FACE", hrs);
          snprintf(b, 34, themeMode == 2 ? "THEME IS AUTO: APPLIES NOW"
                                         : "DOWN ON FACE: THEME AUTO");
        }
        centerSmall(a, 118);
        centerSmall(b, 130);
        centerSmall("UP/DOWN: CHANGE HOUR", 170);
        centerSmall(field == 0 ? "MENU: NEXT   BACK: CANCEL"
                               : "MENU: SAVE   BACK: CANCEL", 182);
        display.display(true);
      }
      delay(40);
    }
    if (save) {
      if (kind) { darkFrom = v[0]; darkTo = v[1]; themeDark = darkNow(); }
      else      { nightFrom = v[0]; nightTo = v[1]; }
      savePrefs();
      buzz(30, 2);
      Serial.printf("[%s] %02d:00 to %02d:00%s\n", kind ? "dark" : "night",
                    v[0], v[1], v[0] == v[1] ? " (off)" : "");
    }
    waitAllRelease();
    myShowMenu(menuIndex, false);
  }

  // My Watch > Birthdays. Four slots. UP/DOWN pick a slot, MENU edits it:
  // eight letters of name, then year, month, day; UP/DOWN change the field
  // (held, they repeat), MENU moves on and saves after the day. A name left
  // all blank removes the person. BACK leaves (inside the editor: cancels).
  bool editPerson(int i) {
    static const char *L = " ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    char nm[9]; memset(nm, ' ', 8); nm[8] = 0;
    for (int c = 0; c < 8 && pplName[i][c]; c++) nm[c] = pplName[i][c];
    int y = 1990, mo = 1, d = 1;
    if (pplBirth[i]) { y = pplBirth[i] / 10000; mo = (pplBirth[i] / 100) % 100; d = pplBirth[i] % 100; }
    int field = 0;                       // 0..7 letters, 8 year, 9 month, 10 day
    bool redraw = true;
    unsigned long last = millis();
    waitAllRelease();
    while (true) {
      if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE) { waitAllRelease(); return false; }
      if (millis() - last > 60000) return false;
      int dir = digitalRead(UP_BTN_PIN) == BTN_ACTIVE ? 1 : digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE ? -1 : 0;
      if (dir) {
        if (field < 8) {
          const char *pos = strchr(L, nm[field]); int ix = pos ? (int)(pos - L) : 0;
          ix = (ix + dir + 27) % 27; nm[field] = L[ix];
        } else if (field == 8) { y += dir; if (y < 1900) y = 1900; if (y > 2100) y = 2100; }
        else if (field == 9)  { mo = (mo - 1 + dir + 12) % 12 + 1; }
        else { d = (d - 1 + dir + 31) % 31 + 1; }
        redraw = true; last = millis(); delay(140);
      }
      if (digitalRead(MENU_BTN_PIN) == BTN_ACTIVE) {
        waitRelease(MENU_BTN_PIN); last = millis(); redraw = true;
        if (field < 10) field++;
        else break;                      // saved below
      }
      if (redraw) {
        redraw = false;
        display.fillScreen(bg()); display.setTextColor(fg());
        display.setFont(&FreeMonoBold9pt7b);
        char t[20]; snprintf(t, 20, "Person %d", i + 1);
        display.setCursor(0, 22); display.print(t);
        for (int c = 0; c < 8; c++) {    // the name, a box per letter
          int x = 8 + c * 23;
          if (c == field) { display.fillRect(x, 40, 21, 24, fg()); display.setTextColor(bg()); }
          else { display.drawFastHLine(x, 64, 21, fg()); display.setTextColor(fg()); }
          display.setCursor(x + 5, 58); char ch[2] = {nm[c] == ' ' ? (c == field ? '_' : ' ') : nm[c], 0};
          display.print(ch);
        }
        display.setTextColor(fg());
        const char *lab[3] = {"Year", "Month", "Day"}; int val[3] = {y, mo, d};
        for (int k = 0; k < 3; k++) {
          int yy = 96 + k * 22; char line[20];
          snprintf(line, 20, k ? "%-5s %02d" : "%-5s %04d", lab[k], val[k]);
          if (field == 8 + k) { display.fillRect(8, yy - 15, 184, 21, fg()); display.setTextColor(bg()); }
          else display.setTextColor(fg());
          display.setCursor(16, yy); display.print(line);
        }
        display.setTextColor(fg()); display.setFont(NULL);
        centerSmall("UP/DOWN: CHANGE   BLANK NAME: REMOVE", 170);
        centerSmall(field < 10 ? "MENU: NEXT   BACK: CANCEL" : "MENU: SAVE   BACK: CANCEL", 182);
        display.display(true);
      }
      delay(30);
    }
    int e = 8; while (e > 0 && nm[e - 1] == ' ') nm[--e] = 0;   // trim
    int b0 = 0; while (nm[b0] == ' ') b0++;
    if (!nm[b0]) { pplName[i][0] = 0; pplBirth[i] = 0; pplMile[i] = 0; }
    else {
      strncpy(pplName[i], nm + b0, 8); pplName[i][8] = 0;
      static const int ML[12] = {31,29,31,30,31,30,31,31,30,31,30,31};
      if (d > ML[mo - 1]) d = ML[mo - 1];
      pplBirth[i] = (uint32_t)y * 10000 + mo * 100 + d;
      pplMile[i] = 0;                    // starts from today's age
    }
    savePeople();
    buzz(30, 2);
    return true;
  }
  void birthdaysScreen() {
    guiState = APP_STATE;
    loadPeople();
    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, BTN_ACTIVE ? INPUT : INPUT_PULLUP); pinMode(DOWN_BTN_PIN, INPUT);
    waitAllRelease();
    int sel = 0;
    bool redraw = true;
    display.setFullWindow();
    unsigned long last = millis();
    while (true) {
      if (redraw) {
        redraw = false;
        display.fillScreen(bg()); display.setTextColor(fg());
        display.setFont(&FreeMonoBold9pt7b);
        display.setCursor(0, 22); display.print("Birthdays");
        for (int i = 0; i < MAX_PEOPLE; i++) {
          int yy = 52 + i * 26; char line[24];
          if (pplBirth[i]) snprintf(line, 24, "%-8s %02lu/%02lu", pplName[i],
                                    (unsigned long)(pplBirth[i] % 100), (unsigned long)((pplBirth[i] / 100) % 100));
          else snprintf(line, 24, "  + add");
          if (i == sel) { display.fillRect(4, yy - 15, 192, 21, fg()); display.setTextColor(bg()); }
          else display.setTextColor(fg());
          display.setCursor(8, yy); display.print(line);
        }
        display.setTextColor(fg()); display.setFont(NULL);
        centerSmall("AGES IN BLOCKS ON JOE (UP)", 160);
        centerSmall("MENU: EDIT   BACK: DONE", 182);
        display.display(true);
      }
      int b = waitForPress(60000);
      if (b < 0 || b == BACK_BTN_PIN) break;
      if (b == UP_BTN_PIN)   { sel = (sel + MAX_PEOPLE - 1) % MAX_PEOPLE; redraw = true; waitAllRelease(); }
      if (b == DOWN_BTN_PIN) { sel = (sel + 1) % MAX_PEOPLE; redraw = true; waitAllRelease(); }
      if (b == MENU_BTN_PIN) { editPerson(sel); redraw = true; }
      last = millis();
    }
    (void)last;
    waitAllRelease();
    myShowMenu(menuIndex, false);
  }

  // One or two rows of choices: Haptics, Battery, Privacy. UP/DOWN cycle the
  // highlighted row's value, MENU moves to the next row and saves on the
  // last, BACK leaves without changing anything (so does 30 s of nothing).
  // Two lines under the rows say what the current choice does.
  enum { OPT_HAPTICS = 0, OPT_BATTERY = 1, OPT_PRIVACY = 2 };
  void optionsScreen(int which) {
    static const char *const ONOFF[2] = { "ON", "OFF" };
    static const char *const STEPS[4] = { "OFF", "$500", "$1000", "$5000" };
    static const uint16_t    STEPV[4] = { 0, 500, 1000, 5000 };
    static const char *const PROF[3]  = { "LIVE", "BALANCED", "SAVER" };
    static const char *const PRIV[2]  = { "SHOWN", "HIDDEN" };
    guiState = APP_STATE;
    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);
    waitAllRelease();
    const char *title = "";
    const char *lab[2] = { "", "" };
    const char *const *opt[2] = { ONOFF, ONOFF };
    int n[2] = { 2, 2 }, val[2] = { 0, 0 }, rows = 1;
    if (which == OPT_HAPTICS) {
      title = "Haptics"; rows = 2;
      lab[0] = "Block"; opt[0] = ONOFF; n[0] = 2; val[0] = buzzBlock ? 0 : 1;
      lab[1] = "Price"; opt[1] = STEPS; n[1] = 4; val[1] = 2;
      for (int k = 0; k < 4; k++) if (STEPV[k] == priceStep) val[1] = k;
    } else if (which == OPT_BATTERY) {
      title = "Battery";
      lab[0] = "Mode"; opt[0] = PROF; n[0] = 3; val[0] = battProfile;
    } else {
      title = "Privacy";
      lab[0] = "Wallet"; opt[0] = PRIV; n[0] = 2; val[0] = walletPrivate ? 1 : 0;
    }
    int field = 0;
    bool redraw = true, save = false;
    unsigned long last = millis();
    display.setFullWindow();
    while (1) {
      if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE) break;
      if (digitalRead(MENU_BTN_PIN) == BTN_ACTIVE) {
        if (field < rows - 1) { field++; redraw = true; last = millis();
                                waitRelease(MENU_BTN_PIN); }
        else { save = true; break; }
      }
      if (digitalRead(UP_BTN_PIN) == BTN_ACTIVE) {
        val[field] = (val[field] + 1) % n[field];
        redraw = true; last = millis(); delay(180); }
      if (digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) {
        val[field] = (val[field] + n[field] - 1) % n[field];
        redraw = true; last = millis(); delay(180); }
      if (millis() - last > 30000) break;
      if (redraw) {
        redraw = false;
        display.fillScreen(bg());
        display.setFont(&FreeMonoBold9pt7b);
        display.setTextColor(fg());
        display.setCursor(0, 22); display.println(title);
        for (int i = 0; i < rows; i++) {
          int y = 60 + i * 30;
          char line[22];
          snprintf(line, 22, "%-6s %s", lab[i], opt[i][val[i]]);
          display.setCursor(16, y);
          if (i == field) {
            display.fillRect(8, y - 15, 184, 21, fg());
            display.setTextColor(bg());
          } else display.setTextColor(fg());
          display.println(line);
        }
        display.setTextColor(fg());
        display.setFont(NULL);
        char a[34] = "", b[34] = "";
        if (which == OPT_HAPTICS) {
          snprintf(a, 34, val[0] == 0 ? "A TICK ON EVERY NEW BLOCK" : "NO BLOCK TICK");
          if (val[1] == 0) snprintf(b, 34, "NO PRICE BUZZ");
          else             snprintf(b, 34, "A BUZZ EACH %s MOVE", STEPS[val[1]]);
        } else if (which == OPT_BATTERY) {
          if (val[0] == BATT_LIVE) {
            // Live follows the chain: 1.5 blocks' worth of time, 10-18 min,
            // the same MIN/BLK the height face shows. Say what it is now.
            snprintf(a, 34, "FETCH PACED BY MIN/BLK");
            snprintf(b, 34, "NOW EVERY %lu MIN, WALLET %dM",
                     (unsigned long)fetchEveryMin(), WALLET_EVERY_MIN);
          } else if (val[0] == BATT_BALANCED) {
            snprintf(a, 34, "FETCH 30 MIN, WALLET 1 H");
            snprintf(b, 34, "ABOUT HALF THE FETCHES");
          } else {
            snprintf(a, 34, "FETCH HOURLY, WALLET 2 H");
            snprintf(b, 34, "A QUARTER OF THE FETCHES");
          }
        } else {
          if (val[0] == 1) {
            snprintf(a, 34, "BALANCE HIDDEN: DOWN PEEKS");
            snprintf(b, 34, "QR VIEWS STAY USABLE");
          } else {
            snprintf(a, 34, "BALANCE ON THE WAL FACE");
            snprintf(b, 34, "HOLD DOWN ON WAL: HIDE");
          }
        }
        int iy = rows == 2 ? 118 : 96;
        centerSmall(a, iy);
        centerSmall(b, iy + 12);
        centerSmall("UP/DOWN: CHANGE", 170);
        centerSmall(field < rows - 1 ? "MENU: NEXT   BACK: CANCEL"
                                     : "MENU: SAVE   BACK: CANCEL", 182);
        display.display(true);
      }
      delay(40);
    }
    if (save) {
      if (which == OPT_HAPTICS) {
        buzzBlock = (val[0] == 0);
        priceStep = STEPV[val[1]];
        priceAnchor = 0;               // re-arm from the next price heard
      } else if (which == OPT_BATTERY) {
        battProfile = (uint8_t)val[0];
      } else {
        walletPrivate = (val[0] == 1);
        walletPeek = false;
      }
      savePrefs();
      buzz(30, 2);
      Serial.printf("[mywatch] %s saved\n", title);
    }
    waitAllRelease();
    myShowMenu(menuIndex, false);
  }

  // Currency: every currency the watch can show, as a list. [x] marks the ones
  // UP cycles through on the price, sats and mcap faces; the title says which
  // one is showing. UP/DOWN move, MENU puts one in or out of the cycle, a held
  // MENU shows it now, BACK is done (so is 30 s of nothing). The cycle always
  // keeps at least one, and the one showing is always in it.
  void currencyScreen() {
    guiState = APP_STATE;
    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);
    waitAllRelease();
    uint32_t on = curOn | (1UL << curIdx);
    uint8_t cur = curIdx < NUM_CUR ? curIdx : 0;
    int sel = cur;
    const int ROWS = 9;
    bool redraw = true;
    unsigned long last = millis();
    display.setFullWindow();
    while (1) {
      if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE) break;
      if (digitalRead(MENU_BTN_PIN) == BTN_ACTIVE) {
        if (heldFor(MENU_BTN_PIN, 600)) {          // held: show this one now
          cur = sel; on |= 1UL << sel;
          buzz(30, 2);
        } else {                                   // tapped: in or out
          uint32_t o2 = on ^ (1UL << sel);
          if (o2 == 0) buzz(80, 2);                // the last one stays
          else {
            on = o2;
            if (!((on >> cur) & 1)) {              // the shown one left:
              for (int k = 1; k <= NUM_CUR; k++) { // the next one kept shows
                int c = (cur + k) % NUM_CUR;
                if ((on >> c) & 1) { cur = c; break; }
              }
            }
          }
        }
        redraw = true; last = millis();
        waitRelease(MENU_BTN_PIN);
      }
      if (digitalRead(UP_BTN_PIN) == BTN_ACTIVE) {
        sel = (sel + NUM_CUR - 1) % NUM_CUR; redraw = true; last = millis(); delay(150); }
      if (digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) {
        sel = (sel + 1) % NUM_CUR; redraw = true; last = millis(); delay(150); }
      if (millis() - last > 30000) break;
      if (redraw) {
        redraw = false;
        display.fillScreen(bg());
        display.setTextColor(fg());
        display.setFont(NULL);
        int kept = 0;
        for (int i = 0; i < NUM_CUR; i++) if ((on >> i) & 1) kept++;
        char t[34];
        snprintf(t, 34, "SHOWING %s  -  %d ON UP", CUR_CODES[cur], kept);
        centerSmall(t, 5);
        int first = sel >= ROWS ? sel - (ROWS - 1) : 0;   // scrolls with you
        for (int i = first; i < NUM_CUR && i < first + ROWS; i++) {
          int y = 31 + (i - first) * 17;
          bool inv = (i == sel);
          if (inv) display.fillRect(2, y - 13, 196, 17, fg());
          display.setTextColor(inv ? bg() : fg());
          display.setFont(&FreeMonoBold9pt7b);
          char line[12];
          snprintf(line, 12, "%s %s", ((on >> i) & 1) ? "[x]" : "[ ]", CUR_CODES[i]);
          display.setCursor(6, y);
          display.print(line);
          display.setFont(NULL);
          display.setCursor(90, y - 8);
          display.print(CUR_NAMES[i]);
        }
        display.setTextColor(fg());
        // where the list is, when it runs past the panel
        if (first > 0)                    display.fillTriangle(194, 6, 190, 11, 198, 11, fg());
        if (first + ROWS < NUM_CUR)       display.fillTriangle(190, 172, 198, 172, 194, 176, fg());
        centerSmall("MENU: IN/OUT   HOLD: SHOW NOW", 178);
        centerSmall("BACK: DONE", 189);
        display.display(true);
      }
      delay(40);
    }
    if (on != curOn || cur != curIdx) {
      curOn = on; curIdx = cur;
      applyCurrency();
      savePrefs();                     // both the cycle and the one showing
      buzz(30, 2);
      Serial.printf("[cur] showing %s, cycle 0x%05lx\n", CUR_CODES[curIdx],
                    (unsigned long)curOn);
    }
    waitAllRelease();
    myShowMenu(menuIndex, false);
  }

  // Faces: which of the nine the BACK button cycles through and the strip
  // shows. UP/DOWN move, MENU shows or hides the highlighted face, BACK is
  // done (the choice is kept, as it is after 30 s of nothing). The last face
  // showing cannot be hidden: a buzz says no.
  void facesScreen() {
    guiState = APP_STATE;
    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);
    waitAllRelease();
    const uint16_t ALL = (1 << NUM_MODES) - 1;
    uint16_t hide = faceHide;
    int sel = 0;
    bool redraw = true;
    unsigned long last = millis();
    display.setFullWindow();
    while (1) {
      if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE) break;
      if (digitalRead(MENU_BTN_PIN) == BTN_ACTIVE) {
        uint16_t h2 = hide ^ (1 << sel);
        if ((h2 & ALL) == ALL) buzz(80, 2);   // the last face stays
        else hide = h2;
        redraw = true; last = millis();
        waitRelease(MENU_BTN_PIN);
      }
      if (digitalRead(UP_BTN_PIN) == BTN_ACTIVE) {
        sel = (sel + NUM_MODES - 1) % NUM_MODES; redraw = true; last = millis(); delay(150); }
      if (digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) {
        sel = (sel + 1) % NUM_MODES; redraw = true; last = millis(); delay(150); }
      if (millis() - last > 30000) break;
      if (redraw) {
        redraw = false;
        display.fillScreen(bg());
        display.setTextColor(fg());
        display.setFont(NULL);
        centerSmall("FACES IN THE CYCLE", 5);
        display.setFont(&FreeMonoBold9pt7b);
        for (int i = 0; i < NUM_MODES; i++) {
          int y = 30 + i * 15;                // ten rows clear the footer
          char line[24];
          snprintf(line, 24, "%s %s", ((hide >> i) & 1) ? "[ ]" : "[x]", MODE_NAMES[i]);
          display.setCursor(6, y);
          if (i == sel) {
            display.fillRect(2, y - 12, 196, 15, fg());
            display.setTextColor(bg());
          } else display.setTextColor(fg());
          display.print(line);
        }
        display.setTextColor(fg());
        display.setFont(NULL);
        centerSmall("MENU: SHOW/HIDE   BACK: DONE", 188);
        display.display(true);
      }
      delay(40);
    }
    if (hide != faceHide) {
      faceHide = hide;
      if (!faceShown(dispMode)) dispMode = nextFace(dispMode);
      savePrefs();
      buzz(30, 2);
      Serial.printf("[faces] hidden mask 0x%03x\n", faceHide);
    }
    waitAllRelease();
    myShowMenu(menuIndex, false);
  }

  // Start Screen: which boot sequence plays when the watch starts (power on,
  // reflash, a crash). Random, the default, never repeats the last one and
  // opens a new firmware on Genesis. UP/DOWN move, MENU picks, a held MENU
  // plays the highlighted one right here, BACK leaves it as it was.
  void bootSeq(int i) {
    switch (i) {
      case 0:  bootSegmentTest(); break;
      case 1:  bootGenesis();     break;
      case 2:  bootHalvings();    break;
      default: bootRing();        break;
    }
  }
  void startScreenScreen() {
    static const char *const NAMES[BOOT_SEQS + 1] = {
      "Random", "Segment Test", "Genesis", "Halvings", "Time Ring" };
    static const char *const ABOUT[BOOT_SEQS + 1] = {
      "A DIFFERENT ONE EACH START", "EVERY SEGMENT, THEN THE MARK",
      "MINING BLOCK 0, 03 JAN 2009", "SIX EPOCHS OF THE SUBSIDY",
      "THE RING: BITCOIN IS TIME" };
    guiState = APP_STATE;
    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);
    waitAllRelease();
    int sel;
    { Preferences p; p.begin("btcchrono", true);
      sel = p.getUChar("bootPick", 0); p.end(); }
    if (sel > BOOT_SEQS) sel = 0;
    const int was = sel;
    bool redraw = true, full = false, save = false;
    unsigned long last = millis();
    display.setFullWindow();
    while (1) {
      if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE) break;
      if (digitalRead(MENU_BTN_PIN) == BTN_ACTIVE) {
        if (heldFor(MENU_BTN_PIN, 600)) {        // held: play it, here
          buzz(30, 2);
          waitAllRelease();
          int seq = sel ? sel - 1 : (int)(esp_random() % BOOT_SEQS);
          Serial.printf("[boot] preview %d\n", seq);
          bootSeq(seq);
          paintedDark = true;                    // it ends on black
          full = true; redraw = true; last = millis();
          waitAllRelease();
          continue;
        }
        save = true; break;                       // tapped: this one
      }
      if (digitalRead(UP_BTN_PIN) == BTN_ACTIVE) {
        sel = (sel + BOOT_SEQS) % (BOOT_SEQS + 1); redraw = true; last = millis(); delay(150); }
      if (digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) {
        sel = (sel + 1) % (BOOT_SEQS + 1); redraw = true; last = millis(); delay(150); }
      if (millis() - last > 30000) break;
      if (redraw) {
        redraw = false;
        themeDark = darkNow();
        display.fillScreen(bg());
        display.setFont(&FreeMonoBold9pt7b);
        display.setTextColor(fg());
        display.setCursor(0, 22); display.println("Start Screen");
        for (int i = 0; i <= BOOT_SEQS; i++) {
          int y = 50 + i * 22;
          char line[22];
          snprintf(line, 22, "%s %s", i == was ? "(o)" : "( )", NAMES[i]);
          display.setCursor(8, y);
          if (i == sel) {
            display.fillRect(2, y - 15, 196, 21, fg());
            display.setTextColor(bg());
          } else display.setTextColor(fg());
          display.print(line);
        }
        display.setTextColor(fg());
        display.setFont(NULL);
        centerSmall(ABOUT[sel], 160);
        centerSmall("MENU: PICK   HOLD MENU: PLAY", 176);
        centerSmall("BACK: CANCEL", 188);
        if (full) { displayFull(); full = false; }   // back from black
        else display.display(themeDark == paintedDark);
        paintedDark = themeDark;
      }
      delay(40);
    }
    if (save && sel != was) {
      Preferences p; p.begin("btcchrono", false);
      p.putUChar("bootPick", sel); p.end();
      buzz(30, 2);
      Serial.printf("[boot] start screen: %s\n", NAMES[sel]);
    }
    waitAllRelease();
    myShowMenu(menuIndex, false);
  }

  void showTimezone() {
    guiState = APP_STATE;
    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);
    // the finger that selected this menu item is still on MENU — wait
    // for release, or the picker instantly "confirms" and exits before
    // ever drawing (2s cap in case a button is stuck)
    unsigned long t0 = millis();
    while ((digitalRead(MENU_BTN_PIN) == BTN_ACTIVE ||
            digitalRead(BACK_BTN_PIN) == BTN_ACTIVE ||
            digitalRead(UP_BTN_PIN)   == BTN_ACTIVE ||
            digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) &&
           millis() - t0 < 2000) delay(20);
    RTC.read(currentTime);
    int sel = tzIndex;
    bool redraw = true;
    unsigned long lastActivity = millis();
    display.setFullWindow();
    while (1) {
      if (digitalRead(MENU_BTN_PIN) == BTN_ACTIVE ||
          digitalRead(BACK_BTN_PIN) == BTN_ACTIVE) { tzIndex = sel; break; }
      if (digitalRead(UP_BTN_PIN) == BTN_ACTIVE)   {
        sel = (sel + 4) % 5; redraw = true; lastActivity = millis(); delay(150); }
      if (digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) {
        sel = (sel + 1) % 5; redraw = true; lastActivity = millis(); delay(150); }
      if (millis() - lastActivity > 20000) { tzIndex = sel; break; }  // timeout
      if (redraw) {
        redraw = false;
        display.fillScreen(bg());
        display.setFont(&FreeMonoBold9pt7b);
        display.setTextColor(fg());
        display.setCursor(0, 22); display.println("Timezone");
        time_t utcNow = makeTime(currentTime) - settings.gmtOffset;
        for (int i = 0; i < 5; i++) {
          int y = 56 + i * 24;
          long off = tzOffsetSec(i, utcNow);
          char line[24];
          snprintf(line, 24, "%s  UTC%+ld", tzName(i, utcNow), off / 3600L);
          display.setCursor(16, y);
          if (i == sel) {
            display.fillRect(8, y - 15, 184, 21, fg());
            display.setTextColor(bg());
          } else display.setTextColor(fg());
          display.println(line);
        }
        // live preview of the clock in the highlighted zone
        time_t lt = makeTime(currentTime);
        if (sel > 0) lt = utcNow + tzOffsetSec(sel, utcNow);
        tmElements_t dt; breakTime(lt, dt);
        display.setTextColor(fg());
        display.setCursor(0, 196);
        char pv[26];
        snprintf(pv, 26, "Now: %02d:%02d  MENU=OK", dt.Hour, dt.Minute);
        display.print(pv);
        display.display(true);
      }
      delay(40);
    }
    savePrefs();                       // zone choice survives power loss
    myShowMenu(menuIndex, false);
  }

  // Setup Wallet: WiFi portal with a zpub field — customers configure
  // from their phone, no computer, no Arduino. Saved to NVS flash.
  // The portal, blocking — which is how it has to be.
  //
  // There was a non-blocking version here that polled BACK so a failed setup
  // could be abandoned instead of trapping the wearer for three minutes. It
  // crashed the watch: when the phone submits the form the library is still
  // mid-request, and stopping the portal from outside its own loop frees
  // things the handler is using. A panic wipes RTC memory, so the watch came
  // back in demo mode having forgotten its networks — a far worse fault than
  // the one being fixed. Two attempts at a grace period did not settle it.
  //
  // So: the library runs its own loop and tears itself down, as it did
  // before. The timeout is shorter than it was, which is the honest fix for
  // being stuck — you wait, but not for three minutes.
  // clientWait: the timeout only counts while no phone is joined to the
  // setup network, so it can be short for someone who never joins and still
  // never cut off someone halfway through pasting.
  bool runPortal(WiFiManager &wm, uint16_t secs, bool clientWait = false,
                 bool wallet = false) {
    g_portalSaved = false;
    wm.setCustomHeadElement(SETUP_HEAD);
    wm.setTitle("BWATCH");
    wm.setClass(wallet ? "bw-wal" : "bw-net-setup");   // which page '/' opens
    wm.setShowInfoUpdate(false);
    wm.setShowInfoErase(false);
    {                                    // without the script: a short menu
      static std::vector<const char *> mWal = {"param", "exit"};
      static std::vector<const char *> mNet = {"wifi", "exit"};
      wm.setMenu(wallet ? mWal : mNet);
    }
    wm.setSaveParamsCallback([]() { g_portalSaved = true; });
    wm.setConfigPortalBlocking(true);
    wm.setAPClientCheck(clientWait);
    wm.setTimeout(secs);
    bool ok = wm.startConfigPortal(WIFI_AP_SSID);
    return ok || g_portalSaved;   // the wallet form can save without a join
  }

  // ---- wallet setup and VERIFY screens: theme colours, built-in font ----
  void wsTitle(const char *t) {
    display.setFullWindow();
    display.fillScreen(bg());
    display.setTextColor(fg());
    display.setFont(NULL);
    bootCenter(t, 8, 2);
    display.setTextSize(1);
    display.drawFastHLine(10, 28, 180, fg());
  }
  void wsFooter(const char *t) {
    display.drawFastHLine(10, 180, 180, fg());
    centerSmall(t, 187);
  }
  // an address in groups of four, three to a row, large: made to be
  // compared character by character. Returns the y under it.
  int wsGrouped(const char *a, int y) {
    int n = strlen(a), g = 0, rl = 0;
    char row[20];
    display.setTextSize(2);
    for (int i = 0; i < n; i += 4) {
      if (rl) row[rl++] = ' ';
      for (int k = 0; k < 4 && i + k < n; k++) row[rl++] = a[i + k];
      if (++g % 3 == 0 || i + 4 >= n) {
        row[rl] = 0;
        display.setCursor(12, y); display.print(row);
        y += 18; rl = 0;
      }
    }
    display.setTextSize(1);
    return y;
  }
  void wsAt(const char *t, int x, int y) { display.setCursor(x, y); display.print(t); }
  void wsInverse(const char *a, const char *b, int y) {    // a black band
    display.fillRect(10, y, 180, b ? 30 : 18, fg());
    display.setTextColor(bg());
    centerSmall(a, y + 5);
    if (b) centerSmall(b, y + 18);
    display.setTextColor(fg());
  }
  // a key as the coordinator shows it: the origin fingerprint, or failing
  // that the end of the xpub
  void wsKeyId(const WalletDesc &d, int k, char *o) {
    if (d.hasFp[k]) snprintf(o, 12, "%02x%02x%02x%02x", d.fp[k][0], d.fp[k][1],
                              d.fp[k][2], d.fp[k][3]);
    else snprintf(o, 12, "..%s", d.tail[k]);
  }

  void wsSeedRefused() {
    wsTitle("NOT SAVED");
    display.fillRect(10, 36, 180, 44, fg());
    display.setTextColor(bg());
    centerSmall("THAT LOOKS LIKE", 42);
    bootCenter("SEED WORDS", 54, 2);
    display.setTextSize(1);
    display.setTextColor(fg());
    centerSmall("OR A PRIVATE KEY. THE WATCH", 90);
    centerSmall("NEVER NEEDS EITHER.", 102);
    centerSmall("IF THEY WERE REAL, TREAT", 118);
    centerSmall("THEM AS EXPOSED: MOVE FUNDS.", 130);
    centerSmall("PASTE A ZPUB OR DESCRIPTOR.", 152);
    wsFooter("ANY BUTTON: OK");
  }

  void wsError(const char *why) {
    wsTitle("NOT SAVED");
    centerSmall("THE WATCH COULD NOT READ IT:", 44);
    wsInverse(why, nullptr, 60);
    centerSmall("NATIVE SEGWIT MULTISIG:", 100);
    centerSmall("wsh(sortedmulti(...))", 112);
    centerSmall("OR A SINGLE-KEY ZPUB.", 124);
    centerSmall("NOTHING WAS CHANGED.", 152);
    wsFooter("ANY BUTTON: OK");
  }

  // multisig: what the wearer compares against their signers. Nothing is
  // saved until UP: this is the moment the phone stops being trusted.
  void wsKeyCheck(const WalletDesc &d, int page) {
    wsTitle("CHECK KEYS");
    char t[34];
    snprintf(t, 34, "MULTISIG %d OF %d", d.m, d.n);
    wsAt(t, 10, 36);
    wsAt(d.nested ? "WRAPPED" : "SEGWIT", d.nested ? 148 : 154, 36);
    wsAt(d.given == 1 ? "CHECKSUM" : "CHECKSUM*", 10, 54);
    display.setTextSize(2); wsAt(d.checksum, 70, 50); display.setTextSize(1);
    int per = 4, first = page * per, y = 72;
    for (int k = first; k < d.n && k < first + per; k++) {
      snprintf(t, 34, "KEY %d", k + 1);
      wsAt(t, 10, y + 4);
      char id[12]; wsKeyId(d, k, id);
      display.setTextSize(2); wsAt(id, 70, y); display.setTextSize(1);
      y += 20;
    }
    if (d.n > per) {
      snprintf(t, 34, "KEYS %d-%d OF %d, DOWN: MORE", first + 1,
               first + per < d.n ? first + per : d.n, d.n);
      centerSmall(t, 154);
    } else {
      centerSmall("SAME AS YOUR SIGNERS SHOW?", 154);
    }
    centerSmall(d.given == 1 ? "NOTHING IS SAVED UNTIL YES"
                             : "*COMPUTED: NONE WAS PASTED", 166);
    wsFooter("UP: YES, SAVE   BACK: NO");
  }

  bool wsConfirmKeys(const WalletDesc &d) {
    int page = 0, pages = (d.n + 3) / 4;
    while (true) {
      wsKeyCheck(d, page);
      display.display(true);
      int b = waitForPress(120000);
      waitAllRelease();
      if (b == UP_BTN_PIN) return true;
      if (b == DOWN_BTN_PIN) { page = (page + 1) % pages; continue; }
      return false;                     // BACK, MENU or nobody: not saved
    }
  }

  // the first address, after a save: check it before trusting the watch
  void wsFirstAddress(const char *title, const char *addr, const char *kind) {
    wsTitle(title);
    int y = wsGrouped(addr, 36);        // six rows for multisig: it leads
    wsAt("#0. CHECK IT IN YOUR WALLET", 10, y + 2);
    wsAt(kind, 10, y + 14);
    wsFooter("ANY BUTTON: DONE");
    display.display(true);
    waitForPress(120000);
    waitAllRelease();
  }

  void wsForgetWallet() {               // the balance and index start over
    haveWallet = false; walletSats = 0; recvIndex = 0;
    descRead = false; descBrOk[0] = descBrOk[1] = false;
  }

  void setupWallet() {
    loadPrefs();
    guiState = APP_STATE;
    display.epd2.setBusyCallback(0);
    WiFiManager wm;
    wm.setTimeout(WIFI_AP_TIMEOUT);
    // One field for both: a zpub, or a multisig descriptor. It opens EMPTY:
    // the library writes a default into value='...' unescaped, and every
    // descriptor carries ' and <> that would break the page. It is better
    // that way anyway: the open setup network never shows the keys to
    // whoever else joins it. Empty means keep the wallet as it is.
    static char cur[DESC_TEXT_MAX + 1];
    cur[0] = 0;
    if (haveDesc) {
      Preferences p; p.begin("btcchrono", true);
      strncpy(cur, p.getString("desc", "").c_str(), DESC_TEXT_MAX);
      p.end();
    } else if (walletConfigured()) strncpy(cur, zpubBuf, DESC_TEXT_MAX);
    cur[DESC_TEXT_MAX] = 0;
    WiFiManagerParameter pZpub(
      "zpub", "Wallet",                  // the page's script adds the rest
      "", DESC_TEXT_MAX);
    wm.addParameter(&pZpub);
    WiFiManagerParameter pLn(
      "lnaddr", "Lightning address (optional)",
      lnAddrBuf, 63);
    wm.addParameter(&pLn);
    // birthdays, as "NAME YYYY-MM-DD": ages in blocks on JOE
    loadPeople();
    static char bdCur[MAX_PEOPLE][24];
    for (int i = 0; i < MAX_PEOPLE; i++) {
      if (pplBirth[i]) snprintf(bdCur[i], 24, "%s %04lu-%02lu-%02lu", pplName[i],
                                (unsigned long)(pplBirth[i] / 10000), (unsigned long)((pplBirth[i] / 100) % 100),
                                (unsigned long)(pplBirth[i] % 100));
      else bdCur[i][0] = 0;
    }
    WiFiManagerParameter pB0("bd0", "Birthday 1 (optional): NAME YYYY-MM-DD", bdCur[0], 23);
    WiFiManagerParameter pB1("bd1", "Birthday 2", bdCur[1], 23);
    WiFiManagerParameter pB2("bd2", "Birthday 3", bdCur[2], 23);
    WiFiManagerParameter pB3("bd3", "Birthday 4", bdCur[3], 23);
    wm.addParameter(&pB0); wm.addParameter(&pB1);
    wm.addParameter(&pB2); wm.addParameter(&pB3);

    wsTitle("WALLET");
    wsAt("ON YOUR PHONE, JOIN WIFI", 10, 38);
    bootCenter(WIFI_AP_SSID, 52, 2);
    display.setTextSize(1);
    wsAt("A PAGE OPENS. PASTE ONE OF:", 10, 76);
    wsAt("- A ZPUB (ONE KEY)", 16, 90);
    wsAt("- A DESCRIPTOR (MULTISIG)", 16, 102);
    wsInverse("PUBLIC KEYS ONLY.", "NEVER YOUR SEED WORDS.", 120);
    // Ask first. BACK cannot stop the setup network once it is open (the
    // note on runPortal: stopping it mid-request crashed the watch), so
    // this is where a wrong turn into Setup Wallet costs nothing.
    wsFooter("UP: START    BACK: CANCEL");
    display.display(true);   // partial: no flash
    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, BTN_ACTIVE ? INPUT : INPUT_PULLUP);
    pinMode(DOWN_BTN_PIN, INPUT);
    waitAllRelease();
    int go = waitForPress(60000);
    waitAllRelease();
    if (go != UP_BTN_PIN) {                       // BACK, MENU, or nobody
      display.epd2.setBusyCallback(WatchyDisplay::busyCallback);
      myShowMenu(menuIndex, false);
      return;
    }
    // open: closes after a minute if no phone joins; while one is joined it
    // waits, and a minute after it leaves
    display.fillRect(0, 150, 200, 50, bg());
    wsAt("CONNECTION OPEN", 10, 154);
    wsAt("1 MIN TO JOIN FROM YOUR PHONE", 10, 166);
    wsFooter("SAVE ON THE PAGE TO FINISH");
    display.display(true);
    runPortal(wm, 60, true, true);
    WiFi.mode(WIFI_OFF); btStop();
    display.epd2.setBusyCallback(WatchyDisplay::busyCallback);

    { WiFiManagerParameter *bp[MAX_PEOPLE] = {&pB0, &pB1, &pB2, &pB3};
      bool changed = false;
      for (int i = 0; i < MAX_PEOPLE; i++) {
        const char *v = bp[i]->getValue();
        if (!v || !strcmp(v, bdCur[i])) continue;      // untouched
        changed = true;
        char nm[9] = {0}; int ni = 0; int y = 0, mo = 0, d = 0;
        const char *q = v;
        while (*q && !(*q >= '0' && *q <= '9')) {     // the name: letters
          char c = *q++;
          if (c >= 'a' && c <= 'z') c -= 32;
          if (((c >= 'A' && c <= 'Z') || (c == ' ' && ni)) && ni < 8) nm[ni++] = c;
        }
        while (ni && nm[ni - 1] == ' ') nm[--ni] = 0;
        if (sscanf(q, "%d-%d-%d", &y, &mo, &d) == 3 && y >= 1900 && y <= 2100 &&
            mo >= 1 && mo <= 12 && d >= 1 && d <= 31) {
          strncpy(pplName[i], ni ? nm : "SOMEONE", 8); pplName[i][8] = 0;
          pplBirth[i] = (uint32_t)y * 10000 + mo * 100 + d;
        } else { pplName[i][0] = 0; pplBirth[i] = 0; }   // cleared (or unreadable)
        pplMile[i] = 0;
      }
      if (changed) savePeople(); }
    const char *lv = pLn.getValue();
    if (lv && strchr(lv, '@')) {
      strncpy(lnAddrBuf, lv, 63); lnAddrBuf[63] = 0;
      lnInvoice[0] = 0; lnVerify[0] = 0;
      savePrefs();
    }

    String in = pZpub.getValue() ? pZpub.getValue() : "";
    in.trim();
    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, BTN_ACTIVE ? INPUT : INPUT_PULLUP);
    pinMode(DOWN_BTN_PIN, INPUT);
    if (in.length() == 0 || in == String(cur)) {
      wsTitle("WALLET");
      centerSmall("NO CHANGE MADE.", 90);
      wsFooter("");
      display.display(true);
      delay(2500);
    } else if (looksPrivate(in.c_str())) {
      // refused before anything else looks at it, and never stored
      wsSeedRefused();
      display.display(true);
      for (int i = 0; i < 3; i++) { buzz(60, 4); delay(150); }
      waitForPress(120000);
      waitAllRelease();
      Serial.println("[wallet] refused: private key or seed words");
    } else if (in.indexOf('(') >= 0) {
      static WalletDesc nd;
      if (!descParse(in.c_str(), nd)) {
        wsError(nd.err);
        display.display(true);
        waitForPress(120000); waitAllRelease();
      } else if (!wsConfirmKeys(nd)) {
        wsTitle("NOT SAVED");
        centerSmall("THE WALLET IS AS IT WAS.", 90);
        wsFooter("");
        display.display(true);
        delay(2500);
      } else {
        Preferences p; p.begin("btcchrono", false);
        p.putString("desc", in);
        p.putString("zpub", "");         // one wallet at a time
        p.end();
        haveDesc = true; zpubBuf[0] = 0;
        wsForgetWallet();
        char a[96]; descAddress(nd, 0, 0, a, sizeof a);
        char kind[34]; snprintf(kind, 34, "MULTISIG %d OF %d  %s", nd.m, nd.n, nd.checksum);
        Serial.printf("[wallet] multisig %d of %d saved, #0 %s\n", nd.m, nd.n, a);
        wsFirstAddress("WALLET SAVED", a, kind);
      }
    } else {
      HDPublicKey hd(in.c_str());
      if (!hd.isValid() || in.length() >= sizeof(zpubBuf)) {
        wsError("NOT A ZPUB");
        display.display(true);
        waitForPress(120000); waitAllRelease();
      } else {
        strncpy(zpubBuf, in.c_str(), sizeof(zpubBuf) - 1);
        zpubBuf[sizeof(zpubBuf) - 1] = 0;
        if (haveDesc) {
          Preferences p; p.begin("btcchrono", false);
          p.remove("desc"); p.end();
          haveDesc = false;
        }
        savePrefs();
        wsForgetWallet();
        String a = deriveAddress(0);
        wsFirstAddress("WALLET SAVED", a.c_str(), "ONE KEY, NATIVE SEGWIT");
      }
    }
    myShowMenu(menuIndex, false);
  }

  // VERIFY: the receive address, worked out on the wrist from public keys
  // alone, set out to be compared by eye with what the phone shows. UP and
  // DOWN step through the neighbours (a wallet app is often a few ahead);
  // MENU goes back to the current receiving one; BACK, or a minute, leaves.
  void verifyScreen() {
    guiState = APP_STATE;
    qrDrawnIdx = -1;                    // the QR after it starts clean
    int idx = recvIndex;
    const WalletDesc *d = walletDesc();
    while (true) {
      String a = walletAddr(0, idx);
      char t[34];
      snprintf(t, 34, "VERIFY #%d", idx);
      wsTitle(t);
      int y = wsGrouped(a.length() ? a.c_str() : "----", 36);
      y += 1;                           // three lines under six rows of a
      if (idx == recvIndex)     wsAt("CURRENT RECEIVING", 10, y);   // multisig
      else if (idx > recvIndex) wsAt("AHEAD: NOT YET USED", 10, y); // address
      else                      wsAt("EARLIER: MAY BE USED", 10, y);
      if (haveWallet) {
        uint32_t ago = wakeMin >= lastWalletMin ? wakeMin - lastWalletMin : 0;
        if (ago < 120) snprintf(t, 34, "SCAN %lu MIN AGO", (unsigned long)ago);
        else if (ago < 2880) snprintf(t, 34, "SCAN %lu H AGO", (unsigned long)(ago / 60));
        else snprintf(t, 34, "SCAN %lu DAYS AGO", (unsigned long)(ago / 1440));
      } else snprintf(t, 34, "NOT SCANNED YET");
      wsAt(t, 10, y + 11);
      if (d) snprintf(t, 34, "MULTISIG %d OF %d  %s", d->m, d->n, d->checksum);
      else   snprintf(t, 34, "ONE KEY, NATIVE SEGWIT");
      wsAt(t, 10, y + 22);
      wsFooter("UP/DOWN: STEP  MENU: CURRENT");
      display.display(true);
      int b = waitForPress(60000);
      waitAllRelease();
      if (b == UP_BTN_PIN)        { if (idx > 0) idx--; }
      else if (b == DOWN_BTN_PIN) { if (idx < 9999) idx++; }
      else if (b == MENU_BTN_PIN) idx = recvIndex;
      else break;                       // BACK, or nobody
    }
    guiState = WATCHFACE_STATE;
  }

  // menu session for DOCKED mode: on USB power, run the menu right in
  // the loop — instant navigation, no deep-sleep wakes, and no stock
  // idle-timeout silently dropping the user back onto the dock face
  // (where DOWN correctly means theme — the source of the "DOWN stopped
  // working in the menu" mystery).
  void dockMenuSession() {
    openMenu();
    unsigned long idle = millis();
    while (digitalRead(USB_DET_PIN) == 1) {
      if (millis() - idle > 120000) break;            // 2 min true idle
      if (digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) {
        idle = millis();
        menuStep(1);
        waitRelease(DOWN_BTN_PIN);
      } else if (digitalRead(UP_BTN_PIN) == BTN_ACTIVE) {
        idle = millis();
        menuStep(-1);
        waitRelease(UP_BTN_PIN);
      } else if (digitalRead(MENU_BTN_PIN) == BTN_ACTIVE) {
        idle = millis();
        waitRelease(MENU_BTN_PIN);
        dispatchMenu();
        if (guiState == APP_STATE) {          // About / Setup WiFi wait
          while (digitalRead(USB_DET_PIN) == 1) {     // for BACK
            if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE) {
              waitRelease(BACK_BTN_PIN);
              myShowMenu(menuIndex, false);
              break;
            }
            delay(30);
          }
        }
        idle = millis();
      } else if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE) {
        waitRelease(BACK_BTN_PIN);
        idle = millis();
        if (!menuBack()) break;                       // Settings -> main,
      }                                               // main -> dock face
      delay(30);
    }
    guiState = WATCHFACE_STATE;
  }

  // non-destructive WiFi setup: stock Watchy calls resetSettings()
  // FIRST, erasing saved credentials every time the menu item opens —
  // so "fixing" WiFi always means retyping everything. This version
  // tries the stored credentials, and only opens the portal on failure
  // (where you can enter new ones).
  // keychain-aware NTP sync: the stock item connects with the single
  // last-persisted credential, so it "fails NTP" anywhere that isn't
  // the last place you connected. Same disease as stock Setup WiFi.
  // The stock About asks WiFi.status(), which is always "not connected"
  // because the radio is off between fetches by design. Report the things
  // that are actually true instead: which networks are saved, how fresh the
  // data is, when the clock was last anchored.
  // ADC_PIN gave 0 mV, so whatever the library defines is not the battery
  // sense line on this board. Rather than guess, read every ADC1-capable pin
  // once and print it: with the usual 1:1 divider the battery pin sits at
  // about half the cell voltage, so a ~3.9 V cell should show near 1950 mV.
  // ADC2 is unusable while WiFi is up, so GPIO1..10 is the search space.
  void scanAdcPins() {
    Serial.println("[adc] scanning ADC1 pins (looking for ~half of Vbat)");
    for (int p = 1; p <= 10; p++) {
      uint32_t mv = 0;
      for (int i = 0; i < 4; i++) { mv += analogReadMilliVolts(p); delay(2); }
      mv /= 4;
      Serial.printf("[adc] GPIO%-2d  %4lu mV   -> cell would be %.3f V%s\n",
                    p, (unsigned long)mv, (mv * 2.0f) / 1000.0f,
                    (mv > 1500 && mv < 2300) ? "   <-- candidate" : "");
    }
  #if defined(ADC_PIN)
    Serial.printf("[adc] library ADC_PIN = %d\n", (int)ADC_PIN);
  #else
    Serial.println("[adc] library does not define ADC_PIN");
  #endif
  }

  // dump the log: hold DOWN on the About screen
  void dumpVlog() {
    Serial.printf("[vlog] lowest true reading ever: %.3fV at wake %lu%s%s\n",
                  minVoltSeen, (unsigned long)minVoltWake,
                  (minVoltFlags & 1) ? " (travelling)" : "",
                  (minVoltFlags & 2) ? " (docked)" : "");
    // Count what is actually there first. The old dump printed the ring
    // position as if it were an hour, so four samples in a fresh log came
    // out labelled 20, 21, 22, 23 — which reads as twenty hours of history
    // that does not exist. Label them by age instead, and say how many.
    // Ages count samples only (an hour each on the cell); a charge mark
    // takes no time, and the hours spent charging are not in the log at all.
    int have = 0;
    for (int i = 0; i < VLOG_N; i++)
      if (vlog[i] != 0 && vlog[i] != VLOG_CHG) have++;
    Serial.printf("[vlog] %d sample%s, one an hour on the cell, newest last\n",
                  have, have == 1 ? "" : "s");
    Serial.println("[vlog]  age   raw    true");
    int n = 0;
    float k4 = 4.20f / (battFullV > 3.0f ? battFullV : 4.20f);
    for (int i = 0; i < VLOG_N; i++) {
      int k = (vlogIdx + i) % VLOG_N;
      if (vlog[k] == 0) continue;
      if (vlog[k] == VLOG_CHG) {
        if (n > 0) Serial.println("[vlog]  ---- charged ----");
        continue;
      }
      float v = vlog[k] / 50.0f;
      int ago = have - 1 - n;                 // samples before the newest
      n++;
      if (ago == 0) Serial.printf("[vlog]  now   %.2f   %.2f\n", v, v * k4);
      else Serial.printf("[vlog] -%2dh   %.2f   %.2f\n", ago, v, v * k4);
    }
  }

  void chargeDiag() {
    pinMode(USB_DET_PIN, INPUT); pinMode(CHG_STAT_PIN, INPUT_PULLUP);
    if (digitalRead(USB_DET_PIN) != 1) return;
    float lib = getBatteryVoltage();
  #if defined(ADC_PIN)
    uint32_t mv = analogReadMilliVolts(ADC_PIN);
  #else
    uint32_t mv = 0;
  #endif
    // One line a minute while plugged in, readable as a timeline: minutes
    // on the cable, the chip's status pin (0 = charging, 1 = not), the
    // watch's verdict (1 charging, 2 full), and the estimate on the face.
    // The voltages are the charger's rail, not the cell, while plugged in.
    int shown = batteryPctShown();
    long onMin = chgFromT ? (long)(rtcNow() - chgFromT) / 60 : -1;
    Serial.printf("[chg] t+%ldmin stat=%d state=%d est=%d%% (from %d%% at %.2f%%/min) "
                  "rail=%.3fV lib=%.3fV adc=%lumV\n",
                  onMin, digitalRead(CHG_STAT_PIN), chargeState(), shown,
                  chgFromPct, chgRate(), batteryVolts(), lib, (unsigned long)mv);
  }

  // Block 1,000,000. The curiosity cards' layout, with an odometer: 999,999
  // rolls over digit by digit to 1,000,000, then a bar of ten segments fills,
  // one per hundred thousand blocks, each with its pulse. Then it waits on the
  // glass like a curiosity (an event), so a fetch nobody watched still keeps it
  // for whoever looks next, with when it was mined and how long ago.
  void drawMillionCard(int stage, long shown, int segs) {
    char s[16], line[34];
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setTextWrap(false);
    display.setFont(NULL); display.setTextSize(1);
    curioWhen(line, 34, 1000000L);
    centerSmall(line, 14);
    // the odometer: 7 digit slots, the leading one blank until it carries
    if (shown >= 1000000L) snprintf(s, 16, "%7ld", shown);
    else snprintf(s, 16, " %06ld", shown);        // the wheels: 000,000
    const GFXfont *F = &DSEG7_Classic_Bold_32;
    int adv = F->glyph['0' - F->first].xAdvance, cw = 6;
    // centred on the ink of 1,000,000, not on the cells: DSEG's "1" sits at
    // the right of its cell, which pushed the whole number right
    GFXglyph *g1 = F->glyph + ('1' - F->first), *g0 = F->glyph + ('0' - F->first);
    int inkL0 = g1->xOffset, inkR0 = 6 * adv + 2 * cw + g0->xOffset + g0->width;
    int w = 7 * adv + 2 * cw, x = 100 - (inkL0 + inkR0) / 2, yb = 108;
    int x0 = x;
    display.setFont(F);
    for (int i = 0; i < 7; i++) {
      if (s[i] != ' ') { char c[2] = {s[i], 0}; display.setCursor(x, yb); display.print(c); }
      x += adv;
      if ((i == 0 && s[0] != ' ') || i == 3) {
        display.fillRect(x + cw / 2 - 1, yb - 1, 3, 5, GxEPD_WHITE);
      }
      if (i == 0 || i == 3) x += cw;
    }
    display.setFont(NULL);
    // ten segments, one per 100,000 blocks, spanning exactly the ink of
    // 1,000,000 (the "1" sits right in its cell, so the cells' own span
    // looked left of the digits)
    (void)w;
    int inkL = x0 + inkL0, inkR = x0 + inkR0;
    const int gap = 2;
    for (int k = 0; k < 10 && stage >= 1; k++) {   // not during the roll
      int a = inkL + (inkR - inkL + gap) * k / 10;
      int b = inkL + (inkR - inkL + gap) * (k + 1) / 10 - gap;
      if (k < segs) display.fillRect(a, 118, b - a, 7, GxEPD_WHITE);
      else display.drawRect(a, 118, b - a, 7, GxEPD_WHITE);
    }
    if (stage >= 1) {
      display.setTextSize(3);
      display.setCursor((200 - 7 * 18) / 2, 34);
      display.print("MILLION");
      display.setTextSize(1);
      centerSmall("BLOCK", 62);
    }
    if (stage >= 2) {
      display.drawFastHLine(30, 136, 140, GxEPD_WHITE);
      int64_t g = heightUtc(0), m = heightUtc(1000000L);
      double yrs = (double)(m - g) / (365.25 * 86400.0);
      snprintf(line, 34, "%.0f YEARS, ONE BLOCK AT A TIME", yrs);
      centerSmall(line, 146);
      char gs[16]; grouped(gs, 16, (long long)(supplyBTC(1000000L) + 0.5));
      snprintf(line, 34, "%s BTC ISSUED", gs);
      centerSmall(line, 160);
      centerSmall("TICK TOCK NEXT BLOCK", 178);
      display.drawRect(0, 0, 200, 200, GxEPD_WHITE);
      display.drawRect(2, 2, 196, 196, GxEPD_WHITE);
    }
  }

  void millionScreen() {
    // the odometer rolls over: each carry a partial
    static const long ROLL[8] = {999999L, 999990L, 999900L, 999000L,
                                 990000L, 900000L, 0L, 1000000L};
    for (int i = 0; i < 8; i++) {
      drawMillionCard(0, ROLL[i], 0);
      display.display(true);
      delay(i == 0 ? 500 : i == 6 ? 260 : 140);
    }
    drawMillionCard(1, 1000000L, 0); display.display(true); delay(300);
    for (int k = 1; k <= 10; k++) {            // ten segments, ten pulses
      drawMillionCard(1, 1000000L, k);
      display.display(true);
      buzz(60, 4);
    }
    drawMillionCard(2, 1000000L, 10);
    displayFull();            // full refresh: this one is worth it
    paintedDark = true;       // always black: the next face flips full
    Serial.printf("[1M] block 1,000,000 marked at tip %ld\n", blockHeight);
    sawMillion = true;
    savePrefs();
    // and it waits on the glass until a press, like a curiosity
    evCurioH = 1000000L;
    fireEvent(EV_CURIO);
    evShown = EV_CURIO;
  }

  // Wait for a button, asleep. The picker can be open for minutes while
  // someone walks the chain forty years out, and it used to spin in a
  // delay() loop with the chip fully awake the whole time. Light sleep keeps
  // RAM (target, step) and wakes on the press within milliseconds, so the
  // screen feels the same at a small fraction of the current.
  // Returns the pin pressed, or -1 if nothing was pressed in timeoutMs.
  // Not 0: on v3 the UP button IS GPIO 0, so 0 cannot mean "nothing".
  int waitForPress(uint32_t timeoutMs) {
    const int pins[4] = { MENU_BTN_PIN, BACK_BTN_PIN, UP_BTN_PIN, DOWN_BTN_PIN };
    const gpio_int_type_t lvl = BTN_ACTIVE ? GPIO_INTR_HIGH_LEVEL
                                           : GPIO_INTR_LOW_LEVEL;
    unsigned long t0 = millis();          // esp_timer runs on through light sleep
    while (true) {
      for (int i = 0; i < 4; i++)
        if (digitalRead(pins[i]) == BTN_ACTIVE) return pins[i];
      unsigned long el = millis() - t0;
      if (el >= timeoutMs) return -1;
      // Never light-sleep with the radio up: docked (the dock holds WiFi
      // the whole time) or on the QR's payment vigil. Light sleep dropped
      // the dock's association (DK- after leaving VERIFY) and, with the
      // radio's interrupts live, ended in an interrupt-watchdog reset.
      // Awake and polling instead; the radio is the bigger draw anyway.
      if (inDocked || WiFi.getMode() != WIFI_OFF) { delay(40); continue; }
      // The display's busy callback arms a wake on its BUSY line and never
      // disarms it. BUSY idles low, so left armed it would end every sleep
      // the instant it began.
      gpio_wakeup_disable((gpio_num_t)DISPLAY_BUSY);
      for (int i = 0; i < 4; i++) gpio_wakeup_enable((gpio_num_t)pins[i], lvl);
      esp_sleep_enable_gpio_wakeup();
      esp_sleep_enable_timer_wakeup((uint64_t)(timeoutMs - el) * 1000ULL);
      Serial.flush();
      esp_light_sleep_start();
      for (int i = 0; i < 4; i++) {
        gpio_wakeup_disable((gpio_num_t)pins[i]);
        // the wake set each pin to interrupt on a LEVEL: left that way, a
        // held button re-fires for as long as it is held
        gpio_set_intr_type((gpio_num_t)pins[i], GPIO_INTR_DISABLE);
      }
      esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
      esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
      // and put the pins back as the rest of the watch reads them: UP on v3
      // is GPIO 0 and needs its pull-up, or it reads as never pressed
      pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
      pinMode(UP_BTN_PIN, BTN_ACTIVE ? INPUT : INPUT_PULLUP);
      pinMode(DOWN_BTN_PIN, INPUT);
    }
  }

  // TIME TRAVEL. Everything the chain guarantees is a function of height:
  // the subsidy, the coins issued, the coins left, the epoch, the halving.
  // None of it needs a network. Price, fees and sentiment are NOT functions
  // of height, so this mode does not pretend to know them.
  //   UP / DOWN  move by the current step; HOLD to scroll, faster the longer
  //   MENU       cycles the step: block, day, month, year, halving, decade
  //   HOLD MENU  launch
  //   BACK       returns
  // Left alone for a minute it goes back to the face without launching:
  // a picker forgotten on a table must not hold the chip awake.
  // forAlarm: the same picker sets the block alarm instead of travelling.
  // Returns 1 when it launched or set, 0 on BACK, -1 when it timed out
  // (and has already gone back to the face).
  int timeTravel(bool forAlarm = false) {
    guiState = APP_STATE;
    long here = travelActive ? blockHeight : estHeight();   // 'now' is real,
    if (here <= 0) here = 964000;                          // even mid-travel
    long target = travelActive ? travelHeight : here;
    if (forAlarm)            // the armed height, or the next halving
      target = (alarmHeight > here) ? alarmHeight
                                    : (here / 210000L + 1) * 210000L;

    const int   NSTEP = 7;
    const int   S_LANDMARK = 5;                    // jumps, not a distance
    const long  STEPS[NSTEP] = { 1, 144, 4320, 52560, 210000, 0, 525600 };
    const char *SNAME[NSTEP] = { "BLOCK", "DAY", "MONTH", "YEAR",
                                 "HALVING", "LANDMARK", "DECADE" };
    int step = 3;
    const uint32_t IDLE_MS = 60000;
    const long MAX_H = 6930000L;                 // the last subsidy era

    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, BTN_ACTIVE ? INPUT : INPUT_PULLUP);   // UP has no
    pinMode(DOWN_BTN_PIN, INPUT);       // external pull-up on v3: floating, it
                                        // would wake the light sleep at random
    waitAllRelease();                   // the press that opened us

    auto clampT = [&](long t) { return t < 0 ? 0 : (t > MAX_H ? MAX_H : t); };
    // one move of the current step; landmarks jump to the next one along
    auto moveT = [&](long t, int dir, long mult) {
      if (step != S_LANDMARK) return clampT(t + dir * STEPS[step] * mult);
      for (long i = 0; i < mult; i++) {
        long nx = landmarkNext(t, dir);
        if (nx < 0 || nx > MAX_H) break;
        t = nx;
      }
      return t;
    };

    auto drawPicker = [&]() {
      display.setFullWindow();
      display.fillScreen(bg());
      display.setTextColor(fg());
      display.setFont(NULL);
      centerText(forAlarm ? "BLOCK ALARM" : "TIME TRAVEL", 12, NULL);

      char big[16]; snprintf(big, 16, "%ld", target);
      drawGrouped(big, 52, fitFont(big));
      display.setFont(NULL);
      char lmName[24];
      bool isLm = landmarkName(target, lmName, 24);
      centerText(forAlarm ? (target <= here ? "ALREADY MINED"
                                            : "ALARM AT HEIGHT")
                 : isLm ? lmName
                 : travelActive ? "BLOCK HEIGHT - TRAVELLING"
                 : (target == here ? "BLOCK HEIGHT - NOW"
                                   : "BLOCK HEIGHT"), 66, NULL);

      // when: the past from the record, the future at the measured pace
      long dBlocks = target - here;
      char line[34], span[20];
      double secs = (double)(heightUtc(target) - heightUtc(here));
      long mins = (long)(secs / 60.0);
      long days = mins / 1440;
      if (dBlocks == 0)      snprintf(span, 20, "TODAY");
      else if (labs(days) < 2)   snprintf(span, 20, "%ld HOURS", labs(mins) / 60);
      else if (labs(days) < 800) snprintf(span, 20, "%ld DAYS", labs(days));
      else                       snprintf(span, 20, "%ld YEARS", labs(days) / 365);
      if (dBlocks == 0) snprintf(line, 34, "TODAY");
      else if (isLm && !forAlarm) {               // a landmark carries its date
        static const char *MON[12] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                      "JUL","AUG","SEP","OCT","NOV","DEC"};
        Ymd c = civil64(heightUtc(target));
        if (dBlocks < 0) snprintf(line, 34, "%s %d - %s AGO", MON[c.m - 1], c.y, span);
        else             snprintf(line, 34, "~%s %d - IN %s", MON[c.m - 1], c.y, span);
      }
      else if (dBlocks > 0) snprintf(line, 34, "IN %s", span);
      else                  snprintf(line, 34, "%s AGO", span);
      centerText(line, 84, NULL);

      // what the chain guarantees at that height
      int era = (int)(target / 210000L);
      double sub = 50.0; for (int i = 0; i < era && i < 33; i++) sub /= 2.0;
      double issued = supplyBTC(target);

      display.drawFastHLine(14, 96, 172, fg());
      display.setCursor(14, 106);  display.printf("REWARD  %.8f", sub);
      display.setCursor(14, 120);  display.printf("EPOCH   %d of 33", era + 1);
      display.setCursor(14, 134);  display.printf("ISSUED  %.0f", issued);
      display.setCursor(14, 148);  display.printf("LEFT    %.0f", supplyCapBTC() - issued);
      display.setCursor(14, 162);  display.printf("MINED   %.4f%%", issued / supplyCapBTC() * 100.0);

      display.drawFastHLine(14, 172, 172, fg());
      snprintf(line, 34, "TRAVEL BY %s", SNAME[step]);
      centerText(line, 178, NULL);
      centerText(forAlarm ? "HOLD MENU TO SET" : "HOLD MENU TO LAUNCH",
                 190, NULL);
      display.display(true);
    };

    drawPicker();
    while (true) {
      int p = waitForPress(IDLE_MS);

      if (p < 0) {                        // nobody is choosing: go home
        Serial.println("[travel] picker idle - back to the face");
        guiState = WATCHFACE_STATE;
        RTC.read(currentTime);
        showFaceFull();
        return -1;
      }

      if (p == BACK_BTN_PIN) {
        waitAllRelease();
        if (!forAlarm) myShowMenu(menuIndex, false);   // alarms redraws itself
        return 0;
      }

      if (p == UP_BTN_PIN || p == DOWN_BTN_PIN) {
        // One step on the press. Held past a short pause, it keeps going and
        // speeds up: x1 for the first second, then x5, then x10 of the step.
        // Each redraw is a partial refresh of about a third of a second, so
        // the panel paces the scroll by itself.
        int dir = (p == UP_BTN_PIN) ? 1 : -1;
        target = moveT(target, dir, 1);
        drawPicker();
        unsigned long held0 = millis();
        while (digitalRead(p) == BTN_ACTIVE) {
          unsigned long h = millis() - held0;
          if (h < 450) { delay(10); continue; }
          long mult = (h < 1450) ? 1 : (h < 2950 ? 5 : 10);
          if (step == S_LANDMARK) { mult = 1; delay(250); }   // read each name
          long next = moveT(target, dir, mult);
          if (next == target) { delay(20); continue; }   // at an end
          target = next;
          drawPicker();
        }
        continue;
      }

      if (p == MENU_BTN_PIN) {
        if (heldFor(MENU_BTN_PIN, 800)) {
          if (forAlarm) {
            if (target <= here) {          // the past cannot ring: refuse
              buzz(30, 6); waitAllRelease(); drawPicker(); continue;
            }
            alarmHeight = target;
            savePrefs();                   // survives power loss
            buzz(50, 4);
            waitAllRelease();
            Serial.printf("[alarm] block alarm set for %ld\n", target);
            return 1;
          }
          // hold MENU: take the whole watch there. Height, supply and the
          // halving almanac follow; price, sats, cap, fees and the wallet
          // blank, because none of them are functions of height.
          travelActive = (target != here);
          travelHeight = target;
          travelWake   = wakeMin;
          if (travelActive) {
            // a moment of transit, and it names which way you are going:
            // the past is fetched, the future is only ever projected
            display.setFullWindow();
            display.fillScreen(bg());
            display.setTextColor(fg());
            if (target < blockHeight) {
              centerText("FETCHING", 92, NULL);
              centerText("THE RECORD", 108, NULL);
              display.display(true);
              fetchPastPrice(target);   // real data beats interpolation
            } else {
              long dB = target - blockHeight;
              char ln[26];
              snprintf(ln, 26, "%ld BLOCKS AHEAD", dB);
              centerText("TRAVELLING", 92, NULL);
              centerText(ln, 108, NULL);
              centerText("NOBODY HAS BEEN HERE", 126, NULL);
              display.display(true);
              delay(900);
            }
          }
          if (travelActive) {
            // the boot splash, reused: same screen the watch opens with,
            // and it already shows the height you are leaving behind
            display.setFullWindow();
            buzz(50, 4); delay(160); buzz(50, 4);
            drawSplash();
          } else buzz(50, 4);
          waitAllRelease();
          Serial.printf("[travel] to %ld (%s)\n", target,
                        travelActive ? "engaged" : "back to now");
          RTC.read(currentTime);
          showFaceFull();
          return 1;
        }
        step = (step + 1) % NSTEP;
        waitAllRelease();
        drawPicker();
      }
    }
  }

  // ---------------- ship mode ----------------
  // For the box. The deepest sleep there is: no minute wakes, no timer, no
  // radio, the accelerometer powered down. The e-paper holds a card telling
  // whoever opens it what to do, which costs nothing to keep up. Only a held
  // button (3 s) or USB brings it back; a button knocked in transit wakes the
  // chip for a moment, sees it was not held, and sleeps again.
  bool usbPresent() { pinMode(USB_DET_PIN, INPUT); return digitalRead(USB_DET_PIN) == 1; }

  // the block the watch was sealed at: its own height and hash tail, from
  // NVS, so it survives the wipe and a dead battery
  long sealHeight(char *tail, int n) {
    Preferences p; p.begin("btcchrono", true);
    long h = p.getLong("sealH", 0);
    String x = p.getString("sealX", "");
    long o = p.getLong("openH", 0);
    p.end();
    if (tail) snprintf(tail, n, "%s", x.c_str());
    openedHeight = o;
    return h;
  }
  long openedHeight = 0;

  void drawShipCard() {
    bool t = themeDark; themeDark = true;          // the card is always dark
    bootClear();
    bootCenter("BWATCH", 22, 3);
    display.drawFastHLine(40, 54, 120, GxEPD_WHITE);
    bootCenter("HOLD ANY BUTTON", 68, 1);
    bootCenter("FOR 3 SECONDS TO START", 82, 1);
    bootCenter("OR PLUG IN TO CHARGE", 102, 1);
    char tail[12];
    long h = sealHeight(tail, sizeof(tail));
    display.drawFastHLine(40, 120, 120, GxEPD_WHITE);
    if (h > 0) {
      bootCenter("SEALED AT BLOCK", 130, 1);
      char g[16]; snprintf(g, sizeof(g), "%ld", h);
      drawGrouped(g, 166, &DSEG7_Classic_Bold_25);
      display.setFont(NULL);
      if (tail[0]) {
        char l[20]; snprintf(l, sizeof(l), "...%s", tail);
        bootCenter(l, 178, 1);
      }
    } else {
      drawTinyB(97, 150);
    }
    themeDark = t;
    displayFull();                        // full: it has to last
  }

  // The block it is sealed at: asked for now, not remembered, so the card
  // never shows a stale one. Height first, then that height's own hash, so
  // the pair is consistent. On no network, the last height if it is fresh.
  void fetchSealBlock() {
    long h = 0; char tail[12] = {0};
    if (myConnectWiFi()) {
      WiFiClientSecure c; c.setInsecure();
      { HTTPClient http; http.setConnectTimeout(4000);
        if (http.begin(c, TIP_URL) && http.GET() == 200) {
          long v = http.getString().toInt();
          if (v >= 100000) h = v;
        }
        http.end(); }
      if (h > 0) {
        HTTPClient http; http.setConnectTimeout(4000);
        if (http.begin(c, "https://mempool.space/api/block-height/" + String(h)) &&
            http.GET() == 200) {
          String x = http.getString(); x.trim();
          if (x.length() == 64) snprintf(tail, sizeof(tail), "%s", x.c_str() + 56);
        }
        http.end();
      }
      WiFi.mode(WIFI_OFF); btStop();
    }
    if (h <= 0 && blockHeight > 0 && heightStaleMinutes() < staleAfter())
      h = blockHeight;                             // fresh enough to stand by
    Preferences p; p.begin("btcchrono", false);
    p.putLong("sealH", h);                         // 0: no block to show
    p.putString("sealX", tail);
    p.putLong("openH", 0);                         // opened: not yet
    p.end();
    Serial.printf("[ship] sealed at %ld ...%s\n", h, tail);
  }

  // Everything that belongs to whoever set this watch up goes: networks,
  // wallet, lightning, alarms, the cached balance. What stays is about the
  // board (battery calibration), the firmware (boot counters), and the
  // timezone, which is set for the buyer before sealing.
  void factoryWipe() {
    Preferences p; p.begin("btcchrono", false);
    for (int i = 0; i < 3; i++) {
      p.remove(("ws" + String(i)).c_str());
      p.remove(("wp" + String(i)).c_str());
    }
    p.remove("desc"); haveDesc = false;            // multisig, too
    p.putString("zpub", "");                       // stored empty: the compiled
    p.putString("lnaddr", "");                     // defaults do not return
    p.remove("wsat"); p.remove("ltx"); p.remove("ridx");
    p.remove("alH"); p.remove("alF");
    p.remove("nFrom"); p.remove("nTo");            // back to the default night
    p.remove("dFrom"); p.remove("dTo"); p.remove("hBlk"); p.remove("hPrc");
    p.remove("face"); p.remove("faceV"); p.remove("batt"); p.remove("wpriv");   // My Watch
    p.remove("cur"); p.remove("curOn");
    p.remove("bootPick");                           // back to Random
    p.end();
    WiFi.persistent(true);                         // the radio's own stored
    WiFi.mode(WIFI_STA);                           // credentials too: erasing
    WiFi.disconnect(true, true);                   // needs the radio started
    WiFi.mode(WIFI_OFF); btStop();
    rtcMagic = 0;                                  // and every cached value:
    sanitizeState();                               // the RTC layout reset
    Serial.println("[ship] wiped: networks, wallet, alarms, cache");
  }

  // Never returns on v3.
  void shipSleep() {
  #ifdef ARDUINO_ESP32S3_DEV
    WiFi.mode(WIFI_OFF); btStop();
    sensor.disableAccel();                         // its own few µA, for weeks
    if (!shipSince) { tmElements_t t; RTC.read(t); shipSince = (uint32_t)makeTime(t); }
    sleptSince = 0;
    Serial.println("[ship] asleep - hold a button 3 s or plug in");
    Serial.flush();
    display.hibernate();
    RTC.clearAlarm();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);   // no timer at all
    pinMode(USB_DET_PIN, INPUT);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)USB_DET_PIN,
                                 digitalRead(USB_DET_PIN) == 1 ? LOW : HIGH);
    rtc_gpio_set_direction((gpio_num_t)USB_DET_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en((gpio_num_t)USB_DET_PIN);
    esp_sleep_enable_ext1_wakeup(BTN_PIN_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
    rtc_gpio_set_direction((gpio_num_t)UP_BTN_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en((gpio_num_t)UP_BTN_PIN);
    rtc_clk_32k_enable(true);                      // the clock keeps time
    esp_deep_sleep_start();
  #endif
  }

  // Out of the box. The clock kept time the whole way, so the watch knows how
  // long it slept: everything it holds is that old, and the first thing it
  // does is fetch.
  void leaveShipMode(const char *why) {
    tmElements_t t; RTC.read(t);
    long el = shipSince ? (long)makeTime(t) - (long)shipSince : 0;
    if (el > 0) wakeMin += (uint32_t)(el / 60);
    shipMode = false; shipSince = 0; sleptSince = 0;
    restState = REST_NONE;
    awakeTill = wakeMin + PRESS_GRACE_MIN;
    sensor.enableAccel();
    forceFetch = true;
    Serial.printf("[ship] awake (%s) after %ld min\n", why, el / 60);
    buzz(50, 4);
  }

  // The first block seen after the box, once: sealed at X, opened at Y.
  void noteOpened() {
    if (openNoted || blockHeight <= 0) return;
    openNoted = true;                              // once per boot is enough
    Preferences p; p.begin("btcchrono", false);
    if (p.getLong("sealH", 0) > 0 && p.getLong("openH", 0) == 0) {
      p.putLong("openH", blockHeight);
      Serial.printf("[ship] opened at %ld\n", blockHeight);
    }
    p.end();
  }
  bool openNoted = false;

  // Out of the box with no network saved: ask, instead of leaving a DEMO
  // face for someone to work out. UP sets it up (five minutes, not two: a
  // first-timer is reading as they go), BACK leaves it for later.
  void firstStartWifi() {
    loadPrefs();
    if (wifiSsid[0][0]) return;                    // already has one
    themeDark = darkNow();
    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, BTN_ACTIVE ? INPUT : INPUT_PULLUP);
    pinMode(DOWN_BTN_PIN, INPUT);
    display.setFullWindow();
    display.fillScreen(bg());
    display.setTextColor(fg());
    display.setFont(NULL);
    centerText("LET'S GET ON WIFI", 30, NULL);
    display.drawFastHLine(14, 44, 172, fg());
    centerSmall("THE WATCH READS THE CHAIN", 62);
    centerSmall("OVER YOUR WIFI, A FEW", 76);
    centerSmall("TIMES AN HOUR.", 90);
    centerSmall("UP    SET UP NOW", 132);
    centerSmall("BACK  LATER (MENU > WIFI)", 148);
    displayFull();
    int p = waitForPress(120000);
    waitAllRelease();
    if (p != UP_BTN_PIN) return;                   // later, or nobody there

    display.fillScreen(bg());
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(0, 25);
    display.println("On your phone,");
    display.println("join WiFi:");
    display.println(WIFI_AP_SSID);
    display.println("");
    display.println("A page opens:");
    display.println("pick your network");
    display.println("and its password.");
    display.println("");
    display.println("Waits 5 min.");
    display.display(true);
    display.epd2.setBusyCallback(0);
    WiFiManager wm;
    bool ok = runPortal(wm, 300);
    display.epd2.setBusyCallback(WatchyDisplay::busyCallback);
    display.setFont(NULL);
    if (ok && WiFi.status() == WL_CONNECTED) {
      saveNetwork(WiFi.SSID(), WiFi.psk());
      forceFetch = true; failCount = 0; nextTryWake = 0;
      buzz(50, 4);
      Serial.printf("[first] joined %s\n", WiFi.SSID().c_str());
    } else {
      Serial.println("[first] no network - DEMO until MENU > Setup WiFi");
    }
    WiFi.mode(WIFI_OFF); btStop();
  }

  // MENU > Ship Mode. Says what will happen, shows the charge, and refuses
  // while USB is in (plugging in is one of the ways out).
  void shipModeScreen() {
    guiState = APP_STATE;
    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, BTN_ACTIVE ? INPUT : INPUT_PULLUP);
    pinMode(DOWN_BTN_PIN, INPUT);
    waitAllRelease();
    while (true) {
      themeDark = darkNow();
      bool usb = usbPresent();
      display.setFullWindow();
      display.fillScreen(bg());
      display.setTextColor(fg());
      display.setFont(NULL);
      centerText("SHIP MODE", 12, NULL);
      display.drawFastHLine(14, 26, 172, fg());
      centerSmall("ASLEEP UNTIL A 3 S BUTTON", 36);
      centerSmall("HOLD, OR PLUGGED IN", 48);
      centerSmall("ERASES WIFI, WALLET", 66);
      centerSmall("AND ALARMS FIRST", 78);
      centerSmall("SEALS AT THE CURRENT BLOCK", 90);
      if (WALLET_ZPUB[0] || WALLET_ADDR_LIST[0] || LN_ADDRESS[0])
        centerSmall("! WALLET IS IN THE FIRMWARE", 150);
      char b[28]; int pct = batteryPctShown();
      if (pct >= 0) snprintf(b, sizeof(b), "BATTERY %d%%", pct);
      else          snprintf(b, sizeof(b), "BATTERY --");
      centerText(b, 110, NULL);
      centerSmall("SHIP AT 40-50% CHARGE", 124);
      display.drawFastHLine(14, 168, 172, fg());
      centerSmall(usb ? "UNPLUG USB FIRST" : "HOLD MENU TO SHIP", 176);
      centerSmall("BACK TO CANCEL", 190);
      display.display(true);

      int p = waitForPress(60000);
      if (p < 0) {
        guiState = WATCHFACE_STATE;
        RTC.read(currentTime);
        showFaceFull();
        return;
      }
      if (p == BACK_BTN_PIN) { waitAllRelease(); myShowMenu(menuIndex, false); return; }
      if (p == MENU_BTN_PIN && heldFor(MENU_BTN_PIN, 800)) {
        if (usbPresent()) { buzz(30, 6); waitAllRelease(); continue; }
        buzz(50, 4);
        waitAllRelease();
        display.fillScreen(bg());
        centerText("SEALING...", 100, NULL);
        display.display(true);
        fetchSealBlock();                          // needs the WiFi: first
        factoryWipe();                             // then nothing is left
        shipMode = true;
        shipSince = 0;
        guiState = WATCHFACE_STATE;                // what it wakes into
        drawShipCard();
        shipSleep();                               // v3: does not return
        return;
      }
      waitAllRelease();                            // anything else: redraw
    }
  }

  // ---------------- alarms ----------------
  enum { EV_BLOCK = 1, EV_FEE = 2, EV_CONF = 4, EV_PAY = 8, EV_CURIO = 16,
         EV_RETG = 32, EV_HALV = 64, EV_AGE = 128 };
  enum { TRACK_NONE = 0, TRACK_RESOLVE = 1, TRACK_ON = 2 };

  bool alarmsArmed() { return alarmHeight > 0 || feeAlarm > 0; }

  // A receive QR (on-chain or lightning) belongs to whoever is scanning it.
  // An alarm still buzzes when it fires, but its screen waits until the QR
  // is put away, and buttons keep doing what the QR screen says they do.
  bool qrOnGlass() { return dispMode == M_WALT && walletView > 0; }

  // the armed indicator: a 7x8 bell, drawn in the status row just left of
  // the corner tag
  void drawBell(int x, int y) {
    display.drawPixel(x + 3, y, fg());              // the loop
    display.drawFastHLine(x + 2, y + 1, 3, fg());
    display.fillRect(x + 1, y + 2, 5, 3, fg());     // the body
    display.drawFastHLine(x, y + 5, 7, fg());       // the lip
    display.drawPixel(x + 3, y + 7, fg());          // the clapper
  }

  void fmtGrouped(long long v, char *out, int n) {  // 1,050,000
    char d[24]; snprintf(d, 24, "%lld", v < 0 ? -v : v);
    int len = strlen(d), o = 0;
    if (v < 0 && o < n - 1) out[o++] = '-';
    for (int i = 0; i < len && o < n - 1; i++) {
      out[o++] = d[i];
      int left = len - i - 1;
      if (left > 0 && left % 3 == 0 && o < n - 1) out[o++] = ',';
    }
    out[o] = 0;
  }

  // Exact BTC, trailing zeros dropped: 1, 0.75, 0.1, 0.12345678. A receipt
  // never rounds what was paid.
  void fmtBtcExact(long long sats, char *out, int n) {
    long long w = sats / 100000000LL, fr = sats % 100000000LL;
    if (fr == 0) { snprintf(out, n, "%lld", w); return; }
    char f[12]; snprintf(f, 12, "%08lld", fr);
    for (int i = 7; i > 0 && f[i] == '0'; i--) f[i] = 0;
    snprintf(out, n, "%lld.%s", w, f);
  }

  // Under 0.1 BTC an amount reads in sats; from 0.1 up, in BTC.
  bool amountInBtc(long long sats) { return sats >= 10000000LL; }

  // the inked box of one glyph
  void glyphBox(const GFXfont *f, char c, int &xo, int &yo, int &w, int &h) {
    int first = (int)pgm_read_byte(&f->first);
    const GFXglyph *g = &f->glyph[(int)c - first];
    w  = (int)pgm_read_byte(&g->width);
    h  = (int)pgm_read_byte(&g->height);
    xo = (int)(int8_t)pgm_read_byte(&g->xOffset);
    yo = (int)(int8_t)pgm_read_byte(&g->yOffset);
  }

  int amountCommaW(const GFXfont *f) {      // as drawGrouped spaces them
    return (f == &DSEG7_Classic_Bold_32) ? 6 : (f == &DSEG7_Classic_Bold_25) ? 4 : 3;
  }

  // ink width of an amount string in a face: plain digits (grouped from five
  // up, like drawGrouped) or a decimal (DSEG's point has no advance)
  int amountInkW(const char *s, const GFXfont *f) {
    int n = (int)strlen(s), digits = 0;
    bool dec = strchr(s, '.') != nullptr;
    for (int i = 0; i < n; i++) if (s[i] >= '0' && s[i] <= '9') digits++;
    int raw = dec ? digits * digitAdvance(f)
                  : n * digitAdvance(f) + ((digits >= 5) ? (digits - 1) / 3 : 0) * amountCommaW(f);
    return raw - glyphLeft(f, s[0]) - glyphRightGap(f, s[n - 1]);
  }

  // ₿ in the font's own terms. A seven-segment display has no B: its B is an
  // 8. So the sign is the font's 8 with the two Bitcoin strokes standing
  // above and below it, about a segment thick. Drawn from its ink edge.
  void drawBtcSign(const GFXfont *f, int inkX, int y) {
    int xo, yo, w, h; glyphBox(f, '8', xo, yo, w, h);
    display.setFont(f);
    display.setCursor(inkX - xo, y);
    display.print('8');
    int th  = (w / 6 < 1) ? 1 : w / 6;      // about a segment thick
    int len = th * 2 + 1;                   // long enough that it never
                                            // reads as a plain 8
    int sx  = inkX + w / 3;
    for (int k = 0; k < 2; k++) {
      int x = sx + k * th * 2;
      display.fillRect(x, y + yo - len, th, len, fg());
      display.fillRect(x, y + yo + h,   th, len, fg());
    }
  }

  // A payment amount as a headline: ₿ then the number, centred together by
  // their ink. The largest face that fits; 1,000,000 sats and up steps down.
  // Returns true when it was set in BTC.
  bool drawAmount(long long sats, int y) {
    char s[24];
    bool btc = amountInBtc(sats);
    if (btc) fmtBtcExact(sats, s, 24); else snprintf(s, 24, "%lld", sats);
    const GFXfont *ladder[4] = { &DSEG7_Classic_Bold_32, &DSEG7_Classic_Bold_25,
                                 &DSEG7_Classic_Bold_18, &DSEG7_Classic_Bold_14 };
    const GFXfont *f = ladder[3];
    int numW = 0, signW = 0, gap = 0;
    for (int i = 0; i < 4; i++) {
      int xo, yo, w, h; glyphBox(ladder[i], '8', xo, yo, w, h);
      // twice the space between two digits: any less and the sign reads
      // as one more digit (5000 became 85000)
      f = ladder[i]; signW = w; gap = 2 * (digitAdvance(ladder[i]) - w);
      numW = amountInkW(s, f);
      if (signW + gap + numW <= 188) break;   // ~6 px of margin each side
    }
    int x = (200 - (signW + gap + numW)) / 2;
    if (x < 0) x = 0;
    drawBtcSign(f, x, y);
    // the number, from its first ink column
    int nx = x + signW + gap - glyphLeft(f, s[0]);
    display.setFont(f);
    if (strchr(s, '.')) {                     // decimals: one size, DSEG's own point
      display.setCursor(nx, y);
      display.print(s);
    } else {
      int digits = (int)strlen(s), cw = amountCommaW(f);
      for (int i = 0; s[i]; i++) {
        char c[2] = { s[i], 0 };
        display.setCursor(nx, y);
        display.print(c);
        nx += digitAdvance(f);
        int left = digits - i - 1;
        if (digits >= 5 && left > 0 && left % 3 == 0) {   // as drawGrouped
          int o = (cw - 2) / 2;
          display.fillRect(nx + o, y - 3, 2, 3, fg());
          display.drawPixel(nx + o, y, fg());
          display.drawPixel(nx + o - 1, y + 1, fg());
          nx += cw;
        }
      }
    }
    display.setFont(NULL);
    return btc;
  }

  time_t localFromUtc(time_t utc) {
    return utc + (tzIndex > 0 ? tzOffsetSec(tzIndex, utc) : settings.gmtOffset);
  }

  // ALARMS: two rows, block and fee. UP/DOWN picks a row, MENU edits it,
  // holding MENU turns it off, BACK leaves. Asleep between presses, home to
  // the face after a minute, like every other screen now.
  void alarmsScreen() {
    guiState = APP_STATE;
    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, BTN_ACTIVE ? INPUT : INPUT_PULLUP);
    pinMode(DOWN_BTN_PIN, INPUT);
    waitAllRelease();
    int row = 0;
    while (true) {
      themeDark = darkNow();
      display.setFullWindow();
      display.fillScreen(bg());
      display.setTextColor(fg());
      display.setFont(NULL);
      centerText("ALARMS", 12, NULL);
      for (int r = 0; r < 2; r++) {
        int y0 = 28 + r * 68;
        bool sel = (row == r);
        if (sel) display.fillRect(6, y0, 188, 62, fg());
        else     display.drawRect(6, y0, 188, 62, fg());
        display.setTextColor(sel ? bg() : fg());
        display.setCursor(14, y0 + 8);
        display.print(r == 0 ? "BLOCK" : "FEE");
        char v[20], sub[32]; sub[0] = 0;
        if (r == 0) {
          if (alarmHeight > 0) {
            fmtGrouped(alarmHeight, v, 20);
            long d = alarmHeight - blockHeight;
            if (blockHeight <= 0)  sub[0] = 0;
            else if (d <= 0)       snprintf(sub, 32, "REACHED");
            else if (d < 144)      snprintf(sub, 32, "IN %ld BLOCKS", d);
            else {
              long days = (long)(d * (double)avgBlockSec / 86400.0);
              if (days < 800) snprintf(sub, 32, "IN ~%ld DAYS", days);
              else            snprintf(sub, 32, "IN ~%ld YEARS", days / 365);
            }
          } else snprintf(v, 20, "OFF");
        } else {
          if (feeAlarm > 0) {
            snprintf(v, 20, "<= %u", feeAlarm);
            if (feeArmed) snprintf(sub, 32, "SAT/VB");
            else snprintf(sub, 32, "FIRED - REARMS AT %u+",
                          feeAlarm + (feeAlarm / 2 > 2 ? feeAlarm / 2 : 2));
          } else snprintf(v, 20, "OFF");
        }
        display.setTextSize(2);
        display.setCursor(14, y0 + 22); display.print(v);
        display.setTextSize(1);
        if (sub[0]) { display.setCursor(14, y0 + 46); display.print(sub); }
        display.setTextColor(fg());
      }
      centerSmall("MENU EDIT - HOLD MENU OFF", 170);
      centerSmall("UP/DOWN PICK - BACK DONE", 184);
      display.display(true);

      int p = waitForPress(60000);
      if (p < 0) {                                  // nobody: home
        guiState = WATCHFACE_STATE;
        RTC.read(currentTime);
        showFaceFull();
        return;
      }
      if (p == BACK_BTN_PIN) { waitAllRelease(); myShowMenu(menuIndex, false); return; }
      if (p == UP_BTN_PIN || p == DOWN_BTN_PIN) { row ^= 1; waitAllRelease(); continue; }
      if (p == MENU_BTN_PIN) {
        if (heldFor(MENU_BTN_PIN, 800)) {           // hold: off
          if (row == 0) alarmHeight = 0;
          else { feeAlarm = 0; feeArmed = true; }
          savePrefs();
          buzz(30, 2);
          waitAllRelease();
          Serial.printf("[alarm] %s alarm off\n", row == 0 ? "block" : "fee");
          continue;
        }
        waitAllRelease();
        if (row == 0) { if (timeTravel(true) < 0) return; }
        else          { if (!editFee()) return; }
      }
    }
  }

  // The fee threshold. UP/DOWN change it (hold to run), HOLD MENU sets it,
  // BACK leaves it as it was. Returns false if it timed out to the face.
  bool editFee() {
    int v = feeAlarm > 0 ? feeAlarm : (fastFee >= 2 ? (int)fastFee - 1 : 1);
    auto clampF = [](int x) { return x < 1 ? 1 : (x > 999 ? 999 : x); };
    auto draw = [&]() {
      themeDark = darkNow();
      display.setFullWindow();
      display.fillScreen(bg());
      display.setTextColor(fg());
      display.setFont(NULL);
      centerText("FEE ALARM", 12, NULL);
      char big[8]; snprintf(big, 8, "%d", v);
      drawGrouped(big, 70, fitFont(big));
      display.setFont(NULL);
      centerSmall("SAT/VB", 80);
      centerSmall("BUZZ AT OR BELOW THIS", 102);
      char a[8], now[28]; fmtFee(a, 8, fastFee);
      snprintf(now, 28, "FAST FEE NOW %s", a);
      centerSmall(now, 120);
      display.drawFastHLine(14, 172, 172, fg());
      centerSmall("UP/DOWN TO CHANGE", 178);
      centerSmall("HOLD MENU TO SET", 190);
      display.display(true);
    };
    draw();
    while (true) {
      int p = waitForPress(60000);
      if (p < 0) {
        guiState = WATCHFACE_STATE;
        RTC.read(currentTime);
        showFaceFull();
        return false;
      }
      if (p == BACK_BTN_PIN) { waitAllRelease(); return true; }
      if (p == UP_BTN_PIN || p == DOWN_BTN_PIN) {
        int dir = (p == UP_BTN_PIN) ? 1 : -1;
        v = clampF(v + dir); draw();
        unsigned long held0 = millis();             // hold: run, then x5
        while (digitalRead(p) == BTN_ACTIVE) {
          unsigned long h = millis() - held0;
          if (h < 450) { delay(10); continue; }
          int next = clampF(v + dir * (h < 1950 ? 1 : 5));
          if (next == v) { delay(20); continue; }
          v = next; draw();
        }
        continue;
      }
      if (p == MENU_BTN_PIN) {
        if (heldFor(MENU_BTN_PIN, 800)) {
          feeAlarm = (uint16_t)v; feeArmed = true;
          savePrefs();
          buzz(50, 4);
          waitAllRelease();
          Serial.printf("[alarm] fee alarm set at %d sat/vB\n", v);
          return true;
        }
        waitAllRelease();                           // a tap does nothing
      }
    }
  }

  // ---- events: what fired, queued until someone has seen it ----
  void fireEvent(uint8_t bit) {
    evQueue |= bit;
    evWake = wakeMin;
    // the same kind firing again (1/6 then 3/6) carries new numbers: if its
    // screen is already up, it must be redrawn, not left saying the old ones
    if (evShown == bit) evShown = 0;
    Serial.printf("[alarm] fired %u (queue %u)\n", bit, evQueue);
    // each its own pattern, so the wrist knows which before the eyes do.
    // Through buzz(): an odd vibMotor length leaves the motor running.
    if (bit == EV_PAY || bit == EV_CURIO || bit == EV_RETG || bit == EV_HALV || bit == EV_AGE) {
      // silent here (a curiosity buzzes in its own animation): whoever saw it land has already played the payment
      // pattern, the same one the wallet face has always used
    } else if (bit == EV_BLOCK) {
      buzz(75, 14);                                         // one long
    } else if (bit == EV_FEE) {
      buzz(75, 4); delay(160); buzz(75, 4);                 // two short
    } else {
      int ticks = evConfN >= 6 ? 6 : (evConfN >= 3 ? 3 : 1);
      for (int i = 0; i < ticks; i++) { buzz(40, 2); delay(140); }
    }
  }

  // Arithmetic only: called after every fetch and on the dock's own polls.
  // Returns true if anything fired.
  bool checkAlarms() {
    bool fired = false;
    if (alarmHeight > 0 && blockHeight > 0 && blockHeight >= alarmHeight) {
      evBlockH = alarmHeight;
      alarmHeight = 0;                   // a block alarm rings once
      savePrefs();
      fireEvent(EV_BLOCK); fired = true;
    }
    if (feeAlarm > 0 && fastFee > 0) {
      uint16_t rearm = feeAlarm + (feeAlarm / 2 > 2 ? feeAlarm / 2 : 2);
      if (feeArmed && fastFee <= feeAlarm) {
        evFee = fastFee; evFeeAt = feeAlarm;
        feeArmed = false;                // quiet until fees rise again
        fireEvent(EV_FEE); fired = true;
      } else if (!feeArmed && fastFee >= rearm) {
        feeArmed = true;
        Serial.printf("[alarm] fee alarm re-armed (%.1f)\n", fastFee);
      }
    }
    if (trackState == TRACK_ON && trackHeight > 0 && blockHeight >= trackHeight) {
      long n = blockHeight - trackHeight + 1;
      uint8_t stage = n >= 6 ? 6 : (n >= 3 ? 3 : 1);
      if (stage > trackShown) {          // a jump (say 0 -> 4 while resting)
        trackShown = stage;              // announces only the highest reached
        evConfN = stage; evConfSats = trackSats; evConfBlock = trackHeight;
        fireEvent(EV_CONF); fired = true;
      }
      if (stage >= 6) {                  // settled: stop following it
        strncpy(lastDoneTx, trackTxid, 8); lastDoneTx[8] = 0;
        trackState = TRACK_NONE;
        Serial.println("[track] settled");
      }
    }
    return fired;
  }

  // The alarm screens. One layout for all three: an inverted tag (the only
  // filled tag on the watch), what happened, one big number, context, and
  // the one thing to do about it.
  void drawEventScreen(uint8_t ev) {
    if (ev == EV_AGE)  { drawAgeCard(); return; }
    if (ev == EV_RETG) { drawRetarget(heldRetarget(), 4); return; }
    if (ev == EV_HALV) { drawHalving(heldHalving(), 4); return; }
    if (ev == EV_CURIO) {
      const char *why = curiosity(evCurioH);
      drawCurioFrame(evCurioH, why ? why : "CURIOUS BLOCK");
      return;
    }
    themeDark = darkNow();
    display.setFullWindow();
    display.fillScreen(bg());
    display.setTextColor(fg());
    display.setFont(NULL);
    { time_t lt = makeTime(currentTime);
      if (tzIndex > 0) {
        time_t utc = lt - settings.gmtOffset;
        lt = utc + tzOffsetSec(tzIndex, utc);
      }
      tmElements_t dt; breakTime(lt, dt);
      char clk[6]; snprintf(clk, 6, "%02d:%02d", dt.Hour, dt.Minute);
      display.setCursor(10, 3); display.print(clk); }
    const char *tag = (ev == EV_CONF) ? "CONF" : (ev == EV_PAY) ? "PAY" : "ALARM";
    int tw = (int)strlen(tag) * 6, tx = 188 - tw;
    display.fillRect(tx - 3, 1, tw + 5, 11, fg());
    display.setTextColor(bg());
    display.setCursor(tx, 3); display.print(tag);
    display.setTextColor(fg());

    char head[24], big[20], l1[30], l2[30], l3[30];
    l1[0] = l2[0] = l3[0] = 0;
    if (ev == EV_BLOCK) {
      snprintf(head, 24, "BLOCK REACHED");
      snprintf(big, 20, "%ld", evBlockH);
      if (rwdHeight == evBlockH && tipBlockTime > 0) {   // its own block's
        tmElements_t d; breakTime(localFromUtc(tipBlockTime), d);   // facts
        snprintf(l1, 30, "MINED %02d:%02d", d.Hour, d.Minute);
        if (poolName[0]) snprintf(l2, 30, "BY %s", poolName);
      } else if (blockHeight > evBlockH) {
        char g[16]; fmtGrouped(blockHeight, g, 16);
        snprintf(l1, 30, "TIP NOW %s", g);
      }
    } else if (ev == EV_PAY) {
      // on-chain is not final yet, so it says where it is and what comes
      // next; lightning is final the moment it lands, so it says that
      snprintf(head, 24, evPayLn ? "LIGHTNING RECEIVED" : "PAYMENT INCOMING");
      big[0] = 0;                      // drawn by drawAmount, with its ₿
      const char *unit = amountInBtc(evPaySats) ? "BTC" : "SATS";
      snprintf(l1, 30, "%s - %s", unit, evPayLn ? "SETTLED" : "IN MEMPOOL");
      if (!evPayLn && trackState != TRACK_NONE)
        snprintf(l2, 30, "TICKS AT 1, 3, 6 CONF");
    } else if (ev == EV_FEE) {
      snprintf(head, 24, "FEES ARE LOW");
      fmtFee(big, 20, evFee);          // the face's own formatter
      snprintf(l1, 30, "SAT/VB - ALARM AT %u", evFeeAt);
      char a[8], b[8]; fmtFee(a, 8, medFee); fmtFee(b, 8, lowFee);
      snprintf(l2, 30, "30M %s - 1H %s", a, b);
    } else {
      snprintf(head, 24, evConfN >= 6 ? "PAYMENT SETTLED" : "PAYMENT CONFIRMED");
      snprintf(big, 20, "%d", evConfN);
      snprintf(l1, 30, "OF 6 CONFIRMATIONS");
      char g[24];
      if (amountInBtc(evConfSats)) { fmtBtcExact(evConfSats, g, 24);
                                     snprintf(l2, 30, "+%s BTC", g); }
      else { fmtGrouped(evConfSats, g, 24); snprintf(l2, 30, "+%s SATS", g); }
      char hb[16]; fmtGrouped(evConfBlock, hb, 16);
      snprintf(l3, 30, "IN BLOCK %s", hb);
    }
    centerSmall(head, 40);
    if (ev == EV_PAY) drawAmount(evPaySats, 92);
    else drawGrouped(big, 92, fitFont(big));
    display.setFont(NULL);
    if (l1[0]) centerSmall(l1, 112);
    if (l2[0]) centerSmall(l2, 128);
    if (l3[0]) centerSmall(l3, 144);
    display.drawRect(5, 186, 190, 13, fg());
    centerSmall("ANY BUTTON: OK", 189);
  }

  uint8_t firstEvent() {
    for (uint8_t b = 1; b; b <<= 1) if (evQueue & b) return b;
    return 0;
  }

  // Put the oldest waiting event on the glass. On a minute wake, if it is
  // already there, nothing is redrawn: the watch just sleeps another minute.
  bool showEvent(bool timerWake) {
    uint8_t ev = firstEvent();
    if (!ev) return false;
    qrDrawnIdx = -1;                              // it covers any QR
    if (timerWake && !inDocked && evShown == ev) {
      if (!longSleep(1)) drawEventScreen(ev);     // v3 does not return
      return true;
    }
    drawEventScreen(ev);
    evShown = ev;
    // the curiosity is black whatever the theme: a flip either way goes full
    bool dark = (ev == EV_CURIO || ev == EV_RETG || ev == EV_HALV || ev == EV_AGE) ? true : themeDark;
    if (timerWake && !inDocked) {                 // first sight: a full
      displayFull();                     // refresh, then sleep
      paintedDark = dark;
      longSleep(1);                               // without a second one
    }
    if (dark != paintedDark) { fullOnClose(); paintedDark = dark; }
    return true;                                  // otherwise the caller
  }                                               // sends it to the panel

  void dismissEvent() {
    uint8_t ev = firstEvent();
    evQueue &= (uint8_t)~ev;
    evShown = 0;
    Serial.printf("[alarm] dismissed %u, queue %u\n", ev, evQueue);
  }

  // ---- late payments ----
  enum { LATE_NONE = 0, LATE_CHAIN = 1, LATE_LN = 2 };

  // A payment found by anything other than the vigil itself: the same buzz,
  // the same bookkeeping, and a PAY screen if the wallet face is not up.
  void paymentLanded(long long sats, bool ln) {
    vibMotor(60, 4); delay(120);
    vibMotor(60, 4); delay(120);
    vibMotor(60, 8);
    Serial.printf("[late] %s payment arrived: %lld sats\n", ln ? "lightning" : "on-chain", sats);
    if (dispMode != M_WALT) {
      evPaySats = sats; evPayLn = ln;
      fireEvent(EV_PAY);
    }
  }

  // Radio on, inside fetchAll: one small request while the watch is on.
  void lateWatchStep() {
    if (lateKind == LATE_NONE) return;
    if ((int32_t)(wakeMin - lateUntil) >= 0) {
      Serial.println("[late] hour is up - the wallet scan will find anything later");
      lateKind = LATE_NONE;
      return;
    }
    if (lateKind == LATE_LN) {
      if (!lnVerify[0]) { lateKind = LATE_NONE; return; }   // consumed or replaced
      if (lnSettled()) {
        lnVerify[0] = 0; lnInvoice[0] = 0;                  // invoice is consumed
        lateKind = LATE_NONE;
        paymentLanded(LN_REQUEST_SATS, true);
      }
      return;
    }
    String a = walletAddr(lateIdx);
    if (!a.length()) { lateKind = LATE_NONE; return; }
    WiFiClientSecure c; c.setInsecure();
    HTTPClient http; http.setConnectTimeout(4000);
    if (http.begin(c, String(ESPLORA_BASE) + a) && http.GET() == 200) {
      JsonDocument doc;
      if (!deserializeJson(doc, http.getString())) {
        uint64_t f =
          (uint64_t)(doc["chain_stats"]["funded_txo_sum"] | 0ULL) +
          (uint64_t)(doc["mempool_stats"]["funded_txo_sum"] | 0ULL);
        if (f > lateBase) {
          uint64_t delta = f - lateBase;
          lateKind = LATE_NONE;
          lastTxSats = (long long)delta;       // the vigil's bookkeeping,
          walletSats += delta;                 // exactly
          haveWallet = true;
          armTracking(a.c_str());
          if (recvIndex == lateIdx) recvIndex++;   // address used: advance
          if (qrOnGlass() && walletView == 1) walletView = 0;
          savePrefs();
          paymentLanded((long long)delta, false);
        }
      }
    }
    http.end();
  }

  // ---- confirmation tracking ----
  // The Esplora instance the wallet already trusts, so a payment's txid goes
  // nowhere the addresses have not already been.
  String esploraRoot() {
    String s = ESPLORA_BASE;                       // ".../api/address/"
    int k = s.lastIndexOf("address/");
    return k > 0 ? s.substring(0, k) : s;
  }

  // a payment was seen arriving at addr: follow it (one at a time)
  void armTracking(const char *addr) {
    if (trackState != TRACK_NONE || !addr || !addr[0]) return;
    if (isDemoWallet()) return;          // tributes to Satoshi are not yours
    strncpy(trackAddr, addr, 63); trackAddr[63] = 0;
    trackTxid[0] = 0; trackSats = 0; trackHeight = 0; trackShown = 0;
    trackWake = wakeMin;
    trackState = TRACK_RESOLVE;
    Serial.printf("[track] armed for %s\n", trackAddr);
  }

  int trackConfs() {
    if (trackState != TRACK_ON || trackHeight <= 0 || blockHeight < trackHeight)
      return 0;
    long n = blockHeight - trackHeight + 1;
    return n > 5 ? 5 : (int)n;                    // 6 ends the tracking
  }

  // Radio on, inside fetchAll. RESOLVE learns which transaction it was;
  // ON asks for its status until it is in a block. After that the count is
  // arithmetic on the tip height, in checkAlarms.
  void trackStep(WiFiClientSecure &client) {
    if (trackState == TRACK_NONE) return;
    if (trackHeight == 0 && wakeMin - trackWake > TRACK_GIVEUP_MIN) {
      Serial.println("[track] no confirmation in 3 days - letting it go");
      if (trackTxid[0]) { strncpy(lastDoneTx, trackTxid, 8); lastDoneTx[8] = 0; }
      trackState = TRACK_NONE;
      return;
    }
    String root = esploraRoot();
    if (trackState == TRACK_RESOLVE) {
      // newest first, mempool first: the payment we just watched land.
      // HTTP/1.0 so the reply is never chunked: it is parsed straight off
      // the socket, which chunk headers would corrupt.
      HTTPClient http; http.setConnectTimeout(4000); http.useHTTP10(true);
      if (http.begin(client, root + "address/" + String(trackAddr) + "/txs") &&
          http.GET() == 200) {
        JsonDocument filter;
        filter[0]["txid"] = true;
        filter[0]["status"]["confirmed"] = true;
        filter[0]["status"]["block_height"] = true;
        filter[0]["vout"][0]["scriptpubkey_address"] = true;
        filter[0]["vout"][0]["value"] = true;
        JsonDocument doc;
        if (!deserializeJson(doc, http.getStream(),
                             DeserializationOption::Filter(filter))) {
          for (JsonObject tx : doc.as<JsonArray>()) {
            long long paid = 0;
            for (JsonObject o : tx["vout"].as<JsonArray>())
              if (strcmp(o["scriptpubkey_address"] | "", trackAddr) == 0)
                paid += o["value"] | 0LL;
            if (paid <= 0) continue;               // a spend, not a receipt
            const char *id = tx["txid"] | "";
            if (strlen(id) != 64) break;
            if (lastDoneTx[0] && strncmp(id, lastDoneTx, 8) == 0) {
              trackState = TRACK_NONE;             // already reported
              Serial.println("[track] already reported - not again");
              break;
            }
            strncpy(trackTxid, id, 64); trackTxid[64] = 0;
            trackSats = paid;
            trackHeight = (tx["status"]["confirmed"] | false)
                            ? (long)(tx["status"]["block_height"] | 0L) : 0;
            trackShown = 0;
            trackState = TRACK_ON;
            Serial.printf("[track] %.8s... %lld sats, %s\n", trackTxid,
                          trackSats, trackHeight ? "confirmed" : "in mempool");
            break;
          }
        }
      }
      http.end();
      return;
    }
    if (trackState == TRACK_ON && trackHeight == 0) {
      HTTPClient http; http.setConnectTimeout(4000);
      if (http.begin(client, root + "tx/" + String(trackTxid) + "/status") &&
          http.GET() == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString()) &&
            (doc["confirmed"] | false)) {
          long bh = doc["block_height"] | 0L;
          if (bh > 0) {
            trackHeight = bh;
            Serial.printf("[track] confirmed in block %ld\n", bh);
          }
        }
      }
      http.end();
    }
  }


  // ---------------- DISPLAY LAB (Oct 17) ----------------
  // About, short DOWN. Scripted refresh sequences on test cards, logged over
  // serial, to MEASURE why images fade and ghost instead of guessing. Each
  // step buzzes and waits for a press (BACK leaves the lab). Photograph the
  // glass at each prompt and ~5 s later.
  uint16_t labBg = GxEPD_BLACK, labFg = GxEPD_WHITE;   // L7+: light cards too
  void labCard(char v, const char *id) {
    display.setFullWindow();
    display.fillScreen(labBg);
    display.setTextColor(labFg);
    display.setFont(NULL); display.setTextSize(3);
    display.setCursor(8, 8); display.print(id);
    display.setTextSize(1);
    if (v == 'A') {
      display.fillCircle(100, 110, 42, labFg);
      display.fillRect(20, 170, 160, 12, labFg);
      display.setCursor(150, 12); display.print("CARD A");
    } else if (v == 'B') {
      display.fillRect(30, 60, 140, 70, labFg);
      display.fillCircle(160, 165, 18, labFg);
      display.setCursor(150, 12); display.print("CARD B");
    } else {
      display.fillTriangle(100, 50, 30, 170, 170, 170, labFg);
      for (int y = 60; y < 120; y += 12) display.fillRect(8, y, 30, 5, labFg);
      display.setCursor(150, 12); display.print("CARD C");
    }
  }
  // a prompt: log, buzz, wait. false = BACK (leave the lab)
  bool labStep(const char *msg) {
    Serial.printf("[lab] %7lu %s  -> press any button (BACK ends)\n", millis(), msg);
    Serial.flush();
    buzz(30, 2);
    waitAllRelease();
    int p = waitForPress(180000);
    if (p < 0 || p == BACK_BTN_PIN) { waitAllRelease(); return false; }
    waitAllRelease();
    return true;
  }
  uint8_t *labBuf() { return display.*epdGet(EpdBufTag()); }
  // full refresh without the library's power-off at the end
  void labFullKeepOn() {
    display.epd2.writeImageForFullRefresh(labBuf(), 0, 0, 200, 200);
    display.epd2.refresh(false);
    display.epd2.writeImageAgain(labBuf(), 0, 0, 200, 200);
  }
  void labFull()    { display.display(false); panelOff(); }
  void labPartial() { display.display(true); }        // Watchy's own partial
  void labDark()  { labBg = GxEPD_BLACK; labFg = GxEPD_WHITE; }
  void labLight() { labBg = GxEPD_WHITE; labFg = GxEPD_BLACK; }
  void labRun() {
    EpdFull::diag(display.epd2);
    Serial.println("\n[lab] ===== DISPLAY LAB 2: dark to light =====");
    // Lab 1 (L1-L6, black cards) all ran clean. The fade shows going from a
    // DARK screen to a LIGHT one. Each test: a black card, then a light card
    // sent a different way. Watch the light card for ~10 s each time.
    // M1: plain full + forced off (lab 1's way)
    labDark();  labCard('A', "M1"); labFull();
    if (!labStep("M1a black card")) return;
    labLight(); labCard('B', "M1"); labFull();
    if (!labStep("M1b LIGHT card: full + off. Watch 10 s: fade?")) return;
    // M2: the watch's real path: displayFull() (full, memory sync, off)
    labDark();  labCard('A', "M2"); labFull();
    if (!labStep("M2a black card")) return;
    labLight(); labCard('B', "M2"); displayFull();
    if (!labStep("M2b LIGHT card: displayFull (with sync). Watch 10 s: fade?")) return;
    // M3: full with only the library's own power-off
    labDark();  labCard('A', "M3"); labFull();
    if (!labStep("M3a black card")) return;
    labLight(); labCard('B', "M3"); display.display(false);
    if (!labStep("M3b LIGHT card: full, library off only. Watch 10 s: fade?")) return;
    panelOff();
    // M4: full, voltage left ON
    labDark();  labCard('A', "M4"); labFull();
    if (!labStep("M4a black card")) return;
    labLight(); labCard('B', "M4"); labFullKeepOn();
    if (!labStep("M4b LIGHT card: full, voltage ON. Watch 10 s: fade?")) return;
    panelOff();
    // M5: the reverse, light to dark, the watch's real path
    labLight(); labCard('A', "M5"); labFull();
    if (!labStep("M5a light card")) return;
    labDark();  labCard('B', "M5"); displayFull();
    if (!labStep("M5b BLACK card: displayFull. Watch 10 s: fade?")) return;
    // M6: the real face, dark then light, through the real button path
    {
      uint8_t keep = themeMode;
      themeMode = 1; buttonWake = true; showWatchFace(true);
      if (!labStep("M6a the real face, DARK")) { themeMode = keep; return; }
      themeMode = 0; buttonWake = true; showWatchFace(true);
      if (!labStep("M6b the real face, LIGHT via the button path. Watch 10 s: fade?")) { themeMode = keep; return; }
      themeMode = keep;
    }
    Serial.println("[lab] ===== done =====");
  }

  void myShowAbout() {
    // FreeMonoBold9pt7b advances 11 px, so a 200 px panel fits 18 characters
    // per line. Every label below is budgeted against that; the closing note
    // drops to the 6 px built-in font, which fits 33.
    guiState = APP_STATE;
    display.setFullWindow();
    display.fillScreen(bg());
    display.setTextColor(fg());
    display.setFont(&FreeMonoBold9pt7b);

    int y = 20;
    display.setCursor(0, y); display.print("BWATCH bw-1");

    int cs = chargeState();
    int p  = batteryPctShown();
    y += 20; display.setCursor(0, y);
    if (p >= 0 && !battEstimated && cs == 0) {   // on the cell: and how long
      char hb[12]; fmtHours(hb, 12, hoursLeft());
      display.printf("Batt %d%% %s", p, hb);
    } else if (p >= 0) display.printf(battEstimated ? "Batt ~%2d%%" : "Batt %3d%%", p);
    else        display.print("Batt  --%");   // charging, and the last
                                              // on-cell reading died with
                                              // the cell

    y += 18; display.setCursor(0, y);
    // the voltage is diagnostic, not decoration: a stuck percentage means
    // either the cell is not charging or the ADC is misreading it, and only
    // the raw figure separates those two
    // On the cable the figure is the charger's, not the cell's: say so.
    if (cs == 0) display.printf("Pwr  bat %.2fV", batteryVolts());   // <=17
    else         display.printf("Pwr %s %.2fV USB",
                                cs == 1 ? "chg" : "ful", batteryVolts());

    y += 18; display.setCursor(0, y);
    if (wifiSsid[0][0]) {
      char s[14]; strncpy(s, wifiSsid[0], 13); s[13] = 0;
      display.printf("Net  %s", s);                                 // <=18
    } else display.print("Net  none");

    // (the saved-networks count gave its line to "Was": the networks
    // screen lists them all, and Seal/Open still need to fit)

    y += 18; display.setCursor(0, y);
    if (!haveData) display.print("Data none");
    else {
      // unsigned: if the counters ever disagree the subtraction wraps to
      // about four billion and About reports "Data 71582788h old"
      uint32_t age = (wakeMin >= lastFetchMin) ? wakeMin - lastFetchMin : 0;
      if (age < 60) display.printf("Data %lum old", (unsigned long)age);
      else          display.printf("Data %luh old", (unsigned long)(age / 60));
    }

    // what the watch was doing before this press: the line that explains a
    // watch that stopped resting. Docked, it is plainly docked.
    y += 18; display.setCursor(0, y);
    if (inDocked)                  display.print("Was  docked");
    else if (lastMode == LM_NIGHT) display.print("Was  night");
    else if (lastMode == LM_REST)  display.print("Was  resting");
    else if (lastMode == LM_AWAKE) display.printf("Was  awake %s", lastWhy ? blockName(lastWhy) : "");
    else                           display.print("Was  --");

    y += 18; display.setCursor(0, y);
    if (lastNtpWake == 0) display.print("Sync never");
    else {
      uint32_t a2 = (wakeMin >= lastNtpWake) ? wakeMin - lastNtpWake : 0;
      if (a2 < 60) display.printf("Sync %lum ago", (unsigned long)a2);
      else         display.printf("Sync %luh ago", (unsigned long)(a2 / 60));
    }

    y += 18; display.setCursor(0, y);
    display.printf("Blk  %ld", blockHeight);                        // <=15
    { long sh = sealHeight(nullptr, 0);         // sealed at, opened at
      if (sh > 0) {
        y += 18; display.setCursor(0, y); display.printf("Seal %ld", sh);
        if (openedHeight > 0) {
          y += 18; display.setCursor(0, y); display.printf("Open %ld", openedHeight);
        }
      } }

    // No on-screen prompt: calibration is a once-per-watch action, set during
    // assembly, and does not deserve a permanent line on a screen checked
    // often. Hold UP here to store what a terminated charge reads.
    display.display(true);

    pinMode(BACK_BTN_PIN, INPUT);
    pinMode(MENU_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, BTN_ACTIVE ? INPUT : INPUT_PULLUP);   // as the picker
    pinMode(DOWN_BTN_PIN, INPUT);
    waitAllRelease();                   // the press that opened us
    // Asleep between presses, like the time-travel picker, and home to the
    // face after a minute untouched. This screen used to spin awake with no
    // way out but a button.
    while (true) {
      int p = waitForPress(60000);
      if (p < 0) {
        Serial.println("[about] idle - back to the face");
        guiState = WATCHFACE_STATE;
        RTC.read(currentTime);
        showFaceFull();
        return;
      }
      if (p == BACK_BTN_PIN || p == MENU_BTN_PIN) { waitAllRelease(); break; }
      if (p == DOWN_BTN_PIN) {
        if (heldFor(DOWN_BTN_PIN, 700)) {
          dumpVlog(); buzz(30, 2); waitAllRelease();
          continue;
        }
        // a short press: the display lab
        labRun();
        guiState = WATCHFACE_STATE;
        RTC.read(currentTime);
        showFaceFull();
        return;
      }
      if (p == UP_BTN_PIN && heldFor(UP_BTN_PIN, 900)) {
        // teach the watch what a terminated charge reads on this board
        battFullV = batteryVolts();
        shownBattPct = -1;
        savePrefs();
        buzz(45, 4);
        Serial.printf("[batt] calibrated: full = %.3fV on this board\n", battFullV);
        waitAllRelease();
        myShowAbout();
        return;
      }
    }
    myShowMenu(menuIndex, false);
  }

  void mySyncNTP() {
    guiState = APP_STATE;
    display.setFullWindow();
    display.fillScreen(bg());
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(fg());
    display.setCursor(0, 30);
    display.println("Syncing NTP...");
    display.display(true);   // partial: no flash
    bool ok = false;
    if (myConnectWiFi()) ok = syncNTP();   // RTC keeps UTC (offset 0)
    if (ok) lastNtpWake = wakeMin;
    display.fillScreen(bg());
    display.setCursor(0, 30);
    if (ok) {
      RTC.read(currentTime);
      char b[6];
      snprintf(b, 6, "%02d:%02d", currentTime.Hour, currentTime.Minute);
      display.println("NTP Sync OK");
      display.println("");
      display.print("UTC now: ");
      display.println(b);
    } else {
      display.println("NTP Sync Failed");
      display.println("");
      display.println("(no known WiFi in");
      display.println("range - or this");
      display.println("network blocks NTP)");
    }
    display.display(true);   // partial: no flash
    WiFi.mode(WIFI_OFF); btStop();
    delay(2500);
    myShowMenu(menuIndex, false);
  }

  void mySetupWifi() {
    display.epd2.setBusyCallback(0);
    display.setFullWindow();
    display.fillScreen(bg());
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(fg());
    display.setCursor(0, 30);
    display.println("Trying saved");
    display.println("networks...");
    display.display(true);   // partial: no flash
    // roam the whole keychain first (home, office, hotspot) — not just
    // WiFiManager's single stored credential
    bool ok = myConnectWiFi();
    WiFiManager wifiManager;
    if (!ok) {
      // nothing in range: open the portal AND SAY SO. The field bug:
      // autoConnect started an invisible portal behind a stale
      // "trying..." screen, and timed out unjoined.
      display.fillScreen(bg());
      display.setCursor(0, 25);
      display.println("No saved network");
      display.println("in range.");
      display.println("");
      display.println("On your phone,");
      display.println("join WiFi AP:");
      display.println(WIFI_AP_SSID);
      display.println("");
      display.println("and pick your");
      display.println("network there.");
      display.println("");
      display.println("Waits 2 min.");
      display.display(true);   // partial: no flash
      ok = runPortal(wifiManager, 120);   // enough to fumble a phone,
                                          // short enough to wait out
    }
    if (!ok) {
      display.fillScreen(bg());
      display.setCursor(0, 30);
      display.println("Not connected.");
      display.println("");
      display.println("Nothing was saved.");
      display.println("The watch keeps");
      display.println("the networks it");
      display.println("already had.");
      display.println("");
      display.println("Any key = back");
      display.display(true);   // partial: no flash
      pinMode(BACK_BTN_PIN, INPUT); pinMode(MENU_BTN_PIN, INPUT);
      pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);
      unsigned long t0 = millis();
      while (millis() - t0 < 12000) {         // or it leaves on its own
        if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE ||
            digitalRead(MENU_BTN_PIN) == BTN_ACTIVE ||
            digitalRead(UP_BTN_PIN)   == BTN_ACTIVE ||
            digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) { waitAllRelease(); break; }
        delay(30);
      }
    } else {
      saveNetwork(WiFi.SSID(), WiFi.psk());
      forceFetch = true;   // proof of life: next face render fetches,
                           // so the user RETURNS to a LIVE watch and
                           // never wonders whether anything "saved"
      failCount = 0;       // and clear the offline backoff: six hours
      nextTryWake = 0;     // of failed fetches leaves a retry lockout
                           // that would ignore the now-working network
                           // for up to 5 more minutes (the OLD 6H
                           // after a successful setup)
      display.fillScreen(bg());
      display.setCursor(0, 30);
      display.println("Connected to:");
      display.println(WiFi.SSID());
      display.println("");
      display.println("Saved to keychain.");
      display.println("The watch connects");
      display.println("on demand - it is");
      display.println("normal to be off-");
      display.println("line between uses.");
      display.println("");
      display.println("UP = add another");
      display.display(true);   // partial: no flash
      pinMode(UP_BTN_PIN, INPUT);
      pinMode(BACK_BTN_PIN, INPUT);
      pinMode(MENU_BTN_PIN, INPUT);
      unsigned long t0 = millis();
      while (millis() - t0 < 6000) {
        if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE ||
            digitalRead(MENU_BTN_PIN) == BTN_ACTIVE) {
          waitAllRelease();                 // BACK works immediately —
          break;                            // no more 5s of dead buttons
        }
        if (digitalRead(UP_BTN_PIN) == BTN_ACTIVE) {
          waitRelease(UP_BTN_PIN);
          display.fillScreen(bg());
          display.setCursor(0, 25);
          display.println("On your phone,");
          display.println("join WiFi AP:");
          display.println(WIFI_AP_SSID);
          display.println("");
          display.println("Waits 2 min.");
          display.display(true);   // partial: no flash
          WiFi.disconnect(true, false);
          delay(300);
          if (runPortal(wifiManager, 120))
            saveNetwork(WiFi.SSID(), WiFi.psk());
          break;
        }
        delay(30);
      }
    }
    WiFi.mode(WIFI_OFF); btStop();
    display.epd2.setBusyCallback(WatchyDisplay::busyCallback);
    guiState = APP_STATE;
  }

  void dispatchMenu() {
    if (!menuLevel) switch (menuIndex) {
      case 0: alarmsScreen(); break;                    // block and fee
      case 1: timeTravel(); break;                      // arithmetic, not data
      case 2: menuLevel = 1; menuIndex = 0;             // into My Watch
              myShowMenu(menuIndex, false); break;
      case 3: setupWallet(); break;                     // ends in our menu
      case 4: menuLevel = 2; menuIndex = 0;             // into Settings
              myShowMenu(menuIndex, false); break;
      case 5: myShowAbout(); break;                     // state, not the radio
    } else if (menuLevel == 1) switch (menuIndex) {
      case 0: facesScreen(); break;                     // which faces cycle
      case 1: currencyScreen(); break;                  // the fiat it speaks
      case 2: hourWindowScreen(1); break;               // AUTO theme hours
      case 3: hourWindowScreen(0); break;               // the wearer's night
      case 4: optionsScreen(OPT_HAPTICS); break;        // block, price buzz
      case 5: optionsScreen(OPT_BATTERY); break;        // fetch cadence
      case 6: optionsScreen(OPT_PRIVACY); break;        // wallet balance
      case 7: birthdaysScreen(); break;                 // people, in blocks
    } else switch (menuIndex) {
      case 0: setTime();   myShowMenu(menuIndex, false); break;
      case 1: showTimezone(); break;                    // ends in our menu
      case 2: mySyncNTP(); break;                       // keychain-aware
      case 3: mySetupWifi(); break;                     // BACK returns to menu
      case 4: showNetworks(); break;                    // list and forget
      case 5: startScreenScreen(); break;               // the boot sequence
      case 6: shipModeScreen(); break;                  // asleep for the box
    }
  }

#ifdef ARDUINO_ESP32S3_DEV
  // ---------------- DOCKED MODE ----------------
  // On USB power: stay awake and fast-poll — blocks every 15 s, price
  // every 60 s, the full fetch on its usual 15 min schedule. Field
  // testing showed sub-20 s block latency is perceptually instant for
  // a ~10 min phenomenon, and plain HTTPS is boring-reliable where the
  // websocket handshake never was. Unplug: back to deep-sleep polling.
  void dockedLoop() {
    Serial.begin(115200);
    // CHARGE FIRST.
    // Measured: plugged in overnight in dock mode, the cell sat at 3.92 V and
    // moved neither up nor down — the charger's output and the running system
    // cancelling out almost exactly. Modem sleep and slower polls were not
    // enough, because the ESP32 being awake at all is the cost. So below 95%
    // the realtime dock is declined and the watch deep-sleeps between minute
    // wakes, which hands the charger's whole budget to the battery.
    // Hold any button while plugging in to force the dock anyway.
    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);
    bool force = (digitalRead(MENU_BTN_PIN) == BTN_ACTIVE ||
                  digitalRead(BACK_BTN_PIN) == BTN_ACTIVE ||
                  digitalRead(UP_BTN_PIN)   == BTN_ACTIVE ||
                  digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE);
    // Ask the charge IC, not the ADC.
    //
    // This gate used to read batteryPct() < 0.95, which never once fired: with
    // USB attached the ADC is on the charger rail, so batteryPct() returns
    // about 100% from the instant the cable goes in, whatever the cell holds.
    // The rule was written for exactly this situation and was dead in it.
    //
    // chargeState() == 2 means the charge IC has terminated AND the cell reads
    // above 4.05 V after the cable has been in long enough to be believed.
    // That is the only trustworthy "it is full" this watch has.
    if (!force && chargeState() != 2) {
      Serial.printf("[dock] charge first: state %d %.2fV — sleeping so it fills\n",
                    chargeState(), batteryVolts());
      inDocked = false;
      return;                       // caller resumes normal minute wakes
    }

    inDocked = true;
    Serial.println("[dock] enter (fast-poll transport)");
    partialRefresh();                      // show the face already drawn
    if (!myConnectWiFi()) {
      Serial.println("[dock] wifi FAIL");
      inDocked = false; return;
    }
    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);
    pinMode(USB_DET_PIN, INPUT);
    // CHARGE PRIORITY. A held WiFi association averages ~100 mA, which is
    // the same order as the charger supplies — so a docked watch below full
    // charges with the leftovers and appears to stall. Below 90% we enable
    // modem sleep and slow the polls; above it, full realtime dock.
    // Same rail problem as the gate above: batteryPct() cannot see the cell
    // while the cable is in. Terminated means full, anything else means keep
    // the radio quiet so the charge finishes sooner.
    bool chargeSaver = (chargeState() != 2);
    WiFi.setSleep(chargeSaver);
    unsigned long lastChargeCheck = 0;
    { pinMode(CHG_STAT_PIN, INPUT_PULLUP);
      Serial.printf("[chg] usb=%d stat=%d state=%d %.3fV\n",
                    digitalRead(USB_DET_PIN), digitalRead(CHG_STAT_PIN),
                    chargeState(), getBatteryVoltage()); }
    Serial.printf("[dock] %s (batt %d%%)\n",
                  chargeSaver ? "charge priority" : "realtime",
                  (int)(batteryPct() * 100.0f + 0.5f));

    uint8_t lastMin = currentTime.Minute;
    unsigned long lastTip = 0, lastPrice = 0, lastAddrPoll = 0;
    // the QR was put away: keep listening a while, like the wrist does
    unsigned long chainWatchUntil = 0, lnWatchUntil = 0;
    int lastQr = 0;
    unsigned long lastWifiTry = 0, lastExtrasTry = 0;
    bool extrasWant = false;
    uint64_t addrBaseline = 0; int baselineIdx = -1;
    int usbLow = 0;
    while (usbLow < 6) {          // exit only after ~6 consecutive low
      if (digitalRead(USB_DET_PIN) == 1) usbLow = 0;   // reads (~300ms):
      else usbLow++;              // chargers glitch; one bounced sample
                                  // must not strand the watch asleep
                                  // on the charger
      bool redraw = false;

      // recheck the cell once a minute: as soon as it is near full, drop
      // modem sleep and give back the realtime dock
      if (millis() - lastChargeCheck > 60000) {
        lastChargeCheck = millis();
        bool want = (chargeState() != 2);
        if (want != chargeSaver) {
          chargeSaver = want;
          WiFi.setSleep(chargeSaver);
          redraw = true;
          Serial.printf("[dock] -> %s\n", chargeSaver ? "charge priority" : "realtime");
        }
      }

      // network watchdog: routers reboot and renew leases (hello,
      // 6:15 AM). If WiFi drops, rejoin once a minute instead of
      // letting every poll time out individually. Tag shows DK-.
      if (WiFi.status() != WL_CONNECTED) {
        if (!dockNetDown) {
          dockNetDown = true; redraw = true;
          Serial.println("[dock] wifi down");
        }
        if (millis() - lastWifiTry > 60000) {
          lastWifiTry = millis();
          if (myConnectWiFi()) {
            dockNetDown = false; redraw = true;
            Serial.println("[dock] wifi back");
          }
        }
      } else if (dockNetDown) { dockNetDown = false; redraw = true; }

      // blocks: poll the tip every 15 s
      if (!dockNetDown && millis() - lastTip > (chargeSaver ? 60000UL : 15000UL)) {
        lastTip = millis();
        WiFiClientSecure c; c.setInsecure();
        HTTPClient http; http.setConnectTimeout(4000);
        if (http.begin(c, TIP_URL) && http.GET() == 200) {
          long h = http.getString().toInt();
          if (h >= 100000 && h > blockHeight) {
            blockHeight = h; lastHeightWake = wakeMin;
            if (buzzBlock) vibMotor(75, 4);
            Serial.printf("[dock] block %ld\n", h);
            if (markMoments()) redraw = true;   // a curiosity, here too
            checkAges();                        // and a block birthday
            extrasWant = true;     // fetch reward/miner AFTER the
            lastExtrasTry = 0;     // extras indexer catches up — an
                                   // instant fetch reads block N-1
            redraw = true;
          }
        }
        http.end();
      }

      // price: every 60 s from the lightweight prices endpoint
      if (!dockNetDown && millis() - lastPrice > (chargeSaver ? 300000UL : 60000UL)) {
        lastPrice = millis();
        WiFiClientSecure c; c.setInsecure();
        HTTPClient http; http.setConnectTimeout(4000);
        if (http.begin(c, "https://api.coinbase.com/v2/"
                          "exchange-rates?currency=BTC")
            && http.GET() == 200) {
          if (readRates(http)) {            // every currency refreshed
            priceBuzzCheck();               // in one docked call
            applyCurrency();
            redraw = true;
          }
        }
        http.end();
        // mempool backlog rides the same 60s cadence while docked
        HTTPClient h3; h3.setConnectTimeout(4000);
        if (h3.begin(c, "https://mempool.space/api/mempool") &&
            h3.GET() == 200) {
          JsonDocument doc2;
          if (!deserializeJson(doc2, h3.getString())) {
            long vs = doc2["vsize"] | 0L;
            if (vs > 0) mempoolBlocks = vs / 1e6;
          }
        }
        h3.end();
      }

      // payment watch: while the receive QR is on screen (docked),
      // poll that address; when sats land in the mempool: triple buzz
      // and flip to the balance, freshly rescanned
      // lightning view: poll invoice settlement fast (it's instant money)
      { int qrNow = qrOnGlass() ? walletView : 0;
        if (lastQr && qrNow != lastQr) {
          if (lastQr == 1) chainWatchUntil = millis() + 120000UL;
          else             lnWatchUntil    = millis() + 120000UL;
        }
        lastQr = qrNow; }
      const bool watchLn    = (qrOnGlass() && walletView == 2) ||
                              (long)(lnWatchUntil - millis()) > 0;
      const bool watchChain = !isDemoWallet() &&
                              ((qrOnGlass() && walletView == 1) ||
                               (long)(chainWatchUntil - millis()) > 0);
      if (!dockNetDown && watchLn &&
          lnVerify[0] && millis() - lastAddrPoll > 5000) {
        lastAddrPoll = millis();
        if (lnSettled()) {
          lnVerify[0] = 0; lnInvoice[0] = 0;     // invoice is consumed
          vibMotor(60, 4); delay(120);
          vibMotor(60, 4); delay(120);
          vibMotor(60, 8);
          Serial.println("[dock] lightning payment settled!");
          lnWatchUntil = 0;
          if (dispMode == M_WALT) {
            drawLnPaid(LN_REQUEST_SATS);         // its own moment
            delay(3500);
          } else {                               // elsewhere: a PAY screen
            evPaySats = LN_REQUEST_SATS; evPayLn = true;
            fireEvent(EV_PAY);
          }
          redraw = true;                         // then a fresh invoice
        }
      }
      if (!dockNetDown && watchChain &&
          millis() - lastAddrPoll > (chargeSaver ? 60000UL : 20000UL)) {
        lastAddrPoll = millis();
        String a = walletAddr(recvIndex);
        if (a.length()) {
          WiFiClientSecure c; c.setInsecure();
          HTTPClient http; http.setConnectTimeout(4000);
          if (http.begin(c, String(ESPLORA_BASE) + a) && http.GET() == 200) {
            JsonDocument doc;
            if (!deserializeJson(doc, http.getString())) {
              uint64_t f =
                (uint64_t)(doc["chain_stats"]["funded_txo_sum"] | 0ULL) +
                (uint64_t)(doc["mempool_stats"]["funded_txo_sum"] | 0ULL);
              if (baselineIdx != recvIndex) {
                baselineIdx = recvIndex; addrBaseline = f;
              } else if (f > addrBaseline) {
                uint64_t delta = f - addrBaseline;
                addrBaseline = f;
                vibMotor(60, 4); delay(120);
                vibMotor(60, 4); delay(120);
                vibMotor(60, 8);                    // payment received!
                Serial.printf("[dock] payment received! +%llu sats\n",
                              (unsigned long long)delta);
                // we WATCHED the delta arrive — trust it. An instant
                // rescan races the explorer's indexer and reads the
                // pre-payment state (the "balance didn't update" bug).
                lastTxSats = (long long)delta;
                walletSats += delta;
                haveWallet = true;
                armTracking(a.c_str());   // follow it to 6 confirmations
                chainWatchUntil = 0;
                if (dispMode != M_WALT) {     // elsewhere: a PAY screen
                  evPaySats = (long long)delta; evPayLn = false;
                  fireEvent(EV_PAY);
                }
                recvIndex++;              // this address is used now
                walletView = 0;           // flip to the updated balance;
                redraw = true;            // the periodic sweep reconciles
                savePrefs();              // receipt -> NVS
              }
            }
          }
          http.end();
        }
      }

      // extras chaser: retry every 3s until the reward/miner carry
      // the tip's own height stamp (the indexer needs a few seconds)
      if (extrasWant && !dockNetDown && millis() - lastExtrasTry > 3000) {
        lastExtrasTry = millis();
        WiFiClientSecure ce; ce.setInsecure();
        HTTPClient he; he.setConnectTimeout(4000);
        if (he.begin(ce, "https://mempool.space/api/v1/blocks") &&
            he.GET() == 200) {
          long got = storeExtras(he.getStream());
          if (got >= blockHeight) {
            extrasWant = false;
            Serial.printf("[dock] extras caught up at %ld\n", got);
            redraw = true;
          }
        }
        he.end();
      }

      RTC.read(currentTime);
      if (!dockNetDown && (lastNtpWake == 0 || wakeMin - lastNtpWake > 360)) {
        if (syncNTP()) { lastNtpWake = wakeMin; RTC.read(currentTime); redraw = true;
          Serial.println("[ntp] re-anchored while docked"); }
      }
      if (currentTime.Minute != lastMin) {    // minute tick: clock + wake
        lastMin = currentTime.Minute;
        wakeMin++;
        redraw = true;
        Serial.printf("[dock] tick %02d:%02d net=%s heap=%u height=%ld\n",
                      currentTime.Hour, currentTime.Minute,
                      dockNetDown ? "DOWN" : "up",
                      (unsigned)ESP.getFreeHeap(), blockHeight);
      }

      // full fetch (sentiment, 24h change, sparkline, difficulty) on
      // the normal cadence, or on demand via UP — with backoff, so a
      // dead network doesn't turn the loop into wall-to-wall timeouts
      if (!dockNetDown &&
          (forceFetch || wakeMin - lastFetchMin >= FETCH_EVERY_MIN) &&
          (forceFetch || wakeMin >= nextTryWake)) {
        forceFetch = false;
        if (fetchAll()) {
          lastFetchMin = wakeMin; failCount = 0; nextTryWake = 0;
          markMoments();                     // the retarget, read just now
        } else {
          failCount++;
          nextTryWake = wakeMin + (failCount > 3 ? 5 : 1);
          Serial.println("[dock] fetch FAIL, backing off");
        }
        redraw = true;
      }

      if (checkAlarms()) redraw = true;      // the dock polls on its own

      // ANY BUTTON: OK on an alarm screen, here as on the wrist
      if (evQueue && evShown && !qrOnGlass() &&
          (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE ||
           digitalRead(MENU_BTN_PIN) == BTN_ACTIVE ||
           digitalRead(UP_BTN_PIN)   == BTN_ACTIVE ||
           digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE)) {
        dismissEvent();
        waitAllRelease();
        buttonWake = true;
        drawWatchFace();
        partialRefresh();
        offIfFull();
        delay(50);
        continue;
      }

      if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE) {
        // LONG BACK while travelling: come home. BACK already means leave,
        // so holding it means leave properly — otherwise returning meant
        // walking back through the menu and the step size to reach now.
        backPressed();
        redraw = true;
        waitRelease(BACK_BTN_PIN);
      }
      if (digitalRead(UP_BTN_PIN) == BTN_ACTIVE) {
        pinMode(UP_BTN_PIN, INPUT);
        if (heldFor(UP_BTN_PIN, 600)) {    // LONG UP, any mode: fetch
          buzz(40, 2);                 // EVERYTHING now — the wrist
          forceFetch = true;               // shouldn't wait 15 minutes
          fetchPending = true;             // and the corner reports it
          if (dispMode == M_WALT)          // (in WAL: rescan too)
            lastWalletMin = (wakeMin > walletEveryMin())
                              ? wakeMin - walletEveryMin() : 0;
        } else if (dispMode == M_JOE) {
          joeAdvance();                // the chain, people, the watch
        } else if (dispMode == M_WALT) walletToggleView();
        else {
          if (dispMode == M_PRICE || dispMode == M_SATS ||
              dispMode == M_MCAP) {        // the CURRENCY dial: SATS
            curIdx = nextCur(curIdx);      // and MCAP inherit whatever
            applyCurrency(); saveCurrency(); // PRICE speaks. Instant: the
                                           // rates are already cached
          } else if (dispMode == M_HALV) { // HLV: six epochs of emission
            cellSel[dispMode] = (cellSel[dispMode] + 1) % cellStops(dispMode);
          } else if (dispMode == M_HGHT) { // HGT: cellStops() decides
            cellSel[dispMode] = (cellSel[dispMode] + 1) % cellStops(dispMode);
          } else {                         // FEE tiers / SAT-CAP box
            cellSel[dispMode] = (cellSel[dispMode] + 1) % cellStops(dispMode);
          }
        }
        redraw = true; waitRelease(UP_BTN_PIN);
      }
      if (digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) {
        downPressed();
        redraw = true; waitRelease(DOWN_BTN_PIN);
      }
      if (digitalRead(MENU_BTN_PIN) == BTN_ACTIVE) {
        waitRelease(MENU_BTN_PIN);
        dockMenuSession();                    // powered, immortal menu
        redraw = true;                        // repaint the face after
      }

      if (redraw) {
        buttonWake = true;                    // don't double-count wakes
        drawWatchFace();                      // inDocked guard: no recursion
        // docked it redraws for hours on partials: clean it the same way
        if (justFull) partialRefresh();       // the draw sent it full: skip
        else if (++facePartials >= PARTIALS_BEFORE_FULL) { facePartials = 0; displayFull(); }
        else partialRefresh();
        offIfFull();                          // docked: awake for hours
      }
      delay(50);
    }
    Serial.println("[dock] exit (usb removed)");
    WiFi.mode(WIFI_OFF); btStop();
    inDocked = false;
  }
#endif

  // call with the button currently pressed: returns true if it stays
  // held past ms (long press), false if released sooner (short press)
  // BACK: tap cycles the instrument, hold comes home from a trip. There are
  // five wake paths that handle this button and they all call here, because
  // the last time a gesture was written out five times it only worked in one
  // of them. Returns true if the press was consumed by coming home.
  bool backPressed() {
    if (travelActive && heldFor(BACK_BTN_PIN, 600)) {
      travelActive = false;
      buzz(40, 2);
      Serial.println("[travel] home");
      return true;
    }
    dispMode = nextFace(dispMode);
    walletPeek = false;               // leaving the wallet ends a peek
    return false;
  }

  // Faces the wearer left out are skipped by BACK and missing from the strip.
  // There is always at least one: the Faces screen will not hide the last.
  bool faceShown(int m) {
    if (m == M_MEMP && travelActive) return false;   // no mempool then, or yet
    return !((faceHide >> m) & 1);
  }
  int nextFace(int from) {
    for (int k = 1; k <= NUM_MODES; k++) {
      int m = (from + k) % NUM_MODES;
      if (faceShown(m)) return m;
    }
    return M_HGHT;
  }

  bool heldFor(int pin, unsigned long ms) {
    unsigned long t0 = millis();
    while (digitalRead(pin) == BTN_ACTIVE) {
      if (millis() - t0 >= ms) return true;
      delay(10);
    }
    return false;
  }

  void waitRelease(int pin) {
    unsigned long t0 = millis();
    while (digitalRead(pin) == BTN_ACTIVE && millis() - t0 < 800) delay(10);
  }
  void waitAllRelease() {
    unsigned long t0 = millis();
    while ((digitalRead(MENU_BTN_PIN) == BTN_ACTIVE ||
            digitalRead(BACK_BTN_PIN) == BTN_ACTIVE ||
            digitalRead(UP_BTN_PIN)   == BTN_ACTIVE ||
            digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) &&
           millis() - t0 < 800) delay(10);
  }

  // ---------------- buttons ----------------
  void handleButtonPress() override {
    sanitizeState();
    if (shipMode) {
      // a knock in the box wakes the chip; only a 3 s hold wakes the watch
      uint64_t w = esp_sleep_get_ext1_wakeup_status();
      int pin = (w & MENU_BTN_MASK) ? MENU_BTN_PIN
              : (w & BACK_BTN_MASK) ? BACK_BTN_PIN
              : (w & UP_BTN_MASK)   ? UP_BTN_PIN : DOWN_BTN_PIN;
      pinMode(pin, (pin == UP_BTN_PIN && !BTN_ACTIVE) ? INPUT_PULLUP : INPUT);
      if (!heldFor(pin, 3000)) { shipSleep(); return; }
      leaveShipMode("button");
      waitAllRelease();
      bootGenesis();                               // a first start, properly
      firstStartWifi();
      guiState = WATCHFACE_STATE;
      RTC.read(currentTime);
      showFaceFull();
      return;
    }
    wakeMin += catchUpWakeClock();     // a press can cut a long sleep short
    uint64_t wake = esp_sleep_get_ext1_wakeup_status();
    buttonWake = true;                 // don't tick the wake clock
    pressRun = true;
    // the accelerometer, not a button: only ever armed for a rest sleep
    bool shook = (wake & ACC_INT_MASK) && !(wake & (BTN_PIN_MASK));
    if (shook) shakeClear();           // release the latched line
    // ANY BUTTON WAKES, and that is all the first press does: it would be
    // rude for the press that woke the watch to also change its mode.
    // A shake wakes the same way.
    if (restState != REST_NONE && guiState == WATCHFACE_STATE) {
      awakeTill = wakeMin + PRESS_GRACE_MIN;
      endRest(shook ? "shake" : "button");
      // Answer at once. A full refresh is ~2 s of flashing and a fetch is
      // seconds more; neither should stand between the press and the glass.
      // So the face goes up as a PARTIAL refresh now. If the data is stale
      // (endRest set fetchPending), that first frame says FTCH, the fetch
      // runs, and the fresh face is the full refresh that clears the ghost
      // of the rest face. If it isn't, the next minute's wake does the full.
      if (!fetchPending) { restExitFull = false; cleanNext = true; }
      if (!shook && maybeDateCard()) { cleanNext = false; return; }
      pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
      pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);
      if (!shook) waitAllRelease();
      RTC.read(currentTime);
      showWatchFace(true);
      return;
    }
    if (shook) {                       // stray: woke but no longer resting
      RTC.read(currentTime);
      showWatchFace(true);
      return;
    }
    awakeTill = wakeMin + PRESS_GRACE_MIN;   // every press buys a few
    stillRun = 0;                            // minutes of the full face
    if (guiState == WATCHFACE_STATE && !evQueue && !qrOnGlass() &&
        maybeDateCard()) return;             // the day's first press: the card
    // ANY BUTTON: OK. The press acknowledges the alarm on the glass and does
    // nothing else; the next one waiting (or the face) takes its place.
    if (evQueue && guiState == WATCHFACE_STATE && !qrOnGlass()) {
      if (evShown) dismissEvent();     // only what has actually been seen
      pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
      pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);
      waitAllRelease();
      RTC.read(currentTime);
      showFaceFull();
      return;
    }
    if (guiState == WATCHFACE_STATE) {
      if (wake & MENU_BTN_MASK) {
        openMenu();
      } else if (wake & BACK_BTN_MASK) {
        backPressed();
        RTC.read(currentTime); showWatchFace(true);
      } else if (wake & UP_BTN_MASK) {
        pinMode(UP_BTN_PIN, INPUT);
        if (heldFor(UP_BTN_PIN, 600)) {    // LONG UP, any mode: fetch
          buzz(40, 2);                 // EVERYTHING now — the wrist
          forceFetch = true;               // shouldn't wait 15 minutes
          fetchPending = true;             // and the corner reports it
          if (dispMode == M_WALT)          // (in WAL: rescan too)
            lastWalletMin = (wakeMin > walletEveryMin())
                              ? wakeMin - walletEveryMin() : 0;
        } else if (dispMode == M_JOE) {
          joeAdvance();                // the chain, people, the watch
        } else if (dispMode == M_WALT) walletToggleView();   // balance <-> QR
        else {
          if (dispMode == M_PRICE || dispMode == M_SATS ||
              dispMode == M_MCAP) {        // the CURRENCY dial: SATS
            curIdx = nextCur(curIdx);      // and MCAP inherit whatever
            applyCurrency(); saveCurrency(); // PRICE speaks. Instant: the
                                           // rates are already cached
          } else if (dispMode == M_HALV) { // HLV: six epochs of emission
            cellSel[dispMode] = (cellSel[dispMode] + 1) % cellStops(dispMode);
          } else if (dispMode == M_HGHT) { // HGT: cellStops() decides
            cellSel[dispMode] = (cellSel[dispMode] + 1) % cellStops(dispMode);
          } else {                         // FEE tiers / SAT-CAP box
            cellSel[dispMode] = (cellSel[dispMode] + 1) % cellStops(dispMode);
          }
        }                          // force refresh
        RTC.read(currentTime); showWatchFace(true);
      } else if (wake & DOWN_BTN_MASK) {
        downPressed();
        RTC.read(currentTime); showWatchFace(true);
      }
    } else if (guiState == MAIN_MENU_STATE) {
      if (wake & MENU_BTN_MASK) {
        dispatchMenu();
      } else if (wake & BACK_BTN_MASK) {
        if (!menuBack()) {               // Settings -> main; main -> face
          RTC.read(currentTime); showFaceFull();
          return;
        }
      } else if (wake & UP_BTN_MASK) {
        menuStep(-1);
      } else if (wake & DOWN_BTN_MASK) {
        menuStep(1);
      }
    } else if (guiState == APP_STATE) {
      if (wake & BACK_BTN_MASK) myShowMenu(menuIndex, false);
    }

    // fast loop: keep responding while the user keeps pressing.
    // Watchface buttons are handled HERE too — previously they were
    // silently eaten for 5s after any press, which made scrolling the
    // mode strip feel like it dropped every other click.
    bool timeout = false;
    long lastTimeout = millis();
    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);
    waitAllRelease();   // the waking press may still be held
    bool dwellDone = false;
    while (!timeout) {
      if (millis() - lastTimeout > 5000) { timeout = true; }
      else if (!dwellDone && guiState == WATCHFACE_STATE && dispMode == M_WALT &&
               walletView == 0 && millis() - lastTimeout >= WAL_DWELL_MS &&
               !radioBackedOff() &&
               (!haveWallet || wakeMin - lastWalletMin >= walletEveryMin())) {
        // The wearer stopped on WAL: they want the balance, not passing
        // through. Sweep now, while awake anyway, instead of at the next
        // minute wake. Any press during it cancels and does its job.
        dwellDone = true;
        Serial.println("[wallet] dwell: sweeping");
        walletDwell = true;
        RTC.read(currentTime); showWatchFace(true);
        walletDwell = false;
        lastTimeout = millis();          // and stay awake for what comes next
      }
      else {
        if (digitalRead(MENU_BTN_PIN) == BTN_ACTIVE) {
          lastTimeout = millis();
          if (guiState == MAIN_MENU_STATE) dispatchMenu();
          else if (guiState == WATCHFACE_STATE) {
            buttonWake = true;
            openMenu();
          }
          waitRelease(MENU_BTN_PIN);
        } else if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE) {
          lastTimeout = millis();
          if (guiState == MAIN_MENU_STATE) {
            if (!menuBack()) { RTC.read(currentTime); showFaceFull(); }
            waitRelease(BACK_BTN_PIN);
          } else if (guiState == APP_STATE) {
            myShowMenu(menuIndex, false);
            waitRelease(BACK_BTN_PIN);
          } else if (guiState == WATCHFACE_STATE) {
            buttonWake = true;
            backPressed();
            RTC.read(currentTime); showWatchFace(true);
            waitRelease(BACK_BTN_PIN);
          }
        } else if (digitalRead(UP_BTN_PIN) == BTN_ACTIVE) {
          lastTimeout = millis();
          if (guiState == MAIN_MENU_STATE) {
            menuStep(-1);
          } else if (guiState == WATCHFACE_STATE) {
            buttonWake = true;
            pinMode(UP_BTN_PIN, INPUT);
        if (heldFor(UP_BTN_PIN, 600)) {    // LONG UP, any mode: fetch
          buzz(40, 2);                 // EVERYTHING now — the wrist
          forceFetch = true;               // shouldn't wait 15 minutes
          fetchPending = true;             // and the corner reports it
          if (dispMode == M_WALT)          // (in WAL: rescan too)
            lastWalletMin = (wakeMin > walletEveryMin())
                              ? wakeMin - walletEveryMin() : 0;
        } else if (dispMode == M_JOE) {
          joeAdvance();                // the chain, people, the watch
        } else if (dispMode == M_WALT) walletToggleView();
            else {
          if (dispMode == M_PRICE || dispMode == M_SATS ||
              dispMode == M_MCAP) {        // the CURRENCY dial: SATS
            curIdx = nextCur(curIdx);      // and MCAP inherit whatever
            applyCurrency(); saveCurrency(); // PRICE speaks. Instant: the
                                           // rates are already cached
          } else if (dispMode == M_HALV) { // HLV: six epochs of emission
            cellSel[dispMode] = (cellSel[dispMode] + 1) % cellStops(dispMode);
          } else if (dispMode == M_HGHT) { // HGT: cellStops() decides
            cellSel[dispMode] = (cellSel[dispMode] + 1) % cellStops(dispMode);
          } else {                         // FEE tiers / SAT-CAP box
            cellSel[dispMode] = (cellSel[dispMode] + 1) % cellStops(dispMode);
          }
        }
            RTC.read(currentTime); showWatchFace(true);
          }
          waitRelease(UP_BTN_PIN);
        } else if (digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) {
          lastTimeout = millis();
          if (guiState == MAIN_MENU_STATE) {
            menuStep(1);
          } else if (guiState == WATCHFACE_STATE) {
            buttonWake = true;
            downPressed();
            RTC.read(currentTime); showWatchFace(true);
          }
          waitRelease(DOWN_BTN_PIN);
        }
      }
    }
  }
};

watchySettings settings{
  .cityID = "", .weatherAPIKey = "", .weatherURL = "", .weatherUnit = "",
  .weatherLang = "", .weatherUpdateInterval = 60,
  .ntpServer = "pool.ntp.org",
  .gmtOffset = 0,               // your UTC offset in seconds
  .vibrateOClock = false,
};

BitcoinChrono watchy(settings);
void setup() { watchy.init(); }
void loop() {}
