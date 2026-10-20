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

## Load

In a Lua config (`hyprland.lua`):

```lua
hl.plugin.load("/path/to/hyprzoom.so")

hl.config({
    plugin = {
        hyprzoom = {
            zoom = 0.4,
            gap = 40,
        },
    },
})

if hl.plugin.hyprzoom then
    hl.plugin.hyprzoom.gesture({ fingers = 4, direction = "pinch" })
    hl.bind("SUPER + TAB", function() hl.plugin.hyprzoom.toggle() end)
end
```

`hl.plugin.hyprzoom` only exists once the plugin has loaded. Hyprland
reloads the config right after loading a plugin, so the guarded block runs
on that second pass.

To try it without touching the config: `hyprctl plugin load "$PWD/hyprzoom.so"`,
then `hyprctl dispatch hyprzoom:overview toggle`.

## Use

| | |
|---|---|
| pinch in / pinch out | zoom out / back in, following the fingers |
| `↑` `↓`, scroll | select the workspace above or below |
| click a window | go to it |
| click a workspace | go to it |
| drag a window onto a workspace | move it there; the outline below the last one makes a new workspace |
| `Enter`, `Space` | go to the selected workspace |
| `Esc` | go back to where the overview was opened |

Your keybinds keep working while it is open: switching workspace moves the
selection with it.

## Lua functions

| | |
|---|---|
| `hl.plugin.hyprzoom.toggle()` | open, or close to the selected workspace |
| `hl.plugin.hyprzoom.open()` | |
| `hl.plugin.hyprzoom.close()` | |
| `hl.plugin.hyprzoom.gesture{ fingers, direction, scale? }` | bind a touchpad gesture; `direction` is any `hl.gesture` direction, `pinch` and `up` read naturally |

The dispatcher `hyprzoom:overview` takes `toggle`, `open` or `close`.

## Config

Under `plugin.hyprzoom`:

| option | default | |
|---|---|---|
| `zoom` | `0.4` | size of a workspace when fully zoomed out |
| `gap` | `40` | pixels between workspaces when zoomed out |
| `backdrop_color` | `0xCC000000` | laid over the wallpaper behind the workspaces |
| `frame_color` | `0x33FFFFFF` | outline of the part of each workspace the monitor shows |
| `selected_color` | `0xCCFFFFFF` | outline of the selected workspace |
| `hover_color` | `0xFFFFFFFF` | outline of the window under the cursor |
| `gesture_distance` | `300` | gesture travel that zooms all the way out |

The zoom follows the `hyprzoom` animation, a child of `workspaces`:

```lua
hl.animation({ leaf = "hyprzoom", enabled = true, speed = 4, bezier = "easeInOut" })
```

## Debugging

The plugin logs at debug level with a `[hyprzoom]` prefix:

```sh
hyprctl rollinglog -f | grep hyprzoom
```
