#include "core/ui.hpp"

#include "core/fold.hpp"
#include "core/index_format.hpp"
#include "core/tags.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace player {
namespace {

using Screen = PlayerUi::Screen;

// simplify: Select in a song view (Songs, an album) of at most 500 songs queues the whole view;
// in a larger one, a window of 500 songs, up to 250 of them before the selected one (10,000
// TrackRows would take MBs of RAM on the board). Upgrade: a Player queue that pages its tracks
// from the Library by view and row.
constexpr std::size_t kSongsQueue = 500;
constexpr std::size_t kSongsBefore = kSongsQueue / 2;
// simplify: a folder reads the tags of the 32 songs around the selection, not of the 500 it can
// queue: a 2,000-song folder would otherwise open 2,000 files before anything plays. The rest are
// named by their file. Upgrade: read a track's tags when it starts, or take the rows from the
// index when the folder is in it.
constexpr std::size_t kTagWindow = 32;

constexpr std::array kMusicViews{View::Artists, View::Albums, View::Songs};

bool is_song_view(View v) { return v == View::Songs || v == View::SongsOfAlbum; }

// The file's name without its folder and its extension: what a row says when its tags were not read.
std::string file_stem(std::string_view path) {
  const auto slash = path.find_last_of('/');
  const auto name = slash == std::string_view::npos ? path : path.substr(slash + 1);
  const auto dot = name.find_last_of('.');
  return std::string{dot == std::string_view::npos ? name : name.substr(0, dot)};
}

TrackRow row_from_file(std::string path) {
  auto title = file_stem(path);
  return {std::move(path), std::move(title), {}, {}, 0};
}

std::string_view list_title(const ViewKey& key) {
  switch (key.view) {
    case View::Artists: return "Artists";
    case View::Albums: return "Albums";
    case View::Songs: return "Songs";
    case View::AlbumsOfArtist: return key.artist.empty() ? "Unknown Artist" : std::string_view{key.artist};
    case View::SongsOfAlbum: return key.album.empty() ? "Unknown Album" : std::string_view{key.album};
  }
  return {};
}

std::string_view screen_name(Screen s) {
  switch (s) {
    case Screen::Main: return "Player";
    case Screen::Music: return "Music";
    case Screen::NowPlaying: return "Now Playing";
    case Screen::Settings: return "Settings";
    case Screen::List:
    case Screen::Folders: break;
  }
  return {};
}

// The title bar of a Folders screen: the folder's own name, and "Card" for the card's root.
std::string_view folder_name(std::string_view dir) {
  while (dir.size() > 1 && dir.ends_with('/')) dir.remove_suffix(1);
  if (dir == "/") return "Card";
  const auto slash = dir.rfind('/');
  return slash == std::string_view::npos ? dir : dir.substr(slash + 1);
}

// Keeps `selected` in [0, count) and on screen.
void move_selection(PlayerUi::Place& p, long steps) {
  if (p.count == 0) return;
  const long last = static_cast<long>(p.count) - 1;
  p.selected = static_cast<std::size_t>(std::clamp(static_cast<long>(p.selected) + steps, 0L, last));
  if (p.selected < p.top) p.top = p.selected;
  if (p.selected >= p.top + PlayerUi::kVisibleRows) p.top = p.selected + 1 - PlayerUi::kVisibleRows;
}

}  // namespace

PlayerUi::PlayerUi(Library* library, Player& player) : library_(library), player_(player) {
  stack_.push_back(menu(Screen::Main));
  player_.set_volume(static_cast<float>(volume_) / kVolumeSteps);
}

std::vector<std::string> PlayerUi::menu_rows(Screen s) const {
  switch (s) {
    case Screen::Main: return {"Music", "Now Playing", "Settings"};
    case Screen::Music: {
      // Folders works from the storage alone, so it is there while the library is missing or
      // rebuilding; the library's views only when it has songs.
      if (!has_music() && !storage_) return {};
      std::vector<std::string> rows{"Folders"};
      if (has_music()) rows.insert(rows.end(), {"Artists", "Albums", "Songs"});
      return rows;
    }
    case Screen::Settings: return {"Output", "Bluetooth"};
    case Screen::List:
    case Screen::NowPlaying:
    case Screen::Folders: break;
  }
  return {};
}

