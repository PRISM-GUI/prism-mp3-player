#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core/wheel.hpp"

#include <doctest.h>

#include <cmath>
#include <numbers>

using namespace player;
using prism::core::Height;
using prism::core::Point;
using prism::core::Rect;
using prism::core::Size;
using prism::core::Width;
using prism::core::X;
using prism::core::Y;

namespace {

const Rect ring{Point{X{0}, Y{0}}, Size{Width{200}, Height{200}}};
const Point centre{X{100}, Y{100}};

// On the ring, `degrees` clockwise from the right (screen coordinates: y grows down).
Point on_ring(double degrees, float radius = 70.f) {
  const double r = degrees * std::numbers::pi / 180.0;
  return {X{100.f + radius * static_cast<float>(std::cos(r))}, Y{100.f + radius * static_cast<float>(std::sin(r))}};
}

int steps_of(const std::vector<WheelEvent>& events) {
  int n = 0;
  for (const auto& e : events)
    if (const auto* r = std::get_if<Rotate>(&e)) n += r->steps;
  return n;
}

// Presses at 0°, turns by `degrees` in 10° moves, releases; the steps given out.
int turn(WheelTracker& t, int degrees) {
  t.press(on_ring(0));
  int steps = 0;
  const int dir = degrees > 0 ? 1 : -1;
  for (int a = 10; a <= std::abs(degrees); a += 10) steps += steps_of(t.move(on_ring(dir * a)));
  CHECK(t.release(on_ring(degrees)).empty());
  return steps;
}

std::vector<WheelEvent> tap(WheelTracker& t, Point p) {
  CHECK(t.press(p).empty());
  return t.release(p);
}

using Events = std::vector<WheelEvent>;

}  // namespace

TEST_CASE("a full clockwise turn is 24 steps") {
  WheelTracker t{ring};
  CHECK(turn(t, 360) == 24);
  CHECK(turn(t, 90) == 6);
}

TEST_CASE("counter-clockwise is negative") {
  WheelTracker t{ring};
  CHECK(turn(t, -360) == -24);
  CHECK(turn(t, -30) == -2);
}

TEST_CASE("a tap in the centre is Select; on the top of the ring, Menu") {
  WheelTracker t{ring};
  CHECK(tap(t, centre) == Events{Button::Select});
  CHECK(tap(t, on_ring(-90)) == Events{Button::Menu});
  CHECK(tap(t, on_ring(90)) == Events{Button::PlayPause});
  CHECK(tap(t, on_ring(180)) == Events{Button::Previous});
  CHECK(tap(t, on_ring(0)) == Events{Button::Next});
  CHECK(tap(t, Point{X{1}, Y{1}}).empty());  // a corner, outside the ring

  SUBCASE("a move of less than a step is still a tap") {
    t.press(on_ring(-90));
    CHECK(t.move(on_ring(-85)).empty());
    CHECK(t.release(on_ring(-85)) == Events{Button::Menu});
  }
}

TEST_CASE("a drag that starts in the centre does not rotate") {
  WheelTracker t{ring};
  t.press(centre);
  for (int a = 0; a <= 360; a += 10) CHECK(t.move(on_ring(a)).empty());
  CHECK(t.release(on_ring(360)).empty());
}

TEST_CASE("a ring press released outside the wheel is no tap") {
  WheelTracker t{ring};
  t.press(on_ring(-90));
  CHECK(t.release(Point{X{300}, Y{100}}).empty());
}

TEST_CASE("a drag released outside the wheel ends: later moves do not rotate") {
  WheelTracker t{ring};
  t.press(on_ring(0));
  CHECK(steps_of(t.move(on_ring(30, 140))) == 2);  // dragged off the ring, still turning
  CHECK(t.release(Point{X{300}, Y{300}}).empty());
  CHECK(t.move(on_ring(90)).empty());
}
