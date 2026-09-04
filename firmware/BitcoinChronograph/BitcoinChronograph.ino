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
#include "qrcodegen_local.h"    // ricmoo/QRCode, vendored into the sketch
                                // folder because the ESP32 core ships its
                                // own qrcode.h that shadows the library

#include "DSEG7_Classic_Bold_32.h"   // big values (6 digits fit)
#include "DSEG7_Classic_Bold_25.h"   // small cell, and 7-digit values
#include "DSEG7_Classic_Bold_18.h"   // 8-digit values (full supply)
#include "DSEG7_Classic_Bold_14.h"   // DIFF%/24H% cell, last resort
#include <Fonts/FreeMonoBold9pt7b.h>

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
#define WALLET_ZPUB  "zpub6rFR7y4Q2AijBEqTUquhVz398htDFrtymD9xYYfG1m4wAcvPhXNfE3EfH1r1ADqtfSdVCToUG868RvUUkgDKf31mGDtKsAYz2oz2AGutZYs"
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
// Watchy v3 routes the charger's status line to GPIO10. Charger status pins
// are open-drain: held LOW while charging, released HIGH once the charge
// terminates. That is ground truth for "is the cell actually taking charge",
// which a voltage reading alone cannot tell you.
#define CHG_STAT_PIN 10

#define FETCH_EVERY_MIN 15
// The single freshness threshold. The corner tag flips LIVE -> OLD here, and
// the height's unit line starts estimating here: both indicators must change
// together or a wearer sees LIVE next to an estimate and cannot tell which to
// believe. Must stay above FETCH_EVERY_MIN so a normal cycle never trips it.
#define STALE_AFTER_MIN 20
#define FETCH_IDLE_MIN  45   // when the watch has been still for a while it
                             // is on a table, not a wrist: nobody is reading
                             // it, so stop spending radio on their behalf
#define STILL_AFTER_MIN 60   // minutes of no movement before that kicks in
#define MOVE_THRESHOLD  90   // raw counts; a wrist easily clears this
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
#define RTC_LAYOUT_MAGIC 0xB17C0127
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
RTC_DATA_ATTR uint8_t  vlog[24] = {0};
RTC_DATA_ATTR uint8_t  vlogIdx  = 0;
RTC_DATA_ATTR uint32_t vlogWake = 0;

RTC_DATA_ATTR double   travelPastPrice = 0;   // real price at the travelled
RTC_DATA_ATTR long     travelPastFor   = -1;  // height, fetched once on
                                              // arrival. The table is only
                                              // the fallback for no network
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
RTC_DATA_ATTR float    mempoolBlocks = 0;   // unconfirmed vMB ~ blocks
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
// network keychain: up to 3 saved networks, most-recent first
RTC_DATA_ATTR char     wifiSsid[3][33] = {{0}};
RTC_DATA_ATTR char     wifiPass[3][65] = {{0}};
RTC_DATA_ATTR int      tzIndex       = 0;      // 0=LOC (compiled offset)
RTC_DATA_ATTR float    avgBlockSec   = 600;    // measured epoch pace

const char *TZ_NAMES[5] = {"LOC","EST","PST","UTC","GMT"};
const long  TZ_OFF  [5] = {0, -18000, -28800, 0, 0};  // fixed, no DST

enum { M_HGHT, M_PRICE, M_SATS, M_MCAP, M_FEES, M_SUPL, M_HALV, M_WALT,
       M_JOE, NUM_MODES };
// 3-char labels: 9 cells across 190px
const char *MODE_LABELS[NUM_MODES] =
  {"HGT","PRC","SAT","CAP","FEE","SUP","HLV","WAL","JOE"};
const char *MODE_NAMES [NUM_MODES] =
  {"BLOCK HEIGHT","PRICE","SATS/FIAT","MCAP","FEE SAT/VB",
   "SUPPLY","HALVING","WALLET BTC","TIME"};
const char *MODE_UNITS [NUM_MODES] =
  {"TIMECHAIN TIP","USD / BTC","SAT / USD","BILLION USD","SAT/VB FAST",
   "MILLION BTC","NEXT HALVING","BTC BALANCE",""};

// the currency dial: UP in PRC/SAT/CAP cycles it; SATS and MCAP
// inherit whatever PRICE speaks. Chart shape stays USD (Coinbase only
// has candle history for real pairs); the %% is computed per-currency.
const char *CUR_CODES[6] = {"USD","EUR","CAD","JPY","GBP","CHF"};
RTC_DATA_ATTR uint8_t curIdx = 0;
// BTC priced in all six, fetched together once per cycle. Fiat rates
// crawl (~0.5%%/day); BTC is the only volatile leg — so the dial turns
// on cached arithmetic instead of waiting on the radio.
RTC_DATA_ATTR float   fxRate[6] = {0};

