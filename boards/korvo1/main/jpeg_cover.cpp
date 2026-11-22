#include "jpeg_cover.hpp"

#include "core/shrink.hpp"

#include <driver/jpeg_decode.h>
#include <esp_heap_caps.h>
#include <prism/render/draw_list.hpp>

#include <cstdint>
#include <cstdio>

namespace player {
namespace {

// simplify: the whole picture is decoded before it is shrunk, so its RGB888 copy must fit in
// PSRAM: 1600×1600 is 7.7 MB. A larger cover shows the placeholder (or the folder's picture).
// Upgrade: decode in strips, or ask the engine for a scaled output where it can.
constexpr std::uint32_t kMaxCoverPixels = 1600u * 1600u;

jpeg_decoder_handle_t engine() {
  static jpeg_decoder_handle_t handle = [] {
    jpeg_decoder_handle_t h = nullptr;
    const jpeg_decode_engine_cfg_t cfg{.intr_priority = 0, .timeout_ms = 200, .flags = {}};
    if (const auto err = jpeg_new_decoder_engine(&cfg, &h); err != ESP_OK)
      std::printf("player: no JPEG engine (%d), covers stay placeholders\n", err);
    return h;
  }();
  return handle;
}

// The engine writes whole MCUs: a picture's rows and columns round up to 16.
std::uint32_t mcu_aligned(std::uint32_t n) { return (n + 15) & ~15u; }

struct DecodeBuffer {
  std::uint8_t* data = nullptr;
  std::size_t size = 0;
  ~DecodeBuffer() { heap_caps_free(data); }
};

}  // namespace

std::shared_ptr<const prism::render::Image> decode_jpeg_cover(std::span<const std::byte> jpeg, int max_side) {
  const auto* bits = reinterpret_cast<const std::uint8_t*>(jpeg.data());
  const auto bytes = static_cast<std::uint32_t>(jpeg.size());
  jpeg_decode_picture_info_t info{};
  if (!engine() || jpeg_decoder_get_info(bits, bytes, &info) != ESP_OK) return nullptr;
  if (info.width == 0 || info.height == 0 || info.width * info.height > kMaxCoverPixels) return nullptr;

  const std::uint32_t stride = mcu_aligned(info.width) * 3;
  const jpeg_decode_memory_alloc_cfg_t mem{.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
  DecodeBuffer out;
  out.data = static_cast<std::uint8_t*>(jpeg_alloc_decoder_mem(stride * mcu_aligned(info.height), &mem, &out.size));
  if (!out.data) return nullptr;

  const jpeg_decode_cfg_t cfg{.output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
                              .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
                              .conv_std = JPEG_YUV_RGB_CONV_STD_BT601};
  std::uint32_t written = 0;
  if (jpeg_decoder_process(engine(), &cfg, bits, bytes, out.data, static_cast<std::uint32_t>(out.size), &written) != ESP_OK)
    return nullptr;
  return shrink_rgb888({out.data, out.size}, static_cast<int>(info.width), static_cast<int>(info.height),
                       static_cast<int>(stride), max_side);
}

}  // namespace player
