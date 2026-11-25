// Host test for firmware/BitcoinChronograph/case_temp.h, run by build.sh
// before the sketch compiles:
//   c++ -I<sketch> -I<Watchy>/src case_temp_test.cpp <Watchy>/src/bma4.c
// Each case puts a temperature register byte on a fake I2C bus, reads it
// back through the pinned driver's own bma4_get_temperature() and the lines
// of BMA423::readTemperature() (bma.cpp, which needs Arduino, so copied
// below), and checks what caseTempFromBma() makes of it. Then the HOT / COLD
// hysteresis at its edges. Exit status: the number of failed checks.
#include <stdio.h>
#include <string.h>
#include "case_temp.h"
#include "bma4.h"

static uint8_t regByte;          // what the fake chip holds at 0x22
static uint16_t fakeRead(uint8_t, uint8_t reg, uint8_t *data, uint16_t len) {
  for (uint16_t i = 0; i < len; i++) data[i] = (reg + i == 0x22) ? regByte : 0;
  return 0;
}
static void fakeDelay(uint32_t) {}

// BMA423::readTemperature(), Watchy v1.4.15 bma.cpp:135-146, line for line
static float readTemperature(struct bma4_dev *dev) {
  int32_t data = 0;
  bma4_get_temperature(&data, BMA4_DEG, dev);
  float res = (float)data / (float)BMA4_SCALE_TEMP;
  if (((data - 23) / BMA4_SCALE_TEMP) == 0x80) res = 0;
  return res;
}

static int fails = 0;
static void check(bool ok, const char *what) {
  if (!ok) { fails++; printf("FAIL %s\n", what); }
}

int main() {
  struct bma4_dev dev;
  memset(&dev, 0, sizeof dev);
  dev.bus_read = fakeRead;
  dev.delay = fakeDelay;
  dev.interface = BMA4_I2C_INTERFACE;
  char what[96];

  // every byte the chip can hold that means a temperature in range
  for (int realC = -40; realC <= 85; realC++) {
    regByte = (uint8_t)(int8_t)(realC - 23);   // two's complement, 0 = 23 C
    float t = readTemperature(&dev);
    int8_t c = 99;
    bool ok = caseTempFromBma(t, 99, &c);
    snprintf(what, sizeof what, "%d C (byte 0x%02X, driver says %.0f) read as %s%d",
             realC, regByte, t, ok ? "" : "no reading, ", c);
    check(ok && c == realC, what);
  }
  // colder or hotter than the clamp: its ends
  regByte = (uint8_t)(int8_t)(-60 - 23);
  { int8_t c = 0; check(caseTempFromBma(readTemperature(&dev), 99, &c) && c == -40, "-60 C clamps to -40"); }
  regByte = (uint8_t)(int8_t)(100 - 23);
  { int8_t c = 0; check(caseTempFromBma(readTemperature(&dev), 99, &c) && c == 85, "100 C clamps to 85"); }

  // 0x80: the chip has no reading. It must not become -105 (-40) and COLD.
  regByte = 0x80;
  { int8_t c = 42; check(!caseTempFromBma(readTemperature(&dev), 20, &c) && c == 42,
                         "byte 0x80 is no reading"); }
  // the same byte from a driver that reads it signed
  { int8_t c = 42; check(!caseTempFromBma(-105.0f, 20, &c) && c == 42, "-105 is no reading"); }
  // the driver's own "no reading" answer, 0: kept out unless the last
  // reading was near 0 C (or there was none)
  { int8_t c = 42; check(!caseTempFromBma(0.0f, 20, &c), "0 after 20 C is no reading"); }
  { int8_t c = 42; check(caseTempFromBma(0.0f, 2, &c) && c == 0, "0 after 2 C is 0 C"); }
  { int8_t c = 42; check(caseTempFromBma(0.0f, 99, &c) && c == 0, "0 as the first reading is 0 C"); }

  // hysteresis: COLD at <= 5, off at >= 8; HOT retired
  check(tempModeNext(TEMP_OK, 6)   == TEMP_OK,   "OK at 6 stays OK");
  check(tempModeNext(TEMP_OK, 5)   == TEMP_COLD, "OK at 5 goes COLD");
  check(tempModeNext(TEMP_OK, -40) == TEMP_COLD, "OK at -40 goes COLD");
  check(tempModeNext(TEMP_COLD, 7) == TEMP_COLD, "COLD at 7 stays COLD");
  check(tempModeNext(TEMP_COLD, 8) == TEMP_OK,   "COLD at 8 goes OK");
  check(tempModeNext(TEMP_OK, 85)  == TEMP_OK,   "OK at 85 stays OK (HOT retired)");
  check(tempModeNext(TEMP_HOT, 60) == TEMP_OK,   "HOT left in RTC memory clears");
  check(tempModeNext(TEMP_HOT, 3)  == TEMP_COLD, "HOT left in RTC memory, at 3: COLD");

  // and the whole path as a fridge run: a cold watch comes in through the
  // driver and ends up COLD
  { uint8_t mode = TEMP_OK; int8_t c = 99;
    const int run[] = {24, 18, 12, 8, 6, 5, 4};
    for (int realC : run) {
      regByte = (uint8_t)(int8_t)(realC - 23);
      if (caseTempFromBma(readTemperature(&dev), c, &c)) mode = tempModeNext(mode, c);
    }
    check(c == 4 && mode == TEMP_COLD, "fridge run 24 -> 4 C ends COLD at 4 C"); }

  printf("case_temp_test: %d failed\n", fails);
  return fails;
}
