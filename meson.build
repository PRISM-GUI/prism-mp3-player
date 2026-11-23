project('prism-mp3-player', 'cpp',
  version : '0.0.1',
  default_options : ['cpp_std=c++26', 'warning_level=2'],
  meson_version : '>= 1.5.0',
)

prism = subproject('prism', default_options : ['tests=disabled', 'prism_debug_tools=disabled'])
prism_core_dep = prism.get_variable('prism_core_dep')
prism_dep = prism.get_variable('prism_dep')
prism_raster_dep = prism.get_variable('prism_raster_dep')
# prism::audio::Output and its device registry live with SDL's backend library (PRISM's meson puts
# them there when SDL is on, so the backend that registers itself is the one that gets linked):
# every target that builds a Player needs it.
prism_audio_dep = prism.get_variable('prism_software_backend_dep')

core_inc = include_directories('.')

player_core = static_library('player_core', files('core/cover_loader.cpp', 'core/dir_storage.cpp', 'core/fold.cpp', 'core/folders.cpp', 'core/library.cpp', 'core/library_build.cpp',
    'core/memory_storage.cpp', 'core/mp3_decoder.cpp', 'core/player.cpp', 'core/ui.cpp', 'core/ui_draw.cpp', 'core/wheel.cpp',
    'core/tags.cpp'),
  include_directories : core_inc,
  dependencies : [prism_core_dep, prism_audio_dep],
)
player_core_dep = declare_dependency(link_with : player_core, include_directories : core_inc,
  dependencies : [prism_core_dep, prism_audio_dep])

subdir('desktop')
subdir('tests')
