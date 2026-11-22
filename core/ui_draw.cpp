#include "core/ui.hpp"

#include <prism/app/widget_tree.hpp>
#include <prism/render/draw_list.hpp>
#include <prism/ui/text_metrics.hpp>

#include <algorithm>
#include <format>

namespace player {

// Where a screen draws: the list, its width, and the measurer of the text (null: 0.6 em a
// character, as PRISM estimates without fonts).
struct PlayerUi::Pen {
  render::DrawList& dl;
  const ui::TextMeasurer* measurer;
  float width;
  float text_width(std::string_view s, float size, render::FontStyle font = {}) const {
    return ui::text_width(measurer, s, size, font);
  }
  float line_height(float size, render::FontStyle font = {}) const { return ui::line_height(measurer, size, font); }
};

namespace {

using core::Color;
using core::Height;
using core::Point;
using core::Rect;
using core::Size;
using core::Width;
using core::X;
using core::Y;
using render::DrawList;
using render::FontStyle;
using render::TextAnchor;
using Pen = PlayerUi::Pen;
using Screen = PlayerUi::Screen;
using State = Player::Status::State;

constexpr Color kWhite = Color::rgba(0xFF, 0xFF, 0xFF);
constexpr Color kInk = Color::rgba(0x10, 0x10, 0x10);
constexpr Color kGreyInk = Color::rgba(0x6E, 0x6E, 0x6E);
constexpr Color kRule = Color::rgba(0xA0, 0xA4, 0xAA);
constexpr Color kTrack = Color::rgba(0xD6, 0xD8, 0xDC);
constexpr Color kCover = Color::rgba(0xC8, 0xCC, 0xD2);
constexpr float kText = 15.f;
constexpr float kSmallText = 12.f;
constexpr float kPad = 10.f;
constexpr float kBarMargin = 14.f;     // the progress and volume bars' side margins
constexpr float kTitleReserve = 54.f;  // the title bar's right end: battery and play state
constexpr const FontStyle kBold{render::FontRole::Ui, true};

Rect rect(float x, float y, float w, float h) { return {Point{X{x}, Y{y}}, Size{Width{w}, Height{h}}}; }
Point pt(float x, float y) { return {X{x}, Y{y}}; }

void text(DrawList& dl, std::string s, float x, float y, float size, Color c, FontStyle font = {}) {
  dl.text(std::move(s), pt(x, y), size, c, 0.f, TextAnchor::TopLeft, font);
}

// `s` drawn `dx, dy` into `box`: what does not fit is cut, not drawn over its neighbours.
void clipped_text(DrawList& dl, std::string s, Rect box, float size, Color c, FontStyle font = {},
                  float dx = 0, float dy = 0) {
  dl.clip_push(box.origin, box.extent);
  text(dl, std::move(s), dx, dy, size, c, font);
  dl.clip_pop();
}

// `s` centred in [x, x + w) when it fits, else from x and cut at the end.
void centred_text(const Pen& pen, std::string s, Rect box, float size, Color c, FontStyle font = {}) {
  const float slack = box.extent.w.raw() - pen.text_width(s, size, font);
  const float dy = (box.extent.h.raw() - pen.line_height(size, font)) / 2;
  clipped_text(pen.dl, std::move(s), box, size, c, font, std::max(0.f, slack / 2), dy);
}

void play_glyph(DrawList& dl, float x, float y, float h, Color c) {
  dl.filled_polygon({pt(x, y), pt(x + h * 0.85f, y + h / 2), pt(x, y + h)}, c);
}

void pause_glyph(DrawList& dl, float x, float y, float h, Color c) {
  dl.filled_rect(rect(x, y, h * 0.3f, h), c);
  dl.filled_rect(rect(x + h * 0.55f, y, h * 0.3f, h), c);
}

// simplify: always shown full; no battery is read yet. Upgrade: the board's fuel gauge.
void battery(DrawList& dl, float x, float y) {
  dl.rect_outline(rect(x, y, 20, 11), PlayerUi::kBattery, 1.f);
  dl.filled_rect(rect(x + 20, y + 3, 2, 5), PlayerUi::kBattery);
  dl.filled_rect(rect(x + 2, y + 2, 16, 7), kGreyInk);
}

void chevron(DrawList& dl, float x, float y, Color c) {
  dl.polyline({pt(x, y - 5), pt(x + 5, y), pt(x, y + 5)}, c, 2.f);
}

std::string clock(double seconds) {
  const auto s = static_cast<long>(std::max(0.0, seconds));
  return std::format("{}:{:02}", s / 60, s % 60);
}

float row_top(std::size_t visible_index) {
  return PlayerUi::kTitleHeight + static_cast<float>(visible_index) * PlayerUi::kRowHeight;
}

// One row of a menu or list: the highlight bar when selected, the label, a chevron when it opens
// another screen, and the row's value (a setting's, an album's artist) right-aligned in grey.
void draw_row(const Pen& pen, float width, std::size_t visible_index, bool selected, const PlayerUi::Row& row,
              bool opens) {
  const float y = row_top(visible_index), h = PlayerUi::kRowHeight - 3;
  pen.dl.filled_rect(rect(0, y, width, PlayerUi::kRowHeight), selected ? PlayerUi::kHighlight : kWhite);
  const Color ink = selected ? kWhite : kInk;
  const float right = width - (opens ? 30.f : kPad);
  const float value_w = std::min(pen.text_width(row.value, kText), (right - kPad) / 2);
  const float label_right = row.value.empty() ? right : right - value_w - 8.f;
  clipped_text(pen.dl, row.label, rect(kPad, y + 3, label_right - kPad, h), kText, ink);
  if (!row.value.empty())
    clipped_text(pen.dl, row.value, rect(right - value_w, y + 3, value_w, h), kText, selected ? kWhite : kGreyInk);
  if (opens) chevron(pen.dl, width - 18.f, y + PlayerUi::kRowHeight / 2, ink);
}

void draw_scrollbar(DrawList& dl, float width, float height, const PlayerUi::Place& p) {
  if (p.count <= PlayerUi::kVisibleRows) return;
  const float track_h = height - PlayerUi::kTitleHeight;
  const float thumb_h = std::max(12.f, track_h * PlayerUi::kVisibleRows / static_cast<float>(p.count));
  const float thumb_y = (track_h - thumb_h) * static_cast<float>(p.top) /
                        static_cast<float>(p.count - PlayerUi::kVisibleRows);
  dl.filled_rect(rect(width - 6, PlayerUi::kTitleHeight, 6, track_h), kTrack);
  dl.filled_rect(rect(width - 6, PlayerUi::kTitleHeight + thumb_y, 6, thumb_h), kRule);
}

void centred_note(const Pen& pen, float y, std::string s, float size = kText) {
  centred_text(pen, std::move(s), rect(kPad, y, pen.width - 2 * kPad, 22), size, kGreyInk);
}

void progress_bar(DrawList& dl, Rect r, float fraction, Color fill) {
  dl.filled_rect(r, kTrack);
  dl.filled_rect(rect(r.origin.x.raw(), r.origin.y.raw(), r.extent.w.raw() * std::clamp(fraction, 0.f, 1.f),
                      r.extent.h.raw()),
                 fill);
}

bool is_song_view(View v) { return v == View::Songs || v == View::SongsOfAlbum; }

std::string or_unknown(std::string s, std::string_view unknown) { return s.empty() ? std::string{unknown} : s; }

}  // namespace

void PlayerUi::canvas(render::DrawList& dl, core::Rect bounds, const ui::WidgetNode& node) {
  const Pen pen{dl, node.theme ? node.theme->measurer : nullptr, bounds.extent.w.raw()};
  dl.filled_rect(rect(0, 0, pen.width, bounds.extent.h.raw()), kWhite);
  draw_title(pen);
  switch (here().screen) {
    case Screen::List:
    case Screen::Folders: draw_list(pen); break;
    case Screen::NowPlaying: draw_now_playing(pen); break;
    default: draw_menu(pen); break;
  }
  draw_scrollbar(dl, pen.width, bounds.extent.h.raw(), here());
}

void PlayerUi::view(app::ViewBuilder& vb) {
  vb.canvas(*this).min_size(Width{kScreenWidth}).min_size(Height{kScreenHeight}).depends_on(revision);
}

void PlayerUi::draw_title(const Pen& pen) const {
  const float width = pen.width;
  pen.dl.filled_rect(rect(0, 0, width, kTitleHeight), kTitleBar);
  pen.dl.filled_rect(rect(0, kTitleHeight - 1, width, 1), kRule);
  const bool library_screen = here().screen == Screen::Music || here().screen == Screen::List;
  const bool updating = has_music() && library_screen && !library_status_.empty();
  centred_text(pen, updating ? library_status_ : std::string{screen_title()}, rect(kTitleReserve, 0, width - 2 * kTitleReserve, kTitleHeight - 1),
               kText, kInk, kBold);
  battery(pen.dl, width - 50, 7);
  const auto state = player_.status().state;
  if (state == State::Playing) play_glyph(pen.dl, width - 22, 7, 12, kInk);
  if (state == State::Paused) pause_glyph(pen.dl, width - 22, 7, 12, kInk);
}

void PlayerUi::draw_menu(const Pen& pen) const {
  const auto& p = here();
  const auto rows = menu_rows(p.screen);
  const auto note = [&] { return library_status_.empty() ? std::string{"No music"} : library_status_; };
  if (rows.empty()) return centred_note(pen, 110, note());
  const bool opens = p.screen != Screen::Settings;
  for (std::size_t i = p.top; i < std::min(rows.size(), p.top + kVisibleRows); ++i) {
    Row row{rows[i], {}};
    if (p.screen == Screen::Settings)
      row.value = i == 0 ? (settings.outputs.empty() ? "" : settings.outputs[settings.output]) : settings.bluetooth;
    if (p.screen == Screen::Music && i == 0 && !storage_) row.value = "No SD card";
    draw_row(pen, pen.width, i - p.top, i == p.selected, row, opens);
  }
  // Folders sits above the note: the card is browsable while the index is missing or being rebuilt.
  if (p.screen == Screen::Music && !has_music()) centred_note(pen, row_top(rows.size()) + 4, note());
  if (p.screen == Screen::Settings)
    text(pen.dl, "The output changes at the next start.", kPad, row_top(3), kSmallText, kGreyInk);
}

// Only the rows on screen are read from the library. In the Albums view, each album's artist is
// its value: two artists' albums of the same name stay apart.
std::vector<PlayerUi::Row> PlayerUi::visible_rows() const {
  const auto& p = here();
  if (p.screen == Screen::Folders) {
    const auto top = std::min(p.top, p.rows.size());
    const auto end = std::min(top + kVisibleRows, p.rows.size());
    return {p.rows.begin() + static_cast<std::ptrdiff_t>(top),
            p.rows.begin() + static_cast<std::ptrdiff_t>(end)};
  }
  std::vector<Row> rows;
  if (is_song_view(p.key.view)) {
    for (auto& t : library_->tracks(p.key, p.top, kVisibleRows)) rows.push_back({std::move(t.title), {}});
    return rows;
  }
  const auto unknown = p.key.view == View::Artists ? "Unknown Artist" : "Unknown Album";
  for (auto& n : library_->names(p.key, p.top, kVisibleRows)) rows.push_back({or_unknown(std::move(n), unknown), {}});
  if (p.key.view != View::Albums) return rows;
  const auto artists = library_->album_artists(p.key, p.top, rows.size());
  for (std::size_t i = 0; i < std::min(rows.size(), artists.size()); ++i)
    rows[i].value = or_unknown(artists[i], "Unknown Artist");
  return rows;
}

void PlayerUi::draw_list(const Pen& pen) const {
  const auto& p = here();
  // A list opens when it holds views; a folder row opens when it is a subfolder, told by its '/'.
  const bool list_opens = p.screen == Screen::List && !is_song_view(p.key.view);
  const auto rows = visible_rows();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const bool opens = p.screen == Screen::Folders ? rows[i].label.ends_with('/') : list_opens;
    draw_row(pen, pen.width - 6, i, p.top + i == p.selected, rows[i], opens);
  }
}

