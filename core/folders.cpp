#include "core/folders.hpp"

#include "core/fold.hpp"
#include "core/path.hpp"
#include "core/tags.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace player {
namespace {

// simplify: Select in a song view (Songs, an album) of at most 500 songs queues the whole view; in a
// larger one, a window of 500 songs, up to 250 of them before the selected one (10,000 TrackRows
// would take MBs of RAM on the board). Upgrade: a Player queue that pages its tracks from the
// Library by view and row.
constexpr std::size_t kQueueSongs = 500;
constexpr std::size_t kQueueBefore = kQueueSongs / 2;
// simplify: a folder reads the tags of the 32 songs around the selection, not of the 500 it can
// queue: a 2,000-song folder would otherwise open 2,000 files before anything plays. The rest are
// named by their file. Upgrade: read a track's tags when it starts, or take the rows from the index
// when the folder is in it.
constexpr std::size_t kTagWindow = 32;

// The file's name without its folder and its extension: what a row says when its tags were not read.
std::string file_stem(std::string_view path) {
  const auto name = file_name(path);
  const auto dot = name.find_last_of('.');
  return std::string{dot == std::string_view::npos ? name : name.substr(0, dot)};
}

TrackRow row_from_file(std::string path) {
  auto title = file_stem(path);
  return {std::move(path), std::move(title), {}, {}, 0};
}

// The folder's own .mp3 files, in listing order, and where the selected row sits among them.
struct Files {
  std::vector<std::string> paths;
  std::size_t selected = 0;
};

Files files_of(std::string_view dir, std::span<const FolderRow> rows, std::size_t row) {
  Files files;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].folder) continue;
    if (i == row) files.selected = files.paths.size();
    files.paths.push_back(join(dir, rows[i].label));
  }
  return files;
}

// The queued rows: tags for the files around the selection, the file's own name for the rest. A
// file that cannot be opened keeps its place in the queue, so the player names the song that failed
// instead of the queue shifting under the selection.
std::vector<TrackRow> queued_tracks(Storage& storage, const Files& files, Window window) {
  const auto tagged_first = files.selected - std::min(files.selected, kTagWindow / 2);
  const auto tagged_last = std::min(tagged_first + kTagWindow, files.paths.size());
  std::vector<TrackRow> tracks;
  tracks.reserve(window.last - window.first);
  for (auto i = window.first; i < window.last; ++i) {
    const auto& path = files.paths[i];
    if (i < tagged_first || i >= tagged_last) {
      tracks.push_back(row_from_file(path));
      continue;
    }
    auto file = storage.open(path);
    if (!file) {
      tracks.push_back(row_from_file(path));
      continue;
    }
    const auto tags = read_tags(*file, file_name(path));
    tracks.push_back({path, tags.title, tags.artist, tags.album, tags.track});
  }
  return tracks;
}

}  // namespace

Window queue_window(std::size_t count, std::size_t row) {
  if (count <= kQueueSongs) return {0, count};
  const auto first = row - std::min(row, kQueueBefore);
  return {first, std::min(first + kQueueSongs, count)};
}

std::vector<FolderRow> folder_rows(Storage& storage, std::string_view dir) {
  struct Entry {
    std::string name;  // what the row shows: a folder keeps its trailing '/'
    std::string key;   // the folded name, made once: sorting thousands folds nothing twice
    bool is_dir;
  };
  std::vector<Entry> entries;
  for (const auto& e : storage.list(dir)) {
    if (e.name.starts_with('.')) continue;
    if (e.is_dir && is_card_system_dir(e.name)) continue;
    if (!e.is_dir && !is_mp3_name(e.name)) continue;
    entries.push_back({e.is_dir ? e.name + "/" : e.name, fold_for_sort(e.name), e.is_dir});
  }
  std::ranges::stable_sort(entries, [](const Entry& a, const Entry& b) {
    if (a.is_dir != b.is_dir) return a.is_dir;
    return a.key < b.key;
  });
  std::vector<FolderRow> rows;
  rows.reserve(entries.size());
  for (auto& e : entries) rows.push_back({std::move(e.name), {}, e.is_dir});
  return rows;
}

std::optional<FolderQueue> folder_queue(Storage& storage, std::string_view dir,
                                        std::span<const FolderRow> rows, std::size_t row) {
  if (row >= rows.size() || rows[row].folder) return std::nullopt;
  const auto files = files_of(dir, rows, row);
  const auto window = queue_window(files.paths.size(), files.selected);
  if (files.selected < window.first || files.selected >= window.last) return std::nullopt;
  return FolderQueue{queued_tracks(storage, files, window), files.selected - window.first};
}

}  // namespace player
