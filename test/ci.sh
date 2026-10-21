#!/usr/bin/env bash
set -euo pipefail

echo 'Server = https://archive.archlinux.org/repos/2026/10/04/$repo/os/$arch' >/etc/pacman.d/mirrorlist
pacman -Syyuu --noconfirm --noprogressbar --needed hyprland pango libinput foot grim swaybg imagemagick jq mesa ttf-dejavu >/dev/null

cd "$(dirname "$0")/.."
make hyprzoom.so test/driver.so

chmod 666 /dev/dri/card0
useradd -m tester
install -d -o tester -m 700 /run/user/1000
chown -R tester .
su tester -c "XDG_RUNTIME_DIR=/run/user/1000 OUT=$PWD/out test/run.sh"
