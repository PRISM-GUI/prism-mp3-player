// Scans a card and writes its index; the byte layout is described at the top of library.cpp.
#include "core/fold.hpp"
#include "core/index_format.hpp"
#include "core/library.hpp"
#include "core/tags.hpp"

#include <algorithm>
#include <array>
#include <numeric>
#include <optional>
#include <ostream>
#include <tuple>

namespace player {
namespace {

using namespace index_format;

struct FileInfo {
  std::string path;
  std::uint64_t size;
  std::int64_t mtime;
};

bool is_mp3(std::string_view name) {
  if (name.size() < 4) return false;
  const auto ext = fold_for_sort(name.substr(name.size() - 4));
  return ext == ".mp3";
}

// Dot names are skipped: the index itself, and the "._x.mp3" metadata files macOS leaves.
void scan_into(Storage& storage, const std::string& dir, std::vector<FileInfo>& out) {
  auto entries = storage.list(dir);
  std::ranges::sort(entries, {}, &DirEntry::name);
  for (const auto& e : entries) {
    if (e.name.starts_with('.')) continue;
    if (e.is_dir) scan_into(storage, join(dir, e.name), out);
    else if (is_mp3(e.name)) out.push_back({join(dir, e.name), e.size, e.mtime});
  }
}

std::vector<FileInfo> scan(Storage& storage, std::string_view root) {
  std::vector<FileInfo> out;
  scan_into(storage, std::string{root}, out);
  return out;
}

// FNV-1a, 64-bit: http://www.isthe.com/chongo/tech/comp/fnv/
std::uint64_t fnv1a(std::uint64_t h, std::string_view bytes) {
  for (const char c : bytes) h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
  return h;
}

std::uint64_t fingerprint_of(const std::vector<FileInfo>& files) {
  std::uint64_t h = 0xcbf29ce484222325ull;
  for (const auto& f : files) {
    std::string record = f.path;
    record += '\0';
    put64(record, f.size);
    put64(record, static_cast<std::uint64_t>(f.mtime));
    h = fnv1a(h, record);
  }
  return h;
}

struct Entry {
  TrackRow row;
  std::string artist_key, album_key, title_key;
};

std::string_view file_name_of(std::string_view path) { return path.substr(path.rfind('/') + 1); }

std::optional<Entry> read_entry(Storage& storage, const FileInfo& info) {
  const auto file = storage.open(info.path);
  if (!file) return std::nullopt;
  auto t = read_tags(*file, file_name_of(info.path));
  Entry e{{info.path, std::move(t.title), std::move(t.artist), std::move(t.album), t.track}, {}, {}, {}};
  e.artist_key = sort_key(e.row.artist);
  e.album_key = sort_key(e.row.album);
  e.title_key = sort_key(e.row.title);
  return e;
}

std::optional<std::vector<Entry>> read_entries(Storage& storage, const std::vector<FileInfo>& files,
                                               const std::function<bool(int)>& progress) {
  std::vector<Entry> out;
  out.reserve(files.size());
  for (std::size_t i = 0; i < files.size(); ++i) {
    if (auto e = read_entry(storage, files[i])) out.push_back(*std::move(e));
    const auto done = static_cast<int>(i + 1);
    if (done % 100 == 0 && !progress(done)) return std::nullopt;
  }
  return out;
}

auto main_order(const Entry& e) {
  return std::tie(e.artist_key, e.album_key, e.row.track, e.title_key, e.row.path);
}
auto title_order(const Entry& e) {
  return std::tie(e.title_key, e.artist_key, e.album_key, e.row.track, e.row.path);
}
auto album_order(const Entry& e) {
  return std::tie(e.album_key, e.row.track, e.title_key, e.artist_key, e.row.path);
}

template <class Order>
std::vector<std::uint32_t> permutation(const std::vector<Entry>& entries, Order order) {
  std::vector<std::uint32_t> p(entries.size());
  std::iota(p.begin(), p.end(), 0u);
  std::ranges::sort(p, {}, [&](std::uint32_t i) { return order(entries[i]); });
  return p;
}

struct Group {
  std::uint32_t first, count;
};

// The runs of [begin, end) whose consecutive members are `same`.
template <class Same>
std::vector<Group> runs(std::uint32_t begin, std::uint32_t end, Same same) {
  std::vector<Group> out;
  for (auto i = begin; i < end; ++i)
    if (i == begin || !same(i - 1, i)) out.push_back({i, 1});
    else ++out.back().count;
  return out;
}

struct Pool {
  std::string bytes;
  std::vector<std::array<std::uint32_t, 8>> refs;  // per track: path, title, artist, album
};

Pool pool_of(const std::vector<Entry>& entries) {
  Pool p;
  for (const auto& e : entries) {
    auto& r = p.refs.emplace_back();
    const std::string_view fields[] = {e.row.path, e.row.title, e.row.artist, e.row.album};
    for (std::size_t f = 0; f < 4; ++f) {
      r[2 * f] = static_cast<std::uint32_t>(p.bytes.size());
      r[2 * f + 1] = static_cast<std::uint32_t>(fields[f].size());
      p.bytes += fields[f];
    }
  }
  return p;
}

enum Field { path_field, title_field, artist_field, album_field };

void put_ref(std::string& out, const Pool& pool, std::uint32_t track, Field f) {
  put32(out, pool.refs[track][2 * f]);
  put32(out, pool.refs[track][2 * f + 1]);
}

// The index in memory while it is built, in the file's terms.
struct Index {
  std::vector<Entry> entries;  // main order
  std::vector<std::uint32_t> songs, album_songs;
  std::vector<Group> artists, artist_albums, albums;
  std::vector<Group> albums_per_artist;  // ranges of artist_albums, one per artist
  Pool pool;
};

Index index_of(std::vector<Entry> entries) {
  Index x;
  std::ranges::sort(entries, {}, main_order);
  x.entries = std::move(entries);
  const auto& es = x.entries;
  const auto n = static_cast<std::uint32_t>(es.size());
  x.songs = permutation(es, title_order);
  x.album_songs = permutation(es, album_order);
  x.artists = runs(0, n, [&](auto a, auto b) { return es[a].row.artist == es[b].row.artist; });
  for (const auto& artist : x.artists) {
    const auto albums = runs(artist.first, artist.first + artist.count,
                             [&](auto a, auto b) { return es[a].row.album == es[b].row.album; });
    x.albums_per_artist.push_back({static_cast<std::uint32_t>(x.artist_albums.size()),
                                   static_cast<std::uint32_t>(albums.size())});
    x.artist_albums.insert(x.artist_albums.end(), albums.begin(), albums.end());
  }
  x.albums = runs(0, n, [&](auto a, auto b) {
    return es[x.album_songs[a]].row.album == es[x.album_songs[b]].row.album;
  });
  x.pool = pool_of(es);
  return x;
}

std::string header_bytes(const Index& x, std::uint64_t fingerprint) {
  std::string out{magic};
  put32(out, version);
  put64(out, fingerprint);
  for (const auto count : {x.entries.size(), x.artists.size(), x.artist_albums.size(),
                           x.albums.size(), x.pool.bytes.size()})
    put32(out, static_cast<std::uint32_t>(count));
  put32(out, 0);
  return out;
}

std::string tracks_bytes(const Index& x) {
  std::string out;
  for (std::uint32_t i = 0; i < x.entries.size(); ++i) {
    for (const auto f : {path_field, title_field, artist_field, album_field})
      put_ref(out, x.pool, i, f);
    put32(out, static_cast<std::uint32_t>(x.entries[i].row.track));
  }
  return out;
}

std::string perm_bytes(const std::vector<std::uint32_t>& p) {
  std::string out;
  for (const auto i : p) put32(out, i);
  return out;
}

// `name_of(group)` is the track whose field names the group.
template <class NameOf>
std::string groups_bytes(const Index& x, const std::vector<Group>& groups, Field field,
                         NameOf name_of) {
  std::string out;
  for (const auto& g : groups) {
    put_ref(out, x.pool, name_of(g), field);
    put32(out, g.first);
    put32(out, g.count);
  }
  return out;
}

std::string artists_bytes(const Index& x) {
  std::string out;
  for (std::size_t a = 0; a < x.artists.size(); ++a) {
    put_ref(out, x.pool, x.artists[a].first, artist_field);
    put32(out, x.artists[a].first);
    put32(out, x.artists[a].count);
    put32(out, x.albums_per_artist[a].first);
    put32(out, x.albums_per_artist[a].count);
  }
  return out;
}

bool write_index(std::ostream& out, const Index& x, std::uint64_t fingerprint) {
  const auto first_track = [](const Group& g) { return g.first; };
  const auto first_album_song = [&](const Group& g) { return x.album_songs[g.first]; };
  const auto put = [&](std::string_view section) {
    out.write(section.data(), static_cast<std::streamsize>(section.size()));
  };
  put(header_bytes(x, fingerprint));
  put(tracks_bytes(x));
  put(perm_bytes(x.songs));
  put(perm_bytes(x.album_songs));
  put(artists_bytes(x));
  put(groups_bytes(x, x.artist_albums, album_field, first_track));
  put(groups_bytes(x, x.albums, album_field, first_album_song));
  put(x.pool.bytes);
  out.flush();
  return out.good();
}

bool replace_index(Storage& storage, std::string_view root, const Index& x,
                   std::uint64_t fingerprint) {
  const auto path = join(root, file_name);
  const auto temp = path + ".new";
  auto out = storage.create(temp);
  if (!out || !write_index(*out, x, fingerprint)) return false;
  out.reset();
  return storage.rename(temp, path);
}

}  // namespace

std::uint64_t fingerprint(Storage& storage, std::string_view root) {
  return fingerprint_of(scan(storage, root));
}

bool Library::build(Storage& storage, std::string_view root,
                    const std::function<bool(int)>& progress) {
  const auto files = scan(storage, root);
  auto entries = read_entries(storage, files, progress);
  if (!entries) return false;
  return replace_index(storage, root, index_of(*std::move(entries)), fingerprint_of(files));
}

}  // namespace player
