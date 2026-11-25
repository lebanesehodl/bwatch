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

// The latch never rises on the cell, because a voltage reading jitters and a
// number that climbs back looks broken. That made a low reading permanent,
// and the radio makes them: the cell reads low for a while after the setup
// network or a fetch. Until Nov 25 every call took its own reading and could
// step the latch down twenty, and JOE calls twice a draw (the glyph, then
// the number), so the draw after Setup Wallet and the next minute's weather
// fetch took OP's watch from 100% to 40% for good.
//
// So:
//   - one sample a minute, by the RTC: every call in that minute gets the
//     same figure, however many draws or screens ask
//   - the latch falls only to the BEST of the last BATT_WINDOW samples. A
//     dip under load lasts a sample or two; a real fall is in all of them
//     and shows BATT_WINDOW samples later (minutes on the wrist, hours in
//     the night rest's hourly wakes, where nothing uses the radio)
//   - by twenty at most a sample, as before
//   - from -1 (a restart, the cable just out, a calibration) the first
//     BATT_WINDOW samples may raise it to their best, so a first reading
//     taken straight after the boot's fetch does not hold for the whole run
static inline int battOnCell(int8_t &shown, BattWin &w, int rounded, uint32_t minute) {
  if (shown < 0 || w.n > BATT_WINDOW) w.n = 0;        // start over (or RTC garbage)
  if (w.n > 0 && minute == w.minute) return shown;    // this minute has its sample
  for (int i = BATT_WINDOW - 1; i > 0; i--) w.samp[i] = w.samp[i - 1];
  w.samp[0] = (int8_t)rounded;
  w.minute = minute;
  if (w.n < BATT_WINDOW) w.n++;
  int best = 0;
  for (int i = 0; i < w.n; i++) if (w.samp[i] > best) best = w.samp[i];
  if (w.n < BATT_WINDOW) { shown = (int8_t)best; return best; }   // starting: may rise
  if (best < shown) {
    int step = shown - best;
    shown = (int8_t)((step > BATT_STEP_MAX) ? shown - BATT_STEP_MAX : best);
  }
  return shown;
}
