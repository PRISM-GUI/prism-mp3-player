// Scans a card and writes its index; the byte layout is described at the top of library.cpp.
// Memory: every string goes once into one pool (artist and album names once per distinct
// name), tracks are 32-byte records, and the file is streamed out in two passes (the CRC
// first, then the bytes), so 10,000 tracks peak around 2 MB.
#include "core/fold.hpp"
#include "core/index_format.hpp"
#include "core/library.hpp"
#include "core/path.hpp"
#include "core/tags.hpp"

#include <prism/profile/trace_format.hpp>

#include <algorithm>
#include <map>
#include <numeric>
#include <ostream>
#include <tuple>

namespace player {

// True for a .mp3 name, whatever the case of its extension.
bool is_mp3_name(std::string_view name) {
  return name.size() >= 4 && fold_for_sort(name.substr(name.size() - 4)) == ".mp3";
}

// Folders a card carries for its own use: never music, and sometimes thousands of entries.
bool is_card_system_dir(std::string_view name) {
  const auto folded = fold_for_sort(name);
  return folded == "system volume information" || folded == "$recycle.bin";
}

namespace {

using namespace index_format;

class Pool {
public:
  StrRef add(std::string_view s) {
    const StrRef r{static_cast<std::uint32_t>(bytes_.size()), static_cast<std::uint32_t>(s.size())};
    bytes_ += s;
    return r;
  }
  std::string_view get(StrRef r) const { return std::string_view{bytes_}.substr(r.offset, r.size); }
  const std::string& bytes() const { return bytes_; }

private:
  std::string bytes_;
};

// Distinct names, each stored once in the pool.
class Names {
public:
  std::uint32_t id(Pool& pool, std::string_view name) {
    const auto it = ids_.find(name);
    if (it != ids_.end()) return it->second;
    refs_.push_back(pool.add(name));
    return ids_.emplace(std::string{name}, refs_.size() - 1).first->second;
  }
  StrRef ref(std::uint32_t id) const { return refs_[id]; }
  std::size_t size() const { return refs_.size(); }

