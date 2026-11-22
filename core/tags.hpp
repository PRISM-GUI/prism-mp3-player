#pragma once
#include "core/storage.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace player {

struct Cover {
  std::uint64_t offset = 0;
  std::uint32_t size = 0;  // 0: no cover
  std::string mime;
};

struct Tags {
  std::string title, artist, album;
  int track = 0;
  std::uint64_t audio_offset = 0;  // where the MP3 frames start, after an ID3v2 tag
  Cover cover;
};

// Never throws. Strings are valid UTF-8. A missing title is the file name without its extension.
Tags read_tags(File& file, std::string_view file_name);

}  // namespace player
