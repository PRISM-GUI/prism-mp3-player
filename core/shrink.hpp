#pragma once
#include <cstdint>
#include <memory>
#include <span>

namespace prism::render {
class Image;
}

namespace player {

// An RGB888 picture (`stride` bytes per row) averaged down to at most `max_side` px on its longer
// side, keeping its aspect ratio; a smaller picture keeps its size. Null on a bad size.
std::shared_ptr<const prism::render::Image> shrink_rgb888(std::span<const std::uint8_t> rgb, int width,
                                                          int height, int stride, int max_side);

}  // namespace player
