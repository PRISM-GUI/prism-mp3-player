#include "core/tags.hpp"

#include "prism/core/utf8.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <span>
#include <vector>

namespace player {
namespace {

using Bytes = std::span<const std::byte>;

constexpr std::size_t kHeaderSize = 10;
constexpr std::uint64_t kMaxTagRead = 1 << 20;
constexpr std::size_t kV1Size = 128;
constexpr std::size_t kMaxFieldBytes = 256;

unsigned u8(std::byte b) { return std::to_integer<unsigned>(b); }

std::optional<std::vector<std::byte>> read_exact(File& f, std::uint64_t offset, std::uint64_t n) {
  if (offset > f.size() || n > f.size() - offset) return std::nullopt;
  std::vector<std::byte> buf(n);
  if (f.read(offset, buf) != n) return std::nullopt;
  return buf;
}

std::uint32_t be32(Bytes b) { return u8(b[0]) << 24 | u8(b[1]) << 16 | u8(b[2]) << 8 | u8(b[3]); }

std::optional<std::uint32_t> synchsafe(Bytes b) {
  if (std::ranges::any_of(b.first(4), [](std::byte x) { return u8(x) >= 0x80; })) return std::nullopt;
  return u8(b[0]) << 21 | u8(b[1]) << 14 | u8(b[2]) << 7 | u8(b[3]);
}

void append_utf8(std::string& out, char32_t c) {
  if (c < 0x80) {
    out += static_cast<char>(c);
  } else if (c < 0x800) {
    out += static_cast<char>(0xc0 | c >> 6);
    out += static_cast<char>(0x80 | (c & 0x3f));
  } else if (c < 0x10000) {
    out += static_cast<char>(0xe0 | c >> 12);
    out += static_cast<char>(0x80 | (c >> 6 & 0x3f));
    out += static_cast<char>(0x80 | (c & 0x3f));
  } else {
    out += static_cast<char>(0xf0 | c >> 18);
    out += static_cast<char>(0x80 | (c >> 12 & 0x3f));
    out += static_cast<char>(0x80 | (c >> 6 & 0x3f));
    out += static_cast<char>(0x80 | (c & 0x3f));
  }
}

std::string latin1_to_utf8(Bytes b) {
  std::string out;
  for (const auto c : b) append_utf8(out, u8(c));
  return out;
}

std::string utf8_or_latin1(Bytes b) {
  const auto* p = reinterpret_cast<const char*>(b.data());
  const std::string_view raw{p, b.size()};
  const auto text = raw.substr(0, raw.find('\0'));
  return prism::core::utf8::sanitize(text) == text ? std::string{text} : latin1_to_utf8(b);
}

std::string utf16_to_utf8(Bytes b, bool big_endian) {
  std::vector<char16_t> units;
  for (std::size_t i = 0; i + 1 < b.size(); i += 2) {
    const auto hi = big_endian ? u8(b[i]) : u8(b[i + 1]);
    const auto lo = big_endian ? u8(b[i + 1]) : u8(b[i]);
    units.push_back(static_cast<char16_t>(hi << 8 | lo));
  }
  std::string out;
  for (std::size_t i = 0; i < units.size(); ++i) {
    const char32_t u = units[i];
    const bool high = u >= 0xd800 && u < 0xdc00;
    const bool low_next = i + 1 < units.size() && units[i + 1] >= 0xdc00 && units[i + 1] < 0xe000;
    if (high && low_next) {
      append_utf8(out, 0x10000 + ((u - 0xd800) << 10) + (units[i + 1] - 0xdc00));
      ++i;
    } else {
      append_utf8(out, u >= 0xd800 && u < 0xe000 ? 0xfffd : u);
    }
  }
  return out;
}

std::string utf16_with_bom_to_utf8(Bytes b) {
  const bool big_endian = b.size() >= 2 && u8(b[0]) == 0xfe && u8(b[1]) == 0xff;
  const bool has_bom = big_endian || (b.size() >= 2 && u8(b[0]) == 0xff && u8(b[1]) == 0xfe);
  return utf16_to_utf8(has_bom ? b.subspan(2) : b, big_endian);
}

bool is_wide(unsigned encoding) { return encoding == 1 || encoding == 2; }

std::optional<std::string> decode_text(unsigned encoding, Bytes b) {
  switch (encoding) {
    case 0: return latin1_to_utf8(b);
    case 1: return utf16_with_bom_to_utf8(b);
    case 2: return utf16_to_utf8(b, true);
    case 3: return utf8_or_latin1(b);
    default: return std::nullopt;
  }
}

std::string up_to_nul(std::string s) {
  s.resize(std::min(s.find('\0'), s.size()));
  return s;
}

std::string capped(std::string s) {
  s.resize(prism::core::utf8::snap(s, kMaxFieldBytes));
  return s;
}

std::optional<std::string> text_frame_value(Bytes body) {
  if (body.empty()) return std::nullopt;
  return decode_text(u8(body[0]), body.subspan(1)).transform(up_to_nul).transform(capped);
}

int leading_number(const std::string& s) {
  int n = 0;
  std::from_chars(s.data(), s.data() + s.size(), n);
  return std::max(n, 0);
}

std::size_t terminator_end(Bytes b, std::size_t from, bool wide) {
  const std::size_t step = wide ? 2 : 1;
  for (std::size_t i = from; i + step <= b.size(); i += step)
    if (u8(b[i]) == 0 && (!wide || u8(b[i + 1]) == 0)) return i + step;
  return b.size();
}

std::optional<Cover> apic_cover(Bytes body, std::uint64_t body_offset) {
  if (body.empty()) return std::nullopt;
  const auto mime_end = terminator_end(body, 1, false);
  if (mime_end >= body.size()) return std::nullopt;
  const auto description_start = mime_end + 1;
  const auto data_start = terminator_end(body, description_start, is_wide(u8(body[0])));
  if (data_start >= body.size()) return std::nullopt;
  return Cover{body_offset + data_start, static_cast<std::uint32_t>(body.size() - data_start),
               latin1_to_utf8(body.subspan(1, mime_end - 2))};
}

struct Frame {
  std::string id;
  Bytes body;
  std::uint64_t offset;  // of the body, in the tag buffer
};

struct FrameFlags {
  bool unusable;  // compressed, encrypted or unsynchronised: not read
  bool has_data_length;
};

FrameFlags frame_flags(int version, Bytes flags) {
  const auto format = u8(flags[1]);
  if (version == 4) return {(format & 0x0e) != 0, (format & 0x01) != 0};
  return {(format & 0xc0) != 0, false};
}

bool is_frame_id(Bytes id) {
  return std::ranges::all_of(id.first(4), [](std::byte b) {
    const auto c = u8(b);
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
  });
}

bool frame_boundary(Bytes tag, std::size_t pos) {
  return pos + kHeaderSize > tag.size() || u8(tag[pos]) == 0 || is_frame_id(tag.subspan(pos, 4));
}

// Old iTunes wrote plain sizes in v2.4 tags: the synchsafe reading is kept only when it lands on a frame.
std::optional<std::uint32_t> frame_size(int version, Bytes tag, std::size_t pos) {
  const auto size_bytes = tag.subspan(pos + 4, 4);
  const auto body_start = pos + kHeaderSize;
  const auto fits = [&](std::uint32_t s) { return s <= tag.size() - body_start; };
  const auto synchsafe_size = version == 4 ? synchsafe(size_bytes) : std::nullopt;
  if (synchsafe_size && fits(*synchsafe_size) && frame_boundary(tag, body_start + *synchsafe_size))
    return synchsafe_size;
  const auto plain = be32(size_bytes);
  return fits(plain) ? std::optional{plain} : std::nullopt;
}

std::size_t extended_header_size(int version, Bytes tag) {
  if (tag.size() < 4) return tag.size();
  const auto declared = version == 4 ? synchsafe(tag).value_or(tag.size()) : std::uint64_t{be32(tag)} + 4;
  return static_cast<std::size_t>(std::min<std::uint64_t>(declared, tag.size()));
}

std::vector<Frame> split_frames(int version, Bytes tag, std::size_t start) {
  std::vector<Frame> frames;
  for (auto pos = start; pos + kHeaderSize <= tag.size() && u8(tag[pos]) != 0;) {
    const auto size = frame_size(version, tag, pos);
    const auto body_start = pos + kHeaderSize;
    if (!size) break;
    const auto flags = frame_flags(version, tag.subspan(pos + 8, 2));
    const auto skip = flags.has_data_length ? std::min<std::size_t>(4, *size) : 0;
    const auto id = latin1_to_utf8(tag.subspan(pos, 4));
    if (!flags.unusable) frames.push_back({id, tag.subspan(body_start + skip, *size - skip), body_start + skip});
    pos = body_start + *size;
  }
  return frames;
}

void apply_text(Tags& t, const Frame& f) {
  const auto text = text_frame_value(f.body);
  if (!text) return;
  if (f.id == "TIT2") t.title = *text;
  else if (f.id == "TPE1") t.artist = *text;
  else if (f.id == "TALB") t.album = *text;
  else if (f.id == "TRCK") t.track = leading_number(*text);
}

void apply_frame(Tags& t, const Frame& f, std::uint64_t tag_file_offset) {
  if (f.id == "APIC" && t.cover.size == 0)
    t.cover = apic_cover(f.body, tag_file_offset + f.offset).value_or(Cover{});
  else
    apply_text(t, f);
}

struct V2Header {
  int version;
  std::uint8_t flags;
  std::uint32_t size;
};

std::optional<V2Header> parse_v2_header(Bytes h) {
  const bool is_id3 = h[0] == std::byte{'I'} && h[1] == std::byte{'D'} && h[2] == std::byte{'3'};
  const auto size = synchsafe(h.subspan(6, 4));
  if (!is_id3 || !size) return std::nullopt;
  return V2Header{static_cast<int>(u8(h[3])), static_cast<std::uint8_t>(u8(h[5])), *size};
}

std::optional<V2Header> any_v2_header(File& f) {
  const auto h = read_exact(f, 0, kHeaderSize);
  return h ? parse_v2_header(*h) : std::nullopt;
}

std::uint64_t v2_audio_offset(File& f, const V2Header& h) {
  const std::uint64_t footer = (h.version == 4 && (h.flags & 0x10)) ? kHeaderSize : 0;
  return std::min<std::uint64_t>(kHeaderSize + h.size + footer, f.size());
}

Tags tags_from_v2(File& f, const V2Header& h) {
  Tags t;
  t.audio_offset = v2_audio_offset(f, h);
  // simplify: only v2.3 and v2.4 frames are read (v2.2 has 3-character frame IDs); add v2.2 if real files need it.
  if (h.version != 3 && h.version != 4) return t;
  // simplify: a tag-wide unsynchronisation flag means no frames are read; undo it when real files need it.
  if (h.flags & 0x80) return t;
  if (h.size > f.size() - kHeaderSize) return t;
  const auto tag = read_exact(f, kHeaderSize, std::min<std::uint64_t>(h.size, kMaxTagRead));
  if (!tag) return t;
  const auto start = (h.flags & 0x40) ? extended_header_size(h.version, *tag) : 0;
  for (const auto& frame : split_frames(h.version, *tag, start)) apply_frame(t, frame, kHeaderSize);
  return t;
}

std::string trimmed_latin1(Bytes field) {
  auto s = up_to_nul(latin1_to_utf8(field));
  s.erase(s.find_last_not_of(' ') + 1);
  return s;
}

std::optional<Tags> tags_from_v1(File& f) {
  if (f.size() < kV1Size) return std::nullopt;
  const auto b = read_exact(f, f.size() - kV1Size, kV1Size);
  if (!b || b->at(0) != std::byte{'T'} || b->at(1) != std::byte{'A'} || b->at(2) != std::byte{'G'})
    return std::nullopt;
  const Bytes v1{*b};
  const bool has_track = u8(v1[125]) == 0 && u8(v1[126]) != 0;
  Tags t;
  t.title = trimmed_latin1(v1.subspan(3, 30));
  t.artist = trimmed_latin1(v1.subspan(33, 30));
  t.album = trimmed_latin1(v1.subspan(63, 30));
  t.track = has_track ? static_cast<int>(u8(v1[126])) : 0;
  return t;
}

Tags merge_v1_under_v2(Tags v2, const Tags& v1) {
  v2.title = v1.title;
  if (v2.artist.empty()) v2.artist = v1.artist;
  if (v2.album.empty()) v2.album = v1.album;
  if (v2.track == 0) v2.track = v1.track;
  return v2;
}

std::string_view without_extension(std::string_view name) {
  const auto dot = name.rfind('.');
  return dot == 0 || dot == std::string_view::npos ? name : name.substr(0, dot);
}

std::string filename_title(std::string_view file_name) {
  const auto base = without_extension(file_name);
  const Bytes raw{reinterpret_cast<const std::byte*>(base.data()), base.size()};
  return capped(utf8_or_latin1(raw));
}

Tags parse(File& f) {
  const auto header = any_v2_header(f);
  auto tags = header ? tags_from_v2(f, *header) : Tags{};
  if (!tags.title.empty()) return tags;
  const auto v1 = tags_from_v1(f);
  return v1 ? merge_v1_under_v2(std::move(tags), *v1) : tags;
}

}  // namespace

Tags read_tags(File& file, std::string_view file_name) {
  Tags tags;
  try {
    tags = parse(file);
  } catch (...) {
    tags = Tags{};
  }
  if (tags.title.empty()) tags.title = filename_title(file_name);
  return tags;
}

}  // namespace player
