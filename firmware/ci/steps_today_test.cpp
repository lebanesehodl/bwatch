// Host test for firmware/BitcoinChronograph/steps_today.h, run by build.sh
// before the sketch compiles:
//   c++ -I<sketch> -I<Watchy>/src steps_today_test.cpp <Watchy>/src/bma4.c
//       <Watchy>/src/bma423.c
// First the chip: a fake BMA423 on a fake I2C bus, read through the pinned
// driver's own functions, the way the sketch reads it. Every way the chip
// can fail must come back as "no count", never as a count of 0. Then the
// days: midnight, a boot, the box, a clock set back.
// Exit status: the number of failed checks.
#include <stdio.h>
#include <string.h>
#include "steps_today.h"

// ---- the fake chip ----
static uint8_t reg[128];               // the register map
static uint8_t feat[64];               // the feature block, behind 0x5E
static const uint16_t FEAT_START = 0x0A5;   // its start, in words (5B/5C);
                                       // not 0, so a read from 0 shows up
static bool busDead = false;
static uint16_t featWord() { return (uint16_t)(reg[0x5C] << 4) | (reg[0x5B] & 0x0F); }

static uint16_t fakeRead(uint8_t, uint8_t r, uint8_t *data, uint16_t len) {
  if (busDead) return 1;
  for (uint16_t i = 0; i < len; i++) {
    if (r == 0x5E) {                   // the feature block, from the pair
      int at = (featWord() - FEAT_START) * 2 + i;
      data[i] = (at >= 0 && at < 64) ? feat[at] : 0xEE;
    } else {
      data[i] = reg[(r + i) & 0x7F];
    }
  }
  return 0;
}
static uint16_t fakeWrite(uint8_t, uint8_t r, uint8_t *data, uint16_t len) {
  if (busDead) return 1;
  if (r == 0x5E) return 0;             // the sketch never writes the block
  for (uint16_t i = 0; i < len; i++) reg[(r + i) & 0x7F] = data[i];
  return 0;
}
static void fakeDelay(uint32_t) {}

static void healthyChip(uint32_t steps) {
  memset(reg, 0, sizeof reg);
  memset(feat, 0, sizeof feat);
  reg[0x00] = 0x13;                    // CHIP_ID: BMA423
  reg[0x2A] = 0x01;                    // INTERNAL_STATUS: initialised
  reg[0x7D] = 0x04;                    // PWR_CTRL: accelerometer on
  reg[0x7C] = 0x03;                    // PWR_CONF: advanced power save on
  reg[0x5B] = FEAT_START & 0x0F;
  reg[0x5C] = (uint8_t)(FEAT_START >> 4);
  feat[BMA423_STEP_CNTR_OFFSET + 1] = BMA423_STEP_CNTR_EN_MSK;   // counting
  for (int i = 0; i < 4; i++) reg[0x1E + i] = (uint8_t)(steps >> (8 * i));
  busDead = false;
}

// the sketch's handle: its own, through bma423_init(), like accOpen()
static bool openDev(struct bma4_dev *dev) {
  memset(dev, 0, sizeof *dev);
  dev->dev_addr = BMA4_I2C_ADDR_PRIMARY;
  dev->interface = BMA4_I2C_INTERFACE;
  dev->bus_read = fakeRead; dev->bus_write = fakeWrite; dev->delay = fakeDelay;
  dev->read_write_len = 8;
  return bma423_init(dev) == BMA4_OK;
}

static int fails = 0;
static void check(bool ok, const char *what) {
  if (!ok) { fails++; printf("FAIL %s\n", what); }
}

// local time, seconds since 1970: day d at hh:mm
static uint32_t at(uint32_t d, int hh, int mm, int ss = 0) {
  return d * 86400u + (uint32_t)(hh * 3600 + mm * 60 + ss);
}
static const uint32_t D = 20415;      // a local day (2025-11-23)
static const uint32_t GAP = 6 * 60;   // the sketch's: REST_WAKE_MIN + 1 min

static int show(const StepSpan &s, uint32_t hw, uint32_t lt, uint32_t *n) {
  *n = 0xDEADBEEF;
  return stepSpanShow(&s, hw, lt, n);
}

