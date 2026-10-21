#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
OUT=${OUT:-$ROOT/out}
mkdir -p "$OUT"
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}

KEY_ESC=1
KEY_ENTER=28
KEY_UP=103
KEY_LEFT=105
KEY_RIGHT=106
KEY_DOWN=108

failures=0
summary="$OUT/summary.md"
: >"$summary"
echo "| check | expected | actual | |" >"$OUT/checks.md"
echo "|---|---|---|---|" >>"$OUT/checks.md"
echo "| step | frames | median ms | p95 ms | max ms |" >"$OUT/frames.md"
echo "|---|---|---|---|---|" >>"$OUT/frames.md"

h() { hyprctl -i 0 "$@"; }
ev() {
    local out
    out=$(h eval "$1")
    [ "$out" = ok ] || { echo "eval failed: $1 -> $out" >&2; return 1; }
}
dispatch() { ev "hl.dispatch($1)"; }
key() { ev "hl.plugin.driver.key($1)"; }
shot() {
    grim -s 0.5 "$OUT/$1.png"
    grim -g "400,240 640x420" "$OUT/$1-row.png"
}

wait_for() {
    for _ in $(seq 100); do
        eval "$1" && return 0
        sleep 0.1
    done
    echo "timed out waiting for: $1" >&2
    return 1
}

check() {
    local name=$1 expected=$2 actual=$3
    if [ "$expected" = "$actual" ]; then
        echo "| $name | $expected | $actual | pass |" >>"$OUT/checks.md"
    else
        echo "| $name | $expected | $actual | **fail** |" >>"$OUT/checks.md"
        failures=$((failures + 1))
    fi
}

frames() {
    local name=$1
    ev "hl.plugin.driver.frames('$OUT/frames-$name.txt')"
    sort -n "$OUT/frames-$name.txt" | awk -v name="$name" '
        { v[NR] = $1 }
        END {
            if (NR == 0) { printf "| %s | 0 | - | - | - |\n", name; exit }
            p = int(NR * 0.95 + 0.5)
            if (p < 1) p = 1
            printf "| %s | %d | %.1f | %.1f | %.1f |\n", name, NR, v[int((NR + 1) / 2)], v[p], v[NR]
        }' >>"$OUT/frames.md"
}
record() { ev "hl.plugin.driver.record()"; }

spawn() {
    local ws=$1 cls=$2 color=$3 alpha=${4:-1.0}
    dispatch "hl.dsp.focus({ workspace = '$ws' })"
    dispatch "hl.dsp.exec_cmd([[foot -a $cls -o colors-dark.background=$color -o colors-dark.alpha=$alpha -o main.font=monospace:size=28 sh -c 'echo $cls; exec sleep 3600']])"
    wait_for "h clients -j | jq -e '.[] | select(.class == \"$cls\")' >/dev/null"
}

point() {
    h clients -j | jq -r --arg c "$1" --argjson fx "$2" --argjson fy "$3" \
        '.[] | select(.class == $c) | "\(432 + (.at[0] + .size[0] * (1 + $fx) / 2) * 0.4) \(270 + (.at[1] + .size[1] * (1 + $fy) / 2) * 0.4)"'
}

drag() {
    local step=$1 from to
    read -r -a from <<<"$(point "$2" 0 0)"
    read -r -a to <<<"$(point "$3" "$4" "$5")"
    ev "hl.plugin.driver.move(${from[0]}, ${from[1]})"
    ev "hl.plugin.driver.button(272, true)"
    ev "hl.plugin.driver.move($(((${from%.*} + ${to%.*}) / 2)), ${from[1]})"
    ev "hl.plugin.driver.move(${to[0]}, ${to[1]})"
    sleep 0.5
    shot "$step-dragging"
    ev "hl.plugin.driver.button(272, false)"
    sleep 1.5
    shot "$step-dropped"
}

order_x() { h clients -j | jq -r '[.[] | select(.workspace.id == 4)] | sort_by(.at[0]) | map("\(.class) \(.size[0])") | join(" ")'; }

active_ws() { h activeworkspace -j | jq -r .id; }
active_class() { h activewindow -j | jq -r .class; }

LIBSEAT_BACKEND=noop AQ_DRM_DEVICES=$CARD Hyprland --config "$HERE/hyprland.lua" >"$OUT/hyprland.log" 2>&1 &
HYPRLAND=$!
finish() {
    {
        echo "## Checks"
        cat "$OUT/checks.md"
        echo
        echo "## Frame intervals while open"
        cat "$OUT/frames.md"
    } >"$summary"
    kill $HYPRLAND 2>/dev/null || true
}
trap finish EXIT

wait_for "h version >/dev/null 2>&1"
h output create headless HEADLESS-1 >/dev/null
wait_for "h monitors -j | jq -e '.[] | select(.name == \"HEADLESS-1\")' >/dev/null"
export WAYLAND_DISPLAY=$(find "$XDG_RUNTIME_DIR" -maxdepth 1 -name 'wayland-[0-9]' -printf '%f\n' | head -1)

magick -size 1440x900 gradient:'#1b2a4a-#c2556b' "$OUT/wall.png"
swaybg -i "$OUT/wall.png" -m fill >/dev/null 2>&1 &

h plugin load "$ROOT/hyprzoom.so"
h plugin load "$HERE/driver.so"
ev 'hl.plugin.hyprzoom.gesture({ fingers = 4, direction = "up" })'

