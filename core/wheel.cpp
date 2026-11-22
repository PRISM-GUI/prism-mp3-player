#include "core/wheel.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace player {
namespace {

// The turn from `from` to `to`, the short way round: (-180, 180].
double angle_delta(double from, double to) {
  double d = std::fmod(to - from, 360.0);
  if (d > 180.0) d -= 360.0;
  if (d <= -180.0) d += 360.0;
  return d;
}

// Whole steps in `degrees`, toward zero. The epsilon keeps a full turn that atan2 rounds to
// 359.9999° at 24 steps.
int whole_steps(double degrees) {
  constexpr double epsilon = 1e-6;
  return static_cast<int>(degrees / WheelTracker::kStepDegrees + (degrees > 0 ? epsilon : -epsilon));
}

}  // namespace

double WheelTracker::angle_of(core::Point p) const {
  const auto c = ring_.center();
  return std::atan2((p.y - c.y).raw(), (p.x - c.x).raw()) * 180.0 / std::numbers::pi;
}

WheelTracker::Zone WheelTracker::zone_of(core::Point p) const {
  const auto c = ring_.center();
  const float radius = std::min(ring_.extent.w.raw(), ring_.extent.h.raw()) / 2.f;
  const float d = std::hypot((p.x - c.x).raw(), (p.y - c.y).raw());
  if (d > radius) return Zone::Outside;
  return d < radius * kCentreRatio ? Zone::Centre : Zone::Ring;
}

// Screen angles grow clockwise from the right: up is -90°.
Button WheelTracker::ring_button(core::Point p) const {
  const double a = angle_of(p);
  if (a >= -135.0 && a < -45.0) return Button::Menu;
  if (a >= 45.0 && a < 135.0) return Button::PlayPause;
  if (a >= -45.0 && a < 45.0) return Button::Next;
  return Button::Previous;
}

std::vector<WheelEvent> WheelTracker::press(core::Point p) {
  const auto zone = zone_of(p);
  press_.reset();
  if (zone != Zone::Outside) press_ = Press{zone, p, angle_of(p)};
  return {};
}

// Leaving the wheel ends the press, as a finger lifting off a touch wheel: no turning resumes
// on the way back, and a lost release leaves nothing held.
std::vector<WheelEvent> WheelTracker::move(core::Point p) {
  if (press_ && zone_of(p) == Zone::Outside) press_.reset();
  if (!press_ || press_->zone != Zone::Ring) return {};
  const double a = angle_of(p);
  press_->turned += angle_delta(press_->last_angle, a);
  press_->last_angle = a;
  const int steps = whole_steps(press_->turned);
  if (steps == 0) return {};
  press_->turned -= steps * kStepDegrees;
  press_->rotated = true;
  return {Rotate{steps}};
}

std::vector<WheelEvent> WheelTracker::release(core::Point p) {
  const auto pressed = std::exchange(press_, std::nullopt);
  const auto zone = zone_of(p);
  if (!pressed || pressed->rotated || zone == Zone::Outside) return {};
  if (pressed->zone == Zone::Centre)
    return zone == Zone::Centre ? std::vector<WheelEvent>{Button::Select} : std::vector<WheelEvent>{};
  return {ring_button(pressed->at)};
}

}  // namespace player
