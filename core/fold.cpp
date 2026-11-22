#include "core/fold.hpp"

#include <iterator>
#include <cstdint>
#include <numeric>

namespace player {
namespace {

// simplify: the fold covers Latin-1 Supplement and Latin Extended-A only, compared byte by byte,
// not full Unicode collation (UCA/ICU): Greek, Cyrillic or CJK keep their code point order and
// their case. Add a real collation table when a card with such names needs a natural order.
struct Run {
  int count;
  std::string_view base;  // empty: the character is not a letter and stays as it is
};

constexpr char32_t first_folded = 0xC0;

// U+00C0 to U+017F, in code point order, as runs of characters sharing one base.
constexpr Run runs[] = {
    {6, "a"}, {1, "ae"}, {1, "c"}, {4, "e"}, {4, "i"}, {1, "d"}, {1, "n"}, {5, "o"}, {1, ""},
    {1, "o"}, {4, "u"},  {1, "y"}, {1, "th"}, {1, "ss"},                         // U+00C0-U+00DF
    {6, "a"}, {1, "ae"}, {1, "c"}, {4, "e"}, {4, "i"}, {1, "d"}, {1, "n"}, {5, "o"}, {1, ""},
    {1, "o"}, {4, "u"},  {1, "y"}, {1, "th"}, {1, "y"},                          // U+00E0-U+00FF
    {6, "a"}, {8, "c"},  {4, "d"}, {10, "e"}, {8, "g"}, {4, "h"}, {10, "i"}, {2, "ij"},
    {2, "j"}, {3, "k"},  {10, "l"}, {9, "n"}, {6, "o"}, {2, "oe"}, {6, "r"}, {8, "s"},
    {6, "t"}, {12, "u"}, {2, "w"}, {3, "y"}, {6, "z"}, {1, "s"},                 // U+0100-U+017F
};

static_assert(std::accumulate(std::begin(runs), std::end(runs), 0,
                              [](int n, const Run& r) { return n + r.count; }) == 0x180 - 0xC0);

std::string_view base_of(char32_t cp) {
  int i = static_cast<int>(cp - first_folded);
  for (const auto& r : runs) {
    if (i < r.count) return r.base;
    i -= r.count;
  }
  return {};
}

// U+00C0-U+017F are exactly the two-byte sequences led by 0xC3, 0xC4 and 0xC5.
bool is_foldable_pair(std::string_view s, std::size_t i) {
  const auto lead = static_cast<unsigned char>(s[i]);
  return lead >= 0xC3 && lead <= 0xC5 && i + 1 < s.size() &&
         (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80;
}

char32_t decode_pair(std::string_view s, std::size_t i) {
  return (static_cast<char32_t>(s[i] & 0x1F) << 6) | static_cast<char32_t>(s[i + 1] & 0x3F);
}

char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

}  // namespace

std::string fold_for_sort(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    const auto base = is_foldable_pair(s, i) ? base_of(decode_pair(s, i)) : std::string_view{};
    if (!base.empty()) {
      out += base;
      i += 2;
    } else {
      out += ascii_lower(s[i]);
      ++i;
    }
  }
  return out;
}

std::string sort_key(std::string_view s) {
  // 0xFF never starts valid UTF-8, so it orders after every name.
  if (s.empty()) return "\xff";
  auto key = fold_for_sort(s);
  key += '\0';
  key += s;
  return key;
}

}  // namespace player