spawn 1 ws1 1d3557 0.7
spawn 3 ws3 2a6f4e
spawn 4 ws4-left 5b2a86
spawn 4 ws4-right b5651d 0.7
spawn 5 ws5 7a1f3d
dispatch "hl.dsp.focus({ window = 'class:ws4-left' })"
dispatch "hl.dsp.focus({ workspace = '1' })"
sleep 1


check "windows sit below the reserved bar area" "[0,30] [1440,870]" "$(h clients -j | jq -r '.[] | select(.class == "ws4-left") | "\(.at | tojson) \(.size | tojson)"')"

shot 00-desktop

record
ev "hl.plugin.hyprzoom.toggle()"
sleep 1
frames open
shot 01-open

record
key $KEY_DOWN
sleep 0.3
key $KEY_DOWN
sleep 1
frames down
shot 02-ws4-selected

record
key $KEY_RIGHT
sleep 1
frames right
shot 03-ws4-right

record
key $KEY_LEFT
sleep 1
frames left
shot 04-ws4-left

record
sleep 1
frames idle

ev "hl.plugin.driver.move(600, 400)"
ev "hl.plugin.driver.move(640, 420)"
sleep 0.5
shot 05-hover

key $KEY_ESC
sleep 1
shot 06-closed
check "Esc returns to the start workspace" 1 "$(active_ws)"

ev "hl.plugin.driver.swipe(4, 0, -400)"
sleep 1
shot 06b-swipe-opened
key $KEY_DOWN
sleep 0.3
key $KEY_ENTER
sleep 1
check "a 4-finger swipe up opens the overview" 3 "$(active_ws)"
dispatch "hl.dsp.focus({ workspace = '1' })"
sleep 0.5

ev "hl.plugin.hyprzoom.toggle()"
sleep 1
ev "hl.plugin.driver.swipe(4, 0, 400)"
sleep 1
shot 06c-swipe-closed
key $KEY_DOWN
sleep 0.3
key $KEY_ENTER
sleep 1
check "a 4-finger swipe down closes the overview" 1 "$(active_ws)"

ev "hl.plugin.hyprzoom.toggle()"
sleep 1
ev "hl.plugin.driver.swipe(3, 0, -400)"
sleep 1
shot 07-swiped-up
check "a 3-finger swipe in the overview leaves the active workspace" 1 "$(active_ws)"
ev "hl.plugin.driver.swipe(3, 0, 400)"
sleep 1
key $KEY_ESC
sleep 1
check "Esc after swiping still returns to the start workspace" 1 "$(active_ws)"

ev "hl.plugin.hyprzoom.toggle()"
sleep 1
key $KEY_DOWN
sleep 0.3
key $KEY_DOWN
sleep 1
drag 08 ws4-right ws4-left -0.8 0
check "dropping on a window's left zone makes the dragged one its own column left of it, widths unchanged" "ws4-right 1440 ws4-left 1440" "$(order_x)"

drag 09 ws4-right ws4-left 0 0.7
check "dropping on a window's bottom zone stacks the dragged one below it" "ws4-left ws4-right" "$(h clients -j | jq -r '[.[] | select(.workspace.id == 4)] | sort_by(.at[1]) | map(.class) | join(" ")')"
check "the stacked windows share a column" 1 "$(h clients -j | jq -r '[.[] | select(.workspace.id == 4) | .at[0]] | unique | length')"

drag 10 ws4-right ws4-left 0.8 0
check "dropping on a window's right zone makes the dragged one its own column right of it, widths unchanged" "ws4-left 1440 ws4-right 1440" "$(order_x)"
key $KEY_ESC
sleep 1

ev "hl.plugin.hyprzoom.toggle()"
sleep 1
key $KEY_DOWN
sleep 0.3
key $KEY_DOWN
sleep 0.3
key $KEY_RIGHT
sleep 0.5
key $KEY_ENTER
sleep 1.5
shot 11-entered
h clients -j | jq -c '.[] | {class, at, size}' >"$OUT/clients.json"
check "Enter goes to the selected workspace" 4 "$(active_ws)"
check "Enter goes to the selected window" ws4-right "$(active_class)"

ev "hl.plugin.driver.move(720, 450)"
sleep 0.5
ev "hl.plugin.hyprzoom.toggle()"
sleep 1
key $KEY_LEFT
sleep 0.5
key $KEY_ENTER
sleep 1.5
ev "hl.plugin.driver.move(724, 452)"
sleep 0.5
shot 12-same-workspace
check "Enter on another window of the same workspace keeps it focused" ws4-left "$(active_class)"

dispatch "hl.dsp.focus({ window = 'class:ws4-right' })"
sleep 1
ev "hl.plugin.driver.move(720, 450)"
ev "hl.plugin.hyprzoom.toggle()"
sleep 1
key $KEY_LEFT
sleep 1
ev "hl.plugin.driver.move(560, 450)"
sleep 0.3
shot 13-click-target
ev "hl.plugin.driver.button(272, true)"
ev "hl.plugin.driver.button(272, false)"
sleep 1.5
ev "hl.plugin.driver.move(564, 452)"
sleep 0.5
shot 13-clicked
check "clicking another window of the same workspace keeps it focused" ws4-left "$(active_class)"

exit $((failures > 0))
