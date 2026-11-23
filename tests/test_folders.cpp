#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core/folders.hpp"
#include "core/memory_storage.hpp"
#include "tests/library_fixtures.hpp"
#include "tests/tag_builder.hpp"

#include <doctest.h>

#include <format>
#include <string>
#include <vector>

using namespace player;
using namespace player::testing;

namespace {

std::vector<std::string> labels(const std::vector<FolderRow>& rows) {
  std::vector<std::string> names;
  for (const auto& row : rows) names.push_back(row.label);
  return names;
}

}  // namespace

TEST_CASE("folder_rows lists subfolders first, then the mp3 files, skipping what the scan skips") {
  MemoryStorage card;
  card.add("/Album/zeta.mp3", fake_audio(16));
  card.add("/Album/Beta.mp3", fake_audio(16));
  card.add("/Album/alpha.mp3", fake_audio(16));
  card.add("/Album/Gamma.MP3", fake_audio(16));
  card.add("/Album/Sub/deep.mp3", fake_audio(16));
  card.add("/Album/System Volume Information/junk.mp3", fake_audio(16));
  card.add("/Album/$RECYCLE.BIN/junk.mp3", fake_audio(16));
  card.add("/Album/.hidden.mp3", fake_audio(16));
  card.add("/Album/notes.txt", bytes_of("not a song"));

  const auto rows = folder_rows(card, "/Album");
  CHECK(labels(rows) == std::vector<std::string>{"Sub/", "alpha.mp3", "Beta.mp3", "Gamma.MP3", "zeta.mp3"});
  REQUIRE(rows.size() == 5U);
  CHECK(rows[0].folder);  // a subfolder keeps its trailing '/' and opens
  CHECK_FALSE(rows[1].folder);
}

TEST_CASE("folder_queue starts at the selected file and titles its neighbours from their tags") {
  MemoryStorage card;
  add_song(card, "/Album/a.mp3", "A", "abba", "Gold", 1);
  add_song(card, "/Album/b.mp3", "B", "abba", "Gold", 2);
  add_song(card, "/Album/c.mp3", "C", "abba", "Gold", 3);
  card.add("/Album/Sub/d.mp3", tagged_song("D", "abba", "Gold", 4));

  const auto rows = folder_rows(card, "/Album");
  REQUIRE(labels(rows) == std::vector<std::string>{"Sub/", "a.mp3", "b.mp3", "c.mp3"});
  const auto queued = folder_queue(card, "/Album", rows, 2);  // b.mp3
  REQUIRE(queued.has_value());
  CHECK(queued->start == 1U);
  REQUIRE(queued->tracks.size() == 3U);  // the subfolder is never in the queue
  CHECK(queued->tracks.front().path == "/Album/a.mp3");
  CHECK(queued->tracks.front().title == "A");
  CHECK(queued->tracks[queued->start].title == "B");
  CHECK(queued->tracks[queued->start].artist == "abba");
  CHECK(queued->tracks.back().title == "C");
}

TEST_CASE("a folder bigger than the tag window names its far rows by their file") {
  MemoryStorage card;
  constexpr int kSongs = 40;
  for (int n = 0; n < kSongs; ++n)
    add_song(card, std::format("/Album/{:02}.mp3", n), std::format("Track {:02}", n), "Band", "Big", n + 1);

  const auto rows = folder_rows(card, "/Album");
  REQUIRE(rows.size() == 40U);
  const auto queued = folder_queue(card, "/Album", rows, 20);
  REQUIRE(queued.has_value());
  CHECK(queued->start == 20U);
  REQUIRE(queued->tracks.size() == 40U);
  CHECK(queued->tracks[20].title == "Track 20");  // near the selection: its tags were read
  CHECK(queued->tracks.front().title == "00");    // far from it: its file name
  CHECK(queued->tracks.back().title == "39");
}

TEST_CASE("a subfolder row queues nothing") {
  MemoryStorage card;
  add_song(card, "/Album/a.mp3", "A", "abba", "Gold", 1);
  card.add("/Album/Sub/b.mp3", tagged_song("B", "abba", "Gold", 2));

  const auto rows = folder_rows(card, "/Album");
  REQUIRE(rows.size() == 2U);
  REQUIRE(rows[0].folder);
  CHECK_FALSE(folder_queue(card, "/Album", rows, 0).has_value());
}
