#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core/shrink.hpp"

#include <doctest.h>
#include <prism/render/draw_list.hpp>

#include <cstdint>
#include <span>
#include <vector>

using player::shrink_rgb888;

namespace {

// A `w`×`h` picture whose left half is red and right half blue, rows padded to `stride` bytes.
std::vector<std::uint8_t> halves(int w, int h, int stride) {
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(stride) * h, 0xEE);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      auto* p = &rgb[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 3];
      p[0] = x < w / 2 ? 255 : 0;
      p[1] = 0;
      p[2] = x < w / 2 ? 0 : 255;
    }
  return rgb;
}

std::uint32_t rgba_at(const prism::render::Image& img, int x, int y) {
  const auto* p = &img.pixels()[(static_cast<std::size_t>(y) * img.width() + x) * 4];
  return std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[2]} << 8 | p[3];
}

}  // namespace

TEST_CASE("a large square picture comes down to max_side, its halves kept") {
  const auto rgb = halves(600, 600, 600 * 3);
  const auto img = shrink_rgb888({rgb, 600, 600, 600 * 3}, 150);
  REQUIRE(img);
  CHECK(img->width() == 150);
  CHECK(img->height() == 150);
  CHECK(rgba_at(*img, 10, 75) == 0xFF0000FFu);
  CHECK(rgba_at(*img, 140, 75) == 0x0000FFFFu);
}

TEST_CASE("a wide picture keeps its aspect ratio, and row padding is skipped") {
  const auto rgb = halves(400, 200, 400 * 3 + 16);
  const auto img = shrink_rgb888({rgb, 400, 200, 400 * 3 + 16}, 100);
  REQUIRE(img);
  CHECK(img->width() == 100);
  CHECK(img->height() == 50);
  CHECK(rgba_at(*img, 99, 49) == 0x0000FFFFu);  // padding bytes (0xEE) never mixed in
}

TEST_CASE("pixels across a block boundary are averaged") {
  const auto rgb = halves(4, 2, 12);
  const auto img = shrink_rgb888({rgb, 4, 2, 12}, 1);
  REQUIRE(img);
  CHECK(img->width() == 1);
  CHECK(rgba_at(*img, 0, 0) == 0x7F007FFFu);  // half red, half blue
}

TEST_CASE("a picture already small enough keeps its size") {
  const auto rgb = halves(80, 60, 240);
  const auto img = shrink_rgb888({rgb, 80, 60, 240}, 150);
  REQUIRE(img);
  CHECK(img->width() == 80);
  CHECK(img->height() == 60);
}

TEST_CASE("a bad size gives null") {
  const auto rgb = halves(10, 10, 30);
  CHECK_FALSE(shrink_rgb888({std::span<const std::uint8_t>{}, 0, 10, 0}, 150));
  CHECK_FALSE(shrink_rgb888({rgb, 10, 10, 30}, 0));
  CHECK_FALSE(shrink_rgb888({rgb, 10, 11, 30}, 150));  // fewer bytes than the size says
}
