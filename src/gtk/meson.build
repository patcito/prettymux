project('prettymux', 'c',
  version: '0.2.20',
  default_options: ['c_std=c17', 'warning_level=2'])

host_system = host_machine.system()
is_linux = host_system == 'linux'
is_macos = host_system == 'darwin'
is_windows = host_system == 'windows'
fs = import('fs')

if is_macos
  add_languages('objc')
endif

gtk4 = dependency('gtk4')
adw = dependency('libadwaita-1', version: '>=1.2')
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
  'theme.c',
  'hover_focus.c',
  'app_settings.c',
  'shortcuts.c',
  'settings_dialog.c',
  'workspace.c',
  'session.c',
  'project_icon_cache.c',
  'close_confirm.c',
  'command_palette.c',
  'port_scanner.c',
  'prettymux_agent_cli.c',
  'app_actions.c',
  'app_state.c',
  'app_support.c',
  'ghostty_actions.c',
  'notifications.c',
  'socket_commands.c',
  'terminal_routing.c',
  'sidebar_ui.c',
  'shortcuts_overlay.c',
  'pane_move_overlay.c',
  'resize_overlay.c',
  glad_dir / 'src' / 'gl.c',
)

browser_sources = []
platform_deps = []
platform_link_args = []
extra_include_dirs = [ghostty_inc]
build_rpath = ''
install_rpath = ''

if is_linux
  browser_sources += files(
    'browser_tab.c',
    'socket_server.c',
    'pip_window.c',
  )
  platform_deps += dependency('webkitgtk-6.0')
  platform_link_args += [
    ghostty_lib_dir / 'libghostty.so',
  ]
  build_rpath = ghostty_lib_dir
  install_rpath = '$ORIGIN/../lib/prettymux'
elif is_macos
  ghostty_macos_xcframework = ''
  ghostty_macos_lib = ''
  foreach candidate : [
    ghostty_dir / 'macos' / 'GhosttyKit.xcframework',
    ghostty_dir / 'zig-out' / 'macos' / 'GhosttyKit.xcframework',
  ]
    if fs.exists(candidate)
      ghostty_macos_xcframework = candidate
      foreach lib_candidate : [
        candidate / 'macos-arm64_x86_64' / 'libghostty.a',
        candidate / 'macos-arm64' / 'libghostty.a',
        candidate / 'macos-arm64_x86_64' / 'ghostty',
        candidate / 'macos-arm64' / 'ghostty',
      ]
        if fs.exists(lib_candidate)
          ghostty_macos_lib = lib_candidate
          break
        endif
      endforeach
      if ghostty_macos_lib != ''
        break
      endif
    endif
  endforeach

  if ghostty_macos_lib == ''
    error('Ghostty macOS library not found in ' + ghostty_dir)
  endif

  browser_sources += files(
    'browser_tab_macos.m',
    'socket_server.c',
    'pip_window_stub.c',
  )
  platform_link_args += [
    ghostty_macos_lib,
    '-framework', 'Cocoa',
    '-framework', 'Carbon',
    '-framework', 'CoreVideo',
    '-framework', 'IOSurface',
    '-framework', 'Metal',
    '-framework', 'QuartzCore',
    '-framework', 'WebKit',
    '-lc++',
  ]
elif is_windows
  webview2_sdk_dir = get_option('webview2_sdk_dir')
  if webview2_sdk_dir == ''
    error('Set WEBVIEW2_SDK_DIR to the unpacked Microsoft.Web.WebView2 SDK directory when building on Windows.')
  endif

  ghostty_windows_lib = ''
  foreach lib_candidate : [
    ghostty_lib_dir / 'libghostty.dll.a',
    ghostty_lib_dir / 'libghostty.a',
    ghostty_lib_dir / 'ghostty.lib',
  ]
    if fs.exists(lib_candidate)
      ghostty_windows_lib = lib_candidate
      break
    endif
  endforeach

  if ghostty_windows_lib == ''
    error('Ghostty Windows import library not found in ' + ghostty_lib_dir)
  endif

  browser_sources += files(
    'browser_tab_windows.c',
    'socket_server_stub.c',
    'pip_window_stub.c',
  )
  extra_include_dirs += [include_directories(webview2_sdk_dir / 'build' / 'native' / 'include')]
  platform_link_args += [
    ghostty_windows_lib,
    '-lole32',
    '-loleaut32',
    '-luuid',
    '-luser32',
  ]
else
  error('Unsupported host platform: ' + host_system)
endif

executable('prettymux', sources + browser_sources,
  dependencies: [gtk4, adw, json_glib] + platform_deps,
  include_directories: extra_include_dirs,
  c_args: [
    '-DPRETTYMUX_SOURCE_DIR="' + meson.current_source_dir() + '"',
    '-DPRETTYMUX_VERSION="' + meson.project_version() + '"',
  ],
  link_args: platform_link_args,
  build_rpath: build_rpath,
  install_rpath: install_rpath,
  install: true,
)

# CLI tool: prettymux-open (standalone, no GTK/ghostty deps)
if not is_windows
  executable('prettymux-open',
    files('prettymux-open.c'),
    install: true,
  )
endif

prettymux_desktop = configure_file(
  input: '../../packaging/prettymux.desktop',
  output: 'prettymux.desktop',
  copy: true,
)

prettymux_desktop_gnome = configure_file(
  input: '../../packaging/dev.prettymux.app.desktop',
  output: 'dev.prettymux.app.desktop',
  copy: true,
)

prettymux_icon = configure_file(
  input: '../../packaging/prettymux.svg',
  output: 'prettymux.svg',
  copy: true,
)

install_data(prettymux_desktop, install_dir: get_option('datadir') / 'applications')
install_data(prettymux_desktop_gnome, install_dir: get_option('datadir') / 'applications')
install_data(prettymux_icon, install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
