#pragma once
#include <prism/core/types.hpp>

#include <optional>
#include <variant>
#include <vector>

namespace player {

namespace core = prism::core;

enum class Button { Menu, PlayPause, Previous, Next, Select };
struct Rotate {
  int steps;  // + clockwise
  bool operator==(const Rotate&) const = default;
};
using WheelEvent = std::variant<Rotate, Button>;

// A finger or mouse on a drawn ring: angle in, events out. 24 steps per turn (the iPod's feel:
// one list row per 15°). Taps: centre = Select; top/bottom/left/right of the ring = Menu,
// PlayPause, Previous, Next. A press that turns the wheel by a step is not a tap, nor one
// released outside the wheel.
class WheelTracker {
public:
  static constexpr double kStepDegrees = 15.0;
  static constexpr float kCentreRatio = 0.35f;  // the centre button's radius over the ring's

  explicit WheelTracker(core::Rect ring) : ring_(ring) {}

  std::vector<WheelEvent> press(core::Point);
  std::vector<WheelEvent> move(core::Point);
  std::vector<WheelEvent> release(core::Point);
  core::Rect ring() const { return ring_; }

  enum class Zone { Outside, Centre, Ring };
  Zone zone_of(core::Point) const;

private:
  struct Press {
    Zone zone;
    core::Point at;
    double last_angle;  // degrees, clockwise on screen
    double turned = 0;  // degrees not yet given out as steps
    bool rotated = false;
  };

  double angle_of(core::Point) const;
  Button ring_button(core::Point) const;

  core::Rect ring_;
  std::optional<Press> press_;
};

}  // namespace player
