#pragma once
#include <string>
#include <string_view>

// Card paths travel as std::string in the index and in Storage; these are the three shapes every
// caller needs, in one place so they cannot drift apart.
namespace player {

inline std::string join(std::string_view dir, std::string_view name) {
  std::string out{dir};
  if (out.empty() || out.back() != '/') out += '/';
  out += name;
  return out;
}

// The part of `path` after its last '/', or the whole of it when there is none.
inline std::string_view file_name(std::string_view path) {
  const auto slash = path.rfind('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// The folder holding `path`: "/" for a name at the card's root, empty for a name with no folder.
inline std::string parent_of(std::string_view path) {
  const auto slash = path.find_last_of('/');
  if (slash == std::string_view::npos) return {};
  return slash == 0 ? std::string{"/"} : std::string{path.substr(0, slash)};
}

}  // namespace player
