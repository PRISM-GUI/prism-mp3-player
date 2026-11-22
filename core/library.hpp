#pragma once
#include "core/storage.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace player {

struct TrackRow {
  std::string path, title, artist, album;  // empty artist/album: unknown (the UI names them)
  int track;
};

enum class View { Artists, Albums, Songs, AlbumsOfArtist, SongsOfAlbum };

// artist/album: the filter where the view needs one. For SongsOfAlbum an empty artist means any
// artist, so an album reached from the Albums view lists every artist's tracks of that name.
struct ViewKey {
  View view;
  std::string artist{}, album{};
};

// A position and record count in the index file.
struct IndexSection {
  std::uint64_t offset = 0;
  std::uint32_t count = 0;
};

// The music index of a card, read on demand: only the header stays in RAM, rows come from the
// file at each call.
class Library {
public:
  // Opens the index at <root>/.prism-library; returns null when it is missing, damaged, or
  // does not match the card (fingerprint differs).
  static std::unique_ptr<Library> open(Storage&, std::string_view root);
  // Scans <root> recursively for .mp3, reads tags, writes the index. `progress(done)` is
  // called every 100 files; returning false stops (the old index stays).
  static bool build(Storage&, std::string_view root, const std::function<bool(int)>& progress);

  std::size_t count(const ViewKey&) const;
  // Rows [first, first + n) of a view: names for Artists/Albums/AlbumsOfArtist, tracks for the
  // song views. A view of the other kind gives no rows.
  std::vector<std::string> names(const ViewKey&, std::size_t first, std::size_t n) const;
  std::vector<TrackRow> tracks(const ViewKey&, std::size_t first, std::size_t n) const;

  struct Layout {
    IndexSection tracks, songs, album_songs, artists, artist_albums, albums, pool;
  };

private:
  Library(std::unique_ptr<File> file, Layout layout) : file_(std::move(file)), layout_(layout) {}

  std::unique_ptr<File> file_;
  Layout layout_;
};

// Over the paths, sizes and mtimes of the .mp3 files under root.
std::uint64_t fingerprint(Storage&, std::string_view root);

}  // namespace player
