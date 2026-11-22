player_desktop_lib = static_library('player_desktop', files('dir_storage.cpp'),
  dependencies : player_core_dep,
)
player_desktop_dep = declare_dependency(link_with : player_desktop_lib, dependencies : player_core_dep)

player_desktop_app_lib = static_library('player_desktop_app', files('desktop_app.cpp', 'sdl_pcm_output.cpp'),
  dependencies : [player_desktop_dep, prism_dep],
)
player_desktop_app_dep = declare_dependency(link_with : player_desktop_app_lib,
  dependencies : [player_desktop_dep, prism_dep])

executable('prism-mp3-player-desktop', files('main.cpp'),
  dependencies : player_desktop_app_dep,
  install : true,
)
