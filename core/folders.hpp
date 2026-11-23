#pragma once
#include "core/library.hpp"
#include "core/storage.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The folder browser's rules, kept out of PlayerUi: what a folder holds, and what a Select on one
// of its files queues. Pure functions over a card, so a test drives them without a screen.
namespace player {

// A row of any screen: its label, a value drawn right-aligned (a setting's, an album's artist), and
// whether Select opens it (a folder) rather than playing it. The lists, the menus and the folder
// browser fill the same type; a folder listing leaves the value empty.
struct Row {
  std::string label, value;
  // A Folders row that opens rather than plays. The label keeps its trailing '/' because that is
  // what the user reads; this flag is what the code decides on.
  bool folder = false;
};

using FolderRow = Row;

// The rows a Select queues from a list of `count` rows: the whole list when it fits in a queue,
// else a queue of kQueueSongs rows reaching back kQueueBefore before the selected one.
struct Window {
  std::size_t first, last;
};

Window queue_window(std::size_t count, std::size_t row);

// What `dir` holds, as the folder browser shows it: subfolders first (each label keeping its
// trailing '/'), then the .mp3 files, each group in the library's order. Dot names and the card's
// system folders are skipped, by the rules the library's own scan uses.
std::vector<FolderRow> folder_rows(Storage& storage, std::string_view dir);

struct FolderQueue {
  std::vector<TrackRow> tracks;
  std::size_t start = 0;  // the selected file's place in `tracks`
};

// What Select on `rows[row]` queues: the folder's own files from `row` on, by the window rule a
// song view uses. Nothing when that row is a subfolder, or when it falls outside the window.
std::optional<FolderQueue> folder_queue(Storage& storage, std::string_view dir,
                                        std::span<const FolderRow> rows, std::size_t row);

}  // namespace player
