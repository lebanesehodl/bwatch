// Host test for firmware/BitcoinChronograph/moon.h, run by build.sh before
// the sketch compiles:  c++ -I<sketch> moon_test.cpp
// The age against real new and full moons (UTC, to the minute), then the
// picture: dark when new, lit when full, the right half at first quarter
// and the left half at last quarter. Exit status: the number of failed checks.
#include <stdio.h>
#include <math.h>
#include "moon.h"

static int fails = 0;
static void check(bool ok, const char *what) {
  if (!ok) { fails++; printf("FAIL %s\n", what); }
}
// days between two ages on the 29.53-day circle
static double off(double a, double b) {
  double d = fabs(a - b);
  return d > MOON_SYNODIC_DAYS / 2 ? MOON_SYNODIC_DAYS - d : d;
}

int main() {
  const double FULL = MOON_SYNODIC_DAYS / 2;
  // new moons
  check(off(moonAgeDays(1704974220LL), 0) < 1, "new moon 2024-01-11 11:57");
  check(off(moonAgeDays(1743245880LL), 0) < 1, "new moon 2025-03-29 10:58");
  // full moons
  check(off(moonAgeDays(1706205240LL), FULL) < 1, "full moon 2024-01-25 17:54");
  check(off(moonAgeDays(1741935300LL), FULL) < 1, "full moon 2025-03-14 06:55");
  // the reference itself, and before it
  check(moonAgeDays(MOON_NEW_2000_UTC) < 1e-9, "age 0 at the reference");
  { double a = moonAgeDays(MOON_NEW_2000_UTC - 86400);
    check(a > 28.4 && a < 28.6, "a day before the reference: 28.5 days"); }
  // always in range
  for (int64_t t = 0; t < 4000000000LL; t += 7777777)
    { double a = moonAgeDays(t); if (a < 0 || a >= MOON_SYNODIC_DAYS) { check(false, "age in range"); break; } }

  // the picture
  int litNew = 0, litFull = 0;
  for (double y = -0.95; y < 1; y += 0.1)
    for (double x = -0.95; x < 1; x += 0.1) {
      if (x * x + y * y > 1) continue;
      litNew += moonLit(0.05, x, y);
      litFull += !moonLit(FULL, x, y);
    }
  check(litNew == 0, "new moon: nothing lit");
  check(litFull == 0, "full moon: everything lit");
  double q1 = MOON_SYNODIC_DAYS / 4, q3 = 3 * MOON_SYNODIC_DAYS / 4;
  check(moonLit(q1, 0.5, 0) && !moonLit(q1, -0.5, 0), "first quarter: right half lit");
  check(moonLit(q3, -0.5, 0) && !moonLit(q3, 0.5, 0), "last quarter: left half lit");
  check(moonLit(5, 0.9, 0) && !moonLit(5, 0, 0), "waxing crescent: a sliver on the right");
  check(moonLit(25, -0.9, 0) && !moonLit(25, 0, 0), "waning crescent: a sliver on the left");

  printf("moon_test: %d failed\n", fails);
  return fails;
}
