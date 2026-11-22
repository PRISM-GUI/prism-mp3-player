#pragma once
#include <cstddef>
#include <memory>
#include <span>

namespace prism::render {
class Image;
}

namespace player {

// A cover JPEG decoded by the chip's JPEG engine, then shrunk to `max_side`. Null for what the
// engine refuses (progressive JPEG, PNG, a picture over kMaxCoverPixels). Call from one thread.
std::shared_ptr<const prism::render::Image> decode_jpeg_cover(std::span<const std::byte> jpeg, int max_side);

}  // namespace player
