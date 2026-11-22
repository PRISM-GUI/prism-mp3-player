#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace player::testing {

using Bytes = std::vector<std::byte>;

inline Bytes bytes_of(std::string_view s) {
  Bytes b;
  for (const char c : s) b.push_back(static_cast<std::byte>(c));
  return b;
}

inline void append(Bytes& to, const Bytes& from) { to.insert(to.end(), from.begin(), from.end()); }

inline Bytes be32(std::uint32_t n) {
  return {std::byte(n >> 24), std::byte(n >> 16), std::byte(n >> 8), std::byte(n)};
}

inline Bytes synchsafe(std::uint32_t n) {
  return {std::byte((n >> 21) & 0x7f), std::byte((n >> 14) & 0x7f), std::byte((n >> 7) & 0x7f),
          std::byte(n & 0x7f)};
}

inline Bytes utf16_with_bom(std::u16string_view s) {
  Bytes b{std::byte{0xff}, std::byte{0xfe}};
  for (const char16_t c : s) {
    b.push_back(std::byte(c & 0xff));
    b.push_back(std::byte(c >> 8));
  }
  return b;
}

// encoding byte followed by the text bytes
inline Bytes text_payload(std::uint8_t encoding, const Bytes& text) {
  Bytes b{std::byte{encoding}};
  append(b, text);
  return b;
}

inline Bytes latin1_text(std::string_view s) { return text_payload(0, bytes_of(s)); }
inline Bytes utf8_text(std::string_view s) { return text_payload(3, bytes_of(s)); }

inline Bytes apic_payload(std::string_view mime, const Bytes& image) {
  Bytes b{std::byte{0}};
  append(b, bytes_of(mime));
  append(b, Bytes{std::byte{0}, std::byte{3}, std::byte{0}});
  append(b, image);
  return b;
}

// version 3: plain 32-bit size; version 4: synchsafe size
inline Bytes frame(int version, std::string_view id, const Bytes& payload) {
  Bytes b = bytes_of(id);
  append(b, version == 4 ? synchsafe(payload.size()) : be32(payload.size()));
  append(b, Bytes{std::byte{0}, std::byte{0}});
  append(b, payload);
  return b;
}

inline Bytes id3v2_with_size(int version, const Bytes& body, std::uint32_t declared_size,
                             std::uint8_t flags = 0) {
  Bytes b = bytes_of("ID3");
  append(b, Bytes{std::byte(version), std::byte{0}, std::byte{flags}});
  append(b, synchsafe(declared_size));
  append(b, body);
  return b;
}

inline Bytes id3v2(int version, const std::vector<Bytes>& frames, std::size_t padding = 0) {
  Bytes body;
  for (const auto& f : frames) append(body, f);
  body.resize(body.size() + padding, std::byte{0});
  return id3v2_with_size(version, body, body.size());
}

inline Bytes fixed_field(std::string_view s, std::size_t n) {
  Bytes b = bytes_of(s);
  b.resize(n, std::byte{0});
  return b;
}

inline Bytes id3v1(std::string_view title, std::string_view artist, std::string_view album,
                   std::uint8_t track) {
  Bytes b = bytes_of("TAG");
  append(b, fixed_field(title, 30));
  append(b, fixed_field(artist, 30));
  append(b, fixed_field(album, 30));
  append(b, fixed_field("2020", 4));
  append(b, fixed_field("", 28));
  append(b, Bytes{std::byte{0}, std::byte{track}, std::byte{0}});
  return b;
}

inline Bytes fake_audio(std::size_t n) { return Bytes(n, std::byte{0xaa}); }

}  // namespace player::testing
