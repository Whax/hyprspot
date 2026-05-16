project('hyprspot', 'cpp',
  version: '0.1.0',
  default_options: ['buildtype=release', 'cpp_std=c++23'],
)

shared_module('hyprspot',
  'src/main.cpp',
  dependencies: [
    dependency('hyprland'),
    dependency('pixman-1'),
    dependency('libdrm'),
    dependency('pangocairo'),
    dependency('cairo'),
    dependency('wayland-server'),
    dependency('xkbcommon'),
  ],
  install: false,
  cpp_args: ['-fPIC', '-Wno-unused-parameter'],
  name_prefix: '',
)