void PlayerUi::draw_now_playing(const Pen& pen) const {
  const auto status = player_.status();
  const auto track = player_.current_track();
  const auto size = player_.queue_size();
  if (status.state == State::Stopped || !track) {
    centred_note(pen, 100, "Nothing playing");
    if (!status.message.empty()) centred_note(pen, 126, status.message, kSmallText);
    return;
  }
  auto& dl = pen.dl;
  const float width = pen.width;

  dl.filled_rect(rect(14, 40, 100, 100), kCover);
  dl.rect_outline(rect(14, 40, 100, 100), kRule);
  const float x = 128, w = width - x - kPad;
  clipped_text(dl, track->title, rect(x, 44, w, 22), 16.f, kInk, kBold);
  clipped_text(dl, or_unknown(track->artist, "Unknown Artist"), rect(x, 70, w, 20), kText, kInk);
  clipped_text(dl, or_unknown(track->album, "Unknown Album"), rect(x, 92, w, 20), kText, kGreyInk);
  text(dl, std::format("{} of {}", status.index + 1, size), x, 116, kSmallText, kGreyInk);
  if (!status.message.empty()) clipped_text(dl, status.message, rect(14, 146, width - 28, 18), kSmallText, kGreyInk);

  const float bar_right = width - kBarMargin;
  const auto fraction = status.duration_s > 0 ? static_cast<float>(status.position_s / status.duration_s) : 0.f;
  progress_bar(dl, rect(kBarMargin, 168, bar_right - kBarMargin, 8), fraction, kHighlight);
  text(dl, clock(status.position_s), kBarMargin, 180, kSmallText, kInk);
  auto remaining = "-" + clock(status.duration_s - status.position_s);
  const float remaining_x = bar_right - pen.text_width(remaining, kSmallText);
  text(dl, std::move(remaining), remaining_x, 180, kSmallText, kInk);
  progress_bar(dl, rect(kBarMargin, 222, bar_right - kBarMargin, 4), static_cast<float>(volume_) / kVolumeSteps,
               kRule);
}

