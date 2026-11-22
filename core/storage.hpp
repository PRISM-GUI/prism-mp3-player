#pragma once
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace player {

struct DirEntry {
  std::string name;
  bool is_dir;
  std::uint64_t size;
  std::int64_t mtime;
};

class File {
public:
  virtual ~File() = default;
  virtual std::uint64_t size() const = 0;
  virtual std::size_t read(std::uint64_t offset, std::span<std::byte> out) = 0;
};

class Storage {
public:
  virtual ~Storage() = default;
  virtual std::vector<DirEntry> list(std::string_view dir) = 0;
  virtual std::unique_ptr<File> open(std::string_view path) = 0;
  virtual std::unique_ptr<std::ostream> create(std::string_view path) = 0;
  // Moves `from` to `to`, replacing `to` if it exists. False when `from` is missing or the move fails.
  virtual bool rename(std::string_view from, std::string_view to) = 0;
};

}  // namespace player
