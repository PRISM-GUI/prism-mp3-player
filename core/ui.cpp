#include "core/ui.hpp"

#include <algorithm>
#include <array>

namespace player {
namespace {

using Screen = PlayerUi::Screen;

// simplify: Select in the Songs view queues the 500 songs from the selected one, not all of
// them (10,000 TrackRows would take MBs of RAM on the board). Upgrade: a Player queue that pages
// its tracks from the Library by view and row.
constexpr std::size_t kSongsQueue = 500;

constexpr std::array kMusicViews{View::Artists, View::Albums, View::Songs};

bool is_song_view(View v) { return v == View::Songs || v == View::SongsOfAlbum; }

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
    case Screen::List: break;
  }
  return {};
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
    case Screen::Music:
      if (!library_) return {};
      return {"Artists", "Albums", "Songs"};
    case Screen::Settings: return {"Output", "Bluetooth"};
    case Screen::List:
    case Screen::NowPlaying: break;
  }
  return {};
}

PlayerUi::Place PlayerUi::menu(Screen s) const { return {.screen = s, .count = menu_rows(s).size()}; }

PlayerUi::Place PlayerUi::list(ViewKey key) const {
  const auto count = library_->count(key);
  return {.screen = Screen::List, .key = std::move(key), .count = count};
}

std::string_view PlayerUi::screen_title() const {
  return here().screen == Screen::List ? list_title(here().key) : screen_name(here().screen);
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
  select_menu_row();
}

void PlayerUi::select_menu_row() {
  const auto row = here().selected;
  switch (here().screen) {
    case Screen::Main:
      open(row == 0 ? menu(Screen::Music) : row == 1 ? menu(Screen::NowPlaying) : menu(Screen::Settings));
      break;
    case Screen::Music: open(list({kMusicViews[row]})); break;
    case Screen::Settings:
      if (row == 0 && !settings.outputs.empty())
        settings.output = (settings.output + 1) % settings.outputs.size();
      break;
    case Screen::List:
    case Screen::NowPlaying: break;
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
  const auto& key = here().key;
  const auto first = key.view == View::Songs ? row : 0;
  auto queue = library_->tracks(key, first, key.view == View::Songs ? kSongsQueue : here().count);
  if (queue.size() <= row - first) return;
  player_.play(std::move(queue), row - first);
  open(menu(Screen::NowPlaying));
}

void PlayerUi::tick() {
  player_.poll();
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
  if (here().screen == Screen::List) stack_ = {menu(Screen::Main)};
  std::erase_if(stack_, [](const Place& p) { return p.screen == Screen::List; });
  for (auto& p : stack_)
    if (p.screen == Screen::Music) p = menu(Screen::Music);
  changed();
}

}  // namespace player
