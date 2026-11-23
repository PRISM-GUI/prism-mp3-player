#pragma once
#include <cstdint>
#include <memory>
#include <span>

namespace prism::render {
class Image;
}

namespace player {

// An RGB888 picture with `stride` bytes per row. The three sizes travel together because swapping
// the width and the stride compiles and silently shifts the picture.
struct Rgb888 {
  std::span<const std::uint8_t> bytes;
  int width, height, stride;
};

// Averaged down to at most `max_side` px on its longer side, keeping its aspect ratio; a smaller
// picture keeps its size. Null on a bad size.
std::shared_ptr<const prism::render::Image> shrink_rgb888(Rgb888 picture, int max_side);

}  // namespace player
