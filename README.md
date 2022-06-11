<div align="center">
	<h1>🖼 wf-wallpaper</h1>
	Advanced wallpaper plugin for <a href="https://wayfire.org/">Wayfire</a>
</div>

<div align="center">
	<h2>⭐️ Features</h2>
</div>

- Lets you have multiple wallpaper configurations that can target specific workspaces (TODO on specific outputs)
- Loads images using gdk-pixbuf (so you can install [webp-pixbuf-loader] etc. to expand the supported formats list)
	- in a subprocess, so a crash in the image loader won't bring the whole desktop down
- Also supports [Shadertoy] style GLSL shaders as wallpapers (TODO animation, mouse, etc.)
	- running heavy animations is a pretty silly waste of power, but making simple gradients, patterns, noise, etc. using shaders is essentially free, esp. in terms of VRAM!
- Can do fit, fill, stretch sizing
- Pretty efficient: each wallpaper is stored only once (as a texture/shader) no matter how many configurations/workspaces use it, rendering happens directly in response to damage, etc.
	
<div align="center">
	<h2>🔧 TODO</h2>
</div>

- Support directory galleries with transitions
- Support gnome xml-described transitions
- Support loading compressed textures to save VRAM?
- Support playing GIF/APNG animations

[Wayfire]: https://github.com/WayfireWM/wayfire
[webp-pixbuf-loader]: https://github.com/aruiz/webp-pixbuf-loader
[Shadertoy]: https://www.shadertoy.com

<div align="center">
	<h2>⬇️ Installing</h2>
</div>

- Do a standard meson build and install into the same prefix as wayfire itself. For default prefix:
```bash
meson build
ninja -Cbuild
sudo ninja -Cbuild install
```
- Add to the list of plugins in the wayfire configuration.

<div align="center">
	<h2>⚙️ Configuration</h2>
</div>

wf-wallpaper uses the dynamic object support in wf-config.
You have to create sections named `wallpaper:SOME-KEY`:

```
[wallpaper:aaaa]
path = /tmp/neon.glsl
workspaces = *

[wallpaper:bbbb]
path = /usr/local/share/backgrounds/gnome/adwaita-day.jpg
workspaces = 0,3,6

[wallpaper:cccc]
path = /usr/local/share/backgrounds/gnome/adwaita-night.jpg
workspaces = 2,5,8
```

[wf-gsettings] implements dynamic objects as relocatable schemas,
and requires each dynamic section to be mentioned in a special key:

```
% dconf dump /org/wayfire/
[gsettings]
dyn-sections=['wallpaper:w1']

[section/wallpaper/w1]
path='/usr/local/share/backgrounds/gnome/adwaita-day.jpg'
```

[wf-gsettings]: https://github.com/DankBSD/wf-gsettings


<div align="center">
	<h2>📄 License</h2>
</div>

This is free and unencumbered software released into the public domain.  
For more information, please refer to the `UNLICENSE` file or [unlicense.org](https://unlicense.org).
