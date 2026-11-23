#pragma once
#include "tests/forwarding_storage.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
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
class CountingStorage final : public ForwardingStorage {
public:
  explicit CountingStorage(Storage& inner) : ForwardingStorage(inner) {}
  std::unique_ptr<File> open(std::string_view path) override {
    opened.emplace_back(path);
    auto f = inner_.open(path);
    return f ? std::make_unique<CountingFile>(std::move(f), bytes_read) : nullptr;
  }
  std::size_t bytes_read = 0;
  std::vector<std::string> opened;
};

}  // namespace player::testing
