// The index file <root>/.prism-library. All integers little-endian.
//
//   header (40 bytes)
//     0  char[4]  magic "PRLB"
//     4  u32      version (1)
//     8  u64      fingerprint of the card (see fingerprint())
//    16  u32      T  track count
//    20  u32      A  artist count
//    24  u32      AA artist-album count
//    28  u32      AL album count
//    32  u32      P  string pool size in bytes
//    36  u32      reserved (0)
//   tracks        T  x 36 B: path, title, artist, album as refs; i32 track number.
//                 In main order: artist, album, track number, title.
//   songs         T  x u32: track indexes by title.
//   album_songs   T  x u32: track indexes by album, then track number, then title.
//   artists       A  x 24 B: name ref; u32 first track, u32 track count (a range of tracks);
//                 u32 first artist-album, u32 artist-album count. By name.
//   artist_albums AA x 16 B: name ref; u32 first track, u32 track count (a range of tracks).
//                 Grouped by artist, by name within one.
//   albums        AL x 16 B: name ref; u32 first, u32 count (a range of album_songs). By name.
//   pool          P bytes of UTF-8, no terminators.
//
// A ref is u32 offset into the pool, u32 size. Sections follow each other without padding, so
// the file size is fixed by the header: a file of any other size is damaged. "By name" is the
// order of sort_key() (core/fold.hpp): case and accents folded, empty names last.
#include "core/library.hpp"

#include "core/fold.hpp"
#include "core/index_format.hpp"

#include <algorithm>
#include <array>
#include <optional>

namespace player {
namespace {

using namespace index_format;

struct StrRef {
  std::uint32_t offset, size;
};

StrRef ref_at(const std::byte* p) { return {get32(p), get32(p + 4)}; }

// Records [begin, begin + count) of a table of `stride`-byte records.
struct Rows {
  IndexSection table;
  std::size_t stride = 0;
  std::uint32_t begin = 0, count = 0;
};

Rows clamped(Rows r) {
  r.begin = std::min(r.begin, r.table.count);
  r.count = std::min(r.count, r.table.count - r.begin);
  return r;
}

Rows window(Rows r, std::size_t first, std::size_t n) {
  r = clamped(r);
  const auto skip = static_cast<std::uint32_t>(std::min<std::size_t>(first, r.count));
  r.begin += skip;
  r.count = static_cast<std::uint32_t>(std::min<std::size_t>(n, r.count - skip));
  return r;
}

std::optional<Header> parse_header(const std::array<std::byte, header_size>& b) {
  if (!std::equal(magic.begin(), magic.end(), b.begin(),
                  [](char c, std::byte x) { return std::byte(c) == x; }) ||
      get32(&b[4]) != version)
    return std::nullopt;
  return Header{get64(&b[8]), get32(&b[16]), get32(&b[20]), get32(&b[24]), get32(&b[28]),
                get32(&b[32])};
}

std::optional<Header> read_header(File& f) {
  std::array<std::byte, header_size> b;
  if (f.read(0, b) != b.size()) return std::nullopt;
  return parse_header(b);
}

// Reads rows from an index whose header was checked; damaged contents give empty strings or
// fewer rows, never a read outside a section.
class IndexReader {
public:
  IndexReader(File& f, const Library::Layout& l) : f_(f), l_(l) {}

  std::vector<std::byte> records(const Rows& r) const {
    std::vector<std::byte> b(r.count * r.stride);
    b.resize(f_.read(r.table.offset + r.begin * r.stride, b));
    return b;
  }

  std::string string(StrRef r) const {
    if (r.offset > l_.pool.count || r.size > l_.pool.count - r.offset) return {};
    std::string s(r.size, '\0');
    const auto n = f_.read(l_.pool.offset + r.offset,
                           std::as_writable_bytes(std::span{s.data(), s.size()}));
    s.resize(n);
    return s;
  }

  std::string name(const Rows& table, std::uint32_t i) const {
    const auto b = records({table.table, table.stride, i, 1});
    return b.size() < ref_size ? std::string{} : string(ref_at(b.data()));
  }

  // The index of the record named `name` among [rows.begin, rows.begin + rows.count), which
  // are sorted by name.
  std::optional<std::uint32_t> find(Rows rows, std::string_view name) const {
    rows = clamped(rows);
    const auto key = sort_key(name);
    std::uint32_t lo = rows.begin, hi = rows.begin + rows.count;
    while (lo < hi) {
      const auto mid = lo + (hi - lo) / 2;
      if (sort_key(this->name(rows, mid)) < key) lo = mid + 1;
      else hi = mid;
    }
    if (lo < rows.begin + rows.count && this->name(rows, lo) == name) return lo;
    return std::nullopt;
  }

