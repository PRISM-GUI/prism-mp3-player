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
