#include "core/ui.hpp"

#include "core/cover_loader.hpp"
#include "core/path.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

namespace player {
namespace {

using Screen = PlayerUi::Screen;

constexpr std::array kMusicViews{View::Artists, View::Albums, View::Songs};

std::string_view list_title(const ViewKey& key) {
  switch (key.view) {
    case View::Artists: return "Artists";
    case View::Albums: return "Albums";
    case View::Songs: return "Songs";
    case View::AlbumsOfArtist:
      return key.artist.empty() ? PlayerUi::kUnknownArtist : std::string_view{key.artist};
    case View::SongsOfAlbum:
      return key.album.empty() ? PlayerUi::kUnknownAlbum : std::string_view{key.album};
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

// Keeps `selected` on the `rows` on screen.
void keep_on_screen(PlayerUi::Place& p, std::size_t rows) {
  if (p.selected < p.top) p.top = p.selected;
  if (p.selected >= p.top + rows) p.top = p.selected + 1 - rows;
}

// Keeps `selected` in [0, count) and on screen.
void move_selection(PlayerUi::Place& p, long steps, std::size_t rows) {
  if (p.count == 0) return;
  const long last = static_cast<long>(p.count) - 1;
  p.selected = static_cast<std::size_t>(std::clamp(static_cast<long>(p.selected) + steps, 0L, last));
  keep_on_screen(p, rows);
}

}  // namespace

// The rows that fit under the title bar of a canvas `height` high; the view scrolls so the
// selected row stays among them. PRISM records a canvas once before layout, with no size: a canvas
// with no room for a row keeps the count it had.
void PlayerUi::fit_rows(float height) {
  const auto rows = static_cast<std::size_t>(std::max(0.f, (height - kTitleHeight) / kRowHeight));
  if (rows == 0) return;
  rows_on_screen_ = rows;
  keep_on_screen(here(), rows_on_screen_);
}

PlayerUi::PlayerUi(Library* library, Player& player) : library_(library), player_(player) {
  clock_ = [] {
    return std::chrono::duration_cast<Seconds>(std::chrono::steady_clock::now().time_since_epoch());
  };
  stack_.push_back(menu(Screen::Main));
  player_.set_volume(volume_fraction());
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
        // Menu leaves the scrubber first; the screen only on a second press.
        if (scrubbing_) {
          scrubbing_ = false;
          break;
        }
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
  if (here().screen != Screen::NowPlaying) return move_selection(here(), steps, rows_on_screen_);
  if (scrubbing_) {
    scrub_step_at_ = clock_();
    player_.seek(player_.status().position + steps * kScrubStep);
    return;
  }
  volume_ = std::clamp(volume_ + steps, 0, kVolumeSteps);
  player_.set_volume(volume_fraction());
}

void PlayerUi::select() {
  // Select in Now Playing starts and stops scrubbing; the wheel seeks while it lasts.
  if (here().screen == Screen::NowPlaying) {
    if (player_.status().state == Player::Status::State::Stopped) return;
    scrubbing_ = !scrubbing_;
    scrub_step_at_ = clock_();
    return;
  }
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
  const auto [first, last] = queue_window(here().count, row);
  auto queue = library_->tracks(here().key, first, last - first);
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
  const auto& row = place.rows[place.selected];
  if (!row.folder) return play_folder(place.dir, place.selected);
  open_folder(join(place.dir, std::string_view{row.label}.substr(0, row.label.size() - 1)));
}

// Lists the folder once, when it is entered: subfolders first with a trailing '/', then its .mp3
// files, by the browser's own rules (core/folders.hpp).
void PlayerUi::open_folder(std::string_view dir) {
  Place place{.screen = Screen::Folders, .dir = std::string{dir}};
  place.rows = folder_rows(*storage_, dir);
  place.count = place.rows.size();
  open(std::move(place));
}

// Plays the folder's own .mp3 files from `row`, by the window rule a song view uses.
void PlayerUi::play_folder(std::string_view dir, std::size_t row) {
  auto queued = folder_queue(*storage_, dir, here().rows, row);
  if (!queued) return;
  player_.play(std::move(queued->tracks), queued->start);
  open(menu(Screen::NowPlaying));
}

void PlayerUi::tick() {
  if (polls_on_tick_) player_.poll();
  watch_cover();
  // Scrub mode lapses on its own: the next wheel step is the volume again.
  if (scrubbing_ && clock_() - scrub_step_at_ > kScrubIdle) {
    scrubbing_ = false;
    changed();  // the marker goes, even on a paused player whose clock does not repaint
  }
  const auto now = player_.status();
  const bool clock_moved =
      here().screen == Screen::NowPlaying &&
      static_cast<long>(now.position.count()) != static_cast<long>(shown_.position.count());
  const bool same = now.state == shown_.state && now.index == shown_.index && now.message == shown_.message;
  if (same && !clock_moved) return;
  shown_ = now;
  changed();
}

// The thread that loads covers is the loader's: here we only say what we want and notice when it
// answered, so a slow card never stalls the UI.
void PlayerUi::watch_cover() {
  if (!cover_loader_) return;
  if (const auto track = player_.current_track(); track && track->path != cover_path_) {
    cover_path_ = track->path;
    cover_loader_->request(cover_path_);
  }
  if (const auto revision = cover_loader_->revision(); revision != cover_revision_) {
    cover_revision_ = revision;
    changed();
  }
}

std::shared_ptr<const render::Image> PlayerUi::cover_image() const {
  if (!cover_loader_ || cover_path_.empty()) return nullptr;
  const auto loaded = cover_loader_->cover(cover_path_);
  return loaded ? *loaded : nullptr;
}

void PlayerUi::refresh_music_menus() {
  for (auto& p : stack_)
    if (p.screen == Screen::Music) p = menu(Screen::Music);
}

void PlayerUi::set_library(Library* library) {
  library_ = library;
  library_status_.clear();
  if (here().screen == Screen::List) stack_ = {menu(Screen::Main)};
  std::erase_if(stack_, [](const Place& p) { return p.screen == Screen::List; });
  refresh_music_menus();
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
  refresh_music_menus();
  changed();
}

}  // namespace player
