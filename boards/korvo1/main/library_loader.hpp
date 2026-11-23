#pragma once

// Builds the card's index off the logic thread and hands it over. The logic thread opens the index
// and owns it throughout; the builder gets a plain pointer to ask is_current() (which reads only
// the fingerprint the Library holds, never its file) and holds no Library of its own.

#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace player {

class Library;
class PlayerUi;
class Storage;

// What the builder thread found of the card: the index it was given is current, a new one waits at
// .prism-library.new, or none could be written.
enum class BuildResult { current, staged, failed };

// What the builder thread leaves for the logic thread, merged until it takes it.
struct Handover {
  bool updating = false;
  std::optional<BuildResult> built;
};

class LibraryLoader {
public:
  explicit LibraryLoader(Storage& storage);
  ~LibraryLoader();

  // Each logic loop: the first opens and shows the index and starts the builder, then what the
  // builder left is applied.
  void pump(PlayerUi& ui, std::shared_ptr<Library>& shown);

private:
  // The index the card has today shows at once, even stale: the old one stays usable while the
  // builder makes a new one.
  void begin(PlayerUi& ui, std::shared_ptr<Library>& shown);
  void start(const Library* index);
  void build(const Library* index);

  // Where the hand-over lands, on the logic thread.
  void apply(PlayerUi& ui, std::shared_ptr<Library>& shown, BuildResult result);

  // FatFS cannot replace an index that is open: the UI stops showing the old one, its Library
  // closes it, and only then may the staged file take its place.
  void install(PlayerUi& ui, std::shared_ptr<Library>& shown);
  void show(PlayerUi& ui, std::shared_ptr<Library>& shown);

  void post(Handover next);
  std::optional<Handover> take();

  Storage& storage_;
  std::thread thread_;
  std::mutex mutex_;
  std::optional<Handover> handover_;  // guarded by mutex_: written on the builder thread
};

}  // namespace player
