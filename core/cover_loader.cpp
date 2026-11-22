#include "core/cover_loader.hpp"

#include "core/fold.hpp"
#include "core/index_format.hpp"
#include "core/tags.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace player {
namespace {

using Bytes = std::vector<std::byte>;

// Tried in this order whatever their extension: the names an album's picture usually has.
constexpr std::array<std::string_view, 4> kPreferredNames{"cover", "folder", "front", "album"};
constexpr std::array<std::string_view, 3> kPictureExtensions{".jpg", ".jpeg", ".png"};

std::string parent_of(std::string_view path) {
  const auto slash = path.find_last_of('/');
  if (slash == std::string_view::npos) return {};
  return slash == 0 ? std::string{"/"} : std::string{path.substr(0, slash)};
}

bool is_picture(std::string_view name) {
  const auto folded = fold_for_sort(name);
  return std::ranges::any_of(kPictureExtensions, [&folded](std::string_view ext) { return folded.ends_with(ext); });
}

// 0 for cover, 1 for folder, … ; past the end for a name that is not one of them.
std::size_t preferred_rank(std::string_view name) {
  const auto folded = fold_for_sort(name);
  const auto dot = folded.rfind('.');
  const auto stem = std::string_view{folded}.substr(0, dot == std::string::npos ? folded.size() : dot);
  const auto found = std::ranges::find(kPreferredNames, stem);
  return static_cast<std::size_t>(found - kPreferredNames.begin());
}

// Exactly `size` bytes at `offset`, or nothing: a short read means the file is not what its tags say.
std::optional<Bytes> read_bytes(File& file, std::uint64_t offset, std::uint64_t size) {
  if (size == 0 || size > kMaxCoverBytes) return std::nullopt;
  Bytes bytes(static_cast<std::size_t>(size));
  if (file.read(offset, bytes) != bytes.size()) return std::nullopt;
  return bytes;
}

// The picture to use for a folder: a preferred name first, else the first name in folded order.
std::string best_picture(const std::vector<DirEntry>& entries) {
  std::vector<std::string> candidates;
  for (const auto& entry : entries)
    if (!entry.is_dir && is_picture(entry.name)) candidates.push_back(entry.name);
  if (candidates.empty()) return {};
  std::ranges::sort(candidates, [](const std::string& a, const std::string& b) {
    if (const auto rank_a = preferred_rank(a), rank_b = preferred_rank(b); rank_a != rank_b) return rank_a < rank_b;
    return fold_for_sort(a) < fold_for_sort(b);
  });
  return candidates.front();
}

}  // namespace

CoverLoader::CoverLoader(Storage& storage, CoverDecoder decoder, int max_side)
    : storage_(storage), decoder_(std::move(decoder)), max_side_(max_side),
      thread_([this](std::stop_token stop) { run(stop); }) {}

CoverLoader::~CoverLoader() {
  thread_.request_stop();
  wake_.notify_all();
}

void CoverLoader::request(std::string path) {
  const std::lock_guard lock(mutex_);
  if (loaded_ && path == loaded_path_) return;
  if (path == active_) return;
  if (wanted_ && *wanted_ == path) return;
  wanted_ = std::move(path);
  wake_.notify_one();
}

std::optional<std::shared_ptr<const prism::render::Image>> CoverLoader::cover(std::string_view path) const {
  const std::lock_guard lock(mutex_);
  if (!loaded_ || path != loaded_path_) return std::nullopt;
  return loaded_image_;
}

std::uint64_t CoverLoader::revision() const {
  const std::lock_guard lock(mutex_);
  return revision_;
}

void CoverLoader::run(std::stop_token stop) {
  while (!stop.stop_requested()) {
    std::string path;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, stop, [this] { return wanted_.has_value(); });
      if (stop.stop_requested()) return;
      path = std::move(*wanted_);
      wanted_.reset();
      active_ = path;
    }
    auto image = load(path);  // slow: file reads and a decode, with no lock held
    {
      const std::lock_guard lock(mutex_);
      active_.clear();
      loaded_path_ = path;
      loaded_image_ = std::move(image);
      loaded_ = true;
      ++revision_;
    }
  }
}

std::shared_ptr<const prism::render::Image> CoverLoader::load(std::string_view path) {
  auto file = storage_.open(path);
  if (!file) return nullptr;
  const auto tags = read_tags(*file, path);
  if (const auto bytes = read_bytes(*file, tags.cover.offset, tags.cover.size))
    if (auto image = decoder_(*bytes, max_side_)) return image;
  // No cover in the tags, or one too big, short or refused: the folder's picture stands in for it.
  return folder_picture(path);
}

std::shared_ptr<const prism::render::Image> CoverLoader::folder_picture(std::string_view track_path) {
  const auto dir = parent_of(track_path);
  if (folder_loaded_ && dir == folder_path_) return folder_image_;
  auto image = [this, &dir] {
    const auto name = best_picture(storage_.list(dir));
    if (name.empty()) return std::shared_ptr<const prism::render::Image>{};
    auto file = storage_.open(index_format::join(dir, name));
    if (!file) return std::shared_ptr<const prism::render::Image>{};
    // simplify: one folder picture per track: a preferred name that fails to decode is not replaced
    // by another picture of the same folder. Upgrade: walk the candidates in order.
    const auto bytes = read_bytes(*file, 0, file->size());
    return bytes ? decoder_(*bytes, max_side_) : nullptr;
  }();
  folder_path_ = dir;
  folder_image_ = image;
  folder_loaded_ = true;  // even when it is null: the folder has no usable picture
  return image;
}

}  // namespace player
