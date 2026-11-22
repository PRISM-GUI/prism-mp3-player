#pragma once
#include <string>
#include <string_view>

namespace player {

// Lowercases ASCII and strips the accents of Latin-1 Supplement and Latin Extended-A letters
// (É→e, Ø→o, ß→ss, Œ→oe), for case- and accent-insensitive sorting. Other text is unchanged.
std::string fold_for_sort(std::string_view s);

// A key whose plain byte order is the library's order: folded name first, the exact name to
// break ties, and the empty name last.
std::string sort_key(std::string_view s);

}  // namespace player