  // The range group record i (artist, artist-album or album) points to; none when i is not found.
  Rows group_range(const Rows& table, std::optional<std::uint32_t> i, IndexSection target,
                   std::size_t stride, std::size_t range_at) const {
    if (!i) return {target, stride, 0, 0};
    const auto b = records({table.table, table.stride, *i, 1});
    if (b.size() < range_at + 8) return {target, stride, 0, 0};
    return {target, stride, get32(&b[range_at]), get32(&b[range_at + 4])};
  }

  TrackRow track(std::uint32_t i) const {
    const auto b = records({l_.tracks, track_size, i, 1});
    if (b.size() < track_size) return {};
    return {string(ref_at(&b[0])), string(ref_at(&b[8])), string(ref_at(&b[16])),
            string(ref_at(&b[24])), static_cast<std::int32_t>(get32(&b[32]))};
  }

  std::vector<std::uint32_t> track_indexes(const Rows& r) const {
    std::vector<std::uint32_t> out;
    if (r.stride == track_size) {
      for (std::uint32_t i = 0; i < r.count; ++i) out.push_back(r.begin + i);
      return out;
    }
    const auto b = records(r);
    for (std::size_t at = 0; at + perm_size <= b.size(); at += perm_size) out.push_back(get32(&b[at]));
    return out;
  }

  Rows artists() const { return {l_.artists, artist_size, 0, l_.artists.count}; }
  Rows albums() const { return {l_.albums, group_size, 0, l_.albums.count}; }
  Rows songs() const { return {l_.songs, perm_size, 0, l_.songs.count}; }

  Rows albums_of_artist(std::string_view artist) const {
    return group_range(artists(), find(artists(), artist), l_.artist_albums, group_size,
                       group_size);
  }

  Rows songs_of_album(std::string_view artist, std::string_view album) const {
    if (artist.empty())
      return group_range(albums(), find(albums(), album), l_.album_songs, perm_size, ref_size);
    const auto of_artist = albums_of_artist(artist);
    return group_range(of_artist, find(of_artist, album), l_.tracks, track_size, ref_size);
  }

  std::optional<Rows> name_rows(const ViewKey& k) const {
    switch (k.view) {
      case View::Artists: return artists();
      case View::Albums: return albums();
      case View::AlbumsOfArtist: return albums_of_artist(k.artist);
      default: return std::nullopt;
    }
  }

  std::optional<Rows> track_rows(const ViewKey& k) const {
    switch (k.view) {
      case View::Songs: return songs();
      case View::SongsOfAlbum: return songs_of_album(k.artist, k.album);
      default: return std::nullopt;
    }
  }

private:
  File& f_;
  const Library::Layout& l_;
};

}  // namespace

std::unique_ptr<Library> Library::open(Storage& storage, std::string_view root) {
  auto file = storage.open(join(root, file_name));
  if (!file) return nullptr;
  const auto header = read_header(*file);
  if (!header) return nullptr;
  const auto layout = layout_of(*header);
  if (end_of(layout) != file->size() || header->fingerprint != fingerprint(storage, root))
    return nullptr;
  return std::unique_ptr<Library>(new Library(std::move(file), layout));
}

std::size_t Library::count(const ViewKey& key) const {
  const IndexReader index{*file_, layout_};
  auto rows = index.name_rows(key);
  if (!rows) rows = index.track_rows(key);
  return rows ? clamped(*rows).count : 0;
}

std::vector<std::string> Library::names(const ViewKey& key, std::size_t first,
                                        std::size_t n) const {
  const IndexReader index{*file_, layout_};
  const auto rows = index.name_rows(key);
  if (!rows) return {};
  const auto r = window(*rows, first, n);
  const auto b = index.records(r);
  std::vector<std::string> out;
  for (std::size_t at = 0; at + r.stride <= b.size(); at += r.stride)
    out.push_back(index.string(ref_at(&b[at])));
  return out;
}

std::vector<TrackRow> Library::tracks(const ViewKey& key, std::size_t first,
                                      std::size_t n) const {
  const IndexReader index{*file_, layout_};
  const auto rows = index.track_rows(key);
  if (!rows) return {};
  std::vector<TrackRow> out;
  for (const auto i : index.track_indexes(window(*rows, first, n)))
    if (i < layout_.tracks.count) out.push_back(index.track(i));
  return out;
}

}  // namespace player