PlayerUi::Place PlayerUi::menu(Screen s) const { return {.screen = s, .count = menu_rows(s).size()}; }

PlayerUi::Place PlayerUi::list(ViewKey key) const {
  const auto count = library_->count(key);
  return {.screen = Screen::List, .key = std::move(key), .count = count};
}

std::string_view PlayerUi::screen_title() const {
  const auto& p = here();
  if (p.screen == Screen::Folders) return folder_name(p.dir);
  return p.screen == Screen::List ? list_title(p.key) : screen_name(p.screen);
}

void PlayerUi::handle(WheelEvent e) {
  if (const auto* r = std::get_if<Rotate>(&e)) {
    rotate(r->steps);
  } else {
    switch (std::get<Button>(e)) {
      case Button::Menu:
        if (stack_.size() > 1) stack_.pop_back();
        break;
      case Button::PlayPause: player_.toggle_pause(); break;
      case Button::Previous: player_.previous(); break;
      case Button::Next: player_.next(); break;
      case Button::Select: select(); break;
    }
  }
  changed();
}

void PlayerUi::rotate(int steps) {
  if (here().screen != Screen::NowPlaying) return move_selection(here(), steps);
  volume_ = std::clamp(volume_ + steps, 0, kVolumeSteps);
  player_.set_volume(static_cast<float>(volume_) / kVolumeSteps);
}

void PlayerUi::select() {
  if (here().count == 0) return;
  if (here().screen == Screen::List) return select_list_row();
  if (here().screen == Screen::Folders) return select_folder_row();
  select_menu_row();
}

void PlayerUi::select_menu_row() {
  const auto row = here().selected;
  switch (here().screen) {
    case Screen::Main:
      open(row == 0 ? menu(Screen::Music) : row == 1 ? menu(Screen::NowPlaying) : menu(Screen::Settings));
      break;
    case Screen::Music:
      if (row == 0) return select_folders();
      open(list({kMusicViews[row - 1]}));
      break;
    case Screen::Settings:
      if (row == 0 && !settings.outputs.empty()) {
        settings.output = (settings.output + 1) % settings.outputs.size();
        if (output_changed_) output_changed_(settings.output);
      }
      break;
    case Screen::List:
    case Screen::NowPlaying:
    case Screen::Folders: break;
  }
}

void PlayerUi::select_list_row() {
  const auto key = here().key;
  const auto row = here().selected;
  if (is_song_view(key.view)) return play_from(row);
  const auto name = library_->names(key, row, 1);
  if (name.empty()) return;
  if (key.view == View::Artists) return open(list({View::AlbumsOfArtist, name[0]}));
  const auto artist = library_->album_artists(key, row, 1);
  if (artist.empty()) return;
  open(list({View::SongsOfAlbum, artist[0], name[0]}));
}

void PlayerUi::play_from(std::size_t row) {
  const bool whole_view = here().count <= kSongsQueue;
  const auto first = whole_view ? 0 : row - std::min(row, kSongsBefore);
  auto queue = library_->tracks(here().key, first, kSongsQueue);
  if (row - first >= queue.size()) return;
  player_.play(std::move(queue), row - first);
  open(menu(Screen::NowPlaying));
}

// The card's root, or nothing when there is no card: the Folders row says why.
void PlayerUi::select_folders() {
  if (storage_) open_folder("/");
}

void PlayerUi::select_folder_row() {
  const auto& place = here();
  if (place.selected >= place.rows.size()) return;
  const auto& label = place.rows[place.selected].label;
  if (!label.ends_with('/')) return play_folder(place.dir, place.selected);
  open_folder(index_format::join(place.dir, std::string_view{label}.substr(0, label.size() - 1)));
}

// Lists the folder once, when it is entered: subfolders first with a trailing '/', then its .mp3
// files, each group by the library's own order, and by the library's own rules about what counts.
void PlayerUi::open_folder(std::string_view dir) {
  Place place{.screen = Screen::Folders, .dir = std::string{dir}};
  place.rows = read_folder(dir);
  place.count = place.rows.size();
  open(std::move(place));
}

