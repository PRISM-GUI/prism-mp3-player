#pragma once
#include "core/storage.hpp"

#include <map>

namespace player {

class MemoryStorage final : public Storage {
public:
  void add(std::string path, std::vector<std::byte> data, std::int64_t mtime = 0);

  std::vector<DirEntry> list(std::string_view dir) override;
  std::unique_ptr<File> open(std::string_view path) override;
  std::unique_ptr<std::ostream> create(std::string_view path) override;
  bool rename(std::string_view from, std::string_view to) override;

private:
  struct Entry {
    std::shared_ptr<const std::vector<std::byte>> data;
    std::int64_t mtime;
  };
  std::map<std::string, Entry, std::less<>> files_;
};

}  // namespace player
