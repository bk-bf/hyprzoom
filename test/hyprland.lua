hl.monitor({ output = "HEADLESS-1", mode = "2880x1800@60", position = "0x0", scale = 2, reserved = { top = 30 } })
hl.monitor({ output = "", disabled = true })

hl.config({
    general = {
        border_size = 0,
        gaps_in     = 0,
        gaps_out    = 0,
        layout      = "scrolling",
    },

    scrolling = {
        column_width = 1.0,
    },

    decoration = {
        rounding = 0,
        blur     = { enabled = true, size = 3, passes = 2 },
        shadow   = { enabled = true, range = 4, render_power = 3, color = "rgba(1a1a1aee)" },
    },

    misc = {
        disable_hyprland_logo    = true,
        disable_splash_rendering = true,
        force_default_wallpaper  = 0,
        background_color         = "rgb(3b2b4f)",
    },

    animations = { enabled = true },

    debug = { disable_logs = false, enable_stdout_logs = true, suppress_errors = true },
})

hl.animation({ leaf = "workspaces", enabled = true, speed = 3, bezier = "default", style = "slidevert" })

hl.gesture({ fingers = 3, direction = "vertical", action = "workspace" })
