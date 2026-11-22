#pragma once
#include "core/storage.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace prism::render {
class Image;
}

namespace player {

// A cover bigger than this is not read at all: either its size is bogus or the picture is one
// nothing on the board can draw anyway.
inline constexpr std::uint32_t kMaxCoverBytes = 4u << 20;  // 4 MiB

// Turns a picture's bytes into an image at most `max_side` px on its longer side; null when it
// cannot (an unsupported format, a corrupt file).
using CoverDecoder =
    std::function<std::shared_ptr<const prism::render::Image>(std::span<const std::byte>, int max_side)>;

// Loads the cover of one track at a time on its own thread, never on the audio or UI path: reading
// a picture off the card and decoding it takes far too long for either.
class CoverLoader {
 public:
  CoverLoader(Storage& storage, CoverDecoder decoder, int max_side);  // starts the thread
  ~CoverLoader();                                                     // stops it, then joins it
  CoverLoader(const CoverLoader&) = delete;
  CoverLoader& operator=(const CoverLoader&) = delete;

  // The track whose cover is wanted. A newer request replaces one not started yet, and asking for
  // the track already loaded or being loaded does nothing.
  void request(std::string path);
  // The cover of `path` once loaded: the image, or null for "loaded, no cover". nullopt: not loaded
  // (yet), so the caller draws its placeholder.
  std::optional<std::shared_ptr<const prism::render::Image>> cover(std::string_view path) const;
  std::uint64_t revision() const;  // bumped each time a load finishes, cover or no cover

 private:
  void run(std::stop_token stop);
  std::shared_ptr<const prism::render::Image> load(std::string_view path);
  // A picture file next to the track, cached per folder: an album's tracks ask once.
  std::shared_ptr<const prism::render::Image> folder_picture(std::string_view track_path);

  Storage& storage_;
  CoverDecoder decoder_;
  int max_side_;
  mutable std::mutex mutex_;
  std::condition_variable_any wake_;
  std::optional<std::string> wanted_;  // asked for, not started yet
  std::string active_;                 // being loaded right now
  std::string loaded_path_;            // what cover() answers for
  std::shared_ptr<const prism::render::Image> loaded_image_;  // null: that track has no cover
  bool loaded_ = false;
  std::uint64_t revision_ = 0;
  // The folder picture of the last track loaded, and which folder it belongs to.
  std::string folder_path_;
  std::shared_ptr<const prism::render::Image> folder_image_;
  bool folder_loaded_ = false;
  std::jthread thread_;
};

}  // namespace player
