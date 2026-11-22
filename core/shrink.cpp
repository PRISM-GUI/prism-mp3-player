#include "core/shrink.hpp"

#include <prism/render/draw_list.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace player {
namespace {

struct Box {
  int x0, x1, y0, y1;  // source pixels [x0, x1) × [y0, y1)
};

// The source pixels that make output pixel (ox, oy): never empty, since the output is no larger.
Box source_box(int ox, int oy, int w, int h, int ow, int oh) {
  return {ox * w / ow, std::max(ox * w / ow + 1, (ox + 1) * w / ow), oy * h / oh,
          std::max(oy * h / oh + 1, (oy + 1) * h / oh)};
}

void average_into(std::uint8_t* out, std::span<const std::uint8_t> rgb, int stride, Box b) {
  std::uint32_t sum[3]{};
  for (int y = b.y0; y < b.y1; ++y)
    for (int x = b.x0; x < b.x1; ++x)
      for (int c = 0; c < 3; ++c) sum[c] += rgb[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 3 + c];
  const auto n = static_cast<std::uint32_t>((b.x1 - b.x0) * (b.y1 - b.y0));
  for (int c = 0; c < 3; ++c) out[c] = static_cast<std::uint8_t>(sum[c] / n);
  out[3] = 255;
}

bool fits(std::span<const std::uint8_t> rgb, int width, int height, int stride) {
  if (width <= 0 || height <= 0 || stride < width * 3) return false;
  return rgb.size() >= static_cast<std::size_t>(stride) * (height - 1) + static_cast<std::size_t>(width) * 3;
}

}  // namespace

std::shared_ptr<const prism::render::Image> shrink_rgb888(std::span<const std::uint8_t> rgb, int width,
                                                          int height, int stride, int max_side) {
  if (max_side <= 0 || !fits(rgb, width, height, stride)) return nullptr;
  const double scale = std::min(1.0, static_cast<double>(max_side) / std::max(width, height));
  const int ow = std::max(1, static_cast<int>(std::lround(width * scale)));
  const int oh = std::max(1, static_cast<int>(std::lround(height * scale)));
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(ow) * oh * 4);
  for (int oy = 0; oy < oh; ++oy)
    for (int ox = 0; ox < ow; ++ox)
      average_into(&rgba[(static_cast<std::size_t>(oy) * ow + ox) * 4], rgb, stride,
                   source_box(ox, oy, width, height, ow, oh));
  auto image = prism::render::Image::from_rgba(ow, oh, std::move(rgba));
  return image ? *image : nullptr;
}

}  // namespace player
