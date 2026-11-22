#pragma once
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

// A fresh directory (with music/x inside) under the system temp dir, unique per process and per
// instance so parallel test processes never collide; removed on destruction.
struct TempDir {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
                               ("player_test_" + std::to_string(::getpid()) + "_" +
                                std::to_string(next_id()));
  TempDir() { std::filesystem::create_directories(path / "music" / "x"); }
  ~TempDir() { std::filesystem::remove_all(path); }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  void write(const std::filesystem::path& rel, const std::vector<std::byte>& data) const {
    std::ofstream(path / rel, std::ios::binary)
        .write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
  }

private:
  static int next_id() {
    static std::atomic<int> counter{0};
    return counter++;
  }
};
