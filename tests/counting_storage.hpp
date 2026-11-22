#pragma once
#include "core/storage.hpp"

#include <memory>
#include <string>
#include <vector>

namespace player::testing {

class CountingFile final : public File {
public:
  CountingFile(std::unique_ptr<File> inner, std::size_t& bytes) : inner_(std::move(inner)), bytes_(bytes) {}
  std::uint64_t size() const override { return inner_->size(); }
  std::size_t read(std::uint64_t offset, std::span<std::byte> out) override {
    const auto n = inner_->read(offset, out);
    bytes_ += n;
    return n;
  }

private:
  std::unique_ptr<File> inner_;
  std::size_t& bytes_;
};

// Counts the bytes read through it and logs the paths opened.
class CountingStorage final : public Storage {
public:
  explicit CountingStorage(Storage& inner) : inner_(inner) {}
  std::vector<DirEntry> list(std::string_view dir) override { return inner_.list(dir); }
  std::unique_ptr<File> open(std::string_view path) override {
    opened.emplace_back(path);
    auto f = inner_.open(path);
    return f ? std::make_unique<CountingFile>(std::move(f), bytes_read) : nullptr;
  }
  std::unique_ptr<std::ostream> create(std::string_view path) override { return inner_.create(path); }
  bool rename(std::string_view from, std::string_view to) override { return inner_.rename(from, to); }
  std::size_t bytes_read = 0;
  std::vector<std::string> opened;

private:
  Storage& inner_;
};

}  // namespace player::testing
