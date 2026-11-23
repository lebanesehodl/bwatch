// The moon's age for JOE: days since the last new moon. Plain C++ with no
// Arduino in it, so firmware/ci/build.sh can test it on the build machine
// (firmware/ci/moon_test.cpp) before the sketch compiles.
//
// Worked out on the watch, with no radio: the mean synodic month (29.530588853
// days) counted from a known new moon, 2000-01-06 18:14 UTC (Meeus, Astronomical
// Algorithms). The real moon runs ahead of or behind that mean by hours, which
// is under a day either way, and the test holds it to that against new and
// full moons of 2024-25. That is fine for a phase picture and an age in whole
// days, the way digital watches have always shown it. It is not good enough
// to time an eclipse.
#pragma once
#include <stdint.h>
#include <math.h>

#define MOON_SYNODIC_DAYS 29.530588853
#define MOON_NEW_2000_UTC 947182440LL      // 2000-01-06 18:14 UTC

// Days since the last new moon at unix time utc: 0 (new) .. 29.53.
static inline double moonAgeDays(int64_t utc) {
  double d = (double)(utc - MOON_NEW_2000_UTC) / 86400.0;
  double a = fmod(d, MOON_SYNODIC_DAYS);
  return a < 0 ? a + MOON_SYNODIC_DAYS : a;
}

// Is the point (dx, dy), in units of the moon's radius from its centre,
// lit at this age? The northern sky's view: a waxing moon is lit on the right.
// The terminator is the ellipse x = cos(phase) * sqrt(1 - y^2).
static inline bool moonLit(double age, double dx, double dy) {
  double th = 2.0 * M_PI * age / MOON_SYNODIC_DAYS;
  double s = sqrt(1.0 - dy * dy);
  return th < M_PI ? dx > cos(th) * s : dx < -cos(th) * s;
}
