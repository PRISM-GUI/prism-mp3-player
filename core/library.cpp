// The index file <root>/.prism-library. All integers little-endian.
//
//   header (40 bytes)
//     0  char[4]  magic "PRLB"
//     4  u32      version (2)
//     8  u64      fingerprint of the card when built (see fingerprint())
//    16  u32      T  track count
//    20  u32      A  artist count
//    24  u32      AL album count (an album is an artist's: one per artist and album name)
//    28  u32      P  string pool size in bytes
//    32  u32      reserved (0)
//    36  u32      CRC-32 (IEEE) of every byte after the header
//   tracks        T  x 36 B: path, title, artist, album as refs; i32 track number.
//                 In main order: artist, album, track number, title.
//   songs         T  x u32: track indexes by title.
//   artists       A  x 24 B: name ref; u32 first track, u32 track count (a range of tracks);
//                 u32 first album, u32 album count (a range of artist_albums). By name.
//   artist_albums AL x 24 B: album name ref, artist name ref; u32 first track, u32 track count
//                 (a range of tracks). In main order: by artist, then album name.
//   albums        AL x u32: artist_albums indexes by album name, then artist name.
//   pool          P bytes of UTF-8, no terminators.
//
// A ref is u32 offset into the pool, u32 size. Sections follow each other without padding, so
// the file size is fixed by the header: a file of any other size is damaged, and so is one
// whose CRC differs. "By name" is the order of sort_key() (core/fold.hpp): case and accents
// folded, empty names (unknown artist or album) last.
#include "core/library.hpp"

#include "core/fold.hpp"
#include "core/index_format.hpp"

#include <prism/profile/trace_format.hpp>

#include <algorithm>
#include <array>
#include <optional>

namespace player {
namespace {

using namespace index_format;

// Rows [begin, begin + count) of a view: records of `table`, either directly or through a u32
// permutation `order` over it.
struct Rows {
  IndexSection table;
  std::size_t stride = 0;
  std::optional<IndexSection> order;
  std::uint32_t begin = 0, count = 0;
};

Rows clamped(Rows r) {
  const auto bound = r.order ? r.order->count : r.table.count;
  r.begin = std::min(r.begin, bound);
  r.count = std::min(r.count, bound - r.begin);
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
                get32(&b[36])};
}

std::optional<Header> read_header(File& f) {
  std::array<std::byte, header_size> b;
  if (f.read(0, b) != b.size()) return std::nullopt;
  return parse_header(b);
}

// Streams the body through a small buffer, so checking costs no RAM whatever the index size.
std::uint32_t body_crc(File& f) {
  std::array<std::byte, 4096> chunk;
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::uint64_t at = header_size;;) {
    const auto n = f.read(at, chunk);
    if (n == 0) break;
    crc = prism::profile::crc32_update(crc, std::span{chunk}.first(n));
    at += n;
  }
  return crc ^ 0xFFFFFFFFu;
}

// Reads rows from an index whose header was checked; contents that still do not make sense
// give empty strings or fewer rows, never a read outside a section.
class IndexReader {
public:
  IndexReader(File& f, const Library::Layout& l) : f_(f), l_(l) {}

  std::vector<std::byte> record(IndexSection table, std::size_t stride, std::uint32_t i) const {
    if (i >= table.count) return {};
    std::vector<std::byte> b(stride);
    b.resize(f_.read(table.offset + std::uint64_t{i} * stride, b));
    return b;
  }

  std::string string(StrRef r) const {
    if (r.size > max_string || r.offset > l_.pool.count || r.size > l_.pool.count - r.offset)
      return {};
    std::string s(r.size, '\0');
    s.resize(f_.read(l_.pool.offset + r.offset,
                     std::as_writable_bytes(std::span{s.data(), s.size()})));
    return s;
  }

  // The record index of row k of `rows` (not clamped: k must be in range).
  std::optional<std::uint32_t> index_at(const Rows& rows, std::uint32_t k) const {
    if (!rows.order) return k;
    const auto b = record(*rows.order, perm_size, k);
    if (b.size() < perm_size) return std::nullopt;
    return get32(b.data());
  }

  std::vector<std::uint32_t> indexes(const Rows& rows) const {
    std::vector<std::uint32_t> out;
    for (std::uint32_t k = rows.begin; k < rows.begin + rows.count; ++k)
      if (const auto i = index_at(rows, k)) out.push_back(*i);
    return out;
  }

  // The string a ref at `at` in record i of the table holds.
  std::string field(const Rows& rows, std::uint32_t i, std::size_t at) const {
    const auto b = record(rows.table, rows.stride, i);
    return b.size() < at + ref_size ? std::string{} : string(ref_at(&b[at]));
  }

