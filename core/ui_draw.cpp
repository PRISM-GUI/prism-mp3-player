#include "core/ui.hpp"

#include <prism/app/widget_tree.hpp>
#include <prism/render/draw_list.hpp>

#include <algorithm>
#include <format>

namespace player {
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
using Screen = PlayerUi::Screen;
using State = Player::Status::State;

constexpr Color kWhite = Color::rgba(0xFF, 0xFF, 0xFF);
constexpr Color kInk = Color::rgba(0x10, 0x10, 0x10);
constexpr Color kGreyInk = Color::rgba(0x6E, 0x6E, 0x6E);
constexpr Color kTitleBar = Color::rgba(0xE4, 0xE6, 0xEA);
constexpr Color kRule = Color::rgba(0xA0, 0xA4, 0xAA);
constexpr Color kTrack = Color::rgba(0xD6, 0xD8, 0xDC);
constexpr Color kCover = Color::rgba(0xC8, 0xCC, 0xD2);
constexpr float kText = 15.f;
constexpr float kSmallText = 12.f;
constexpr float kPad = 10.f;
constexpr const FontStyle kBold{render::FontRole::Ui, true};

Rect rect(float x, float y, float w, float h) { return {Point{X{x}, Y{y}}, Size{Width{w}, Height{h}}}; }
Point pt(float x, float y) { return {X{x}, Y{y}}; }

void text(DrawList& dl, std::string s, float x, float y, float size, Color c, FontStyle font = {}) {
  dl.text(std::move(s), pt(x, y), size, c, 0.f, TextAnchor::TopLeft, font);
}

// `s` inside [x, x + w) on one line: what does not fit is cut, not drawn over its neighbours.
void clipped_text(DrawList& dl, std::string s, Rect box, float size, Color c, FontStyle font = {}) {
  dl.clip_push(box.origin, box.extent);
  text(dl, std::move(s), 0.f, 0.f, size, c, font);
  dl.clip_pop();
}

void play_glyph(DrawList& dl, float x, float y, float h, Color c) {
  dl.filled_polygon({pt(x, y), pt(x + h * 0.85f, y + h / 2), pt(x, y + h)}, c);
}

void pause_glyph(DrawList& dl, float x, float y, float h, Color c) {
  dl.filled_rect(rect(x, y, h * 0.3f, h), c);
  dl.filled_rect(rect(x + h * 0.55f, y, h * 0.3f, h), c);
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
// another screen. `value`, when given, is shown at the right half (a setting's value).
void draw_row(DrawList& dl, float width, std::size_t visible_index, bool selected, std::string label,
              bool opens, std::string value = {}) {
  const float y = row_top(visible_index);
  dl.filled_rect(rect(0, y, width, PlayerUi::kRowHeight), selected ? PlayerUi::kHighlight : kWhite);
  const Color ink = selected ? kWhite : kInk;
  const float label_width = (value.empty() ? width - 30.f : width / 2) - kPad;
  clipped_text(dl, std::move(label), rect(kPad, y + 3, label_width, PlayerUi::kRowHeight - 3), kText, ink);
  if (!value.empty())
    clipped_text(dl, std::move(value), rect(width / 2, y + 3, width / 2 - kPad, PlayerUi::kRowHeight - 3),
                 kText, selected ? kWhite : kGreyInk);
  if (opens) chevron(dl, width - 18.f, y + PlayerUi::kRowHeight / 2, ink);
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

void centred_note(DrawList& dl, float width, float y, std::string s) {
  dl.text(std::move(s), pt(width / 2, y), kText, kGreyInk, 0.f, TextAnchor::Center);
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

void PlayerUi::canvas(render::DrawList& dl, core::Rect bounds, const ui::WidgetNode&) {
  const float width = bounds.extent.w.raw();
  dl.filled_rect(rect(0, 0, width, bounds.extent.h.raw()), kWhite);
  draw_title(dl, width);
  switch (here().screen) {
    case Screen::List: draw_list(dl, width); break;
    case Screen::NowPlaying: draw_now_playing(dl, width); break;
    default: draw_menu(dl, width); break;
  }
  draw_scrollbar(dl, width, bounds.extent.h.raw(), here());
}

void PlayerUi::view(app::ViewBuilder& vb) {
  vb.canvas(*this).min_size(Width{320}).min_size(Height{240}).depends_on(revision);
}

void PlayerUi::draw_title(render::DrawList& dl, float width) const {
  dl.filled_rect(rect(0, 0, width, kTitleHeight), kTitleBar);
  dl.filled_rect(rect(0, kTitleHeight - 1, width, 1), kRule);
  dl.text(std::string{screen_title()}, pt(width / 2, kTitleHeight / 2), kText, kInk, 0.f, TextAnchor::Center, kBold);
  const auto state = player_.status().state;
  if (state == State::Playing) play_glyph(dl, width - 22, 7, 12, kInk);
  if (state == State::Paused) pause_glyph(dl, width - 22, 7, 12, kInk);
}

void PlayerUi::draw_menu(render::DrawList& dl, float width) const {
  const auto& p = here();
  if (p.screen == Screen::Music && !library_) return centred_note(dl, width, 120, "No music");
  const auto rows = menu_rows(p.screen);
  const bool opens = p.screen != Screen::Settings;
  for (std::size_t i = p.top; i < std::min(rows.size(), p.top + kVisibleRows); ++i) {
    std::string value;
    if (p.screen == Screen::Settings)
      value = i == 0 ? (settings.outputs.empty() ? "" : settings.outputs[settings.output]) : settings.bluetooth;
    draw_row(dl, width, i - p.top, i == p.selected, rows[i], opens, std::move(value));
  }
  if (p.screen == Screen::Settings)
    text(dl, "The output changes at the next start.", kPad, row_top(3), kSmallText, kGreyInk);
}

// Only the rows on screen are read from the library.
std::vector<std::string> PlayerUi::visible_rows() const {
  const auto& p = here();
  std::vector<std::string> rows;
  if (is_song_view(p.key.view)) {
    for (auto& t : library_->tracks(p.key, p.top, kVisibleRows)) rows.push_back(std::move(t.title));
    return rows;
  }
  const auto unknown = p.key.view == View::Artists ? "Unknown Artist" : "Unknown Album";
  for (auto& n : library_->names(p.key, p.top, kVisibleRows)) rows.push_back(or_unknown(std::move(n), unknown));
  return rows;
}

void PlayerUi::draw_list(render::DrawList& dl, float width) const {
  const auto& p = here();
  const bool opens = !is_song_view(p.key.view);
  const auto rows = visible_rows();
  for (std::size_t i = 0; i < rows.size(); ++i)
    draw_row(dl, width - 6, i, p.top + i == p.selected, rows[i], opens);
}

void PlayerUi::draw_now_playing(render::DrawList& dl, float width) const {
  const auto status = player_.status();
  const auto& queue = player_.queue();
  if (queue.empty() || status.index >= queue.size()) return centred_note(dl, width, 120, "Nothing playing");
  const auto& track = queue[status.index];

  dl.filled_rect(rect(14, 40, 100, 100), kCover);
  dl.rect_outline(rect(14, 40, 100, 100), kRule);
  const float x = 128, w = width - x - kPad;
  clipped_text(dl, track.title, rect(x, 44, w, 22), 16.f, kInk, kBold);
  clipped_text(dl, or_unknown(track.artist, "Unknown Artist"), rect(x, 70, w, 20), kText, kInk);
  clipped_text(dl, or_unknown(track.album, "Unknown Album"), rect(x, 92, w, 20), kText, kGreyInk);
  text(dl, std::format("{} of {}", status.index + 1, queue.size()), x, 116, kSmallText, kGreyInk);
  if (!status.message.empty()) clipped_text(dl, status.message, rect(14, 146, width - 28, 18), kSmallText, kGreyInk);

  const auto fraction = status.duration_s > 0 ? static_cast<float>(status.position_s / status.duration_s) : 0.f;
  progress_bar(dl, rect(14, 168, width - 28, 8), fraction, kHighlight);
  text(dl, clock(status.position_s), 14, 180, kSmallText, kInk);
  text(dl, "-" + clock(status.duration_s - status.position_s), width - 54, 180, kSmallText, kInk);
  progress_bar(dl, rect(14, 222, width - 28, 4), static_cast<float>(volume_) / kVolumeSteps, kRule);
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

void WheelWidget::view(app::ViewBuilder& vb) {
  vb.canvas(*this).min_size(Width{200}).min_size(Height{200});
}

}  // namespace player
