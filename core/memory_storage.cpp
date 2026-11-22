#include "core/memory_storage.hpp"

#include <algorithm>
#include <functional>
#include <ostream>
#include <set>
#include <sstream>

namespace player {
namespace {

class MemoryFile final : public File {
public:
  explicit MemoryFile(std::vector<std::byte> data) : data_(std::move(data)) {}
  std::uint64_t size() const override { return data_.size(); }
  std::size_t read(std::uint64_t offset, std::span<std::byte> out) override {
    if (offset >= data_.size()) return 0;
    const auto n = std::min<std::size_t>(out.size(), data_.size() - offset);
    std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(offset), n, out.begin());
    return n;
  }

private:
  std::vector<std::byte> data_;
};

class CommitOnClose final : public std::ostringstream {
public:
  explicit CommitOnClose(std::function<void(std::string)> commit) : commit_(std::move(commit)) {}
  ~CommitOnClose() override { commit_(str()); }

private:
  std::function<void(std::string)> commit_;
};

std::vector<std::byte> to_bytes(const std::string& s) {
  const auto* p = reinterpret_cast<const std::byte*>(s.data());
  return {p, p + s.size()};
}

std::string dir_prefix(std::string_view dir) {
  std::string prefix{dir};
  if (prefix.empty() || prefix.back() != '/') prefix += '/';
  return prefix;
}

}  // namespace

void MemoryStorage::add(std::string path, std::vector<std::byte> data, std::int64_t mtime) {
  files_.insert_or_assign(std::move(path), Entry{std::move(data), mtime});
}

std::vector<DirEntry> MemoryStorage::list(std::string_view dir) {
  const auto prefix = dir_prefix(dir);
  std::vector<DirEntry> out;
  std::set<std::string> seen_dirs;
  for (auto it = files_.lower_bound(prefix); it != files_.end() && it->first.starts_with(prefix);
       ++it) {
    const auto rest = std::string_view{it->first}.substr(prefix.size());
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos)
      out.push_back({std::string{rest}, false, it->second.data.size(), it->second.mtime});
    else if (seen_dirs.emplace(rest.substr(0, slash)).second)
      out.push_back({std::string{rest.substr(0, slash)}, true, 0, 0});
  }
  return out;
}

std::unique_ptr<File> MemoryStorage::open(std::string_view path) {
  const auto it = files_.find(path);
  if (it == files_.end()) return nullptr;
  return std::make_unique<MemoryFile>(it->second.data);
}

std::unique_ptr<std::ostream> MemoryStorage::create(std::string_view path) {
  return std::make_unique<CommitOnClose>(
      [this, p = std::string{path}](std::string content) { add(p, to_bytes(content)); });
}

}  // namespace player
