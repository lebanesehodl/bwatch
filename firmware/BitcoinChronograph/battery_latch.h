// The battery figure on the cell: what the face and About print while no
// charger is in, from the rounded reading batteryPctShown() takes. Plain C++
// with no Arduino in it, so firmware/ci/build.sh can test it on the build
// machine (firmware/ci/battery_latch_test.cpp) before the sketch compiles.
// The charger's own figures (charging, full) stay in the sketch.
#pragma once
#include <stdint.h>

#define BATT_WINDOW   5     // samples the latch looks back over
#define BATT_STEP_MAX 20    // the most it falls in one go

struct BattWin {            // kept in RTC memory beside the latch
  int8_t   samp[BATT_WINDOW];   // newest first
  uint8_t  n;                   // samples since the latch started
  uint32_t minute;              // RTC minute of samp[0]
};

// The latch: -1 until a reading, then never rising on the cell, because a
// voltage reading jitters and a number that climbs back looks broken. A real
// cell does not fall twenty points between two wakes. If it seems to, that is
// a bad reading, not a battery: step down by twenty and let the next wake
// confirm it.
static inline int battOnCell(int8_t &shown, BattWin &w, int rounded, uint32_t minute) {
  (void)w; (void)minute;
  if (shown < 0) { shown = (int8_t)rounded; return rounded; }
  if (rounded < shown) {
    int step = shown - rounded;
    shown = (int8_t)((step > BATT_STEP_MAX) ? shown - BATT_STEP_MAX : rounded);
  }
  return shown;
}
