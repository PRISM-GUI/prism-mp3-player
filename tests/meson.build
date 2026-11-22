doctest_dep = dependency('doctest', fallback : ['doctest', 'doctest_dep'])

test('storage', executable('test_storage', files('test_storage.cpp'),
  dependencies : [player_desktop_dep, doctest_dep]))

test('tags', executable('test_tags', files('test_tags.cpp'),
  dependencies : [player_core_dep, doctest_dep]))

test('library', executable('test_library', files('test_library.cpp'),
  dependencies : [player_desktop_dep, doctest_dep]))

test('player', executable('test_player', files('test_player.cpp'),
  cpp_args : '-DTEST_DATA_DIR="@0@"'.format(meson.current_source_dir() / 'data'),
  dependencies : [player_core_dep, doctest_dep]))

test('player_threads', executable('test_player_threads', files('test_player_threads.cpp'),
  cpp_args : '-DTEST_DATA_DIR="@0@"'.format(meson.current_source_dir() / 'data'),
  dependencies : [player_core_dep, doctest_dep]))

test('wheel', executable('test_wheel', files('test_wheel.cpp'),
  dependencies : [player_core_dep, doctest_dep]))

test('ui', executable('test_ui', files('test_ui.cpp'),
  cpp_args : '-DTEST_DATA_DIR="@0@"'.format(meson.current_source_dir() / 'data'),
  dependencies : [player_core_dep, prism_raster_dep, doctest_dep]))

# SDL's display-free drivers, as PRISM's own SDL tests run (its tests/meson.build).
offscreen_env = environment({'SDL_VIDEODRIVER' : 'offscreen', 'SDL_AUDIO_DRIVER' : 'dummy'})

test('desktop_smoke', executable('test_desktop_smoke', files('test_desktop_smoke.cpp'),
  cpp_args : '-DTEST_DATA_DIR="@0@"'.format(meson.current_source_dir() / 'data'),
  dependencies : [player_desktop_app_dep, doctest_dep]),
  env : offscreen_env)
