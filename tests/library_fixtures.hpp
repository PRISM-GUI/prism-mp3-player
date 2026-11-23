#pragma once
#include "core/memory_storage.hpp"
#include "tests/tag_builder.hpp"

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace player::testing {

// A tagged song at `path`. The audio defaults to 16 bytes of noise: enough for the index, not for
// the decoder, so a test that plays it passes the real thing instead.
inline void add_song(MemoryStorage& s, std::string path, std::string_view title, std::string_view artist,
                     std::string_view album, int track, const Bytes& audio = fake_audio(16)) {
  s.add(std::move(path), tagged_song(title, artist, album, track, audio), 1000);
}

// Two artists, three albums, six songs: the shape every browse test walks.
inline void fill_six(MemoryStorage& s) {
  add_song(s, "/music/b/zeta2.mp3", "Second", "Beta Band", "Zeta", 2);
  add_song(s, "/music/b/zeta1.mp3", "First", "Beta Band", "Zeta", 1);
  add_song(s, "/music/b/alpha.mp3", "Only", "Beta Band", "Alpha", 1);
  add_song(s, "/music/a/g3.mp3", "Three", "abba", "Gold", 3);
  add_song(s, "/music/a/g1.mp3", "One", "abba", "Gold", 1);
  add_song(s, "/music/a/g2.mp3", "Two", "abba", "Gold", 2);
}

// `artists` artists of `per_artist` songs each, one folder per artist, titled "Song 00000" upward.
inline void fill_generated(MemoryStorage& s, int artists, int per_artist) {
  for (int a = 0; a < artists; ++a)
    for (int t = 0; t < per_artist; ++t) {
      const int n = a * per_artist + t;
      add_song(s, std::format("/music/{:03}/{:05}.mp3", a, n), std::format("Song {:05}", n),
               std::format("Artist {:03}", a), std::format("Album {:03}", a), t + 1);
    }
}

}  // namespace player::testing
