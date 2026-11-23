#pragma once
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
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

// Exactly `n` bytes at `offset`, or nothing: a range past the end of the file or a short read is
// not something a tag or a picture parser can use half of.
inline std::optional<std::vector<std::byte>> read_exact(File& file, std::uint64_t offset,
                                                        std::uint64_t n) {
  if (offset > file.size() || n > file.size() - offset) return std::nullopt;
  std::vector<std::byte> bytes(static_cast<std::size_t>(n));
  if (file.read(offset, bytes) != bytes.size()) return std::nullopt;
  return bytes;
}

}  // namespace player
