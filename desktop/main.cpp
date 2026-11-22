#include "desktop/desktop_app.hpp"

#include <prism/app/model_app.hpp>

#include <cstdio>
#include <filesystem>

int main(int argc, char* argv[]) {
  if (argc != 2 || !std::filesystem::is_directory(argv[1])) {
    std::fprintf(stderr, "usage: %s <music folder>\n", argv[0]);
    return 2;
  }
  // The backend before the app: the app's sound output must close before the backend's
  // SDL_Quit(), which frees every SDL audio stream still open.
  auto backend = prism::app::Backend::software(prism::app::RenderConfig{});
  player::DesktopApp app{argv[1]};
  auto& window = backend.create_window({.title = "PRISM MP3 Player", .width = 600, .height = 300});
  prism::app::model_app(backend, window, app, [&](prism::app::AppContext& ctx) { app.start(ctx); });
  return 0;
}
