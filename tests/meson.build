doctest_dep = dependency('doctest', fallback : ['doctest', 'doctest_dep'])

test('storage', executable('test_storage', files('test_storage.cpp'),
  dependencies : [player_desktop_dep, doctest_dep]))
