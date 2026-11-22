#pragma once
#include "core/storage.hpp"

#include <filesystem>

namespace player {

class DirStorage final : public Storage {
public:
  explicit DirStorage(std::filesystem::path root) : root_(std::move(root)) {}

  std::vector<DirEntry> list(std::string_view dir) override;
  std::unique_ptr<File> open(std::string_view path) override;
  std::unique_ptr<std::ostream> create(std::string_view path) override;
  bool rename(std::string_view from, std::string_view to) override;

private:
  std::filesystem::path resolve(std::string_view path) const;
  std::filesystem::path root_;
};

}  // namespace player
