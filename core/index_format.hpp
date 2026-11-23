#pragma once
// The index file's constants and little-endian helpers, shared by the writer (library_build.cpp)
// and the reader (library.cpp). The byte layout is described at the top of library.cpp.
#include "core/library.hpp"
#include "core/path.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace player::index_format {

constexpr std::string_view magic = "PRLB";
constexpr std::uint32_t version = 2;
constexpr std::string_view index_file_name = ".prism-library";

constexpr std::size_t header_size = 40;
constexpr std::size_t ref_size = 8;
constexpr std::size_t track_size = 4 * ref_size + 4;
constexpr std::size_t perm_size = 4;
constexpr std::size_t artist_size = ref_size + 16;
constexpr std::size_t album_size = 2 * ref_size + 8;
// Every string in the index is shorter: tag fields are capped at 256 bytes, and the scan skips
// longer paths.
constexpr std::size_t max_string = 1024;

struct StrRef {
  std::uint32_t offset = 0, size = 0;
};

struct Header {
  std::uint64_t fingerprint = 0;
  std::uint32_t tracks = 0, artists = 0, albums = 0, pool_bytes = 0, crc = 0;
};

inline Library::Layout layout_of(const Header& h) {
  Library::Layout l;
  std::uint64_t at = header_size;
  auto next = [&](std::uint32_t count, std::size_t record_size) {
    const IndexSection s{at, count};
    at += std::uint64_t{count} * record_size;
    return s;
  };
  l.tracks = next(h.tracks, track_size);
  l.songs = next(h.tracks, perm_size);
  l.artists = next(h.artists, artist_size);
  l.artist_albums = next(h.albums, album_size);
  l.albums = next(h.albums, perm_size);
  l.pool = next(h.pool_bytes, 1);
  return l;
}

inline std::uint64_t end_of(const Library::Layout& l) { return l.pool.offset + l.pool.count; }

inline void put32(std::string& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xff);
}

inline void put64(std::string& out, std::uint64_t v) {
  put32(out, static_cast<std::uint32_t>(v));
  put32(out, static_cast<std::uint32_t>(v >> 32));
}

inline std::uint32_t get32(const std::byte* p) {
  return std::to_integer<std::uint32_t>(p[0]) | std::to_integer<std::uint32_t>(p[1]) << 8 |
         std::to_integer<std::uint32_t>(p[2]) << 16 | std::to_integer<std::uint32_t>(p[3]) << 24;
}

inline std::uint64_t get64(const std::byte* p) {
  return get32(p) | std::uint64_t{get32(p + 4)} << 32;
}

inline StrRef ref_at(const std::byte* p) { return {get32(p), get32(p + 4)}; }

}  // namespace player::index_format