// PER-MODE box dial (declared after the enum that sizes it). It was
// one shared integer, and clicking FEE's tier silently re-aimed the
// halving almanac two epochs deep ("1.5625 halving in 2037").
RTC_DATA_ATTR uint8_t  cellSel[NUM_MODES] = {0};

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
    unsigned long t0 = millis();
    while (multi.run(4000) != WL_CONNECTED && millis() - t0 < 12000)
      delay(100);
    if (WiFi.status() == WL_CONNECTED) {
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
    if (BUZZ_PRICE_STEP <= 0 || p <= 0) return;
    if (priceAnchor <= 0) { priceAnchor = p; return; }  // arm only
    if (p - priceAnchor >= BUZZ_PRICE_STEP) {
      priceAnchor = p;
      vibMotor(75, 4); delay(160); vibMotor(75, 4);   // two quick: up
    } else if (priceAnchor - p >= BUZZ_PRICE_STEP) {
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
    filter[0]["extras"]["reward"] = true;
    filter[0]["extras"]["pool"]["name"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, s, DeserializationOption::Filter(filter)))
      return -1;
    long h = doc[0]["height"] | 0L;
    if (h > rwdHeight) {
      rwdHeight = h;
      long long r = doc[0]["extras"]["reward"] | 0LL;
      if (r > 0) lastRewardBtc = r / 1e8;
      uint32_t ts = doc[0]["timestamp"] | 0UL;
      if (ts > 0) tipBlockTime = ts;
      storePool(doc[0]["extras"]["pool"]["name"] | "");
    }
    return h;
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
  void applyCurrency() {
    if (fxRate[curIdx] > 0) btcPrice = fxRate[curIdx];
    if (blockHeight > 0)
      btcMcapB = btcPrice * supplyBTC(blockHeight) / 1e9;
  }

  // ONE call prices BTC in every currency we speak, plus gold in USD
  // (the %% GOLD ratio is currency-invariant, so USD on both legs)
  bool fetchRates(bool ownRadio) {
    if (ownRadio && !myConnectWiFi()) return false;
    WiFiClientSecure client; client.setInsecure();
    bool ok = false;
    { HTTPClient http; http.setConnectTimeout(5000);
      if (http.begin(client, "https://api.coinbase.com/v2/"
                             "exchange-rates?currency=BTC")
          && http.GET() == 200) {
        JsonDocument filter; filter["data"]["rates"] = true;
        JsonDocument doc;
        // getString, NOT getStream: Coinbase answers chunked, and the
        // raw stream includes chunk-size headers the parser chokes on
        // (field: "price is 0 in all currencies")
        if (!deserializeJson(doc, http.getString(),
                             DeserializationOption::Filter(filter))) {
          for (int i = 0; i < 6; i++) {
            float r = atof(doc["data"]["rates"][CUR_CODES[i]] | "0");
            if (r > 0) fxRate[i] = r;
          }
          if (fxRate[0] > 0) ok = true;
        }
      }
      http.end(); }
    { // gold, USD: XAU spot x ~6.95B above-ground ounces (216k tonnes,
      // drifts ~1.5%%/yr from mining — revisit the constant eventually)
      HTTPClient http; http.setConnectTimeout(4000);
      if (http.begin(client, "https://api.coinbase.com/v2/prices/"
                             "XAU-USD/spot") && http.GET() == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString())) {
          float oz = atof(doc["data"]["amount"] | "0");
          if (oz > 0) goldMcapB = oz * 6.95f;   // billions USD
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
    Serial.printf("[chart] %d candles, 24h %.2f%%\n", nc, btcChange24h);
    return true;   // the %% is USD-derived and reused for every
  }                // currency: fiat drift is ~0.3pp, invisible at 1dp

  // ---------------- data ----------------
  bool fetchAll() {
    if (!myConnectWiFi()) return false;
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
    bool ok = false;
    long heightBefore = blockHeight;
    WiFiClientSecure client; client.setInsecure();
    fetchCandles(client);                  // chart + the 24h %%
    if (fetchRates(false)) haveData = ok = true;   // all six + gold
    { HTTPClient http; http.setConnectTimeout(4000);
      if (http.begin(client, FNG_URL) && http.GET() == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString()))
          fearGreed = atoi(doc["data"][0]["value"] | "50");
      }
      http.end(); }
    { HTTPClient http; http.setConnectTimeout(4000);
      if (http.begin(client, FEES_URL) && http.GET() == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString()))
          fastFee = doc["fastestFee"] | fastFee;   // float-safe
          medFee  = doc["halfHourFee"] | medFee;
          lowFee  = doc["hourFee"]     | lowFee;
      }
      http.end(); }
    { const char *tips[2] = {TIP_URL, TIP_URL2};
      for (int i = 0; i < 2; i++) {
        HTTPClient http; http.setConnectTimeout(4000);
        bool got = false;
        if (http.begin(client, tips[i]) && http.GET() == 200) {
          long h = http.getString().toInt();
          if (h >= 100000) {                      // sanity: a real height
            if (h > blockHeight) blockHeight = h; // only moves forward
            lastHeightWake = wakeMin;             // height-specific stamp
            got = true;
          }
        }
        http.end();
        if (got) break;                           // fallback only on failure
      } }
    { HTTPClient http; http.setConnectTimeout(4000);
      if (http.begin(client, DIFF_URL) && http.GET() == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString())) {
          diffChangeEst = doc["difficultyChange"] | 0.0f;
          float ta = doc["timeAvg"] | 0.0f;      // ms, current epoch
          if (ta > 0) avgBlockSec = constrain(ta / 1000.0f, 300.0f, 1200.0f);
        }
      }
      http.end(); }
    // last block's total reward (subsidy + fees) — what mining paid
    { HTTPClient http; http.setConnectTimeout(4000);
      if (http.begin(client, "https://mempool.space/api/v1/blocks") &&
          http.GET() == 200) {
        storeExtras(http.getStream());
      }
      http.end(); }
    // mempool backlog, denominated in blocks (unconfirmed vMB)
    { HTTPClient http; http.setConnectTimeout(4000);
      if (http.begin(client, "https://mempool.space/api/mempool") &&
          http.GET() == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString())) {
          long vs = doc["vsize"] | 0L;
          if (vs > 0) mempoolBlocks = vs / 1e6;
        }
      }
      http.end(); }
    if (!inDocked) { WiFi.mode(WIFI_OFF); btStop(); }  // docked: stay on

    // ---- haptics (after radio off; buzzing draws its own current) ----
    if (BUZZ_ON_BLOCK && heightBefore > 0 && blockHeight > heightBefore) {
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
    if (!sensor.getAccel(a)) return true;      // no reading: assume worn
    long d = labs((long)a.x - lastAx) + labs((long)a.y - lastAy)
           + labs((long)a.z - lastAz);
    lastAx = a.x; lastAy = a.y; lastAz = a.z;
    if (!accelSeeded) { accelSeeded = true; lastMoveWake = wakeMin; return true; }
    return d > MOVE_THRESHOLD;
  }

  bool watchIsIdle() {
    return accelSeeded && (wakeMin - lastMoveWake) >= STILL_AFTER_MIN;
  }

  void maybeFetch() {
    if (inDocked) return;   // docked loop schedules fetches itself,
                            // ONE TLS session at a time (heap can't
                            // hold the websocket + HTTPS together)
    if (travelActive && wakeMin - travelWake > 30) {
      travelActive = false;                  // half an hour is long enough to
      Serial.println("[travel] expired");    // look at the future
    }
    uint32_t every = watchIsIdle() ? FETCH_IDLE_MIN : FETCH_EVERY_MIN;
    bool due = forceFetch || !haveData ||
               (wakeMin - lastFetchMin >= every);
    if (due && (forceFetch || wakeMin >= nextTryWake)) {
      if (fetchAll()) {
        lastFetchMin = wakeMin;
        failCount = 0; nextTryWake = 0;
      } else {
        // offline: back off 5 -> 10 -> 20 -> 40 -> 60 min between radio
        // attempts instead of a 10s WiFi timeout on every single wake.
        failCount++;
        uint32_t backoff = 5u << (failCount > 4 ? 4 : failCount - 1);
        if (backoff > 60) backoff = 60;
        nextTryWake = wakeMin + backoff;
      }
    }
    // the milestone: checked where a new height first becomes known
    if (!sawMillion && blockHeight >= 1000000L) millionScreen();

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
    sawMillion = p.getBool("m1", false);
    p.end();
    if (z.length() > 20) strncpy(zpubBuf, z.c_str(), sizeof(zpubBuf) - 1);
    else                 strncpy(zpubBuf, WALLET_ZPUB, sizeof(zpubBuf) - 1);
    zpubBuf[sizeof(zpubBuf) - 1] = 0;
    String ln = p.getString("lnaddr", "");
    if (ln.indexOf('@') > 0) strncpy(lnAddrBuf, ln.c_str(), 63);
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
    p.end();
  }

  // A zpub is an ACCOUNT key with two branches under it: .../0/i receive
  // and .../1/i change. Spending consumes a whole utxo from the receive
  // branch and returns the remainder to the change branch — so a wallet
  // that scans only chain 0 undercounts by the change the moment you send.
  String deriveAddress(int chain, int index) {
    loadPrefs();
    HDPublicKey hd(zpubBuf);
    if (!hd.isValid()) return "";
    return hd.child(chain).child(index).address();
  }
  String deriveAddress(int index) { return deriveAddress(0, index); }

  // flipping QR -> balance re-arms a scan: the natural moment someone
  // checks their balance is right after getting paid
  bool lnConfigured() { return lnAddrBuf[0] != 0 || LN_BOLT12[0] != 0; }

  // UP cycles: balance -> chain QR -> lightning QR (if configured) ->
  // balance. Landing back on balance re-arms a scan.
  void walletToggleView() {
    walletView = (walletView + 1) % (lnConfigured() ? 3 : 2);
    if (walletView == 0) {
      lastWalletMin = (wakeMin > WALLET_EVERY_MIN)
                        ? wakeMin - WALLET_EVERY_MIN : 0;
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

  // PAYMENT VIGIL (wrist): showing the QR arms a ~2 minute awake watch
  // on that address — buzz + flip to a rescanned balance the moment
  // sats hit the mempool. Costs ~1.3% battery per vigil; a payment
  // arriving later is still caught by the UP-rescan path.
  void paymentVigil() {
    Serial.println("[vigil] armed 120s");
    pinMode(UP_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    // lightning view: poll the invoice's verify URL instead of an
    // address — settlement lands in seconds, so poll every 5
    if (walletView == 2) {
      if (!lnVerify[0]) return;
      if (!myConnectWiFi()) return;
      unsigned long start = millis(), lastPoll = 0;
      while (millis() - start < 120000) {
        if (millis() - lastPoll > 5000) {
          lastPoll = millis();
          if (lnSettled()) {
            lnVerify[0] = 0; lnInvoice[0] = 0;   // invoice is consumed
            vibMotor(60, 4); delay(120);
            vibMotor(60, 4); delay(120);
            vibMotor(60, 8);
            Serial.println("[vigil] lightning payment settled!");
            drawLnPaid(LN_REQUEST_SATS);         // its own moment —
            delay(3500);                         // the chain balance
            buttonWake = true;                   // is not involved
            drawWatchFace();                     // fresh invoice QR
            display.display(true);
            WiFi.mode(WIFI_OFF); btStop();
            return;
          }
        }
        if (digitalRead(UP_BTN_PIN) == BTN_ACTIVE) {
          waitRelease(UP_BTN_PIN);
          walletToggleView(); buttonWake = true;
          drawWatchFace(); display.display(true);
          break;
        }
        if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE) {
          backPressed();                 // test the hold BEFORE releasing:
          waitRelease(BACK_BTN_PIN);     // afterwards there is nothing to
          buttonWake = true;             // measure and the shortcut dies
          drawWatchFace(); display.display(true);
          break;
        }
        delay(40);
      }
      WiFi.mode(WIFI_OFF); btStop();
      Serial.println("[vigil] done (ln)");
      return;
    }
    String a = walletAddr(recvIndex);
    if (!a.length()) return;
    if (!myConnectWiFi()) return;           // no network: sleep as usual
    unsigned long start = millis(), lastPoll = 0;
    uint64_t base = 0; bool haveBase = false;
    while (millis() - start < 120000) {
      if (!haveBase || millis() - lastPoll > 15000) {
        lastPoll = millis();
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
              vibMotor(60, 4); delay(120);
              vibMotor(60, 4); delay(120);
              vibMotor(60, 8);                  // payment received!
              Serial.printf("[vigil] payment received! +%llu sats\n",
                            (unsigned long long)delta);
              // trust the observed delta — an instant rescan races the
              // explorer's indexer and reads pre-payment state
              lastTxSats = (long long)delta;
              walletSats += delta;
              haveWallet = true;
              recvIndex++;                      // address used: advance
              walletView = 0;                   // show the new balance
              savePrefs();                      // receipt -> NVS
              buttonWake = true;
              drawWatchFace();
              display.display(true);
              WiFi.mode(WIFI_OFF); btStop();
              return;
            }
          }
        }
        http.end();
      }
      if (digitalRead(UP_BTN_PIN) == BTN_ACTIVE) {     // to balance
        waitRelease(UP_BTN_PIN);
        walletToggleView(); buttonWake = true;
        drawWatchFace(); display.display(true);
        break;
      }
      if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE) {   // next mode
        backPressed();                   // hold first, release after
        waitRelease(BACK_BTN_PIN);
        buttonWake = true;
        drawWatchFace(); display.display(true);
        break;
      }
      delay(40);
    }
    WiFi.mode(WIFI_OFF); btStop();
    Serial.println("[vigil] done");
  }

  int walletListCount() {
    const char *s = WALLET_ADDR_LIST;
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
      String all(WALLET_ADDR_LIST);
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
  int pressedButton() {
    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);
    if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE) return BACK_BTN_PIN;
    if (digitalRead(MENU_BTN_PIN) == BTN_ACTIVE) return MENU_BTN_PIN;
    if (digitalRead(UP_BTN_PIN)   == BTN_ACTIVE) return UP_BTN_PIN;
    if (digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) return DOWN_BTN_PIN;
    return 0;
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
      if (btn) {
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
      return (dt.Hour >= 20 || dt.Hour < 7);
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
    if (!myConnectWiFi()) return false;
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
    float hiP = btcPrice   > 0 ? (float)btcPrice : A[N_ANCHORS-1].usd;

    long  loH = A[0].h;  float loP = A[0].usd;
    for (int i = 0; i < N_ANCHORS; i++) {
      if (A[i].h <= targetH) { loH = A[i].h; loP = A[i].usd; }
      else { hiH = A[i].h; hiP = A[i].usd; break; }
    }
    if (hiH <= loH || loP <= 0 || hiP <= 0) return loP;
    double f = (double)(targetH - loH) / (double)(hiH - loH);
    return exp(log(loP) + f * (log(hiP) - log(loP)));   // log-linear
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
    if (e < STALE_AFTER_MIN) return blockHeight;

    return blockHeight + (long)blocksForward(e);
  }

  // Blocks arrive as a Poisson process, so an estimate of n blocks carries a
  // standard error of about sqrt(n) blocks. Guessing 36 blocks over six hours
  // means the true tip is plausibly +/-6 away. The watch should say so rather
  // than print a confident number that is quietly wrong.
  // How long the current tip has stood. Counted from the block's own
  // timestamp against the RTC, so it advances every minute without asking
  // anyone. Returns -1 when no timestamp has been seen yet.
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
    if (e < STALE_AFTER_MIN) return 0;    // same gate as estHeight: no
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
    int total = (int)strlen(whole) * digitAdvance(f) + commas * commaW;
    if (frac[0]) {
      total += 1 + 2 + (int)strlen(frac) * digitAdvance(sf);
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
    display.setCursor(cx - w/2, cy - r - 10);
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
      double mp = past ? pastPrice(th) : modelPrice(th);
      switch (dispMode) {
        case M_PRICE:                       // modelled, not fetched
          if (mp <= 0) { strcpy(big, "----"); return; }
          snprintf(big, 16, "%.0f", mp);    // spelled out: 6,450,000, not 6.45
          return;
        case M_SATS:                        // sats per unit of fiat
          if (mp <= 0) { strcpy(big, "----"); return; }
          snprintf(big, 16, "%.0f", 1e8 / mp);
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
      case M_PRICE:
        // spelled out, whatever the currency: 12,760,000 rather than 12.76.
        // The comment this replaces said eight digits would not fit — true
        // before fitFont existed, but the face now steps the numerals down a
        // size instead of the number stepping down a scale.
        snprintf(big, 14, "%.0f", btcPrice);
        break;
      case M_SATS:  snprintf(big, 12, "%.0f", btcPrice > 0 ? 1e8/btcPrice : 0); break;
      case M_MCAP:  fmtCap(big, 14, btcMcapB); break;
      case M_FEES:  fmtFee(big, 14, fastFee); break;
      case M_SUPL: {
        // every coin issued, and every coin still to come. Both move by one
        // subsidy per block — one climbing, one falling — so the decimals are
        // the part worth watching.
        double issued = supplyBTC(estHeight());
        if (cellSel[M_SUPL] % 2) snprintf(big, 14, "%.2f", supplyCapBTC() - issued);
        else                     snprintf(big, 14, "%.2f", issued);
        break;
      }
      case M_HALV: {
        // the DATE of the selected halving (UP walks future epochs),
        // projected at the chain's MEASURED pace, not idealized 10 min.
        // Big display carries the year; the unit line the day.
        if (estHeight() > 0) {
          int t = cellSel[dispMode] % 6;
          int era = (int)(estHeight() / 210000L);
          long targetH = (long)(era + 1 + t) * 210000L;
          long blocksLeft = targetH - estHeight();
          if (blocksLeft < 0) blocksLeft = 0;
          // base the projection on WHEN YOU ARE, not on today. Travelling,
          // the blocks remaining are counted from the target height, so
          // adding them to the present would land the halving too early by
          // exactly the distance travelled.
          time_t utc = makeTime(currentTime) - settings.gmtOffset;
          if (travelActive)
            utc += (time_t)((travelHeight - blockHeight) * (double)avgBlockSec);
          time_t when = utc + (time_t)(blocksLeft * (double)avgBlockSec);
          tmElements_t hd; breakTime(when, hd);
          Serial.printf("[hlv] est=%ld left=%ld avg=%.0f utc=%lu -> %d\n",
                        estHeight(), blocksLeft, avgBlockSec,
                        (unsigned long)utc, tmYearToCalendar(hd.Year));
          snprintf(big, 12, "%d", tmYearToCalendar(hd.Year));
        } else snprintf(big, 12, "----");
        break; }
      case M_WALT: {
        if (walletPrivate) { snprintf(big, 12, "------"); break; }
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
    display.display(false);   // full refresh: a celebration deserves
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
    display.display(false);
  }

  // ---------------- RECV: QR screen (v1 chrome: status row + strip) ----
  void drawReceive() {
    display.setFont(NULL);
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
    display.print(lnConfigured() ? "  (UP = LN)" : "  (UP = BALANCE)");
    drawModeStrip();
  }

  // DSEG's '.' glyph has zero xAdvance at 18px — print one char at a
  // time and step the cursor past the dot manually
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
    pinMode(USB_DET_PIN, INPUT);
    if (digitalRead(USB_DET_PIN) != 1) return 0;
    pinMode(CHG_STAT_PIN, INPUT_PULLUP);
    delayMicroseconds(60);
    if (digitalRead(CHG_STAT_PIN) == LOW) return 1;      // asserted: charging
    return (battVoltsTrue() >= 4.05f) ? 2 : 1;           // HIGH needs a second
  }                                                      // opinion

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

  float batteryPct() {
    if (chargeState() == 2) return 1.0f;   // terminated: the guard now lives
                                           // inside chargeState itself                         // terminated AND the cell agrees
    float v = battVoltsTrue();
    float p;
    if      (v >= 4.15f) p = 1.00f;
    else if (v >= 4.00f) p = 0.85f + (v - 4.00f) * (0.15f / 0.15f);
    else if (v >= 3.85f) p = 0.60f + (v - 3.85f) * (0.25f / 0.15f);
    else if (v >= 3.70f) p = 0.35f + (v - 3.70f) * (0.25f / 0.15f);
    else if (v >= 3.55f) p = 0.15f + (v - 3.55f) * (0.20f / 0.15f);
    else if (v >= 3.30f) p = (v - 3.30f) * (0.15f / 0.25f);
    else                 p = 0.0f;
    return p < 0 ? 0 : (p > 1 ? 1 : p);
  }

  // What About prints: rounded to 5%, because a voltage-derived reading is
  // not accurate to 1%, and never rising while on battery. The latch releases
  // as soon as a charger is attached so it can climb again.
  int batteryPctShown() {
    int raw     = (int)(batteryPct() * 100.0f + 0.5f);
    int rounded = ((raw + 2) / 5) * 5;
    if (rounded > 100) rounded = 100;
    if (rounded < 0)   rounded = 0;
    if (chargeState() != 0) { shownBattPct = rounded; return rounded; }
    if (shownBattPct < 0 || rounded < shownBattPct) shownBattPct = rounded;
    return shownBattPct;
  }

  // Battery glyph. Three states, because "high voltage" and "charging" are
  // not the same thing: a cell on the charger reads 4.2 V whether it has
  // been there a minute or an hour.
  //   charging, not yet full : outline + bolt
  //   charging, full         : solid fill
  //   on battery             : proportional fill, '!' below 15%
  void drawBattery(int x, int y) {
    float pct = batteryPct();
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
    int sy = 186, cw = 190 / NUM_MODES;
    display.setFont(NULL);
    for (int i = 0; i < NUM_MODES; i++) {
      int x = 5 + i * cw;
      if (i == dispMode) {
        display.fillRect(x, sy, cw - 2, 13, fg());
        display.setTextColor(bg());
      } else {
        display.drawRect(x, sy, cw - 2, 13, fg());
        display.setTextColor(fg());
      }
      display.setCursor(x + 2, sy + 3);
      display.print(MODE_LABELS[i]);
    }
    display.setTextColor(fg());
  }

  // ---------------- face (v1 layout, 200x200) ----------------
  // boot splash, shown once per power-on (survives deep sleep via RTC flag)
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
  bool showThemeName = false; // DOWN was pressed: announce the theme
                              // in the divider title for one render
                              // (the status row has no free pixels)
  bool inDocked   = false;   // true while the docked realtime loop runs
  bool dockNetDown = false;  // docked but WiFi has dropped
  // How many stops each instrument's box dial has. The increment used to be
  // a flat %6 for every mode while each renderer applied its own modulo, so
  // a 5-stop dial took six presses with the first repeating.
  // DOWN cycles the theme; held in WAL it toggles privacy instead, which is
  // the one spare gesture left on a four-button watch.
  void downPressed() {
    if (dispMode == M_WALT && heldFor(DOWN_BTN_PIN, 700)) {
      walletPrivate = !walletPrivate;
      savePrefs();
      buzz(40, 2);
      Serial.printf("[wal] privacy %s\n", walletPrivate ? "ON" : "OFF");
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
      return (blockHeight > 0 && blockHeight < 1000000L) ? 7 : 6;
    }
                                    // block age, unconf, and the countdown
                                    // to a million while it is still coming
    if (m == M_FEES) return 3;      // high, medium, low
    if (m == M_SUPL) return 2;      // issued, remaining
    return 6;                       // halving almanac, currency dials
  }

  bool prePaint = false;      // re-entering the render to show FTCH before
                              // the radio goes out; suppresses the fetch
  bool walletScanArmed = false, inWalletScan = false;
  int  scanCancelled = 0;      // pin of the button that interrupted a scan

  void sanitizeState() {
    if (rtcMagic == RTC_LAYOUT_MAGIC) return;
    rtcMagic = RTC_LAYOUT_MAGIC;
    Serial.begin(115200);
    Serial.println("[rtc] layout changed - state reset");
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
    for (int i = 0; i < 6; i++) fxRate[i] = 0;
    lastRewardBtc = 0; mempoolBlocks = 0; poolName[0] = 0; rwdHeight = 0;
    goldMcapB = 0; lastNtpWake = 0; tipBlockTime = 0; fetchPending = false;
    travelActive = false; travelHeight = 0; travelWake = 0;
    travelPastPrice = 0; travelPastFor = -1;
    walletPrivate = false;
    lastAx = lastAy = lastAz = 0; lastMoveWake = 0; accelSeeded = false;
    shownBattPct = -1;      // let the latch re-learn from the next reading
    medFee = 0; lowFee = 0;
    wakeMin = 0; failCount = 0; nextTryWake = 0; forceFetch = false;
    zpubBuf[0] = 0; lnAddrBuf[0] = 0;
    prefsLoaded = false;                    // NVS reloads the real ones
    memset(wifiSsid, 0, sizeof(wifiSsid));
    memset(wifiPass, 0, sizeof(wifiPass));
    avgBlockSec = 600;
    guiState = 0;                          // library state too:
    menuIndex = 0;                         // WATCHFACE_STATE, top item
    alreadyInMenu = true;
  }

  // JOE. A watch that looks like a watch: time, day, date, and nothing else.
  // No height, no mode strip, no reason for anyone to ask what you are
  // wearing. The chain carries on behind it — ticks still land, fetches still
  // run — you are simply not advertising any of it.
  void drawJoeFace() {
    display.fillScreen(bg());
    display.setTextColor(fg());
    RTC.read(currentTime);

    // local time exactly as the status row does it, so JOE and the instrument
    // faces can never disagree about what the clock says
    time_t lt = makeTime(currentTime);
    if (tzIndex > 0) {
      time_t utc = lt - settings.gmtOffset;
      lt = utc + tzOffsetSec(tzIndex, utc);
    }
    tmElements_t dt; breakTime(lt, dt);

    char t[8]; snprintf(t, 8, "%02d:%02d", dt.Hour, dt.Minute);
    centerText(t, 110, &DSEG7_Classic_Bold_32);

    static const char *DOW[7] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
    static const char *MON[12] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                  "JUL","AUG","SEP","OCT","NOV","DEC"};
    int wd = (dt.Wday >= 1 && dt.Wday <= 7) ? dt.Wday - 1 : 0;
    int mo = (dt.Month >= 1 && dt.Month <= 12) ? dt.Month - 1 : 0;
    char d[24];
    snprintf(d, 24, "%s  %d %s", DOW[wd], dt.Day, MON[mo]);
    display.setFont(NULL);
    { int16_t x1, y1; uint16_t w, h2;
      display.getTextBounds(d, 0, 0, &x1, &y1, &w, &h2);
      display.setCursor((200 - (int)w) / 2, 136); display.print(d); }

    drawBattery(92, 160);        // the one thing a plain watch still needs
  }

  void drawWatchFace() override {
    if (dispMode == M_JOE) { drawJoeFace(); return; }
    sanitizeState();
    loadPrefs();
    themeDark = darkNow();
    if (!buttonWake) wakeMin++;        // one tick per minute-wake
    // hourly discharge sample
    if (wakeMin - vlogWake >= 60 || vlogWake == 0) {
      vlogWake = wakeMin;
      float v = batteryVolts();
      vlog[vlogIdx % 24] = (uint8_t)(v * 50.0f);
      vlogIdx++;
      Serial.printf("[vlog] %02d  %.3fV raw  %.3fV true  %d%%\n",
                    vlogIdx % 24, v, battVoltsTrue(), batteryPctShown());
    }
    if (!accelSeeded) scanAdcPins();   // once per cold boot
    chargeDiag();                      // silent unless a charger is present
    if (!buttonWake && !inDocked) {
      bool wasIdle = watchIsIdle();
      if (movedSinceLastWake()) {
        lastMoveWake = wakeMin;
        // picked up after a long rest: the face is stale, so refresh now
        // rather than making the wearer wait for the next slot
        if (wasIdle) { forceFetch = true; Serial.println("[accel] picked up"); }
      }
    }
    buttonWake = false;
    // BITCOIN IS TIME on every true boot (power-on, reflash, crash) —
    // but never on the once-a-minute deep-sleep wake. Reset reason has
    // real semantics; a sticky RTC flag survives too many resets.
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) drawSplash();
    // wallet scans are slow (10 sequential TLS requests) — never do them
    // before the face is on screen. Render first, scan after (see the
    // hook at the end of this function). SCAN in the tag meanwhile.
    if (dispMode < 0 || dispMode >= NUM_MODES) dispMode = 0;
    if (tzIndex < 0 || tzIndex > 4) tzIndex = 0;
    if (themeMode < 0 || themeMode > 2) themeMode = 0;
    for (int i = 0; i < NUM_MODES; i++)  // index-ish RTC values must clamp,
      if (cellSel[i] >= cellStops(i)) cellSel[i] = 0;   // survives a stop count change
    if (curIdx > 5) curIdx = 0;
    if (walletView > 2) walletView = 0;  // never dereference

    walletScanArmed = !inWalletScan && (dispMode == M_WALT) &&
      (forceFetch || !haveWallet ||
       wakeMin - lastWalletMin >= WALLET_EVERY_MIN);

    // A requested fetch paints FIRST, then goes to the radio. maybeFetch()
    // lives inside this function, so fetching here meant the request was
    // serviced and the flag cleared before a single pixel was drawn: FTCH
    // only ever showed on the dock, where the loop redraws, and never on the
    // wrist, where there is one draw per wake. Re-enter once to paint the
    // pending state, then fetch, then fall through and paint the result.
    if (fetchPending && !prePaint) {
      prePaint = true;
      drawWatchFace();                // corner reads FTCH; no fetch inside
      display.display(true);
      prePaint = false;
    }
    if (!prePaint) maybeFetch();
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
      long e = (dispMode == M_HGHT) ? heightStaleMinutes() : staleMinutes();
      if (e < STALE_AFTER_MIN) strcpy(tag, "LIVE");
      else if (e < 60) snprintf(tag, 8, "OLD %ldM", e);
      else             snprintf(tag, 8, "OLD %ldH", e / 60 > 48 ? 48 : e / 60);
    }
    display.setCursor(188 - (int)strlen(tag) * 6, 3);
    display.print(tag);

    if (dispMode == M_WALT && walletView > 0) {
      if (walletView == 2) drawLightning();
      else drawReceive();
      display.display(false);   // FULL refresh: partial updates smear
                                // large black<->white flips (ghost
                                // "LN..." fragments over the QR in dark
                                // mode) and a QR must scan crisply
      if (vigilPending && !inDocked) {
        vigilPending = false;
        paymentVigil();                   // stand watch ~2 min
      }
      return;
    }

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
      if (dispMode == M_SUPL) {
        unit = (cellSel[M_SUPL] % 2) ? "BTC REMAINING" : "BTC ISSUED";
      }
      if (!travelActive && dispMode == M_HGHT && !inDocked &&
          heightStaleMinutes() >= STALE_AFTER_MIN && estBlocksAhead() > 0) {
        // dead-reckoning: name the estimate and its error bar
        // ASCII only: the built-in GFX font maps high bytes via CP437 and
        // would draw a shaded block instead of a proper plus-minus
        snprintf(ub, 24, "EST +%d BLK +/-%d",
                 estBlocksAhead(), estUncertainty());
        unit = ub;
      }
      if (travelActive) {
        static char tu[26];
        if (dispMode == M_PRICE) {
          snprintf(tu, 26, "%s / BTC", CUR_CODES[curIdx]);
          unit = tu;
        } else if (dispMode == M_SATS) {
          snprintf(tu, 26, "SAT / %s", CUR_CODES[curIdx]); unit = tu;
        } else if (dispMode == M_MCAP) {
          double cb = supplyBTC(estHeight()) *
                      (estHeight() < blockHeight ? pastPrice(estHeight())
                                                 : modelPrice(estHeight())) / 1e9;
          snprintf(tu, 26, capInTrillions(cb) ? "TRILLION %s" : "BILLION %s",
                   CUR_CODES[curIdx]); unit = tu;
        } else if (dispMode == M_FEES) {
          unit = "NOT DERIVABLE";
        }
        // everything below is a present-mode unit: it does not apply here
      } else if (dispMode == M_PRICE) {
        snprintf(ub, 24, "%s / BTC", CUR_CODES[curIdx]);
        unit = ub;
      } else if (dispMode == M_SATS) {
        snprintf(ub, 24, "SAT / %s", CUR_CODES[curIdx]);
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
        time_t utc = makeTime(currentTime) - settings.gmtOffset;
        if (travelActive)
          utc += (time_t)((travelHeight - blockHeight) * (double)avgBlockSec);
        time_t when = utc + (time_t)(blocksLeft * (double)avgBlockSec);
        tmElements_t hd; breakTime(when, hd);
        snprintf(ub, 24, "#%d - %s %d",
                 era + 1 + t, MN[hd.Month], hd.Day);
        unit = ub;
      }
      if (dispMode == M_WALT && haveWallet && walletSats < 1000000ULL)
        unit = "SATS BALANCE";

      int16_t x1,y1; uint16_t w,h;
      display.getTextBounds(unit,0,0,&x1,&y1,&w,&h);
      display.setCursor((200-w)/2, 49); display.print(unit); }

    // --- middle band: EPOCH subdial | cell | HALVING subdial ---
    // (the difficulty adjustment "readjusts every 2016 ticks" — the
    //  conductor of the orchestra; the halving epoch is the calendar year)
    long eh = estHeight();
    float epochPct = eh > 0 ? (eh % 2016L)   / 2016.0f   : 0;
    float halvPct  = eh > 0 ? (eh % 210000L) / 210000.0f : 0;
    drawSubdial(30, 80, 16, epochPct, "EPOC");
    drawSubdial(170, 80, 16, halvPct, "HALV");

    // single-line cell: [ value   LABEL ] — label beside the number,
    // bottoms aligned, instead of stacked underneath
    display.drawRect(52, 66, 96, 22, fg());
    char cell[18]; const char *cellLabel;   // 18: "POWER LAW MODEL" plus
                                           // room, not 12 which truncates it
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
      double mp = estHeight() < blockHeight ? pastPrice(estHeight())
                                            : modelPrice(estHeight());
      cellLabel = CUR_CODES[curIdx];
      cellIsText = true;                 // separators need the text font
      if (mp <= 0 || !haveWallet) strcpy(cell, "----");
      else fmtMoney(cell, 18, (double)walletSats / 1e8 * mp);
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
      double sub = 50.0 / (double)(1LL << (era + 1 + t));
      static const char *HL[6] =
        {"B NEXT", "B +2", "B +3", "B +4", "B +5", "B +6"};
      cellLabel = HL[t];
      snprintf(cell, 18, sub >= 1 ? "%.3f" : "%.4f", sub);
    } else if (dispMode == M_SATS) {
      // MOSCOW TIME: sats-per-unit as a clock — the oldest meme in
      // the book, native to a 7-segment display (":" included)
      cellLabel = "MOSCOW";
      double pr = !travelActive ? btcPrice
                : (estHeight() < blockHeight ? pastPrice(estHeight())
                                             : modelPrice(estHeight()));
      long s = pr > 0 ? (long)(1e8 / pr) : 0;
      if (s >= 0 && s <= 9999)          // a cheap coin means few sats: 8
        snprintf(cell, 18, "%02ld:%02ld", s / 100, s % 100);   // reads 00:08
      else snprintf(cell, 18, "%ld", s);  // beyond a clock: raw
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
      int t = cellSel[dispMode] % cellStops(M_HGHT);   // seven until 1M
      if (t == 0) {
        cellLabel = "BTC RWD";         // subsidy + tx fees
        snprintf(cell, 18, "%.2f", lastRewardBtc);
      } else if (t == 1 && dispMode == M_HGHT) {
        cellLabel = "MINER";           // who found the last block
        snprintf(cell, 18, "%s", poolName[0] ? poolName : "...");
        cellIsText = true;
      } else if ((t == 2 && dispMode == M_HGHT) ||
                 (t == 1 && dispMode != M_HGHT)) {
        if (dispMode == M_HGHT) {
          cellLabel = "RETARGET";      // names the event, not jargon.
          // decimals yield when magnitude goes double-digit, or the
          // value collides with the 8-char label (-12.3 vs RETARGET)
          snprintf(cell, 18, fabs(diffChangeEst) >= 10 ? "%s%.0f" : "%s%.1f",
                   diffChangeEst < 0 ? "-" : "", fabs(diffChangeEst));
        } else {
          cellLabel = "24H %";
          snprintf(cell, 18, "%s%.1f",
                   btcChange24h < 0 ? "-" : "", fabs(btcChange24h));
        }
      } else if (t == 6 && dispMode == M_HGHT) {
        // only exists until it happens, then the dial is one stop shorter
        cellLabel = "TO 1M";
        snprintf(cell, 18, "%ld", 1000000L - estHeight());
      } else if (t == 4 && dispMode == M_HGHT) {
        // age of the tip we KNOW about: right after a fetch this can read
        // anything from 0M upward, since it counts from the block's own
        // timestamp rather than from our fetch. It overstates only when a
        // newer block has landed that we have not seen — which is exactly
        // when a fetch would be worth making.
        // DSEG has no letters, so a unit has to come from the text renderer —
        // the same path the miner name uses. "14 MIN" beats a bare 14.
        // The age of the tip we hold, whatever its age. Marking it UNKNOWN when
        // stale was tempting, but every other cell — reward, miner, price,
        // fees — is equally a last-known value, and the corner tag already
        // says so for all of them at once. One honesty mechanism, not eight.
        int a = blockAgeMin();
        cellLabel = "BLK AGE";
        if (a < 0) strcpy(cell, "--");
        else       snprintf(cell, 18, "%d MIN", a);
        cellIsText = true;
      } else if (t == 3 && dispMode == M_HGHT) {
        // how fast the chain is actually running: the same figure the
        // dead-reckoning uses, so a wearer can see why an estimate drifts
        cellLabel = "MIN/BLK";
        snprintf(cell, 18, "%.1f", avgBlockSec / 60.0f);
      } else {
        cellLabel = "UNCONF";          // blocks-worth, abbreviated:
        snprintf(cell, 18, "%.1f", mempoolBlocks); // UNCONFIRMED
      }                                            // overflows the box
    } else if (dispMode == M_WALT && walletPrivate) {
      cellLabel = "PRIVATE";    // the last movement is as telling as the
      strcpy(cell, "----");     // balance, so it hides behind the same switch
    } else if (dispMode == M_WALT) {
      cellLabel = "TX";         // the wallet's last movement: received
      long long a = lastTxSats < 0 ? -lastTxSats : lastTxSats;
      const char *plus = lastTxSats > 0 ? "+" : "";   // deposits wear
      if (lastTxSats == 0) snprintf(cell, 18, "----"); // their sign
      else if (a < 100000)
        snprintf(cell, 18, "%s%lld", plus, lastTxSats);
      else {                    // large moves read better in BTC
        double b = lastTxSats / 1e8;
        snprintf(cell, 18, (fabs(b) < 10) ? "%s%.3f" : "%s%.1f", plus, b);
      }
    } else {
      cellLabel = "24H %";
      snprintf(cell, 18, "%s%.1f", btcChange24h < 0 ? "-" : "", fabs(btcChange24h));
    }
    if (cellIsText) {
      display.setFont(NULL);
      display.setCursor(57, 77);       // letters: small caps font,
      display.print(cell);             // aligned with the label row
    } else {
      printCellValue(cell, 57, 84);
    }
    display.setFont(NULL);
    display.setCursor(146 - (int)strlen(cellLabel) * 6, 77);
    display.print(cellLabel);

    if (travelActive) {
      // Sentiment and the 24h chart are facts about now — neither survives
      // the trip. What does: how much of 21 million exists at this height,
      // and where that sits on the whole emission curve. Pure arithmetic.
      long th = estHeight();
      double issued = supplyBTC(th);
      float mined = (float)(issued / supplyCapBTC());

      int by = 112;                                    // supply bar
      display.drawRect(8, by, 184, 9, fg());
      int fillw = (int)(180.0f * mined + 0.5f);
      if (fillw > 0) display.fillRect(10, by + 2, fillw, 5, fg());
      display.setFont(NULL);
      display.setCursor(8, by + 13);   display.print("0");
      display.setCursor(174, by + 13); display.print("21M");
      { char m[20]; snprintf(m, 20, "%.4f%% MINED", mined * 100.0);
        int16_t x1,y1; uint16_t w,h;
        display.getTextBounds(m,0,0,&x1,&y1,&w,&h);
        display.setCursor((200-(int)w)/2, by + 13); display.print(m); }

      int cy2 = 140, ch2 = 34;                         // emission curve
      display.drawRect(6, cy2, 188, ch2, fg());
      // no label: the curve leaves the bottom-left corner immediately, and
      // anything written there gets crossed out by it
      int px2 = -1, py2 = -1;
      for (int i = 0; i <= 182; i += 2) {
        long h2 = (long)((double)i / 182.0 * 6930000.0);
        int x = 9 + i;
        int y = cy2 + ch2 - 4 - (int)(supplyBTC(h2) / supplyCapBTC() * (ch2 - 10));
        if (px2 >= 0) display.drawLine(px2, py2, x, y, fg());
        px2 = x; py2 = y;
      }
      int mx = 9 + (int)((double)th / 6930000.0 * 182.0);
      int my = cy2 + ch2 - 4 - (int)(mined * (ch2 - 10));
      display.drawFastVLine(mx, cy2 + 2, ch2 - 4, fg());
      display.fillCircle(mx, my, 2, fg());
      display.setCursor(160, cy2 + ch2 - 9); display.print("2140");
    } else {

    // --- fear/greed gauge ---
    int ry = 113;
    display.drawFastHLine(6, ry, 188, fg());
    for (int i = 0; i <= 20; i++)
      display.drawFastVLine(6 + i * 188 / 20, ry - (i%5==0 ? 8 : 4),
                            (i%5==0 ? 8 : 4), fg());
    int nx = 6 + fearGreed * 188 / 100;
    display.fillTriangle(nx-5, ry-16, nx+5, ry-16, nx, ry-9, fg());
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
    int cy = 136, ch = 38;
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
      time_t now = time(nullptr);
      if (now < 1700000000) { tmElements_t tn; RTC.read(tn); now = makeTime(tn); }
      time_t then = now + (time_t)(dB * (double)avgBlockSec);
      static const char *MON[12] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                    "JUL","AUG","SEP","OCT","NOV","DEC"};
      tmElements_t td; breakTime(then, td);
      int mo = (td.Month >= 1 && td.Month <= 12) ? td.Month - 1 : 0;
      snprintf(destLine, 26, "%s %s %d",
               dB >= 0 ? "ARRIVING" : "ARRIVED", MON[mo],
               tmYearToCalendar(td.Year));
    }
    const char *title = travelActive     ? destLine
                      : showThemeName    ? THEME_NAMES[themeMode]
                      : (dispMode == M_WALT) ? "UP = RECEIVE QR"
                                             : "TICK TOCK NEXT BLOCK";
    showThemeName = false;
    { int16_t x1,y1; uint16_t w,h;
      display.getTextBounds(title,0,0,&x1,&y1,&w,&h);
      int tx = (200-w)/2;
      display.setCursor(tx, 178); display.print(title);
      display.drawFastHLine(6, 181, tx - 10, fg());
      display.drawFastHLine(tx + w + 4, 181, 194 - (tx + w + 4), fg()); }
    drawModeStrip();

    // first WAL entry / due rescan: the face above goes to the panel
    // NOW, then the slow address sweep runs, then we re-render.
    if (walletScanArmed && !inWalletScan) {
      inWalletScan = true;
      scanCancelled = 0;
      display.display(true);              // instant frame, tag = SCAN
      bool ok = fetchWallet();
      if (ok) lastWalletMin = wakeMin;
      fetchPending = false;                // the sweep is over either way
      Serial.printf("[wallet] sweep %s\n",
                    ok ? "OK" : (scanCancelled ? "CANCELLED" : "FAIL"));
      // the press that cancelled the scan still has to do its job, or the
      // watch just sits there having ignored you
      if (scanCancelled) {
        int btn = scanCancelled; scanCancelled = 0;
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
          myShowMenu(menuIndex, false);
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
  static const int MY_MENU_LEN = 7;

  void myShowMenu(byte idx, bool partial) {
    const char *items[MY_MENU_LEN] = {
      "About BWATCH", "Set Time", "Setup WiFi", "Sync NTP",
      "Setup Wallet", "Set Timezone", "Time Travel"};
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    int16_t x1, y1; uint16_t w, h;
    for (int i = 0; i < MY_MENU_LEN; i++) {
      int16_t yPos = 22 + 22 * i;
      display.setCursor(0, yPos);
      if (i == idx) {
        display.getTextBounds(items[i], 0, yPos, &x1, &y1, &w, &h);
        display.fillRect(x1 - 1, y1 - 10, 200, h + 15, GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.println(items[i]);
      } else {
        display.setTextColor(GxEPD_WHITE);
        display.println(items[i]);
      }
    }
    // always partial: the entry flash annoyed the owner, and the
    // full refresh on exit-to-watchface is the ghost janitor anyway
    display.display(true);
    guiState = MAIN_MENU_STATE;
    alreadyInMenu = false;
  }

  // Timezone picker, in the style of the stock setTime() app
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
        display.fillScreen(GxEPD_BLACK);
        display.setFont(&FreeMonoBold9pt7b);
        display.setTextColor(GxEPD_WHITE);
        display.setCursor(0, 22); display.println("Timezone");
        time_t utcNow = makeTime(currentTime) - settings.gmtOffset;
        for (int i = 0; i < 5; i++) {
          int y = 56 + i * 24;
          long off = tzOffsetSec(i, utcNow);
          char line[24];
          snprintf(line, 24, "%s  UTC%+ld", tzName(i, utcNow), off / 3600L);
          display.setCursor(16, y);
          if (i == sel) {
            display.fillRect(8, y - 15, 184, 21, GxEPD_WHITE);
            display.setTextColor(GxEPD_BLACK);
          } else display.setTextColor(GxEPD_WHITE);
          display.println(line);
        }
        // live preview of the clock in the highlighted zone
        time_t lt = makeTime(currentTime);
        if (sel > 0) lt = utcNow + tzOffsetSec(sel, utcNow);
        tmElements_t dt; breakTime(lt, dt);
        display.setTextColor(GxEPD_WHITE);
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
  void setupWallet() {
    loadPrefs();
    guiState = APP_STATE;
    display.epd2.setBusyCallback(0);
    WiFiManager wm;
    wm.setTimeout(WIFI_AP_TIMEOUT);
    WiFiManagerParameter pZpub(
      "zpub", "Watch-only zpub (NEVER a private key or seed)",
      zpubBuf, 120);
    wm.addParameter(&pZpub);
    WiFiManagerParameter pLn(
      "lnaddr", "Lightning address (optional, user@domain)",
      lnAddrBuf, 63);
    wm.addParameter(&pLn);
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(0, 25);
    display.println("On your phone,");
    display.println("join WiFi AP:");
    display.println(WIFI_AP_SSID);
    display.println("");
    display.println("Open setup page,");
    display.println("paste your zpub.");
    display.println("");
    display.println("Addresses only -");
    display.println("NEVER a seed!");
    display.display(true);   // partial: no flash
    wm.startConfigPortal(WIFI_AP_SSID);
    const char *lv = pLn.getValue();
    if (lv && strchr(lv, '@')) {
      strncpy(lnAddrBuf, lv, 63); lnAddrBuf[63] = 0;
      lnInvoice[0] = 0; lnVerify[0] = 0;
      savePrefs();
    }
    const char *v = pZpub.getValue();
    display.fillScreen(GxEPD_BLACK);
    display.setCursor(0, 30);
    if (v && strlen(v) > 20 && strncmp(v, zpubBuf, sizeof(zpubBuf)) != 0) {
      strncpy(zpubBuf, v, sizeof(zpubBuf) - 1);
      zpubBuf[sizeof(zpubBuf) - 1] = 0;
      savePrefs();
      haveWallet = false; walletSats = 0; recvIndex = 0;   // rescan
      HDPublicKey hd(zpubBuf);
      if (hd.isValid()) {
        display.println("Wallet saved!");
        display.println("");
        display.println("First address:");
        String a = deriveAddress(0);
        display.println(a.substring(0, 18));
        display.println(a.substring(18));
        display.println("");
        display.println("VERIFY it against");
        display.println("your wallet app!");
      } else {
        display.println("Saved, but this");
        display.println("zpub looks");
        display.println("INVALID - check");
        display.println("and re-enter.");
      }
    } else {
      display.println("No change made.");
    }
    display.display(true);   // partial: no flash
    delay(5000);
    WiFi.mode(WIFI_OFF); btStop();
    display.epd2.setBusyCallback(WatchyDisplay::busyCallback);
    myShowMenu(menuIndex, false);
  }

  // menu session for DOCKED mode: on USB power, run the menu right in
  // the loop — instant navigation, no deep-sleep wakes, and no stock
  // idle-timeout silently dropping the user back onto the dock face
  // (where DOWN correctly means theme — the source of the "DOWN stopped
  // working in the menu" mystery).
  void dockMenuSession() {
    myShowMenu(menuIndex, false);
    unsigned long idle = millis();
    while (digitalRead(USB_DET_PIN) == 1) {
      if (millis() - idle > 120000) break;            // 2 min true idle
      if (digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) {
        idle = millis();
        menuIndex++; if (menuIndex > MY_MENU_LEN - 1) menuIndex = 0;
        myShowMenu(menuIndex, true);
        waitRelease(DOWN_BTN_PIN);
      } else if (digitalRead(UP_BTN_PIN) == BTN_ACTIVE) {
        idle = millis();
        menuIndex--; if (menuIndex < 0) menuIndex = MY_MENU_LEN - 1;
        myShowMenu(menuIndex, true);
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
        break;                                        // back to dock face
      }
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
    Serial.println("[vlog] hour  raw    true   (oldest first)");
    for (int i = 0; i < 24; i++) {
      int k = (vlogIdx + i) % 24;
      if (vlog[k] == 0) continue;
      float v = vlog[k] / 50.0f;
      Serial.printf("[vlog]  %2d   %.2f   %.2f\n", i, v,
                    v * (4.20f / (battFullV > 3.0f ? battFullV : 4.20f)));
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
    Serial.printf("[chg] stat=%d state=%d lib=%.3fV cal=%.3fV (adc %lumV) pct=%d%%\n",
                  digitalRead(CHG_STAT_PIN), chargeState(), lib, batteryVolts(),
                  (unsigned long)mv, (int)(batteryPct() * 100.0f + 0.5f));
  }

  // Block 1,000,000. Fires once, on the first face draw that sees it, and
  // then never again. Ten pulses — one per hundred thousand blocks since the
  // genesis block — and a screen that holds until a button is pressed.
  void millionScreen() {
    guiState = APP_STATE;
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);

    display.setFont(NULL);
    centerText("BLOCK HEIGHT", 26, NULL);   // the face's own header

    char big[16]; snprintf(big, 16, "%ld", blockHeight);
    drawGrouped(big, 84, fitFont(big));

    display.setFont(NULL);
    centerText("ONE MILLION BLOCKS", 108, NULL);

    RTC.read(currentTime);
    char when[26];
    snprintf(when, 26, "%04d-%02d-%02d  %02d:%02d UTC",
             tmYearToCalendar(currentTime.Year), currentTime.Month,
             currentTime.Day, currentTime.Hour, currentTime.Minute);
    centerText(when, 126, NULL);

    display.drawFastHLine(30, 140, 140, GxEPD_WHITE);
    centerText("EVERY TEN MINUTES", 156, NULL);
    centerText("SINCE JANUARY 2009", 168, NULL);
    centerText("TICK TOCK NEXT BLOCK", 190, NULL);
    display.display(false);            // full refresh: this one is worth it

    for (int i = 0; i < 10; i++) { buzz(60, 4); delay(140); }

    Serial.printf("[1M] block %ld — marked\n", blockHeight);
    pinMode(BACK_BTN_PIN, INPUT); pinMode(MENU_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);
    unsigned long t0 = millis();
    while (millis() - t0 < 120000) {   // holds two minutes, or until pressed
      if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE ||
          digitalRead(MENU_BTN_PIN) == BTN_ACTIVE ||
          digitalRead(UP_BTN_PIN)   == BTN_ACTIVE ||
          digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) { waitAllRelease(); break; }
      delay(50);
    }
    sawMillion = true;
    savePrefs();
  }

  // TIME TRAVEL. Everything the chain guarantees is a function of height:
  // the subsidy, the coins issued, the coins left, the epoch, the halving.
  // None of it needs a network. Price, fees and sentiment are NOT functions
  // of height, so this mode does not pretend to know them.
  //   UP / DOWN  move by the current step
  //   MENU       cycles the step: block, day, month, year, halving
  //   BACK       returns
  void timeTravel() {
    guiState = APP_STATE;
    long here = travelActive ? blockHeight : estHeight();   // 'now' is real,
    if (here <= 0) here = 964000;                          // even mid-travel
    long target = travelActive ? travelHeight : here;

    const long  STEPS[5] = { 1, 144, 4320, 52560, 210000 };
    const char *SNAME[5] = { "BLOCK", "DAY", "MONTH", "YEAR", "HALVING" };
    int step = 3;

    pinMode(MENU_BTN_PIN, INPUT); pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, INPUT);   pinMode(DOWN_BTN_PIN, INPUT);

    while (true) {
      if (target < 0) target = 0;
      if (target > 6930000L) target = 6930000L;   // the last subsidy era

      display.setFullWindow();
      display.fillScreen(GxEPD_BLACK);
      display.setTextColor(GxEPD_WHITE);
      display.setFont(NULL);
      centerText("TIME TRAVEL", 12, NULL);

      char big[16]; snprintf(big, 16, "%ld", target);
      drawGrouped(big, 52, fitFont(big));
      display.setFont(NULL);
      centerText(travelActive ? "BLOCK HEIGHT - TRAVELLING"
                              : (target == here ? "BLOCK HEIGHT - NOW"
                                                : "BLOCK HEIGHT"), 66, NULL);

      // when: blocks from now at the chain's measured pace
      long dBlocks = target - here;
      char line[34];
      long mins = (long)(dBlocks * (avgBlockSec / 60.0f));
      long days = mins / 1440;
      if (dBlocks == 0)      snprintf(line, 34, "TODAY");
      else if (labs(days) < 2)
        snprintf(line, 34, "%s %ld HOURS", dBlocks > 0 ? "IN" : "AGO", labs(mins) / 60);
      else if (labs(days) < 800)
        snprintf(line, 34, "%s %ld DAYS", dBlocks > 0 ? "IN" : "AGO", labs(days));
      else
        snprintf(line, 34, "%s %ld YEARS", dBlocks > 0 ? "IN" : "AGO", labs(days) / 365);
      centerText(line, 84, NULL);

      // what the chain guarantees at that height
      int era = (int)(target / 210000L);
      double sub = 50.0; for (int i = 0; i < era && i < 33; i++) sub /= 2.0;
      double issued = supplyBTC(target);

      display.drawFastHLine(14, 96, 172, GxEPD_WHITE);
      display.setCursor(14, 106);  display.printf("REWARD  %.8f", sub);
      display.setCursor(14, 120);  display.printf("EPOCH   %d of 33", era + 1);
      display.setCursor(14, 134);  display.printf("ISSUED  %.0f", issued);
      display.setCursor(14, 148);  display.printf("LEFT    %.0f", supplyCapBTC() - issued);
      display.setCursor(14, 162);  display.printf("MINED   %.4f%%", issued / supplyCapBTC() * 100.0);

      display.drawFastHLine(14, 172, 172, GxEPD_WHITE);
      snprintf(line, 34, "TRAVEL BY %s", SNAME[step]);
      centerText(line, 178, NULL);
      centerText("HOLD MENU TO LAUNCH", 190, NULL);
      display.display(true);

      // wait for a press
      while (true) {
        if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE) {
          waitAllRelease(); myShowMenu(menuIndex, false); return;
        }
        if (digitalRead(UP_BTN_PIN) == BTN_ACTIVE) {
          target += STEPS[step]; waitAllRelease(); break;
        }
        if (digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE) {
          target -= STEPS[step]; waitAllRelease(); break;
        }
        if (digitalRead(MENU_BTN_PIN) == BTN_ACTIVE) {
          if (heldFor(MENU_BTN_PIN, 800)) {
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
              display.fillScreen(GxEPD_BLACK);
              display.setTextColor(GxEPD_WHITE);
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
            showWatchFace(false);
            return;
          }
          step = (step + 1) % 5; waitAllRelease(); break;
        }
        delay(40);
      }
    }
  }

  void myShowAbout() {
    // FreeMonoBold9pt7b advances 11 px, so a 200 px panel fits 18 characters
    // per line. Every label below is budgeted against that; the closing note
    // drops to the 6 px built-in font, which fits 33.
    guiState = APP_STATE;
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setFont(&FreeMonoBold9pt7b);

    int y = 20;
    display.setCursor(0, y); display.print("BWATCH bw-1");

    int cs = chargeState();
    int p  = batteryPctShown();
    y += 20; display.setCursor(0, y);
    display.printf("Batt %3d%%", p);                                // 9

    y += 18; display.setCursor(0, y);
    // the voltage is diagnostic, not decoration: a stuck percentage means
    // either the cell is not charging or the ADC is misreading it, and only
    // the raw figure separates those two
    display.printf("Pwr  %s %.2fV", cs == 0 ? "bat" : (cs == 1 ? "chg" : "ful"),
                   batteryVolts());                                 // <=17

    y += 18; display.setCursor(0, y);
    if (wifiSsid[0][0]) {
      char s[14]; strncpy(s, wifiSsid[0], 13); s[13] = 0;
      display.printf("Net  %s", s);                                 // <=18
    } else display.print("Net  none");

    int saved = 0;
    for (int i = 0; i < 3; i++) if (wifiSsid[i][0]) saved++;
    y += 18; display.setCursor(0, y);
    display.printf("Keys %d of 3", saved);                          // 11

    y += 18; display.setCursor(0, y);
    if (!haveData) display.print("Data none");
    else {
      uint32_t age = wakeMin - lastFetchMin;
      if (age < 60) display.printf("Data %lum old", (unsigned long)age);
      else          display.printf("Data %luh old", (unsigned long)(age / 60));
    }

    y += 18; display.setCursor(0, y);
    if (lastNtpWake == 0) display.print("Sync never");
    else {
      uint32_t a2 = wakeMin - lastNtpWake;
      if (a2 < 60) display.printf("Sync %lum ago", (unsigned long)a2);
      else         display.printf("Sync %luh ago", (unsigned long)(a2 / 60));
    }

    y += 18; display.setCursor(0, y);
    display.printf("Blk  %ld", blockHeight);                        // <=15

    // No on-screen prompt: calibration is a once-per-watch action, set during
    // assembly, and does not deserve a permanent line on a screen checked
    // often. Hold UP here to store what a terminated charge reads.
    display.display(true);

    pinMode(BACK_BTN_PIN, INPUT);
    pinMode(MENU_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, INPUT);
    while (true) {
      if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE ||
          digitalRead(MENU_BTN_PIN) == BTN_ACTIVE) { waitAllRelease(); break; }
      if (digitalRead(DOWN_BTN_PIN) == BTN_ACTIVE && heldFor(DOWN_BTN_PIN, 700)) {
        dumpVlog(); buzz(30, 2); waitAllRelease();
      }
      if (digitalRead(UP_BTN_PIN) == BTN_ACTIVE && heldFor(UP_BTN_PIN, 900)) {
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
      delay(40);
    }
    myShowMenu(menuIndex, false);
  }

  void mySyncNTP() {
    guiState = APP_STATE;
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(0, 30);
    display.println("Syncing NTP...");
    display.display(true);   // partial: no flash
    bool ok = false;
    if (myConnectWiFi()) ok = syncNTP();   // RTC keeps UTC (offset 0)
    if (ok) lastNtpWake = wakeMin;
    display.fillScreen(GxEPD_BLACK);
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
    display.fillScreen(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_WHITE);
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
      display.fillScreen(GxEPD_BLACK);
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
      display.display(true);   // partial: no flash
      wifiManager.setTimeout(180);   // real phone-fumbling time, not 60s
      ok = wifiManager.startConfigPortal(WIFI_AP_SSID);
    }
    if (!ok) {
      display.fillScreen(GxEPD_BLACK);
      display.setCursor(0, 30);
      display.println("Setup failed &");
      display.println("timed out!");
      display.display(true);   // partial: no flash
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
      display.fillScreen(GxEPD_BLACK);
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
          display.fillScreen(GxEPD_BLACK);
          display.setCursor(0, 25);
          display.println("On your phone,");
          display.println("join WiFi AP:");
          display.println(WIFI_AP_SSID);
          display.display(true);   // partial: no flash
          WiFi.disconnect(true, false);
          delay(300);
          wifiManager.setTimeout(180);
          if (wifiManager.startConfigPortal(WIFI_AP_SSID))
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
    switch (menuIndex) {
      case 0: myShowAbout(); break;                     // state, not the radio
      case 1: setTime();   myShowMenu(menuIndex, false); break;
      case 2: mySetupWifi(); break;                     // BACK returns to menu
      case 3: mySyncNTP(); break;                       // keychain-aware
      case 4: setupWallet(); break;                     // ends in our menu
      case 5: showTimezone(); break;                    // ends in our menu
      case 6: timeTravel(); break;                      // arithmetic, not data
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
    if (!force && batteryPct() < 0.95f) {
      Serial.printf("[dock] charge first: %d%% %.2fV — sleeping so it fills\n",
                    (int)(batteryPct()*100.0f+0.5f), batteryVolts());
      inDocked = false;
      return;                       // caller resumes normal minute wakes
    }

    inDocked = true;
    Serial.println("[dock] enter (fast-poll transport)");
    display.display(true);                 // show the face already drawn
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
    bool chargeSaver = batteryPct() < 0.90f;
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
        bool want = batteryPct() < 0.90f;
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
            if (BUZZ_ON_BLOCK) vibMotor(75, 4);
            Serial.printf("[dock] block %ld\n", h);
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
          JsonDocument filter; filter["data"]["rates"] = true;
          JsonDocument doc;
          if (!deserializeJson(doc, http.getString(),
                               DeserializationOption::Filter(filter))) {
            for (int i = 0; i < 6; i++) {
              float r = atof(doc["data"]["rates"][CUR_CODES[i]] | "0");
              if (r > 0) fxRate[i] = r;
            }
            if (fxRate[0] > 0) {            // every currency refreshed
              priceBuzzCheck();             // in one docked call
              applyCurrency();
              redraw = true;
            }
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
      if (!dockNetDown && dispMode == M_WALT && walletView == 2 &&
          lnVerify[0] && millis() - lastAddrPoll > 5000) {
        lastAddrPoll = millis();
        if (lnSettled()) {
          lnVerify[0] = 0; lnInvoice[0] = 0;     // invoice is consumed
          vibMotor(60, 4); delay(120);
          vibMotor(60, 4); delay(120);
          vibMotor(60, 8);
          Serial.println("[dock] lightning payment settled!");
          drawLnPaid(LN_REQUEST_SATS);           // its own moment
          delay(3500);
          redraw = true;                         // then a fresh invoice
        }
      }
      if (!dockNetDown && dispMode == M_WALT && walletView == 1 &&
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
        } else {
          failCount++;
          nextTryWake = wakeMin + (failCount > 3 ? 5 : 1);
          Serial.println("[dock] fetch FAIL, backing off");
        }
        redraw = true;
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
            lastWalletMin = (wakeMin > WALLET_EVERY_MIN)
                              ? wakeMin - WALLET_EVERY_MIN : 0;
        } else if (dispMode == M_WALT) walletToggleView();
        else {
          if (dispMode == M_PRICE || dispMode == M_SATS ||
              dispMode == M_MCAP) {        // the CURRENCY dial: SATS
            curIdx = (curIdx + 1) % 6;     // and MCAP inherit whatever
            applyCurrency();               // PRICE speaks. Instant: the
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
        display.display(true);
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
    dispMode = (dispMode + 1) % NUM_MODES;
    return false;
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
    uint64_t wake = esp_sleep_get_ext1_wakeup_status();
    buttonWake = true;                 // don't tick the wake clock
    if (guiState == WATCHFACE_STATE) {
      if (wake & MENU_BTN_MASK) {
        myShowMenu(menuIndex, false);
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
            lastWalletMin = (wakeMin > WALLET_EVERY_MIN)
                              ? wakeMin - WALLET_EVERY_MIN : 0;
        } else if (dispMode == M_WALT) walletToggleView();   // balance <-> QR
        else {
          if (dispMode == M_PRICE || dispMode == M_SATS ||
              dispMode == M_MCAP) {        // the CURRENCY dial: SATS
            curIdx = (curIdx + 1) % 6;     // and MCAP inherit whatever
            applyCurrency();               // PRICE speaks. Instant: the
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
        RTC.read(currentTime); showWatchFace(false);
        return;
      } else if (wake & UP_BTN_MASK) {
        menuIndex--; if (menuIndex < 0) menuIndex = MY_MENU_LEN - 1;
        myShowMenu(menuIndex, true);
      } else if (wake & DOWN_BTN_MASK) {
        menuIndex++; if (menuIndex > MY_MENU_LEN - 1) menuIndex = 0;
        myShowMenu(menuIndex, true);
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
    while (!timeout) {
      if (millis() - lastTimeout > 5000) { timeout = true; }
      else {
        if (digitalRead(MENU_BTN_PIN) == BTN_ACTIVE) {
          lastTimeout = millis();
          if (guiState == MAIN_MENU_STATE) dispatchMenu();
          else if (guiState == WATCHFACE_STATE) {
            buttonWake = true;
            myShowMenu(menuIndex, false);
          }
          waitRelease(MENU_BTN_PIN);
        } else if (digitalRead(BACK_BTN_PIN) == BTN_ACTIVE) {
          lastTimeout = millis();
          if (guiState == MAIN_MENU_STATE) {
            RTC.read(currentTime); showWatchFace(false);
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
            menuIndex--; if (menuIndex < 0) menuIndex = MY_MENU_LEN - 1;
            myShowMenu(menuIndex, true);
          } else if (guiState == WATCHFACE_STATE) {
            buttonWake = true;
            pinMode(UP_BTN_PIN, INPUT);
        if (heldFor(UP_BTN_PIN, 600)) {    // LONG UP, any mode: fetch
          buzz(40, 2);                 // EVERYTHING now — the wrist
          forceFetch = true;               // shouldn't wait 15 minutes
          fetchPending = true;             // and the corner reports it
          if (dispMode == M_WALT)          // (in WAL: rescan too)
            lastWalletMin = (wakeMin > WALLET_EVERY_MIN)
                              ? wakeMin - WALLET_EVERY_MIN : 0;
        } else if (dispMode == M_WALT) walletToggleView();
            else {
          if (dispMode == M_PRICE || dispMode == M_SATS ||
              dispMode == M_MCAP) {        // the CURRENCY dial: SATS
            curIdx = (curIdx + 1) % 6;     // and MCAP inherit whatever
            applyCurrency();               // PRICE speaks. Instant: the
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
            menuIndex++; if (menuIndex > MY_MENU_LEN - 1) menuIndex = 0;
            myShowMenu(menuIndex, true);
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
