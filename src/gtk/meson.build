project('prettymux', 'c',
  version: '0.2.0',
  default_options: ['c_std=c17', 'warning_level=2'])

gtk4 = dependency('gtk4')
adw = dependency('libadwaita-1', version: '>=1.4')
webkit = dependency('webkitgtk-6.0')
json_glib = dependency('json-glib-1.0')

ghostty_dir = get_option('ghostty_dir')
glad_dir = ghostty_dir / 'vendor' / 'glad'

ghostty_inc = include_directories(
  ghostty_dir / 'include',
  glad_dir / 'include',
)

ghostty_lib_dir = ghostty_dir / 'zig-out' / 'lib'

sources = files(
  'main.c',
  'ghostty_terminal.c',
  'browser_tab.c',
  'theme.c',
  'shortcuts.c',
  'workspace.c',
  'session.c',
  'command_palette.c',
  'port_scanner.c',
  'socket_server.c',
  glad_dir / 'src' / 'gl.c',
)

executable('prettymux', sources,
  dependencies: [gtk4, adw, webkit, json_glib],
  include_directories: ghostty_inc,
  c_args: ['-DPRETTYMUX_SOURCE_DIR="' + meson.current_source_dir() + '"'],
  link_args: ['-L' + ghostty_lib_dir, '-lghostty', '-Wl,-rpath,' + ghostty_lib_dir],
  install: true,
)
