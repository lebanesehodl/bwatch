// STEPS TODAY, on JOE: the accelerometer's own step counter, cut into local
// days. Plain C++ over the pinned Watchy driver's bma4 code, with no Arduino
// in it, so firmware/ci/build.sh can test it on the build machine
// (firmware/ci/steps_today_test.cpp) before the sketch compiles.
//
// The BMA423 counts steps by itself while the ESP32 is in deep sleep. The
// library switches its step counter on and zeroes it on every real boot
// (Watchy v1.4.15, Watchy.cpp _bmaConfig()), and the sketch never writes to
// it after that. So the watch does not reset the chip at midnight. It
// notes what the counter read at the first wake of each local day, and
// today's steps are the difference. The chip is only ever read.
//
// What the face may then say:
//   TODAY        the count began at local midnight
//   SINCE hh:mm  it began later the same day: a boot (the library zeroed
//                the chip), out of the box (the accelerometer was off),
//                the counter going backwards, or the clock set back a day
//   --           no count to stand behind: no reading, a sensor that is not
//                set up, no date, or nothing read yet today. Never a 0.
#pragma once
#include <stdint.h>
#include "bma423.h"   // and bma4.h, inside its extern "C": the driver is C

// Kept in RTC memory as one struct (RTC_DATA_ATTR in the sketch).
struct StepSpan {
  uint32_t base;    // the counter when today's count began
  uint32_t last;    // the counter at the last good read
  uint32_t seen;    // local time (s since 1970) of the last good read
  uint32_t day;     // the local day (days since 1970) it counts; 0: none
  uint16_t from;    // the local minute of that day it began; 0: midnight
  uint8_t  setup;   // the chip's step counter: STEPCTR_ below
};

enum { STEPS_NONE = 0, STEPS_TODAY = 1, STEPS_SINCE = 2 };
// Whether the chip's step counter is switched on: not known (not asked since
// the boot or a layout reset, or the chip did not answer), on, or off.
enum { STEPCTR_UNKNOWN = 0, STEPCTR_ON = 1, STEPCTR_OFF = 2 };

// More than this in one day is a bad read, not a walk (~750 km of steps).
#define STEPS_MAX_DAY 999999UL

// Forget the count: the next read starts a new one, labelled SINCE.
static inline void stepSpanForget(StepSpan *s) { s->day = 0; }

// A new count, beginning at this read.
static inline void stepSpanStart(StepSpan *s, uint32_t hw, uint32_t lt) {
  s->base = hw;
  s->day = lt / 86400;
  s->from = (uint16_t)((lt % 86400) / 60);
}

// A good read: the counter is hw at local time lt. The first read of a new
// day starts that day's count. It counts from midnight (TODAY) only if the
// last read was no more than gapS earlier: the watch was reading right up
// to midnight, and what falls between the two reads is at most gapS of
// walking (a minute while awake; a rest's few minutes, with the watch lying
// still). Otherwise the day counts from this read (SINCE).
static inline void stepSpanNote(StepSpan *s, uint32_t hw, uint32_t lt,
                                uint32_t gapS) {
  uint32_t day = lt / 86400;
  if (s->day == 0 || hw < s->last || day < s->day) {
    // nothing yet; the chip started again from 0; the clock went back a day
    stepSpanStart(s, hw, lt);
  } else if (day > s->day) {
    bool upToMidnight = lt >= s->seen && lt - s->seen <= gapS;
    stepSpanStart(s, hw, lt);
    if (upToMidnight) s->from = 0;
  }
  s->last = hw;
  s->seen = lt;
}

// What the face shows for a good read hw at local time lt, already noted:
// STEPS_TODAY or STEPS_SINCE (s->from says since when) with the count in
// *steps, or STEPS_NONE.
static inline int stepSpanShow(const StepSpan *s, uint32_t hw, uint32_t lt,
                               uint32_t *steps) {
  if (s->day == 0 || lt / 86400 != s->day || hw < s->base) return STEPS_NONE;
  uint32_t n = hw - s->base;
  if (n > STEPS_MAX_DAY) return STEPS_NONE;
  *steps = n;
  return s->from == 0 ? STEPS_TODAY : STEPS_SINCE;
}

// ---- the chip ----
// dev has been through bma423_init() (chip id read, feature length set).

// Counting now: a BMA423 whose features are running (INTERNAL_STATUS is
// "initialised", the test the driver itself applies after loading them,
// bma4.c bma4_write_config_file()) and whose accelerometer is on (the box
// switches it off). Two one-byte reads.
static inline bool stepChipCounting(struct bma4_dev *dev) {
  if (dev->chip_id != BMA423_CHIP_ID) return false;
  uint8_t st = 0, en = 0;
  if (bma4_read_regs(BMA4_INTERNAL_STAT, &st, 1, dev) != BMA4_OK ||
      st != BMA4_ASIC_INITIALIZED) return false;
  if (bma4_get_accel_enable(&en, dev) != BMA4_OK || en != BMA4_ENABLE) return false;
  return true;
}

// Is the step counter switched on? It is a bit in the feature block. The
// sketch asks once per boot, not every wake, because the driver turns the
// chip's power saving off and back on around every read of that block.
// The answer is STEPCTR_UNKNOWN when the chip does not answer, and then the
// sketch asks again next time.
// The driver reads the block in pieces through an address register pair
// (0x5B/0x5C) and puts back the block's start, from dev->asic_data, when it
// is done. bma423_init() leaves that zero, and only the config upload fills
// it in (bma4.c get_feature_config_start_addr(), into the library's own
// handle, which is private). So read it here the way that function does:
// at rest the pair holds the start, because every driver access puts it back.
static inline uint8_t stepCounterState(struct bma4_dev *dev) {
  if (dev->chip_id != BMA423_CHIP_ID) return STEPCTR_UNKNOWN;
  uint8_t lsb = 0, msb = 0;
  if (bma4_read_regs(BMA4_RESERVED_REG_5B_ADDR, &lsb, 1, dev) != BMA4_OK ||
      bma4_read_regs(BMA4_RESERVED_REG_5C_ADDR, &msb, 1, dev) != BMA4_OK)
    return STEPCTR_UNKNOWN;
  dev->asic_data.asic_lsb = lsb & 0x0F;
  dev->asic_data.asic_msb = msb;
  uint8_t f[BMA423_STEP_CNTR_OFFSET + 2] = {0};   // up to the enable byte; even
  if (bma4_read_regs(BMA4_FEATURE_CONFIG_ADDR, f, sizeof f, dev) != BMA4_OK)
    return STEPCTR_UNKNOWN;
  return (f[BMA423_STEP_CNTR_OFFSET + 1] & BMA423_STEP_CNTR_EN_MSK) ? STEPCTR_ON
                                                                    : STEPCTR_OFF;
}

// The counter itself, four bytes. False: no reading (not a 0).
static inline bool stepCountRead(struct bma4_dev *dev, uint32_t *out) {
  if (dev->chip_id != BMA423_CHIP_ID) return false;
  uint32_t n = 0;
  if (bma423_step_counter_output(&n, dev) != BMA4_OK) return false;
  *out = n;
  return true;
}
