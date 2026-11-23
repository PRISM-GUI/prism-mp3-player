#pragma once
#include "tests/tag_builder.hpp"

#include <doctest.h>

#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace player::testing {

// A file from tests/data, read whole: the decoder tests need real audio, not fake_audio().
inline Bytes load(std::string_view name) {
  std::ifstream in{std::string{TEST_DATA_DIR} + "/" + std::string{name}, std::ios::binary};
  REQUIRE(in);
  std::vector<char> chars{std::istreambuf_iterator<char>{in}, {}};
  const auto* p = reinterpret_cast<const std::byte*>(chars.data());
  return {p, p + chars.size()};
}

}  // namespace player::testing
