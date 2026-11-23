# The player's OS-free core, the same list as player_core in the root meson.build. Its headers include
# "core/..." and "third_party/minimp3/...": the repository's root is the include directory.
set(root "${CMAKE_CURRENT_LIST_DIR}/../../../..")
# An explicit list, not a glob: ESP-IDF would not see a new core file until a reconfigure.
set(sources
    "${root}/core/cover_loader.cpp"
    "${root}/core/dir_storage.cpp"
    "${root}/core/fold.cpp"
    "${root}/core/folders.cpp"
    "${root}/core/library.cpp"
    "${root}/core/library_build.cpp"
    "${root}/core/memory_storage.cpp"
    "${root}/core/mp3_decoder.cpp"
    "${root}/core/player.cpp"
    "${root}/core/tags.cpp"
    "${root}/core/ui.cpp"
    "${root}/core/ui_draw.cpp"
    "${root}/core/wheel.cpp"
)

idf_component_register(
    SRCS ${sources}
    INCLUDE_DIRS "${root}"
    REQUIRES prism
)