  // rank[id]: the name's position in sort_key() order.
  std::vector<std::uint32_t> ranks(const Pool& pool) const {
    std::vector<std::string> keys;
    for (const auto r : refs_) keys.push_back(sort_key(pool.get(r)));
    std::vector<std::uint32_t> by_key(refs_.size());
    std::iota(by_key.begin(), by_key.end(), 0u);
    std::ranges::sort(by_key, {}, [&](std::uint32_t id) -> const std::string& { return keys[id]; });
    std::vector<std::uint32_t> rank(refs_.size());
    for (std::uint32_t k = 0; k < by_key.size(); ++k) rank[by_key[k]] = k;
    return rank;
  }

private:
  std::map<std::string, std::uint32_t, std::less<>> ids_;
  std::vector<StrRef> refs_;
};

// Calls on_file(path, entry) for each .mp3 under dir, in path order. Dot names are skipped: the
// index itself, and the "._x.mp3" files macOS leaves; so are the card's system folders, and paths
// the index could not hold.
template <class OnFile>
void walk(Storage& storage, const std::string& dir, OnFile& on_file) {
  auto entries = storage.list(dir);
  std::ranges::sort(entries, {}, &DirEntry::name);
  for (const auto& e : entries) {
    if (e.name.starts_with('.')) continue;
    if (e.is_dir && is_card_system_dir(e.name)) continue;
    const auto path = join(dir, e.name);
    if (e.is_dir) walk(storage, path, on_file);
    else if (is_mp3_name(e.name) && path.size() <= max_string) on_file(path, e);
  }
}

// FNV-1a, 64-bit: http://www.isthe.com/chongo/tech/comp/fnv/
class Fingerprint {
public:
  void add(std::string_view path, const DirEntry& e) {
    std::string record{path};
    record += '\0';
    put64(record, e.size);
    put64(record, static_cast<std::uint64_t>(e.mtime));
    for (const char c : record) h_ = (h_ ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
  }
  std::uint64_t value() const { return h_; }

private:
  std::uint64_t h_ = 0xcbf29ce484222325ull;
};

struct Track {
  StrRef path, title;
  std::uint32_t artist, album;  // ids in Collected's Names
  std::int32_t number;
  std::uint32_t title_rank = 0;
};

struct Collected {
  Pool pool;
  std::vector<StrRef> paths;
  std::uint64_t fingerprint = 0;
  std::vector<Track> tracks;
  Names artists, albums;
};

Collected scan(Storage& storage, std::string_view root) {
  Collected c;
  Fingerprint fp;
  auto on_file = [&](std::string_view path, const DirEntry& e) {
    fp.add(path, e);
    c.paths.push_back(c.pool.add(path));
  };
  walk(storage, std::string{root}, on_file);
  c.fingerprint = fp.value();
  return c;
}

void read_track(Storage& storage, Collected& c, StrRef path) {
  const auto file = storage.open(c.pool.get(path));
  if (!file) return;
  // read_tags names an untagged file by the name it is handed: the file's own, not its path.
  const auto t = read_tags(*file, file_name(c.pool.get(path)));
  const auto title = c.pool.add(t.title);
  c.tracks.push_back({path, title, c.artists.id(c.pool, t.artist), c.albums.id(c.pool, t.album),
                      t.track});
}

bool read_tracks(Storage& storage, Collected& c, const std::function<bool(int)>& progress) {
  c.tracks.reserve(c.paths.size());
  for (std::size_t i = 0; i < c.paths.size(); ++i) {
    read_track(storage, c, c.paths[i]);
    const auto done = static_cast<int>(i + 1);
    if (done % 100 == 0 && !progress(done)) return false;
  }
  c.paths = {};
  return true;
}

// simplify: titles are folded in the comparator, twice per comparison, instead of keeping a
// key per track: no extra memory, ~300k folds for 10,000 tracks. Keep the keys if the board's
// build is too slow.
void rank_titles(std::vector<Track>& tracks, const Pool& pool) {
  std::vector<std::uint32_t> by_title(tracks.size());
  std::iota(by_title.begin(), by_title.end(), 0u);
  const auto title = [&](std::uint32_t i) { return pool.get(tracks[i].title); };
  std::ranges::sort(by_title, [&](auto a, auto b) { return sort_key(title(a)) < sort_key(title(b)); });
  std::uint32_t rank = 0;
  for (std::size_t k = 0; k < by_title.size(); ++k) {
    if (k > 0 && title(by_title[k]) != title(by_title[k - 1])) ++rank;
    tracks[by_title[k]].title_rank = rank;
  }
}

struct Artist {
  std::uint32_t name, first, count, first_album, album_count;
};

struct Album {
  std::uint32_t name, artist, first, count;
};

// The index in memory while it is built, in the file's terms.
struct Index {
  Collected c;  // tracks in main order
  std::vector<std::uint32_t> songs;
  std::vector<Artist> artists;
  std::vector<Album> albums;  // by artist, then name
  std::vector<std::uint32_t> albums_by_name;
};

// The runs of [begin, end) whose consecutive members are `same`, as (first, count).
template <class Same>
std::vector<std::pair<std::uint32_t, std::uint32_t>> runs(std::uint32_t begin, std::uint32_t end,
                                                          Same same) {
  std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
  for (auto i = begin; i < end; ++i)
    if (i == begin || !same(i - 1, i)) out.emplace_back(i, 1);
    else ++out.back().second;
  return out;
}

template <class Order>
std::vector<std::uint32_t> sorted_indexes(std::size_t n, Order order) {
  std::vector<std::uint32_t> p(n);
  std::iota(p.begin(), p.end(), 0u);
  std::ranges::sort(p, {}, order);
  return p;
}

void group(Index& x) {
  const auto& ts = x.c.tracks;
  const auto n = static_cast<std::uint32_t>(ts.size());
  for (const auto& [first, count] : runs(0, n, [&](auto a, auto b) { return ts[a].artist == ts[b].artist; })) {
    const auto albums = runs(first, first + count, [&](auto a, auto b) { return ts[a].album == ts[b].album; });
    x.artists.push_back({ts[first].artist, first, count, static_cast<std::uint32_t>(x.albums.size()),
                         static_cast<std::uint32_t>(albums.size())});
    for (const auto& [af, ac] : albums) x.albums.push_back({ts[af].album, ts[af].artist, af, ac});
  }
}

Index index_of(Collected c) {
  rank_titles(c.tracks, c.pool);
  const auto artist_rank = c.artists.ranks(c.pool);
  const auto album_rank = c.albums.ranks(c.pool);
  std::ranges::sort(c.tracks, {}, [&](const Track& t) {
    return std::tuple{artist_rank[t.artist], album_rank[t.album], t.number, t.title_rank, t.path.offset};
  });
  Index x{std::move(c), {}, {}, {}, {}};
  const auto& ts = x.c.tracks;
  x.songs = sorted_indexes(ts.size(), [&](std::uint32_t i) {
    return std::tuple{ts[i].title_rank, artist_rank[ts[i].artist], album_rank[ts[i].album], ts[i].number, i};
  });
  group(x);
  x.albums_by_name = sorted_indexes(x.albums.size(), [&](std::uint32_t i) {
    return std::pair{album_rank[x.albums[i].name], artist_rank[x.albums[i].artist]};
  });
  return x;
}

// Where the bytes go: a CRC, and the stream when there is one, through a small buffer.
class Sink {
public:
  explicit Sink(std::ostream* out) : out_(out) {}
  void u32(std::uint32_t v) {
    put32(buf_, v);
    if (buf_.size() >= 4096) flush();
  }
  void ref(StrRef r) {
    u32(r.offset);
    u32(r.size);
  }
  void bytes(std::string_view s) {
    flush();
    feed(s);
  }
  void flush() {
    feed(buf_);
    buf_.clear();
  }
  std::uint32_t crc() const { return crc_ ^ 0xFFFFFFFFu; }

private:
  void feed(std::string_view s) {
    crc_ = prism::profile::crc32_update(crc_, std::as_bytes(std::span{s.data(), s.size()}));
    if (out_) out_->write(s.data(), static_cast<std::streamsize>(s.size()));
  }
  std::ostream* out_;
  std::string buf_;
  std::uint32_t crc_ = 0xFFFFFFFFu;
};

void write_body(Sink& out, const Index& x) {
  const auto& c = x.c;
  for (const auto& t : c.tracks) {
    for (const auto r : {t.path, t.title, c.artists.ref(t.artist), c.albums.ref(t.album)}) out.ref(r);
    out.u32(static_cast<std::uint32_t>(t.number));
  }
  for (const auto i : x.songs) out.u32(i);
  for (const auto& a : x.artists) {
    out.ref(c.artists.ref(a.name));
    for (const auto v : {a.first, a.count, a.first_album, a.album_count}) out.u32(v);
  }
  for (const auto& a : x.albums) {
    out.ref(c.albums.ref(a.name));
    out.ref(c.artists.ref(a.artist));
    out.u32(a.first);
    out.u32(a.count);
  }
  for (const auto i : x.albums_by_name) out.u32(i);
  out.bytes(c.pool.bytes());
  out.flush();
}

std::string header_bytes(const Index& x, std::uint32_t crc) {
  std::string out{magic};
  put32(out, version);
  put64(out, x.c.fingerprint);
  for (const auto count : {x.c.tracks.size(), x.artists.size(), x.albums.size(), x.c.pool.bytes().size()})
    put32(out, static_cast<std::uint32_t>(count));
  put32(out, 0);
  put32(out, crc);
  return out;
}

bool write_index(std::ostream& out, const Index& x) {
  Sink crc_only{nullptr};
  write_body(crc_only, x);
  const auto header = header_bytes(x, crc_only.crc());
  out.write(header.data(), static_cast<std::streamsize>(header.size()));
  Sink file{&out};
  write_body(file, x);
  out.flush();
  return out.good();
}

std::string staged_path(std::string_view root) { return join(root, index_file_name) + ".new"; }

bool write_index_file(Storage& storage, std::string_view path, const Index& x) {
  auto out = storage.create(path);
  return out && write_index(*out, x);
}

}  // namespace

std::uint64_t fingerprint(Storage& storage, std::string_view root) {
  Fingerprint fp;
  auto on_file = [&](std::string_view path, const DirEntry& e) { fp.add(path, e); };
  walk(storage, std::string{root}, on_file);
  return fp.value();
}

bool Library::build(Storage& storage, std::string_view root,
                    const std::function<bool(int)>& progress, Install install) {
  auto collected = scan(storage, root);
  if (!read_tracks(storage, collected, progress)) return false;
  if (!write_index_file(storage, staged_path(root), index_of(std::move(collected)))) return false;
  return install == Install::staged || Library::install(storage, root);
}

bool Library::install(Storage& storage, std::string_view root) {
  const auto staged = staged_path(root);
  if (!storage.open(staged)) return false;
  return storage.rename(staged, join(root, index_file_name));
}

}  // namespace player