namespace {

// The largest square centred in `r`, inset a little: the wheel's ring.
Rect ring_in(Rect r) {
  const float side = std::min(r.extent.w.raw(), r.extent.h.raw()) - 8.f;
  const auto c = r.center();
  return rect(c.x.raw() - side / 2, c.y.raw() - side / 2, side, side);
}

constexpr Color kWheel = Color::rgba(0xEC, 0xED, 0xEF);
constexpr Color kWheelEdge = Color::rgba(0xC4, 0xC7, 0xCC);
constexpr Color kWheelInk = Color::rgba(0x8A, 0x8E, 0x94);

void skip_glyph(DrawList& dl, Point at, float h, bool forward) {
  const float d = forward ? 1.f : -1.f;
  const float x = at.x.raw(), y = at.y.raw();
  for (const float step : {-0.5f, 0.3f}) {
    const float x0 = x + d * step * h;
    dl.filled_polygon({pt(x0, y - h / 2), pt(x0 + d * h * 0.7f, y), pt(x0, y + h / 2)}, kWheelInk);
  }
  dl.filled_rect(rect(forward ? x + 0.9f * h : x - 1.0f * h, y - h / 2, 2, h), kWheelInk);
}

}  // namespace

void WheelWidget::canvas(render::DrawList& dl, core::Rect bounds, const ui::WidgetNode&) {
  const Rect ring = ring_in(Rect{Point{X{0}, Y{0}}, bounds.extent});
  const auto c = ring.center();
  const float radius = ring.extent.w.raw() / 2;
  const float inner = radius * WheelTracker::kCentreRatio;
  dl.circle(c, radius, kWheel);
  dl.circle(c, radius, kWheelEdge, 1.5f);
  dl.circle(c, inner, kWhite);
  dl.circle(c, inner, kWheelEdge, 1.5f);

  const float label = (radius + inner) / 2;
  const float cx = c.x.raw(), cy = c.y.raw();
  dl.text("MENU", pt(cx, cy - label), 13.f, kWheelInk, 0.f, TextAnchor::Center, kBold);
  skip_glyph(dl, pt(cx + label, cy), 10, true);
  skip_glyph(dl, pt(cx - label, cy), 10, false);
  play_glyph(dl, cx - 10, cy + label - 5, 10, kWheelInk);
  pause_glyph(dl, cx + 2, cy + label - 5, 10, kWheelInk);
}

void WheelWidget::handle_canvas_input(const input::InputEvent& ev, ui::WidgetNode&, core::Rect bounds) {
  const Rect ring = ring_in(Rect{Point{X{0}, Y{0}}, bounds.extent});
  if (tracker_.ring() != ring) tracker_ = WheelTracker{ring};
  std::vector<WheelEvent> events;
  if (const auto* b = std::get_if<input::MouseButton>(&ev); b && b->button == input::buttons::left)
    events = b->pressed ? tracker_.press(b->position) : tracker_.release(b->position);
  else if (const auto* m = std::get_if<input::MouseMove>(&ev); m && m->buttons != 0)
    events = tracker_.move(m->position);
  else if (const auto* s = std::get_if<input::MouseScroll>(&ev); s && s->dy.raw() != 0)
    events = {Rotate{s->dy.raw() > 0 ? -1 : 1}};
  for (const auto& e : events) ui_.handle(e);
}

void WheelWidget::view(app::ViewBuilder& vb, core::Size size) {
  vb.canvas(*this).min_size(size.w).min_size(size.h);
}

}  // namespace player
