// The case temperature: the accelerometer's thermometer, and the HOT / COLD
// modes it sets. Plain C++ with no Arduino in it, so firmware/ci/build.sh can
// test it on the build machine (firmware/ci/case_temp_test.cpp) before the
// sketch compiles.
#pragma once
#include <stdint.h>
#include <math.h>

// ---- the case temperature (Nov 8): the accelerometer's thermometer ----
// The panel picks its waveform from its own sensor; in direct sun the glass
// runs hotter than that and the ink comes out washed out, and in the cold
// the ink is sluggish and the cell sags. The case reading (whole degrees,
// lagging the face by minutes) sets a mode, with hysteresis so it does not
// flicker at the edge:
//   HOT  at >= 38 C, off at <= 35 C: a wrist alone keeps the case ~28-34 C
//   COLD at <=  5 C, off at >=  8 C: a car, a windowsill, a coat pocket
// Starting values, to be tuned from what About shows in the wild.
// HOT RETIRED (Nov 8): the sun fade is light breaking the full refresh's
// waveform (labs 5-6 under a phone LED), not heat; and docked, a charging,
// awake watch read HOT and froze in a HOT refresh. Out of reach until the
// mode is either proven or removed.
#define TEMP_HOT_ON  127
#define TEMP_HOT_OFF  35
#define TEMP_COLD_ON   5
#define TEMP_COLD_OFF  8
#define TEMP_PANEL_C  25      // what the panel is told while HOT
enum { TEMP_OK = 0, TEMP_HOT = 1, TEMP_COLD = 2 };

// What sensor.readTemperature() returns, as whole degrees C. The BMA423
// keeps the temperature as a signed byte, 0 = 23 C. The pinned Watchy driver
// (v1.4.15 bma4.c, bma4_get_temperature(); master is the same) reads that
// byte unsigned, so every reading below 23 C comes back 256 too high, as
// 151-278. The clamp below made all of those 85 C, and COLD (<= 5) and TOO
// COLD TO CHARGE (<= 0) could never happen. Undone here:
//   151 (the byte 0x80) is the chip's "no reading", and so is -105, the same
//   byte read signed by a driver that gets this right.
//   152-278 are the readings below 23 C: minus 256.
//   0 is what the driver answers when it decides there is no reading (its
//   test for 0x80 is off and catches the byte 0x6A instead). That stays a
//   non-reading unless the last reading was close to 0, where it can be a
//   real 0 C from a driver that gets the sign right (the rule from before).
// Then the clamp, as before. False: no reading, keep the last one.
static inline bool caseTempFromBma(float reported, int8_t last, int8_t *out) {
  long r = lroundf(reported);
  if (r == 151 || r == -105) return false;
  if (r == 0 && last != 99 && (last > 4 || last < -4)) return false;
  if (r >= 152 && r <= 278) r -= 256;
  if (r < -40) r = -40;
  if (r > 85) r = 85;
  *out = (int8_t)r;
  return true;
}

// The mode after a reading of c degrees, from the mode before it.
static inline uint8_t tempModeNext(uint8_t mode, int c) {
  if (mode == TEMP_HOT  && c <= TEMP_HOT_OFF)  mode = TEMP_OK;
  if (mode == TEMP_HOT  && TEMP_HOT_ON > 100)  mode = TEMP_OK;  // retired: a watch
                                     // that froze in HOT keeps it in RTC memory
  if (mode == TEMP_COLD && c >= TEMP_COLD_OFF) mode = TEMP_OK;
  if (mode == TEMP_OK) {
    if (c >= TEMP_HOT_ON)  mode = TEMP_HOT;
    if (c <= TEMP_COLD_ON) mode = TEMP_COLD;
  }
  return mode;
}
