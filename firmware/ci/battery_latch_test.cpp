// Host test for firmware/BitcoinChronograph/battery_latch.h, run by build.sh
// before the sketch compiles:  c++ -I<sketch> battery_latch_test.cpp
// Each call below is one batteryPctShown() on the cell: a fresh reading,
// rounded to 5%, and the RTC minute it was taken in. JOE calls it twice a
// draw (the glyph, then the number), About once more.
// Exit status: the number of failed checks.
#include <stdio.h>
#include "battery_latch.h"

static int fails = 0;
static void check(bool ok, const char *what) {
  if (!ok) { fails++; printf("FAIL %s\n", what); }
}

struct Watch {
  int8_t shown = -1;
  BattWin w = BattWin{};
  uint32_t minute = 1000;
  int last = -1;
  int lowest = 999;                 // the lowest figure it ever printed
  int call(int rounded) {
    last = battOnCell(shown, w, rounded, minute);
    if (last < lowest) lowest = last;
    return last;
  }
  void draw(int rounded) { call(rounded); call(rounded); }   // JOE: glyph, number
  void minutes(int n, int rounded) { for (int i = 0; i < n; i++) { minute++; draw(rounded); } }
};

int main() {
  // OP, Nov 25 on 1e2ebfc: 100% on JOE, a minute in Setup Wallet to set a
  // weather place, back to the face at 40%. The setup network and then JOE's
  // first weather fetch for the new place (on the next minute wake, just
  // before it draws) each leave the cell reading low for a while: 40 is
  // about 4.00-4.02 V on curvePct(). Then the cell recovers.
  { Watch t;
    t.minutes(10, 100);
    t.minute++; t.draw(40);          // BACK out of the menu: the face, after setup
    t.minute++; t.draw(40);          // the minute wake: weather fetch, then draw
    t.minutes(10, 100);              // recovered
    check(t.lowest == 100, "Setup Wallet then the weather fetch: never below 100");
    check(t.last == 100, "Setup Wallet then the weather fetch: 100 afterwards");
    if (t.lowest != 100) printf("     (lowest %d, now %d)\n", t.lowest, t.last); }

  // the same, opening About in that minute too: a third call
  { Watch t;
    t.minutes(10, 100);
    t.minute++; t.draw(40); t.call(40);
    t.minute++; t.draw(40);
    t.minutes(3, 100);
    check(t.lowest == 100, "About after setup: never below 100"); }

  // no ratchet inside a minute: however many draws, one figure
  { Watch t;
    t.minutes(10, 80);
    t.minute++;
    int first = t.call(0);
    bool same = true;
    for (int i = 0; i < 10; i++) same &= (t.call(0) == first);
    check(same, "ten calls in one minute: one figure");
    check(first == 80, "one zero reading against four good ones: still 80"); }

  // ...even where a drop is real: it lands once, not once per call
  { Watch t;
    t.minutes(10, 100);
    t.minutes(4, 70);
    t.minute++;
    int a = t.call(70), b = t.call(70), c = t.call(70);
    check(a == b && b == c, "a real drop: the same figure for every call that minute"); }

  // a real fall still shows, once it is in every sample of the window
  { Watch t;
    t.minutes(10, 100);
    t.minutes(BATT_WINDOW - 1, 70);
    check(t.last == 100, "a fall shorter than the window: not shown yet");
    t.minutes(1, 70);
    check(t.last == 80, "the fall in every sample: down, by twenty at most");
    t.minutes(1, 70);
    check(t.last == 70, "and the rest of it the minute after"); }

  // a dying cell gets to 0, twenty a minute once the window agrees
  { Watch t;
    t.minutes(10, 100);
    t.minutes(BATT_WINDOW + 4, 0);
    check(t.last == 0, "a collapse reaches 0"); }

  // never rising on the cell once it has started
  { Watch t;
    t.minutes(10, 70);
    t.minutes(10, 85);
    check(t.last == 70, "on the cell it does not climb back"); }

  // a restart (the latch at -1: a reflash, the cable just out): a first
  // reading taken straight after the boot's fetch does not hold all run
  { Watch t;
    t.minute++; t.draw(40);
    t.minutes(3, 100);
    check(t.last == 100, "a low first reading after a restart: recovers"); }
  { Watch t;                         // ...but only while it is starting
    t.minutes(BATT_WINDOW, 60);
    t.minutes(5, 100);
    check(t.last == 60, "after the first window: no climbing"); }
  { Watch t;                         // the latch set back to -1 starts over
    t.minutes(10, 60);
    t.shown = -1;
    t.minutes(2, 90);
    check(t.last == 90, "the latch at -1 learns again"); }

  // the clock set back or forward is another minute, so a new sample
  { Watch t;
    t.minutes(10, 100);
    t.minutes(BATT_WINDOW, 50);
    int before = t.last;
    t.minute -= 30; t.draw(50);
    check(t.last <= before, "the clock set back: a sample, never a rise"); }

  // RTC bytes from another layout: no read past the window
  { Watch t;
    t.minutes(10, 100);
    t.w.n = 200;
    t.minute++; int v = t.call(90);
    check(v >= 0 && v <= 100, "a garbage count: a figure in range"); }

  if (fails) printf("battery_latch_test: %d failed\n", fails);
  else printf("battery_latch_test: all passed\n");
  return fails;
}
