project('wf-wallpaper', ['cpp'], version: '0.0.1', license: 'Unlicense',
	default_options: ['cpp_std=c++17', 'warning_level=2'])

wayfire = dependency('wayfire')
wlroots = dependency('wlroots', fallback: ['wlroots', 'wlroots'])
pixbuf = dependency('gdk-pixbuf-2.0')

plugin_wallpaper = shared_module('wallpaper',
	'wallpaper.cpp',
	dependencies: [wayfire, wlroots, pixbuf],
	install: true,
	install_dir: 'lib/wayfire')

install_data('wallpaper.xml', install_dir: 'share/wayfire/metadata')