std::vector<PlayerUi::Row> PlayerUi::read_folder(std::string_view dir) const {
  struct Entry {
    std::string name;  // what the row shows: a folder keeps its trailing '/'
    std::string key;   // the folded name, made once: sorting thousands folds nothing twice
    bool is_dir;
  };
  std::vector<Entry> entries;
  for (const auto& e : storage_->list(dir)) {
    if (e.name.starts_with('.')) continue;
    if (e.is_dir && is_card_system_dir(e.name)) continue;
    if (!e.is_dir && !is_mp3_name(e.name)) continue;
    entries.push_back({e.is_dir ? e.name + "/" : e.name, fold_for_sort(e.name), e.is_dir});
  }
  std::ranges::stable_sort(entries, [](const Entry& a, const Entry& b) {
    if (a.is_dir != b.is_dir) return a.is_dir;
    return a.key < b.key;
  });
  std::vector<Row> rows;
  rows.reserve(entries.size());
  for (auto& e : entries) rows.push_back({std::move(e.name), {}});
  return rows;
}

// Plays the folder's own .mp3 files from `row`, by the window rule a song view uses. A subfolder is
// never in the queue, and a file that cannot be opened keeps its place in it, so the player names
// the song that failed instead of the queue shifting under the selection.
void PlayerUi::play_folder(std::string_view dir, std::size_t row) {
  const auto& place = here();
  std::vector<std::string> paths;
  std::size_t first_file = 0;
  for (std::size_t i = 0; i < place.rows.size(); ++i) {
    if (place.rows[i].label.ends_with('/')) continue;
    if (i == row) first_file = paths.size();
    paths.push_back(index_format::join(dir, place.rows[i].label));
  }
  if (paths.empty()) return;
  const bool whole_folder = paths.size() <= kSongsQueue;
  const auto first = whole_folder ? 0 : first_file - std::min(first_file, kSongsBefore);
  const auto last = whole_folder ? paths.size() : std::min(first + kSongsQueue, paths.size());
  if (first_file < first || first_file >= last) return;
  const auto tagged = first_file - std::min(first_file, kTagWindow / 2);
  const auto tagged_end = std::min(tagged + kTagWindow, paths.size());
  std::vector<TrackRow> queue;
  queue.reserve(last - first);
  for (auto i = first; i < last; ++i) {
    if (i < tagged || i >= tagged_end) {
      queue.push_back(row_from_file(paths[i]));
      continue;
    }
    auto file = storage_->open(paths[i]);
    if (!file) {
      queue.push_back(row_from_file(paths[i]));
      continue;
    }
    const auto tags = read_tags(*file, paths[i]);
    queue.push_back({paths[i], tags.title, tags.artist, tags.album, tags.track});
  }
  player_.play(std::move(queue), first_file - first);
  open(menu(Screen::NowPlaying));
}

void PlayerUi::tick() {
  if (polls_on_tick_) player_.poll();
  const auto now = player_.status();
  const bool clock_moved = here().screen == Screen::NowPlaying &&
                           static_cast<long>(now.position_s) != static_cast<long>(shown_.position_s);
  const bool same = now.state == shown_.state && now.index == shown_.index && now.message == shown_.message;
  if (same && !clock_moved) return;
  shown_ = now;
  changed();
}

void PlayerUi::set_library(Library* library) {
  library_ = library;
  library_status_.clear();
  if (here().screen == Screen::List) stack_ = {menu(Screen::Main)};
  std::erase_if(stack_, [](const Place& p) { return p.screen == Screen::List; });
  for (auto& p : stack_)
    if (p.screen == Screen::Music) p = menu(Screen::Music);
  changed();
}

void PlayerUi::set_library_status(std::string status) {
  library_status_ = std::move(status);
  changed();
}

// The card changed under the browser: drop what was read from it and re-count the Music menu's
// rows. In practice this runs once, at boot, before anything is on the stack but Main.
void PlayerUi::set_storage(Storage* storage) {
  if (storage_ == storage) return;
  storage_ = storage;
  std::erase_if(stack_, [](const Place& p) { return p.screen == Screen::Folders; });
  for (auto& p : stack_)
    if (p.screen == Screen::Music) p = menu(Screen::Music);
  changed();
}

}  // namespace player
