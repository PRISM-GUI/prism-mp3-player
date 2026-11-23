#pragma once
#include "core/storage.hpp"

#include <memory>
#include <string_view>
#include <vector>

namespace player::testing {

// A Storage that forwards all four calls to the one it wraps: the base for the test decorators,
// which then override `open` alone.
class ForwardingStorage : public Storage {
public:
  explicit ForwardingStorage(Storage& inner) : inner_(inner) {}
  std::vector<DirEntry> list(std::string_view dir) override { return inner_.list(dir); }
  std::unique_ptr<File> open(std::string_view path) override { return inner_.open(path); }
  std::unique_ptr<std::ostream> create(std::string_view path) override { return inner_.create(path); }
  bool rename(std::string_view from, std::string_view to) override { return inner_.rename(from, to); }

protected:
  Storage& inner_;
};

}  // namespace player::testing