  // The row of `rows` named `name`; rows are sorted by name.
  std::optional<std::uint32_t> find(Rows rows, std::string_view name) const {
    rows = clamped(rows);
    const auto key = sort_key(name);
    std::uint32_t lo = rows.begin, hi = rows.begin + rows.count;
    while (lo < hi) {
      const auto mid = lo + (hi - lo) / 2;
      if (sort_key(field(rows, mid, 0)) < key) lo = mid + 1;
      else hi = mid;
    }
    if (lo < rows.begin + rows.count && field(rows, lo, 0) == name) return lo;
    return std::nullopt;
  }

  // The range record i of `rows` points to (a u32 pair at `at`); none when i is not found.
  Rows range(const Rows& rows, std::optional<std::uint32_t> i, IndexSection target,
             std::size_t stride, std::size_t at) const {
    if (!i) return {target, stride, std::nullopt, 0, 0};
    const auto b = record(rows.table, rows.stride, *i);
    if (b.size() < at + 8) return {target, stride, std::nullopt, 0, 0};
    return {target, stride, std::nullopt, get32(&b[at]), get32(&b[at + 4])};
  }

  TrackRow track(std::uint32_t i) const {
    const auto b = record(l_.tracks, track_size, i);
    if (b.size() < track_size) return {};
    return {string(ref_at(&b[0])), string(ref_at(&b[8])), string(ref_at(&b[16])),
            string(ref_at(&b[24])), static_cast<std::int32_t>(get32(&b[32]))};
  }

  Rows artists() const { return {l_.artists, artist_size, std::nullopt, 0, l_.artists.count}; }
  Rows albums() const {
    return {l_.artist_albums, album_size, l_.albums, 0, l_.albums.count};
  }
  Rows songs() const { return {l_.tracks, track_size, l_.songs, 0, l_.songs.count}; }

  Rows albums_of_artist(std::string_view artist) const {
    return range(artists(), find(artists(), artist), l_.artist_albums, album_size, 16);
  }

  Rows songs_of_album(std::string_view artist, std::string_view album) const {
    const auto of_artist = albums_of_artist(artist);
    return range(of_artist, find(of_artist, album), l_.tracks, track_size, 16);
  }

  std::optional<Rows> album_rows(const ViewKey& k) const {
    switch (k.view) {
      case View::Albums: return albums();
      case View::AlbumsOfArtist: return albums_of_artist(k.artist);
      default: return std::nullopt;
    }
  }

  std::optional<Rows> name_rows(const ViewKey& k) const {
    if (k.view == View::Artists) return artists();
    return album_rows(k);
  }

  std::optional<Rows> track_rows(const ViewKey& k) const {
    switch (k.view) {
      case View::Songs: return songs();
      case View::SongsOfAlbum: return songs_of_album(k.artist, k.album);
      default: return std::nullopt;
    }
  }

  // The strings at `at` in the records of rows [first, first + n).
  std::vector<std::string> fields(const std::optional<Rows>& rows, std::size_t first,
                                  std::size_t n, std::size_t at) const {
    if (!rows) return {};
    const auto w = window(*rows, first, n);
    std::vector<std::string> out;
    for (const auto i : indexes(w)) out.push_back(field(w, i, at));
    return out;
  }

private:
  File& f_;
  const Library::Layout& l_;
};

}  // namespace

std::unique_ptr<Library> Library::open(Storage& storage, std::string_view root) {
  auto file = storage.open(join(root, index_file_name));
  if (!file) return nullptr;
  const auto header = read_header(*file);
  if (!header) return nullptr;
  const auto layout = layout_of(*header);
  if (end_of(layout) != file->size() || body_crc(*file) != header->crc) return nullptr;
  return std::unique_ptr<Library>(new Library(std::move(file), layout, header->fingerprint));
}

bool Library::is_current(Storage& storage, std::string_view root) const {
  return fingerprint(storage, root) == fingerprint_;
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
  return index.fields(index.name_rows(key), first, n, 0);
}

std::vector<std::string> Library::album_artists(const ViewKey& key, std::size_t first,
                                                std::size_t n) const {
  const IndexReader index{*file_, layout_};
  return index.fields(index.album_rows(key), first, n, ref_size);
}

std::vector<TrackRow> Library::tracks(const ViewKey& key, std::size_t first,
                                      std::size_t n) const {
  const IndexReader index{*file_, layout_};
  const auto rows = index.track_rows(key);
  if (!rows) return {};
  std::vector<TrackRow> out;
  for (const auto i : index.indexes(window(*rows, first, n))) out.push_back(index.track(i));
  return out;
}

}  // namespace player