int main() {
  struct bma4_dev dev;
  uint32_t n = 0;

  // ---- the chip ----
  healthyChip(4812);
  check(openDev(&dev), "healthy chip opens");
  check(stepChipCounting(&dev), "healthy chip is counting");
  check(stepCounterState(&dev) == STEPCTR_ON, "healthy chip has its step counter on");
  check(featWord() == FEAT_START, "the feature address is put back after the read");
  check(stepCountRead(&dev, &n) && n == 4812, "healthy chip reads 4812");
  for (int i = 0; i < 4; i++) reg[0x1E + i] = 0xFF;
  check(stepCountRead(&dev, &n) && n == 0xFFFFFFFFu, "all four bytes of the counter");

  healthyChip(0);
  openDev(&dev);
  check(stepCountRead(&dev, &n) && n == 0, "a real 0 reads as 0");

  healthyChip(4812);
  openDev(&dev);
  reg[0x2A] = 0x00;                    // features never started
  check(!stepChipCounting(&dev), "INTERNAL_STATUS not_init: not counting");
  reg[0x2A] = 0x02;                    // init_err
  check(!stepChipCounting(&dev), "INTERNAL_STATUS init_err: not counting");
  reg[0x2A] = 0x41;                    // initialised, but ODR under 50 Hz
  check(!stepChipCounting(&dev), "INTERNAL_STATUS odr_50hz_error: not counting");

  healthyChip(4812);
  openDev(&dev);
  reg[0x7D] = 0x00;                    // the box: accelerometer off
  check(!stepChipCounting(&dev), "accelerometer off: not counting");

  healthyChip(4812);
  openDev(&dev);
  feat[BMA423_STEP_CNTR_OFFSET + 1] = 0;   // the library's enable never landed
  check(stepCounterState(&dev) == STEPCTR_OFF, "step counter not switched on: off");

  healthyChip(4812);
  check(openDev(&dev), "opens");
  busDead = true;                      // the bus stops answering
  n = 77;
  check(!stepCountRead(&dev, &n) && n == 77, "dead bus: no count, and nothing written");
  check(!stepChipCounting(&dev), "dead bus: not counting");
  check(stepCounterState(&dev) == STEPCTR_UNKNOWN, "dead bus: not known, ask again");

  healthyChip(4812);
  busDead = true;
  check(!openDev(&dev), "dead bus at open: fails");
  n = 77;
  check(!stepCountRead(&dev, &n) && n == 77, "never opened: no count");

  healthyChip(4812);
  reg[0x00] = 0x11;                    // some other chip
  openDev(&dev);
  check(!stepChipCounting(&dev) && stepCounterState(&dev) == STEPCTR_UNKNOWN &&
        !stepCountRead(&dev, &n),
        "not a BMA423: nothing");

  // ---- the days ----
  StepSpan s;
  memset(&s, 0, sizeof s);
  check(show(s, 100, at(D, 9, 0), &n) == STEPS_NONE, "nothing noted: no count");

  // a boot at 14:32: counts SINCE 14:32
  stepSpanNote(&s, 0, at(D, 14, 32), GAP);
  check(show(s, 0, at(D, 14, 32), &n) == STEPS_SINCE && n == 0 && s.from == 14 * 60 + 32,
        "boot at 14:32: 0 SINCE 14:32");
  stepSpanNote(&s, 1500, at(D, 18, 0), GAP);
  check(show(s, 1500, at(D, 18, 0), &n) == STEPS_SINCE && n == 1500,
        "1500 SINCE 14:32 at 18:00");

  // read every minute to midnight: the new day counts from midnight
  stepSpanNote(&s, 4000, at(D, 23, 59), GAP);
  stepSpanNote(&s, 4010, at(D + 1, 0, 0), GAP);
  check(show(s, 4010, at(D + 1, 0, 0), &n) == STEPS_TODAY && n == 0,
        "midnight: 0 TODAY");
  stepSpanNote(&s, 9822, at(D + 1, 17, 45), GAP);
  check(show(s, 9822, at(D + 1, 17, 45), &n) == STEPS_TODAY && n == 5812,
        "5812 TODAY at 17:45");

  // and yesterday's count is not shown today
  check(show(s, 9822, at(D + 2, 0, 1), &n) == STEPS_NONE,
        "a count from yesterday, not noted today: no count");

  // resting by day across midnight (still): reads 5 minutes apart
  stepSpanNote(&s, 12000, at(D + 1, 23, 57), GAP);
  stepSpanNote(&s, 12000, at(D + 2, 0, 2), GAP);
  check(show(s, 12000, at(D + 2, 0, 2), &n) == STEPS_TODAY && n == 0,
        "a rest read at 23:57 and 00:02: TODAY");

  // the night rest wakes on the hour: 23:00, then 00:00
  stepSpanNote(&s, 12500, at(D + 2, 23, 0), GAP);
  stepSpanNote(&s, 12520, at(D + 3, 0, 0, 5), GAP);
  check(show(s, 12520, at(D + 3, 0, 0, 5), &n) == STEPS_TODAY && n == 0,
        "night rest, 23:00 then 00:00: TODAY from midnight");

  // nothing read across midnight for hours (a flat cell resting): SINCE
  stepSpanNote(&s, 13000, at(D + 3, 22, 0), GAP);
  stepSpanNote(&s, 13000, at(D + 4, 7, 10), GAP);
  check(show(s, 13000, at(D + 4, 7, 10), &n) == STEPS_SINCE && s.from == 7 * 60 + 10,
        "last read 22:00, next 07:10: SINCE 07:10");

  // read at 23:59, then not until 00:00 two days on: that read is midnight
  stepSpanNote(&s, 13100, at(D + 4, 23, 59), GAP);
  stepSpanNote(&s, 13100, at(D + 6, 0, 0), GAP);
  check(show(s, 13100, at(D + 6, 0, 0), &n) == STEPS_TODAY && n == 0,
        "a reading at 00:00 itself counts from midnight");
  stepSpanNote(&s, 13200, at(D + 6, 23, 59), GAP);
  stepSpanNote(&s, 13200, at(D + 8, 0, 3), GAP);
  check(show(s, 13200, at(D + 8, 0, 3), &n) == STEPS_SINCE && s.from == 3,
        "a day skipped: SINCE 00:03, not TODAY");

  // the chip starts again from 0 (it was reset): the count starts again
  stepSpanNote(&s, 15000, at(D + 8, 12, 0), GAP);
  stepSpanNote(&s, 40, at(D + 8, 12, 1), GAP);
  check(show(s, 40, at(D + 8, 12, 1), &n) == STEPS_SINCE && n == 0 && s.from == 12 * 60 + 1,
        "counter went backwards: 0 SINCE 12:01");
  stepSpanNote(&s, 340, at(D + 8, 12, 30), GAP);
  check(show(s, 340, at(D + 8, 12, 30), &n) == STEPS_SINCE && n == 300,
        "and counts on from there");

  // the clock goes back a few seconds (a time sync): the same day goes on
  stepSpanNote(&s, 400, at(D + 8, 12, 40, 30), GAP);
  stepSpanNote(&s, 410, at(D + 8, 12, 40, 10), GAP);
  check(show(s, 410, at(D + 8, 12, 40, 10), &n) == STEPS_SINCE && n == 370 &&
        s.from == 12 * 60 + 1, "clock back 20 s: same count");

  // the clock set back a whole day: start again
  stepSpanNote(&s, 500, at(D + 7, 12, 0), GAP);
  check(show(s, 500, at(D + 7, 12, 0), &n) == STEPS_SINCE && n == 0 && s.day == D + 7,
        "clock back a day: 0 SINCE 12:00 on that day");

  // forgotten (a boot, out of the box): no count until the next read
  stepSpanForget(&s);
  check(show(s, 600, at(D + 7, 12, 5), &n) == STEPS_NONE, "forgotten: no count");
  stepSpanNote(&s, 600, at(D + 7, 12, 5), GAP);
  check(show(s, 600, at(D + 7, 12, 5), &n) == STEPS_SINCE && n == 0, "then SINCE");

  // a count no day can hold is a bad read
  stepSpanNote(&s, 600 + STEPS_MAX_DAY + 1, at(D + 7, 13, 0), GAP);
  check(show(s, 600 + STEPS_MAX_DAY + 1, at(D + 7, 13, 0), &n) == STEPS_NONE,
        "a million steps: no count");
  check(show(s, 599, at(D + 7, 13, 0), &n) == STEPS_NONE, "below the base: no count");

  printf("steps_today_test: %d failed\n", fails);
  return fails;
}
