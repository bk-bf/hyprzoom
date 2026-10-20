# hyprzoom

A Hyprland plugin that zooms the desktop out into a vertical stack of
workspaces, the way niri's overview does. It works with Hyprland's own
layouts, the native `scrolling` layout included, and shows each workspace's
whole strip of windows, not only the part on screen.

Built against Hyprland **0.56.2**. A plugin only loads into the exact
Hyprland it was built for, so rebuild it after every Hyprland update.

## Build

```sh
make
```

Needs the Hyprland headers (`pkg-config --modversion hyprland` must print the
running version) and `pango`, `libinput`, `libdrm`, `pixman`.
