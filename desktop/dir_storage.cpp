#include "desktop/dir_storage.hpp"

#include <chrono>
#include <fstream>

namespace player {
namespace fs = std::filesystem;
namespace {

class StdFile final : public File {
public:
  StdFile(std::ifstream in, std::uint64_t size) : in_(std::move(in)), size_(size) {}
  std::uint64_t size() const override { return size_; }
  std::size_t read(std::uint64_t offset, std::span<std::byte> out) override {
    in_.clear();
    in_.seekg(static_cast<std::streamoff>(offset));
    in_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<std::size_t>(in_.gcount());
  }

private:
  std::ifstream in_;
  std::uint64_t size_;
};

std::int64_t unix_seconds(fs::file_time_type t) {
  const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
  return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

DirEntry to_entry(const fs::directory_entry& e) {
  const bool is_dir = e.is_directory();
  return {e.path().filename().string(), is_dir, is_dir ? 0 : e.file_size(),
          is_dir ? 0 : unix_seconds(e.last_write_time())};
}

}  // namespace

fs::path DirStorage::resolve(std::string_view path) const {
  while (path.starts_with('/')) path.remove_prefix(1);
  return root_ / path;
}

std::vector<DirEntry> DirStorage::list(std::string_view dir) {
  std::error_code ec;
  std::vector<DirEntry> out;
  for (fs::directory_iterator it{resolve(dir), ec}; !ec && it != fs::directory_iterator{};
       it.increment(ec))
    out.push_back(to_entry(*it));
  return out;
}

std::unique_ptr<File> DirStorage::open(std::string_view path) {
  const auto p = resolve(path);
  std::error_code ec;
  if (!fs::is_regular_file(p, ec)) return nullptr;
  std::ifstream in{p, std::ios::binary};
  if (!in) return nullptr;
  return std::make_unique<StdFile>(std::move(in), fs::file_size(p, ec));
}

std::unique_ptr<std::ostream> DirStorage::create(std::string_view path) {
  const auto p = resolve(path);
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  auto out = std::make_unique<std::ofstream>(p, std::ios::binary | std::ios::trunc);
  if (!*out) return nullptr;
  return out;
}

}  // namespace player
